// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual C62 transport using bounded FIFO/register models.
#include "fakes/uart.h"
#include <errno.h>
#include <ht/companion.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static struct device fake_device;
static int uart_companion_pins_config;
static bool ready = true, physical_ptt, rx_on, err_on, uart_pins;
static int pin_error, callback_error, gpio_error, read_error;
static uart_irq_callback_user_data_t callback;
static unsigned configure_count, gpio_samples;
static int mux_read_error, mux_gpio_error, mux_restore_error;
static bool late_ready;

static void delayed_sleep(k_timeout_t timeout) {
    if (late_ready) {
        k_sleep(K_MSEC(20));
        tx_ready = true;
        late_ready = false;
    } else {
        k_sleep(timeout);
    }
}

static int configure(const struct uart_config *config) {
    zassert_equal(config->baudrate, 115200);
    ++configure_count;
    return 0;
}

static int pins(const int *config) {
    if (pin_error && config == &uart_companion_pins_config) {
        return pin_error;
    }
    uart_pins = config == &uart_companion_pins_config;
    return 0;
}

static int set_callback(uart_irq_callback_user_data_t next) {
    if (callback_error) {
        return callback_error;
    }
    callback = next;
    return 0;
}

static int sample_gpio() {
    ++gpio_samples;
    // With UART selected, DATAIN no longer tracks the PTT pad.
    return read_error ? read_error : uart_pins ? 0 : int(physical_ptt);
}

#include "fakes/IOMuxManager.h"

int32_t IOMuxManager_GetConfig(uint8_t pad, uint8_t pin, uint32_t *function) {
    zassert_equal(pad, CSK_IOMUX_PAD_A);
    zassert_equal(pin, 15);
    *function = uart_pins ? 3 : CSK_IOMUX_FUNC_DEFAULT;
    return mux_read_error;
}

int32_t IOMuxManager_PinConfigure(uint8_t pad, uint8_t pin, uint32_t function) {
    zassert_equal(pad, CSK_IOMUX_PAD_A);
    zassert_equal(pin, 15);
    const int error = function == CSK_IOMUX_FUNC_DEFAULT ? mux_gpio_error : mux_restore_error;
    if (!error) {
        uart_pins = function != CSK_IOMUX_FUNC_DEFAULT;
    }
    return error;
}

#undef GPIO_DT_SPEC_GET
#define GPIO_DT_SPEC_GET(node, prop) {nullptr, 15, 0}
#define gpio_is_ready_dt(spec) ready
#define gpio_pin_configure_dt(spec, flags) gpio_error
#define gpio_pin_get_dt(spec) sample_gpio()
#undef DT_NODELABEL
#define DT_NODELABEL(node) node
#undef DT_REG_ADDR
#define DT_REG_ADDR(node) ((uintptr_t)&fake_hw)
#undef DEVICE_DT_GET
#define DEVICE_DT_GET(node) (&fake_device)
#undef PINCTRL_DT_DEFINE
#define PINCTRL_DT_DEFINE(node)
#undef PINCTRL_DT_DEV_CONFIG_GET
#define TEST_PIN_CONFIG(node) (&node##_pins_config)
#define PINCTRL_DT_DEV_CONFIG_GET(node) TEST_PIN_CONFIG(node)
#define device_is_ready(dev) ready
#define uart_configure(dev, config) configure(config)
#define pinctrl_apply_state(config, state) pins(config)
#define uart_irq_callback_user_data_set(dev, cb, arg) set_callback(cb)
#define uart_irq_rx_enable(dev) (rx_on = true)
#define uart_irq_rx_disable(dev) (rx_on = false)
#define uart_irq_err_enable(dev) (err_on = true)
#define uart_irq_err_disable(dev) (err_on = false)
#define k_sleep(timeout) delayed_sleep(timeout)
#include "../../../backends/c62/companion.c"

static void before(void *) {
    ready = true;
    physical_ptt = false;
    initialized = false;
    ring_buf_reset(&rx);
    rx_error = 0;
    pin_error = callback_error = gpio_error = read_error = 0;
    input_size = input_pos = output_size = status_reads = configure_count = 0;
    rx_on = err_on = uart_pins = false;
    tx_ready = true;
    late_ready = false;
    line_error = 0;
    mux_read_error = mux_gpio_error = mux_restore_error = 0;
    gpio_samples = 0;
    zassert_ok(ht_companion_restore_console());
}

static void receive(unsigned count = 32) {
    input_pos = 0;
    input_size = count;
    for (unsigned i = 0; i < count; ++i) {
        input[i] = i;
    }
    callback(&fake_device, nullptr);
}

static void arm() {
    physical_ptt = false;
    zassert_equal(c62_uart_ptt_read(), 0);
    k_sleep(K_MSEC(20));
    zassert_equal(c62_uart_ptt_read(), 0);
}

static void press_break() {
    physical_ptt = true;
    line_error = UARTC_LSR_BI | UARTC_LSR_FE;
    receive(1);
}

ZTEST(c62_companion, test_continuous_reception_and_audio_restore) {
    receive(1);
    uint8_t data[2];
    zassert_equal(ht_companion_read(data, sizeof(data)), 1, "UART available immediately");
    zassert_true(rx_on && err_on && uart_pins);
    const auto configs = configure_count;
    zassert_ok(ht_companion_restore_console());
    physical_ptt = true;
    zassert_true(rx_on && err_on && uart_pins, "PTT must not steal the UART mux");
    zassert_ok(ht_companion_restore_console());
    zassert_equal(configure_count, configs);
}

ZTEST(c62_companion, test_initialization_errors_retry_without_arming_ptt) {
    initialized = false;
    ready = false;
    zassert_equal(ht_companion_restore_console(), -ENODEV);
    ready = true;
    gpio_error = -EIO;
    zassert_equal(ht_companion_restore_console(), -EIO);
    gpio_error = 0;
    pin_error = -EINVAL;
    zassert_equal(ht_companion_restore_console(), -EINVAL);
    pin_error = 0;
    callback_error = -ENOTSUP;
    zassert_equal(ht_companion_restore_console(), -ENOTSUP);
    zassert_false(initialized);
    callback_error = 0;
    zassert_ok(ht_companion_restore_console());
    read_error = -EIO;
    zassert_equal(c62_uart_ptt_read(), -EIO);
}

ZTEST(c62_companion, test_delayed_press_immediate_release_and_startup_hold) {
    press_break();
    k_sleep(K_MSEC(500));
    zassert_equal(c62_uart_ptt_read(), 0, "Startup hold must be consumed");
    arm();
    press_break();
    zassert_equal(c62_uart_ptt_read(), 0);
    k_sleep(K_MSEC(250));
    zassert_equal(c62_uart_ptt_read(), 0);
    k_sleep(K_MSEC(50));
    zassert_equal(c62_uart_ptt_read(), 1);
    physical_ptt = false;
    zassert_equal(c62_uart_ptt_read(), 0, "Release has no hold delay");
}

ZTEST(c62_companion, test_valid_data_resets_hold_even_when_every_sample_is_low) {
    arm();
    physical_ptt = true;
    zassert_equal(c62_uart_ptt_read(), 0);
    for (unsigned i = 0; i < 80; ++i) {
        receive(1); // Stop bits occur between low GPIO samples.
        k_sleep(K_MSEC(5));
        zassert_equal(c62_uart_ptt_read(), 0);
    }
    k_sleep(K_MSEC(UART_QUIET_MS));
    zassert_equal(c62_uart_ptt_read(), 0);
    k_sleep(K_MSEC(300));
    zassert_equal(c62_uart_ptt_read(), 1, "A subsequent sustained low is physical PTT");
}

ZTEST(c62_companion, test_plain_low_needs_no_break_and_short_pulses_do_not_accumulate) {
    arm();
    for (unsigned i = 0; i < 3; ++i) {
        physical_ptt = true;
        zassert_equal(c62_uart_ptt_read(), 0);
        k_sleep(K_MSEC(200));
        zassert_equal(c62_uart_ptt_read(), 0);
        physical_ptt = false;
        zassert_equal(c62_uart_ptt_read(), 0);
    }
    physical_ptt = true;
    zassert_equal(c62_uart_ptt_read(), 0);
    k_sleep(K_MSEC(300));
    zassert_equal(c62_uart_ptt_read(), 1);
    physical_ptt = false;
    zassert_equal(c62_uart_ptt_read(), 0);
    receive(1);
    uint8_t byte;
    zassert_equal(ht_companion_read(&byte, 1), 1, "UART still works after PTT release");
}

ZTEST(c62_companion, test_mux_sample_restores_uart_and_propagates_failures) {
    arm();
    physical_ptt = true;
    zassert_equal(sample_gpio(), 0, "Old UART-muxed read cannot see the press");
    zassert_equal(c62_uart_ptt_read(), 0);
    zassert_true(uart_pins);
    k_sleep(K_MSEC(300));
    zassert_equal(c62_uart_ptt_read(), 1);
    zassert_true(uart_pins);
    read_error = -EIO;
    zassert_equal(c62_uart_ptt_read(), -EIO);
    zassert_true(uart_pins, "Restore UART even when the GPIO read fails");
    read_error = 0;
    mux_gpio_error = -1;
    zassert_equal(c62_uart_ptt_read(), -EIO);
    zassert_true(uart_pins);
    mux_gpio_error = 0;
    mux_read_error = -1;
    zassert_equal(c62_uart_ptt_read(), -EIO);
    zassert_true(uart_pins);
    mux_read_error = 0;
    mux_restore_error = -1;
    zassert_equal(c62_uart_ptt_read(), -EIO);
}

ZTEST(c62_companion, test_active_uart_burst_is_not_probed_as_gpio) {
    arm();
    physical_ptt = true;
    const unsigned samples = gpio_samples;
    for (unsigned i = 0; i < 80; ++i) {
        receive(1);
        zassert_equal(c62_uart_ptt_read(), 0);
        zassert_true(uart_pins && rx_on && err_on);
    }
    zassert_equal(gpio_samples, samples);
    received_at = k_uptime_get() - (UART_QUIET_MS - 1);
    zassert_equal(c62_uart_ptt_read(), 0);
    zassert_equal(gpio_samples, samples);
    received_at = k_uptime_get() - UART_QUIET_MS;
    zassert_equal(c62_uart_ptt_read(), 0);
    zassert_equal(gpio_samples, samples + 1);
    zassert_true(uart_pins && rx_on && err_on);
}

ZTEST(c62_companion, test_threshold_and_pending_uart_byte) {
    arm();
    physical_ptt = true;
    zassert_equal(c62_uart_ptt_read(), 0);
    low_at = k_uptime_get() - 299;
    zassert_equal(c62_uart_ptt_read(), 0);
    low_at = k_uptime_get() - 300;
    zassert_equal(c62_uart_ptt_read(), 1);
    input_pos = 0;
    input_size = 1; // A valid byte is pending before its ISR runs.
    zassert_equal(c62_uart_ptt_read(), 0);
    callback(&fake_device, nullptr);
    zassert_equal(c62_uart_ptt_read(), 0);
}

ZTEST(c62_companion, test_repeated_breaks_and_framing_errors_do_not_restart_hold) {
    arm();
    physical_ptt = true;
    zassert_equal(c62_uart_ptt_read(), 0);
    for (unsigned i = 0; i < 4; ++i) {
        line_error = i % 2 ? UARTC_LSR_FE : UARTC_LSR_BI | UARTC_LSR_FE;
        receive(1);
        k_sleep(K_MSEC(100));
    }
    zassert_equal(c62_uart_ptt_read(), 1);
}

ZTEST(c62_companion, test_clear_on_read_break_survives_tx_and_control_polls) {
    arm();
    physical_ptt = true;
    line_error = UARTC_LSR_BI | UARTC_LSR_FE;
    input_pos = 0;
    input_size = 1;
    uint8_t byte = 7;
    zassert_equal(ht_companion_write(&byte, 1, 10), 1);
    zassert_equal(c62_uart_ptt_read(), 0);
    callback(&fake_device, nullptr); // BI has already been acknowledged by TX.
    k_sleep(K_MSEC(300));
    zassert_equal(c62_uart_ptt_read(), 1);
    zassert_equal(ht_companion_read(&byte, 1), -EIO);
    zassert_equal(ht_companion_read(&byte, 1), 0, "Break byte is never protocol input");
}

ZTEST(c62_companion, test_full_duplex_rx_overflow_and_line_error_recovery) {
    tx_ready = false; // A vendor fifo_read would spin forever here.
    status_reads = 0;
    receive();
    zassert_equal(status_reads, 32, "ISR drain must be bounded by FIFO depth");
    uint8_t data[512];
    zassert_equal(ht_companion_read(data, sizeof(data)), 32);
    for (unsigned i = 0; i < 32; ++i) {
        zassert_equal(data[i], i);
    }
    for (unsigned i = 0; i < 65; ++i) {
        receive();
    }
    zassert_equal(ht_companion_read(data, sizeof(data)), -EOVERFLOW);
    zassert_equal(ht_companion_read(data, sizeof(data)), 0);
    line_error = UARTC_LSR_FE;
    receive(1);
    zassert_equal(ht_companion_read(data, sizeof(data)), -EIO);
    receive(1);
    zassert_equal(ht_companion_read(data, sizeof(data)), 1);
    zassert_equal(ht_companion_read(data, sizeof(data)), 0);
}

ZTEST(c62_companion, test_transmit_deadline_validation_and_initialization) {
    uint8_t bytes[] = {0, 1, 255};
    initialized = false;
    zassert_equal(ht_companion_write(bytes, sizeof(bytes), 10), -EACCES);
    zassert_ok(ht_companion_restore_console());
    zassert_equal(ht_companion_write(bytes, sizeof(bytes), 10), 3);
    zassert_mem_equal(bytes, output, 3);
    line_error = UARTC_LSR_OE;
    zassert_equal(ht_companion_write(bytes, sizeof(bytes), 10), 3);
    uint8_t received;
    zassert_equal(ht_companion_read(&received, 1), -EIO, "TX poll must preserve RX errors");
    tx_ready = false;
    const auto start = k_uptime_get();
    zassert_equal(ht_companion_write(bytes, sizeof(bytes), 10), -ETIMEDOUT);
    zassert_true(k_uptime_get() - start >= 10);
    late_ready = true;
    const auto sent = output_size;
    zassert_equal(ht_companion_write(bytes, sizeof(bytes), 10), -ETIMEDOUT);
    zassert_equal(output_size, sent, "Delayed readiness must not permit a late byte");
    tx_ready = true;
    zassert_equal(ht_companion_write(nullptr, 1, 10), -EINVAL);
    zassert_equal(ht_companion_write(bytes, 257, 10), -EINVAL);
    zassert_equal(ht_companion_write(bytes, 1, 101), -EINVAL);
    zassert_ok(ht_companion_restore_console(), "Timeout releases ownership lock");
    ready = false;
    initialized = false;
    zassert_equal(ht_companion_restore_console(), -ENODEV);
}

ZTEST_SUITE(c62_companion, nullptr, nullptr, before, nullptr, nullptr);
