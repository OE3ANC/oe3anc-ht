/* SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Pin scanning and backlight duty adapted from keyboard_c62/display_c62.
 */
#include "ui_io.h"
#include <errno.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_lcd_backlight));
static const struct gpio_dt_spec keyboard_light =
    GPIO_DT_SPEC_GET(DT_NODELABEL(ledkeyboard), gpios);
static const struct gpio_dt_spec white = GPIO_DT_SPEC_GET(DT_NODELABEL(flashlight), gpios);
static const struct gpio_dt_spec ptt = GPIO_DT_SPEC_GET(DT_NODELABEL(button_ptt), gpios);
static const struct gpio_dt_spec side = GPIO_DT_SPEC_GET(DT_NODELABEL(sidekey), gpios);
static const struct gpio_dt_spec drives[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(col1), gpios), GPIO_DT_SPEC_GET(DT_NODELABEL(col2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(col3), gpios), GPIO_DT_SPEC_GET(DT_NODELABEL(col4), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(col5), gpios)};
static const struct gpio_dt_spec senses[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(row1), gpios), GPIO_DT_SPEC_GET(DT_NODELABEL(row2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(row3), gpios), GPIO_DT_SPEC_GET(DT_NODELABEL(row4), gpios)};

static K_MUTEX_DEFINE(display_lock);
static K_MUTEX_DEFINE(ptt_lock);
static bool ptt_enabled = true;
static bool ptt_rearm;
static int64_t ptt_released_at = -1;
static bool power_allowed = !IS_ENABLED(CONFIG_HT_POWER);
static bool display_started;
static uint8_t backlight_level = 100;
static atomic_t redraw_requested;

/* All callers hold display_lock. Attempt each independent inactive output. */
static int lights_off(void) {
    const int white_error =
        gpio_is_ready_dt(&white) ? gpio_pin_configure_dt(&white, GPIO_OUTPUT_INACTIVE) : -ENODEV;
    const int keyboard_error = gpio_is_ready_dt(&keyboard_light)
                                   ? gpio_pin_configure_dt(&keyboard_light, GPIO_OUTPUT_INACTIVE)
                                   : -ENODEV;
    const int backlight_error =
        device_is_ready(backlight.dev) ? pwm_set_dt(&backlight, PWM_USEC(1000), 0) : -ENODEV;
    return white_error ? white_error : keyboard_error ? keyboard_error : backlight_error;
}

static int lights_on(void) {
    int error = gpio_pin_configure_dt(&white, GPIO_OUTPUT_INACTIVE);
    if (!error) {
        error = gpio_pin_configure_dt(&keyboard_light, GPIO_OUTPUT_ACTIVE);
    }
    if (!error) {
        /* Preserve the reference's nonlinear level curve without float math. */
        const uint32_t duty =
            backlight_level ? (backlight_level * backlight_level + 52) / 105 + 5 : 0;
        error = pwm_set_dt(&backlight, PWM_USEC(1000), PWM_USEC(10 * duty));
    }
    if (error) {
        power_allowed = false;
        (void)lights_off();
    }
    return error;
}

int c62_backlight_set(uint8_t percent) {
    if (percent > 100) {
        return -EINVAL;
    }
    k_mutex_lock(&display_lock, K_FOREVER);
    const bool changed = percent != backlight_level;
    backlight_level = percent;
    int error = 0;
    if (changed && power_allowed && display_started) {
        const uint32_t duty = percent ? (percent * percent + 52) / 105 + 5 : 0;
        error = pwm_set_dt(&backlight, PWM_USEC(1000), PWM_USEC(10 * duty));
        if (error) {
            power_allowed = false;
            (void)lights_off();
        }
    }
    k_mutex_unlock(&display_lock);
    return error;
}

int c62_ui_set_power(bool active) {
    k_mutex_lock(&display_lock, K_FOREVER);
    const bool was_allowed = power_allowed;
    power_allowed = active;
    const int error = !active ? lights_off() : display_started ? lights_on() : 0;
    if (!error && active && !was_allowed) {
        atomic_set(&redraw_requested, 1);
    }
    k_mutex_unlock(&display_lock);
    return error;
}

bool c62_display_redraw_requested(void) {
    return atomic_cas(&redraw_requested, 1, 0);
}

int c62_controls_init(void) {
    if (!gpio_is_ready_dt(&ptt) || !gpio_is_ready_dt(&side)) {
        return -ENODEV;
    }
    int error = gpio_pin_configure_dt(&ptt, GPIO_INPUT);
    if (!error) {
        error = gpio_pin_configure_dt(&side, GPIO_INPUT);
    }
    for (size_t i = 0; !error && i < ARRAY_SIZE(drives); ++i) {
        error = gpio_is_ready_dt(&drives[i])
                    ? gpio_pin_configure_dt(&drives[i], GPIO_OUTPUT_INACTIVE)
                    : -ENODEV;
    }
    for (size_t i = 0; !error && i < ARRAY_SIZE(senses); ++i) {
        error =
            gpio_is_ready_dt(&senses[i]) ? gpio_pin_configure_dt(&senses[i], GPIO_INPUT) : -ENODEV;
    }
    return error;
}

int c62_ptt_read(void) {
    k_mutex_lock(&ptt_lock, K_FOREVER);
    int value = ptt_enabled ? gpio_pin_get_dt(&ptt) : 0;
    if (ptt_enabled && ptt_rearm && value >= 0) {
        if (value) {
            ptt_released_at = -1;
        } else if (ptt_released_at < 0) {
            ptt_released_at = k_uptime_get();
        } else if (k_uptime_get() - ptt_released_at >= 20) {
            ptt_rearm = false;
        }
        value = 0;
    }
    k_mutex_unlock(&ptt_lock);
    return value;
}

int c62_ptt_enable(bool enabled) {
    k_mutex_lock(&ptt_lock, K_FOREVER);
    int error = 0;
    if (!enabled && ptt_enabled) {
        const int value = gpio_pin_get_dt(&ptt);
        error = value < 0 ? value : value ? -EBUSY : 0;
    } else if (enabled && !ptt_enabled) {
        error = gpio_pin_configure_dt(&ptt, GPIO_INPUT);
    }
    if (!error) {
        ptt_enabled = enabled;
        ptt_rearm = true;
        ptt_released_at = -1;
    }
    k_mutex_unlock(&ptt_lock);
    return error;
}

int c62_monitor_read(void) {
    return gpio_pin_get_dt(&side);
}

int c62_keys_read(uint32_t *keys) {
    *keys = 0;
    for (size_t row = 0; row < ARRAY_SIZE(drives); ++row) {
        int error = gpio_pin_set_dt(&drives[row], 1);
        if (!error) {
            k_busy_wait(1);
            for (size_t col = 0; col < ARRAY_SIZE(senses); ++col) {
                const int value = gpio_pin_get_dt(&senses[col]);
                if (value < 0) {
                    error = value;
                    break;
                }
                if (value) {
                    *keys |= 1U << (row * 4 + col);
                }
            }
        }
        const int restore = gpio_pin_set_dt(&drives[row], 0);
        if (error || restore) {
            return error ? error : restore;
        }
    }
    return 0;
}

int c62_display_init(void) {
    k_mutex_lock(&display_lock, K_FOREVER);
    int error = 0;
    if (!device_is_ready(display) || !device_is_ready(backlight.dev) ||
        !gpio_is_ready_dt(&keyboard_light) || !gpio_is_ready_dt(&white)) {
        error = -ENODEV;
    } else {
        struct display_capabilities caps;
        display_get_capabilities(display, &caps);
        if (caps.x_resolution != 160 || caps.y_resolution != 128 ||
            caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
            error = -ENOTSUP;
        } else {
            error = display_blanking_off(display);
            if (!error) {
                display_started = true;
                error = power_allowed ? lights_on() : lights_off();
            }
        }
    }
    k_mutex_unlock(&display_lock);
    return error;
}

int c62_display_write(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const void *pixels,
                      size_t size) {
    const struct display_buffer_descriptor descriptor = {
        .buf_size = size, .width = width, .height = height, .pitch = width};
    k_mutex_lock(&display_lock, K_FOREVER);
    const int error = power_allowed ? display_write(display, x, y, &descriptor, pixels) : 0;
    k_mutex_unlock(&display_lock);
    return error;
}
