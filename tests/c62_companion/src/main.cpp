// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual C62 transport using bounded FIFO/register models.
#include "fakes/uart.h"
#include <errno.h>
#include <ht/companion.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static struct device fake_device;
static int uart2_pins_config, uart_companion_pins_config;
static bool ready = true, physical_ptt, ptt_on = true, rx_on, err_on, uart_pins;
static int pin_error, callback_error, restore_error;
static uart_irq_callback_user_data_t callback;
static unsigned configure_count;
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

extern "C" int c62_ptt_enable(bool enabled) {
    if (!enabled && physical_ptt) {
        return -EBUSY;
    }
    if (enabled && restore_error) {
        return restore_error;
    }
    ptt_on = enabled;
    return 0;
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
    restore_error = 0;
    zassert_ok(ht_companion_set_enabled(false));
    physical_ptt = false;
    pin_error = callback_error = 0;
    input_size = input_pos = output_size = status_reads = configure_count = 0;
    tx_ready = true;
    late_ready = false;
    line_error = 0;
}

static void receive(unsigned count = 32) {
    input_pos = 0;
    input_size = count;
    for (unsigned i = 0; i < count; ++i) {
        input[i] = i;
    }
    callback(&fake_device, nullptr);
}

ZTEST(c62_companion, test_ownership_rollback_and_audio_restore) {
    physical_ptt = true;
    zassert_equal(ht_companion_set_enabled(true), -EBUSY);
    physical_ptt = false;
    pin_error = -EIO;
    zassert_equal(ht_companion_set_enabled(true), -EIO);
    zassert_true(ptt_on);
    zassert_false(ht_companion_enabled());
    pin_error = 0;
    callback_error = -ENOTSUP;
    zassert_equal(ht_companion_set_enabled(true), -ENOTSUP);
    zassert_true(ptt_on);
    callback_error = 0;
    zassert_ok(ht_companion_set_enabled(true));
    zassert_false(ptt_on);
    zassert_true(rx_on);
    zassert_true(err_on);
    zassert_true(uart_pins);
    const auto configs = configure_count;
    zassert_ok(ht_companion_restore_console());
    zassert_equal(configure_count, configs, "Audio must not reset active transport");
    restore_error = -EIO;
    zassert_equal(ht_companion_set_enabled(false), -EIO);
    zassert_true(ht_companion_enabled());
    zassert_true(rx_on);
    restore_error = 0;
    zassert_ok(ht_companion_set_enabled(false));
    zassert_true(ptt_on);
    zassert_false(rx_on);
    zassert_false(err_on);
    zassert_ok(ht_companion_restore_console());
    zassert_equal(configure_count, configs + 1);
}

ZTEST(c62_companion, test_full_duplex_rx_overflow_and_line_error_recovery) {
    zassert_ok(ht_companion_set_enabled(true));
    tx_ready = false; // A vendor fifo_read would spin forever here.
    status_reads = 0;
    receive();
    zassert_equal(status_reads, 32, "ISR drain must be bounded by FIFO depth");
    uint8_t data[512];
    zassert_equal(ht_companion_read(data, sizeof(data)), 32);
    for (unsigned i = 0; i < 32; ++i) {
        zassert_equal(data[i], i);
    }
    for (unsigned i = 0; i < 17; ++i) {
        receive();
    }
    zassert_equal(ht_companion_read(data, sizeof(data)), -EOVERFLOW);
    zassert_equal(ht_companion_read(data, sizeof(data)), 0);
    line_error = UARTC_LSR_FE;
    receive(1);
    zassert_equal(ht_companion_read(data, sizeof(data)), -EIO);
    receive(1);
    zassert_equal(ht_companion_read(data, sizeof(data)), 1);
    zassert_ok(ht_companion_set_enabled(false));
    zassert_equal(ht_companion_read(data, sizeof(data)), -EACCES);
}

ZTEST(c62_companion, test_transmit_deadline_validation_and_mode_exclusion) {
    uint8_t bytes[] = {0, 1, 255};
    zassert_equal(ht_companion_write(bytes, sizeof(bytes), 10), -EACCES);
    zassert_ok(ht_companion_set_enabled(true));
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
    zassert_ok(ht_companion_set_enabled(false), "Timeout must release ownership lock");
    ready = false;
    zassert_equal(ht_companion_set_enabled(true), -ENODEV);
}

ZTEST_SUITE(c62_companion, nullptr, nullptr, before, nullptr, nullptr);
