/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_io.h"
#include <errno.h>
#include <ht/companion.h>
#include <uart.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

PINCTRL_DT_DEFINE(DT_NODELABEL(uart_companion));
PINCTRL_DT_DEFINE(DT_NODELABEL(uart2));
static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart2));
static DW_UART_RegDef *const registers = (DW_UART_RegDef *)DT_REG_ADDR(DT_NODELABEL(uart2));
static K_MUTEX_DEFINE(owner_lock);
static struct k_spinlock rx_lock;
RING_BUF_DECLARE(rx, 512);
static atomic_t active;
static int rx_error;

/* Caller holds rx_lock. Every LSR read acknowledges RX error flags, including
 * TX readiness polls, so retain those errors for the transport consumer.
 */
static uint32_t line_status(void) {
    const uint32_t status = registers->REG_LSR.all;
    if (status & (UARTC_LSR_OE | UARTC_LSR_PE | UARTC_LSR_FE | UARTC_LSR_BI)) {
        rx_error = -EIO;
    }
    return status;
}

bool ht_companion_enabled(void) {
    return atomic_get(&active) != 0;
}

/* Vendor fifo_read waits for TX empty, so use bounded register reads here.
 * The radio can receive serial bytes while TX is busy, without spinning in ISR.
 */
static void receive_irq(const struct device *dev, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    for (unsigned i = 0; i < CSK_UART2_FIFO; ++i) {
        const uint32_t status = line_status();
        if (!(status & UARTC_LSR_RDR)) {
            break;
        }
        const uint8_t byte = registers->REG_RBR.all;
        if (atomic_get(&active) && ring_buf_put(&rx, &byte, 1) != 1) {
            rx_error = -EOVERFLOW;
        }
    }
    k_spin_unlock(&rx_lock, key);
}

static void discard_rx(void) {
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    ring_buf_reset(&rx);
    rx_error = 0;
    /* RX remains connected until exit completes; clear at most one FIFO. */
    for (unsigned i = 0; i < CSK_UART2_FIFO; ++i) {
        if (!(registers->REG_LSR.all & UARTC_LSR_RDR)) {
            break;
        }
        (void)registers->REG_RBR.all;
    }
    k_spin_unlock(&rx_lock, key);
}

static int normal_uart(void) {
    const struct uart_config config = {
        .baudrate = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    const int error =
        pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(uart2)), PINCTRL_STATE_DEFAULT);
    return error ? error : uart_configure(uart, &config);
}

int ht_companion_restore_console(void) {
    if (!device_is_ready(uart)) {
        return -ENODEV;
    }
    k_mutex_lock(&owner_lock, K_FOREVER);
    const int error = ht_companion_enabled() ? 0 : normal_uart();
    k_mutex_unlock(&owner_lock);
    return error;
}

int ht_companion_set_enabled(bool enabled) {
    if (!device_is_ready(uart)) {
        return -ENODEV;
    }
    k_mutex_lock(&owner_lock, K_FOREVER);
    int error = 0;
    if (enabled == ht_companion_enabled()) {
        goto done;
    }
    uart_irq_rx_disable(uart);
    uart_irq_err_disable(uart);
    discard_rx();
    if (enabled) {
        error = c62_ptt_enable(false);
        if (error) {
            goto done;
        }
        error = normal_uart();
        if (!error) {
            error = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(uart_companion)),
                                        PINCTRL_STATE_DEFAULT);
        }
        if (!error) {
            error = uart_irq_callback_user_data_set(uart, receive_irq, NULL);
        }
        if (error) {
            const int restore_error = c62_ptt_enable(true);
            if (restore_error) {
                error = restore_error;
            }
            goto done;
        }
        atomic_set(&active, 1);
        uart_irq_rx_enable(uart);
        uart_irq_err_enable(uart);
    } else {
        /* Keep PTT suppressed until GPIO ownership and rearming are installed. */
        error = c62_ptt_enable(true);
        if (!error) {
            atomic_clear(&active);
        } else {
            uart_irq_rx_enable(uart);
            uart_irq_err_enable(uart);
        }
    }
done:
    k_mutex_unlock(&owner_lock);
    return error;
}

int ht_companion_read(uint8_t *data, size_t size) {
    if (!data || !size || size > 512) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    int result;
    if (!ht_companion_enabled()) {
        result = -EACCES;
    } else if (rx_error) {
        result = rx_error;
        ring_buf_reset(&rx);
        rx_error = 0;
    } else {
        result = ring_buf_get(&rx, data, size);
    }
    k_spin_unlock(&rx_lock, key);
    return result;
}

int ht_companion_write(const uint8_t *data, size_t size, uint32_t timeout_ms) {
    if (!data || !size || size > 256 || !timeout_ms || timeout_ms > 100) {
        return -EINVAL;
    }
    if (k_mutex_lock(&owner_lock, K_NO_WAIT)) {
        return -EBUSY;
    }
    int result = -EACCES;
    const int64_t deadline = k_uptime_get() + timeout_ms;
    if (!ht_companion_enabled()) {
        goto done;
    }
    for (size_t i = 0; i <= size; ++i) {
        const uint32_t ready = i == size ? UARTC_LSR_TEMT : UARTC_LSR_THRE;
        while (true) {
            if (k_uptime_get() >= deadline) {
                result = -ETIMEDOUT;
                goto done;
            }
            k_spinlock_key_t key = k_spin_lock(&rx_lock);
            const uint32_t status = line_status();
            k_spin_unlock(&rx_lock, key);
            if (status & ready) {
                break;
            }
            k_sleep(K_USEC(50));
        }
        if (k_uptime_get() >= deadline) {
            result = -ETIMEDOUT;
            goto done;
        }
        if (i < size) {
            registers->REG_THR.all = data[i];
        }
    }
    result = size;
done:
    k_mutex_unlock(&owner_lock);
    return result;
}
