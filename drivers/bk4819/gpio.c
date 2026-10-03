/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Adapted from reference BK4819.c's tested three-wire GPIO transaction.
 */
#include "bk4819.h"
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define RADIO DT_NODELABEL(bk4819)
static const struct gpio_dt_spec clock = GPIO_DT_SPEC_GET(RADIO, clock_gpios);
static const struct gpio_dt_spec data = GPIO_DT_SPEC_GET(RADIO, data_gpios);
static const struct gpio_dt_spec select = GPIO_DT_SPEC_GET(RADIO, select_gpios);
static bool ready;

int bk4819_bus_init(void) {
    ready = false;
    if (!gpio_is_ready_dt(&clock) || !gpio_is_ready_dt(&data) || !gpio_is_ready_dt(&select)) {
        return -ENODEV;
    }
    int error = gpio_pin_configure_dt(&select, GPIO_OUTPUT_INACTIVE);
    if (!error) {
        error = gpio_pin_configure_dt(&clock, GPIO_OUTPUT_LOW);
    }
    if (!error) {
        error = gpio_pin_configure_dt(&data, GPIO_OUTPUT_HIGH);
    }
    ready = !error;
    return error;
}

static int write_byte(uint8_t value) {
    int error = gpio_pin_set_dt(&clock, 0);
    if (!error) {
        error = gpio_pin_configure_dt(&data, GPIO_OUTPUT);
    }
    for (unsigned bit = 0; bit < 8 && !error; ++bit) {
        error = gpio_pin_set_dt(&data, (value & 0x80) != 0);
        if (!error) {
            error = gpio_pin_set_dt(&clock, 1);
        }
        k_busy_wait(1);
        if (!error) {
            error = gpio_pin_set_dt(&clock, 0);
        }
        k_busy_wait(1);
        value <<= 1;
    }
    return error;
}

int bk4819_write(uint8_t address, uint16_t value) {
    if (!ready) {
        return -ENODEV;
    }
    if (address > 0x7f) {
        return -EINVAL;
    }
    int error = gpio_pin_set_dt(&select, 1);
    k_busy_wait(1);
    if (!error) {
        error = write_byte(address);
    }
    if (!error) {
        error = write_byte(value >> 8);
    }
    if (!error) {
        error = write_byte(value & 0xff);
    }
    k_busy_wait(1);
    const int deselect = gpio_pin_set_dt(&select, 0);
    return error ? error : deselect;
}

int bk4819_read(uint8_t address, uint16_t *value) {
    if (!ready) {
        return -ENODEV;
    }
    if (address > 0x7f || !value) {
        return -EINVAL;
    }
    int error = gpio_pin_set_dt(&select, 1);
    k_busy_wait(1);
    if (!error) {
        error = write_byte(address | 0x80);
    }
    if (!error) {
        error = gpio_pin_configure_dt(&data, GPIO_INPUT);
    }
    if (!error) {
        error = gpio_pin_set_dt(&clock, 0);
    }
    uint16_t result = 0;
    for (unsigned bit = 0; bit < 16 && !error; ++bit) {
        result <<= 1;
        error = gpio_pin_set_dt(&clock, 0);
        k_busy_wait(1);
        if (!error) {
            error = gpio_pin_set_dt(&clock, 1);
        }
        k_busy_wait(1);
        if (!error) {
            const int level = gpio_pin_get_dt(&data);
            if (level < 0) {
                error = level;
            } else {
                result |= level != 0;
            }
        }
    }
    k_busy_wait(1);
    const int deselect = gpio_pin_set_dt(&select, 0);
    if (!error) {
        error = deselect;
    }
    if (!error) {
        *value = result;
    }
    return error;
}
