/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_io.h"
#include <IOMuxManager.h>
#include <errno.h>
#include <ht/companion.h>
#include <uart.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

PINCTRL_DT_DEFINE(DT_NODELABEL(uart_companion));
static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart2));
static DW_UART_RegDef *const registers = (DW_UART_RegDef *)DT_REG_ADDR(DT_NODELABEL(uart2));
static K_MUTEX_DEFINE(owner_lock);
static struct k_spinlock rx_lock;
// A maximum escaped KISS response occupies 58 ms at 115200 baud.
// Retain simultaneous host traffic across that bounded serial write.
RING_BUF_DECLARE(rx, 2048);
static int rx_error;
static bool initialized;
static const struct gpio_dt_spec ptt = GPIO_DT_SPEC_GET(DT_NODELABEL(button_ptt), gpios);
/* rx_lock owns the detector as well as all reads of the clear-on-read LSR. */
static bool ptt_armed, invalid_byte;
static int64_t released_at = -1, low_at = -1, received_at = -1;
#define PTT_HOLD_MS 300
#define PTT_RELEASE_MS 20
#define UART_QUIET_MS 20

static void disarm_ptt(void) {
    ptt_armed = false;
    released_at = low_at = -1;
}

/* Caller holds rx_lock. Every LSR read acknowledges RX error flags, including
 * TX readiness polls, so retain those errors for the transport consumer.
 */
static uint32_t line_status(void) {
    const uint32_t status = registers->REG_LSR.all;
    if (status & (UARTC_LSR_OE | UARTC_LSR_PE | UARTC_LSR_FE | UARTC_LSR_BI)) {
        rx_error = -EIO;
    }
    if (status & (UARTC_LSR_PE | UARTC_LSR_FE | UARTC_LSR_BI)) {
        invalid_byte = true;
    }
    return status;
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
        if (invalid_byte) {
            invalid_byte = false;
        } else {
            /* Even bytes sampled low by the controls thread cancel PTT. */
            low_at = -1;
            received_at = k_uptime_get();
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

/* owner_lock held. Configure GPIO input/pull once, then select UART RX. */
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
    invalid_byte = false;
    received_at = -1;
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

/* rx_lock held. GPIO DATAIN cannot be used as a pad monitor with UART selected.
 * Select GPIO only for the sample, then restore the UART function even on error.
 * The HAL changes only FSEL, preserving input direction, pull and TX pin setup.
 */
static int sample_ptt(void) {
    uint32_t function;
    if (IOMuxManager_GetConfig(CSK_IOMUX_PAD_A, ptt.pin, &function)) {
        return -EIO;
    }
    const int error =
        IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, ptt.pin, CSK_IOMUX_FUNC_DEFAULT);
    int down = -EIO;
    if (!error) {
        k_busy_wait(1); // Allow the GPIO input synchronizer to observe the selected pad.
        down = gpio_pin_get_dt(&ptt);
    }
    if (IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, ptt.pin, function)) {
        return -EIO;
    }
    return down;
}

int c62_uart_ptt_read(void) {
    k_spinlock_key_t key = k_spin_lock(&rx_lock);
    int result = 0;
    if (initialized) {
        const uint32_t status = line_status();
        const int64_t now = k_uptime_get();
        if (((status & UARTC_LSR_RDR) && !invalid_byte) ||
            (received_at >= 0 && now - received_at < UART_QUIET_MS)) {
            // Do not disturb an active serial burst with GPIO mux probes.
            low_at = -1;
            k_spin_unlock(&rx_lock, key);
            return 0;
        }
        const int down = sample_ptt();
        if (down < 0) {
            disarm_ptt();
            result = down;
        } else if (!down) {
            low_at = -1;
            if (released_at < 0) {
                released_at = now;
            }
            if (now - released_at >= PTT_RELEASE_MS) {
                ptt_armed = true;
            }
        } else {
            released_at = -1;
            if (low_at < 0) {
                low_at = now;
            }
            result = ptt_armed && low_at >= 0 && now - low_at >= PTT_HOLD_MS;
        }
    }
    k_spin_unlock(&rx_lock, key);
    return result;
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
