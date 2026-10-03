/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rf_io.h"
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>

static const struct pwm_dt_spec apc = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rf_apc));
static const struct gpio_dt_spec speaker = GPIO_DT_SPEC_GET(DT_NODELABEL(speaker_enable), gpios);
static const struct gpio_dt_spec dtmf = GPIO_DT_SPEC_GET(DT_NODELABEL(dtmf_enable), gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_NODELABEL(ledrx), gpios);

int c62_apc(uint8_t duty_percent) {
    if (duty_percent > 100) {
        return -EINVAL;
    }
    // Preserve the tested 1 kHz APC; requested milliwatts are not measured RF power.
    return pwm_set_dt(&apc, PWM_USEC(1000), PWM_USEC(10 * duty_percent));
}

int c62_speaker(bool enabled) {
    return gpio_pin_set_dt(&speaker, enabled);
}

int c62_receive_led(bool enabled) {
    return gpio_pin_set_dt(&green, enabled);
}

int c62_rf_io_init(void) {
    if (!device_is_ready(apc.dev) || !gpio_is_ready_dt(&speaker) || !gpio_is_ready_dt(&dtmf) ||
        !gpio_is_ready_dt(&green)) {
        return -ENODEV;
    }
    // Try every inactive output even if another operation fails.
    const int power_error = c62_apc(0);
    const int speaker_error = gpio_pin_configure_dt(&speaker, GPIO_OUTPUT_INACTIVE);
    const int dtmf_error = gpio_pin_configure_dt(&dtmf, GPIO_OUTPUT_INACTIVE);
    const int led_error = gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
    return power_error     ? power_error
           : speaker_error ? speaker_error
           : dtmf_error    ? dtmf_error
                           : led_error;
}
