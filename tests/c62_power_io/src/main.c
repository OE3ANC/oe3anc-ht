/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <errno.h>

static unsigned configs, pwm_calls, writes, blanking;
static int gpio_error, pwm_error;
static bool keyboard_on, white_on, block_write;
static uint32_t pulse;
static K_SEM_DEFINE(write_entered, 0, 1);
static K_SEM_DEFINE(write_finish, 0, 1);
static K_SEM_DEFINE(off_done, 0, 1);

enum {
    ledkeyboard = 0,
    flashlight,
    button_ptt,
    sidekey,
    col1,
    col2,
    col3,
    col4,
    col5,
    row1,
    row2,
    row3,
    row4
};

static int configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags) {
    ARG_UNUSED(dev);
    ++configs;
    if (pin == ledkeyboard) {
        keyboard_on = (flags & GPIO_OUTPUT_INIT_HIGH) != 0;
    }
    if (pin == flashlight) {
        white_on = (flags & GPIO_OUTPUT_INIT_HIGH) != 0;
    }
    return gpio_error;
}

static int set_cycles(const struct device *dev, uint32_t channel, uint32_t period, uint32_t duty,
                      pwm_flags_t flags) {
    ARG_UNUSED(dev);
    ARG_UNUSED(channel);
    ARG_UNUSED(flags);
    zassert_equal(period, 1000);
    ++pwm_calls;
    pulse = duty;
    return pwm_error;
}

static int get_cycles(const struct device *dev, uint32_t channel, uint64_t *cycles) {
    ARG_UNUSED(dev);
    ARG_UNUSED(channel);
    *cycles = 1000000;
    return 0;
}

static void capabilities(const struct device *dev, struct display_capabilities *caps) {
    ARG_UNUSED(dev);
    *caps = (struct display_capabilities){
        .x_resolution = 160, .y_resolution = 128, .current_pixel_format = PIXEL_FORMAT_RGB_565};
}

static int blank_off(const struct device *dev) {
    ARG_UNUSED(dev);
    ++blanking;
    return 0;
}

static int display_write_fake(const struct device *dev, uint16_t x, uint16_t y,
                              const struct display_buffer_descriptor *desc, const void *pixels) {
    ARG_UNUSED(dev);
    ARG_UNUSED(x);
    ARG_UNUSED(y);
    ARG_UNUSED(desc);
    ARG_UNUSED(pixels);
    ++writes;
    if (block_write) {
        k_sem_give(&write_entered);
        k_sem_take(&write_finish, K_FOREVER);
    }
    return 0;
}
static const struct gpio_driver_config gpio_config = {.port_pin_mask = UINT32_MAX};
static struct gpio_driver_data gpio_data;
static const struct gpio_driver_api gpio_api = {.pin_configure = configure};
static const struct pwm_driver_api pwm_api = {.set_cycles = set_cycles,
                                              .get_cycles_per_sec = get_cycles};
static const struct display_driver_api display_api = {
    .write = display_write_fake, .get_capabilities = capabilities, .blanking_off = blank_off};
static struct device_state ready = {.initialized = true};
static const struct device gpio_device = {
    .api = &gpio_api, .config = &gpio_config, .data = &gpio_data, .state = &ready};
static const struct device pwm_device = {.api = &pwm_api, .state = &ready};
static const struct device display_device = {.api = &display_api, .state = &ready};
#undef DT_NODELABEL
#define DT_NODELABEL(node) node
#undef DEVICE_DT_GET
#define DEVICE_DT_GET(node) (&display_device)
#undef GPIO_DT_SPEC_GET
#define GPIO_DT_SPEC_GET(node, prop) {.port = &gpio_device, .pin = node, .dt_flags = 0}
#undef PWM_DT_SPEC_GET
#define PWM_DT_SPEC_GET(node) {.dev = &pwm_device, .channel = 0, .period = 1000000, .flags = 0}
#include "../../../backends/c62/ui_io.c"

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    configs = pwm_calls = writes = blanking = 0;
    gpio_error = pwm_error = 0;
    keyboard_on = white_on = block_write = false;
    backlight_level = 100;
    pulse = 0;
    display_started = false;
    power_allowed = false;
    atomic_clear(&redraw_requested);
}

ZTEST(c62_power_io, test_gate_does_not_initialize_display_and_blocks_flushes) {
    zassert_ok(c62_ui_set_power(false));
    zassert_false(c62_display_redraw_requested());
    zassert_equal(configs, 2);
    zassert_equal(pwm_calls, 1);
    zassert_equal(blanking, 0);
    zassert_false(keyboard_on || white_on);
    zassert_equal(pulse, 0);
    zassert_ok(c62_display_write(0, 0, 1, 1, NULL, 2));
    zassert_equal(writes, 0);
    zassert_ok(c62_ui_set_power(true)); // no light before initial display startup
    zassert_equal(pulse, 0);
    zassert_equal(blanking, 0);
    zassert_true(c62_display_redraw_requested());
    zassert_false(c62_display_redraw_requested());
    zassert_ok(c62_display_init());
    zassert_true(keyboard_on);
    zassert_equal(pulse, 1000);
    zassert_equal(blanking, 1);
    zassert_ok(c62_display_write(0, 0, 1, 1, NULL, 2));
    zassert_equal(writes, 1);
    zassert_ok(c62_ui_set_power(false));
    zassert_false(keyboard_on);
    zassert_equal(pulse, 0);
    zassert_ok(c62_display_write(0, 0, 1, 1, NULL, 2));
    zassert_equal(writes, 1);
    zassert_ok(c62_ui_set_power(true));
    zassert_true(keyboard_on);
    zassert_equal(pulse, 1000);
    zassert_true(c62_display_redraw_requested());
    zassert_false(c62_display_redraw_requested());
}

ZTEST(c62_power_io, test_inactive_init_and_failed_activation_attempt_all_off_outputs) {
    zassert_ok(c62_display_init()); // race with switch-off does not light
    zassert_false(keyboard_on);
    zassert_equal(pulse, 0);
    gpio_error = -EIO;
    zassert_equal(c62_ui_set_power(false), -EIO);
    zassert_equal(configs, 4);
    zassert_equal(pwm_calls, 2); // GPIO error does not skip PWM
    gpio_error = 0;
    pwm_error = -EPIPE;
    zassert_equal(c62_ui_set_power(true), -EPIPE);
    zassert_false(power_allowed);
    zassert_false(keyboard_on);
    zassert_equal(pulse, 0);
    zassert_false(c62_display_redraw_requested());
    zassert_ok(c62_display_write(0, 0, 1, 1, NULL, 2));
    zassert_equal(writes, 0);
}

K_THREAD_STACK_DEFINE(write_stack, 1024);
K_THREAD_STACK_DEFINE(off_stack, 1024);
static struct k_thread writer, stopper;

static void write_worker(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    zassert_ok(c62_display_write(0, 0, 1, 1, NULL, 2));
}

static void off_worker(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    zassert_ok(c62_ui_set_power(false));
    k_sem_give(&off_done);
}

ZTEST(c62_power_io, test_shutdown_serializes_with_inflight_flush) {
    zassert_ok(c62_ui_set_power(true));
    zassert_ok(c62_display_init());
    block_write = true;
    k_thread_create(&writer, write_stack, K_THREAD_STACK_SIZEOF(write_stack), write_worker, NULL,
                    NULL, NULL, 1, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&write_entered, K_MSEC(50)));
    k_thread_create(&stopper, off_stack, K_THREAD_STACK_SIZEOF(off_stack), off_worker, NULL, NULL,
                    NULL, 1, 0, K_NO_WAIT);
    zassert_equal(k_sem_take(&off_done, K_MSEC(10)), -EAGAIN);
    zassert_true(keyboard_on);
    zassert_equal(pulse, 1000);
    k_sem_give(&write_finish);
    zassert_ok(k_sem_take(&off_done, K_MSEC(50)));
    zassert_ok(k_thread_join(&writer, K_MSEC(50)));
    zassert_ok(k_thread_join(&stopper, K_MSEC(50)));
    zassert_false(keyboard_on);
    zassert_equal(pulse, 0);
    zassert_equal(writes, 1);
    zassert_ok(c62_display_write(0, 0, 1, 1, NULL, 2));
    zassert_equal(writes, 1);
}

ZTEST_SUITE(c62_power_io, NULL, NULL, before, NULL, NULL);

void power_io_test_reset(void) {
    before(NULL);
}

unsigned power_io_test_off_calls(void) {
    return pwm_calls;
}

ZTEST(c62_power_io, test_brightness_curve_noop_and_inactive_gate) {
    zassert_ok(c62_backlight_set(25));
    zassert_equal(pwm_calls, 0);
    zassert_ok(c62_ui_set_power(true));
    zassert_ok(c62_display_init());
    zassert_equal(pulse, 110);
    const unsigned calls = pwm_calls;
    zassert_ok(c62_backlight_set(25));
    zassert_equal(pwm_calls, calls);
    zassert_ok(c62_backlight_set(50));
    zassert_equal(pulse, 290);
    zassert_ok(c62_backlight_set(75));
    zassert_equal(pulse, 590);
    zassert_ok(c62_backlight_set(100));
    zassert_equal(pulse, 1000);
    zassert_ok(c62_backlight_set(10));
    zassert_equal(pulse, 60);
    zassert_ok(c62_backlight_set(20));
    zassert_equal(pulse, 90);
    zassert_ok(c62_backlight_set(30));
    zassert_equal(pulse, 140);
    zassert_ok(c62_backlight_set(0));
    zassert_equal(pulse, 0);
    zassert_equal(c62_backlight_set(101), -EINVAL);
    zassert_equal(pulse, 0);
    zassert_ok(c62_ui_set_power(false));
    const unsigned off_calls = pwm_calls;
    zassert_ok(c62_backlight_set(75));
    zassert_equal(pwm_calls, off_calls);
    zassert_equal(pulse, 0);
    zassert_false(power_allowed);
    zassert_ok(c62_ui_set_power(true));
    zassert_equal(pulse, 590);
}

ZTEST(c62_power_io, test_brightness_failure_closes_output_gate) {
    zassert_ok(c62_ui_set_power(true));
    zassert_ok(c62_display_init());
    pwm_error = -EIO;
    zassert_equal(c62_backlight_set(25), -EIO);
    zassert_false(power_allowed);
    zassert_false(keyboard_on);
    pwm_error = 0;
    const unsigned calls = pwm_calls;
    zassert_ok(c62_backlight_set(100));
    zassert_equal(pwm_calls, calls);
    zassert_ok(c62_display_write(0, 0, 1, 1, NULL, 2));
    zassert_equal(writes, 0);
}
