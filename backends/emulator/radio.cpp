// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/emulator.hpp>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace ht {
static constexpr RadioCapabilities capabilities{
    {{136000000, 174000000}, {400000000, 480000000}}, 5000, true, true, false, true, true, true};
static K_MUTEX_DEFINE(peripheral_mutex);
static BackendStatus observation;
static bool transmitting;
static bool monitor;
static RadioConfig configuration;
static uint32_t tuned_frequency;
static uint16_t registers[128];
static int next_error;
static atomic_t reset_requested;

const RadioCapabilities &backend_capabilities() {
    return capabilities;
}

static int operation() {
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    const int error = next_error;
    next_error = 0;
    k_mutex_unlock(&peripheral_mutex);
    return error;
}

int backend_init() {
    const int error = operation();
    if (error) {
        return error;
    }
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    observation = {};
    memset(registers, 0, sizeof(registers));
    transmitting = false;
    monitor = false;
    tuned_frequency = 0;
    k_mutex_unlock(&peripheral_mutex);
    return 0;
}

int backend_configure(const RadioConfig &config, const FmRxControls &controls) {
    if (!valid_fm_rx_controls(controls)) {
        return -EINVAL;
    }
    const int validation = validate_config(config);
    if (validation) {
        return validation;
    }
    const int error = operation();
    if (error) {
        return error;
    }
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    configuration = config;
    k_mutex_unlock(&peripheral_mutex);
    return 0;
}

int backend_receive() {
    const int error = operation();
    if (error) {
        return error;
    }
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    tuned_frequency = configuration.rx_frequency_hz;
    k_mutex_unlock(&peripheral_mutex);
    return 0;
}

int backend_transmit(int64_t &started_ms) {
    const int error = operation();
    if (error) {
        return error;
    }
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    if (configuration.tx_inhibit) {
        k_mutex_unlock(&peripheral_mutex);
        return -EPERM;
    }
    tuned_frequency = configuration.tx_frequency_hz;
    transmitting = true;
    started_ms = k_uptime_get();
    k_mutex_unlock(&peripheral_mutex);
    return 0;
}

int backend_monitor(bool enabled) {
    const int error = operation();
    if (error) {
        return error;
    }
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    const int result = transmitting ? -EBUSY : configuration.mode != Mode::Fm ? -ENOTSUP : 0;
    if (!result) {
        monitor = enabled;
    }
    k_mutex_unlock(&peripheral_mutex);
    return result;
}

int backend_finish_transmit(int64_t) {
    return 0; // Emulator models control state, without live audio.
}

void backend_stop() {
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    transmitting = false;
    monitor = false;
    observation.rx_active = false;
    k_mutex_unlock(&peripheral_mutex);
}

BackendStatus backend_status() {
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    BackendStatus result = observation;
    if (monitor && !result.error) {
        result.rx_active = true;
    }
    k_mutex_unlock(&peripheral_mutex);
    return result;
}

int backend_read_register(uint8_t address, uint16_t &value) {
    if (address >= 128) {
        return -EINVAL;
    }
    const int error = operation();
    if (error) {
        return error;
    }
    value = registers[address];
    return 0;
}

int backend_write_register(uint8_t address, uint16_t value) {
    if (address >= 128) {
        return -EINVAL;
    }
    const int error = operation();
    if (error) {
        return error;
    }
    registers[address] = value;
    return 0;
}

void emulator_inject(const BackendStatus &status) {
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    observation = status;
    k_mutex_unlock(&peripheral_mutex);
}

void emulator_fail_next(int error) {
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    next_error = error < 0 ? error : -EIO;
    k_mutex_unlock(&peripheral_mutex);
}

bool emulator_transmitting() {
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    const bool result = transmitting;
    k_mutex_unlock(&peripheral_mutex);
    return result;
}

uint32_t emulator_tuned_frequency() {
    k_mutex_lock(&peripheral_mutex, K_FOREVER);
    const uint32_t result = tuned_frequency;
    k_mutex_unlock(&peripheral_mutex);
    return result;
}

void emulator_request_reset() {
    atomic_set(&reset_requested, 1);
}

bool emulator_take_reset_request() {
    return atomic_cas(&reset_requested, 1, 0);
}
} // namespace ht
