/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_io.h"
#include <errno.h>
#include <ht/companion.h>
#include <uart.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

PINCTRL_DT_DEFINE(DT_NODELABEL(uart_companion));
static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart2));
static DW_UART_RegDef *const registers = (DW_UART_RegDef *)DT_REG_ADDR(DT_NODELABEL(uart2));
static K_MUTEX_DEFINE(owner_lock);
static struct k_spinlock rx_lock;
RING_BUF_DECLARE(rx, 512);
static atomic_t active;
static int rx_error;
static bool initialized;
static const struct gpio_dt_spec ptt = GPIO_DT_SPEC_GET(DT_NODELABEL(button_ptt), gpios);
/* rx_lock owns the detector as well as all reads of the clear-on-read LSR. */
static bool ptt_armed, break_byte;
static int64_t released_at = -1, break_at = -1;
#define PTT_HOLD_MS 300
#define PTT_RELEASE_MS 20

static void disarm_ptt(void) {
    ptt_armed = false;
    released_at = break_at = -1;
}

/* Caller holds rx_lock. Every LSR read acknowledges RX error flags, including
 * TX readiness polls, so retain those errors for the transport consumer.
 */
static uint32_t line_status(void) {
    const uint32_t status = registers->REG_LSR.all;
    if (status & (UARTC_LSR_OE | UARTC_LSR_PE | UARTC_LSR_FE | UARTC_LSR_BI)) {
        rx_error = -EIO;
        if ((status & (UARTC_LSR_OE | UARTC_LSR_PE)) ||
            ((status & UARTC_LSR_FE) && !(status & UARTC_LSR_BI))) {
            disarm_ptt();
        }
    }
    if (status & UARTC_LSR_BI) {
        break_byte = true;
        if (ptt_armed && !atomic_get(&active)) {
            break_at = k_uptime_get();
        }
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
        if (break_byte) {
            break_byte = false;
        } else {
            /* Even bytes sampled low by the controls thread cancel PTT. */
            disarm_ptt();
            if (ring_buf_put(&rx, &byte, 1) != 1) {
                rx_error = -EOVERFLOW;
            }
        }
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
    const int error = pinctrl_apply_state(
        PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(uart_companion)), PINCTRL_STATE_DEFAULT);
    return error ? error : uart_configure(uart, &config);
}

/* owner_lock held. Configure GPIO input/pull once, then leave A15 on UART RX.
 * DATAIN observes the pad without gpio_pin_configure stealing the UART mux.
 */
static int initialize(void) {
    if (initialized) {
        return 0;
    }
    if (!device_is_ready(uart) || !gpio_is_ready_dt(&ptt)) {
        return -ENODEV;
    }
    uart_irq_rx_disable(uart);
    uart_irq_err_disable(uart);
    int error = gpio_pin_configure_dt(&ptt, GPIO_INPUT);
    if (!error) {
        error = normal_uart();
    }
    if (!error) {
        error = uart_irq_callback_user_data_set(uart, receive_irq, NULL);
    }
    if (error) {
        return error;
    }
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    disarm_ptt();
    break_byte = false;
    initialized = true;
    k_spin_unlock(&rx_lock, key);
    uart_irq_rx_enable(uart);
    uart_irq_err_enable(uart);
    return 0;
}

int ht_companion_restore_console(void) {
    k_mutex_lock(&owner_lock, K_FOREVER);
    const int error = initialize();
    k_mutex_unlock(&owner_lock);
    return error;
}

int c62_uart_ptt_read(void) {
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    int result = 0;
    if (initialized) {
        const uint32_t status = line_status();
        const int down = gpio_pin_get_dt(&ptt);
        const int64_t now = k_uptime_get();
        if (down < 0 || atomic_get(&active) ||
            ((status & UARTC_LSR_RDR) && !break_byte)) {
            disarm_ptt();
            result = down < 0 ? down : 0;
        } else if (!down) {
            break_at = -1;
            if (released_at < 0) {
                released_at = now;
            }
            if (now - released_at >= PTT_RELEASE_MS) {
                ptt_armed = true;
            }
        } else {
            released_at = -1;
            result = ptt_armed && break_at >= 0 && now - break_at >= PTT_HOLD_MS;
        }
    }
    k_spin_unlock(&rx_lock, key);
    return result;
}

int ht_companion_set_enabled(bool enabled) {
    k_mutex_lock(&owner_lock, K_FOREVER);
    int error = initialize();
    if (!error) {
        k_spinlock_key_t key = k_spin_lock(&rx_lock);
        if (enabled != (atomic_get(&active) != 0)) {
            const int down = gpio_pin_get_dt(&ptt);
            /* A pending physical hold must not be converted into a session. */
            error = down < 0 ? down : enabled && break_at >= 0 && down ? -EBUSY : 0;
            if (!error) {
                atomic_set(&active, enabled);
                disarm_ptt();
                if (!enabled) {
                    ring_buf_reset(&rx);
                    rx_error = 0;
                }
            }
        }
        k_spin_unlock(&rx_lock, key);
    }
    k_mutex_unlock(&owner_lock);
    return error;
}

int ht_companion_read(uint8_t *data, size_t size) {
    if (!data || !size || size > 512) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    int result;
    if (!initialized) {
        result = -EAGAIN;
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
    if (!initialized) {
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
