// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/battery_backend.hpp>
#include <ht/emulator.hpp>
#include <errno.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

namespace ht {
static const BatteryCapabilities capabilities{true, true, true};
static K_MUTEX_DEFINE(input_lock);
static BatteryReading input{7200, false, true};
static int input_error;

const BatteryCapabilities &battery_backend_capabilities() {
    return capabilities;
}

static bool initialized;

// Decimal digits only; no whitespace, signs, overflow or partial parsing.
static bool option(const char *name, unsigned limit, unsigned &value) {
    const char *text = getenv(name);
    if (!text) {
        return true;
    }
    if (!*text) {
        return false;
    }
    unsigned result = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || result > limit / 10) {
            return false;
        }
        result = result * 10 + unsigned(*text - '0');
        if (result > limit) {
            return false;
        }
    }
    value = result;
    return true;
}

int battery_backend_init() {
    k_mutex_lock(&input_lock, K_FOREVER);
    if (!initialized) {
        unsigned mv = input.millivolts, charger = input.charger_input;
        unsigned on = input.switch_on, error = 0;
        const bool valid = option("HT_BATTERY_MV", 20000, mv) &&
                           option("HT_CHARGER_INPUT", 1, charger) &&
                           option("HT_POWER_SWITCH", 1, on) && option("HT_BATTERY_ERROR", 1, error);
        if (valid) {
            input = {mv, charger != 0, on != 0};
        }
        input_error = valid ? error ? -EIO : input_error : -EINVAL;
        initialized = true;
    }
    k_mutex_unlock(&input_lock);
    return 0;
}

int battery_backend_read(BatteryReading &reading) {
    k_mutex_lock(&input_lock, K_FOREVER);
    const int error = input_error;
    if (!error) {
        reading = input;
    }
    k_mutex_unlock(&input_lock);
    return error;
}

BatteryReading emulator_battery_input(int &error) {
    k_mutex_lock(&input_lock, K_FOREVER);
    const auto reading = input;
    error = input_error;
    k_mutex_unlock(&input_lock);
    return reading;
}

void emulator_inject_battery(const BatteryReading &reading, int error) {
    k_mutex_lock(&input_lock, K_FOREVER);
    initialized = true;
    input = reading;
    input_error = error;
    k_mutex_unlock(&input_lock);
}
} // namespace ht
