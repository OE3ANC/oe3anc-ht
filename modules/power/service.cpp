// SPDX-License-Identifier: GPL-3.0-or-later
#include "switch.hpp"
#include <ht/power.hpp>
#include <ht/power_backend.hpp>
#include <zephyr/sys/atomic.h>

namespace ht {
static PowerSwitchFilter filter;
static atomic_t confirmed_on;
static int prepare_error;
static int output_error;
static bool applied_active;

int power_prepare() {
    radio_power(false);
    filter = {};
    atomic_clear(&confirmed_on);
    applied_active = false;
    output_error = 0;
    prepare_error = power_backend_prepare();
    return prepare_error;
}

void power_sample(const BatterySnapshot &snapshot, int64_t now_ms) {
    const bool active = filter.update(snapshot, now_ms) && !prepare_error;
    if (active != (atomic_get(&confirmed_on) != 0)) {
        atomic_set(&confirmed_on, active);
        radio_power(active);
    }
}

bool power_switch_on() {
    return atomic_get(&confirmed_on) != 0;
}

int power_apply_outputs(const RadioState &state) {
    if (prepare_error) {
        return prepare_error;
    }
    if (state.power_active != applied_active) {
        applied_active = state.power_active;
        const int error = power_backend_outputs(applied_active);
        if (error && !output_error) {
            output_error = error;
        }
    }
    return output_error;
}
} // namespace ht
