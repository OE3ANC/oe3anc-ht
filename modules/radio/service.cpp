// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion_contract.hpp>
#include <ht/radio.hpp>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace ht {
static RadioController controller;
static K_MUTEX_DEFINE(state_mutex);
K_MSGQ_DEFINE(commands, sizeof(RadioCommand), 8, 4);
static atomic_t startup_chime_requested;

void radio_startup_chime(bool enabled) {
    atomic_set(&startup_chime_requested, enabled);
}
static atomic_t ptt_pressed;
static atomic_t ptt_released;
static atomic_t ptt_activity;
static atomic_t ptt_press_sequence;
static atomic_t monitor_pressed;
static atomic_t monitor_released;
static atomic_t monitor_press_sequence;
static atomic_t peripheral_fault;
static atomic_t controller_fault;
static atomic_t power_active = 1;
static atomic_t power_off;

// Lock order: state_mutex -> remote_lock. Independent producers take only
// remote_lock; never call a mutex-taking radio API while holding it.
static k_spinlock remote_lock;
static uint64_t remote_session;
static uint32_t remote_token;
static bool remote_held, remote_released;
static bool using_remote; // Owned by state_mutex; distinguishes independent release paths.
static int64_t remote_deadline;

static void remote_cancel(int64_t now) {
    remote_held = false;
    remote_released = true;
    remote_deadline = now;
}

static void remote_expire(int64_t now) {
    if (remote_held && (now >= remote_deadline || !radio_power_requested() ||
                        radio_latched_fault())) {
        remote_cancel(now);
    }
}

void radio_remote_session(uint64_t session) {
    const auto key = k_spin_lock(&remote_lock);
    if (session != remote_session || !session) {
        remote_cancel(k_uptime_get());
        remote_session = session;
        remote_token = 0;
    }
    k_spin_unlock(&remote_lock, key);
}

int radio_remote_ptt_press(uint64_t session, uint32_t token) {
    if (!session || !token) {
        return -EINVAL;
    }
    // Acceptance validates the current controller state without blocking its
    // preparation/drain. The owner validates again before touching RF.
    if (k_mutex_lock(&state_mutex, K_NO_WAIT)) {
        return -EBUSY;
    }
    const auto &state = controller.state();
    int error = !radio_power_requested() || !state.power_active                    ? -EHOSTDOWN
                : state.fault || radio_latched_fault()                            ? -EIO
                : atomic_get(&ptt_pressed) || state.phase != RadioPhase::Receiving ? -EBUSY
                : state.config.tx_inhibit                                         ? -EPERM
                : state.config.mode == Mode::M17 && !valid_callsign(state.config.callsign)
                    ? -EINVAL
                    : 0;
    const auto key = k_spin_lock(&remote_lock);
    const auto now = k_uptime_get();
    remote_expire(now);
    if (!error && session != remote_session) {
        error = -ESTALE;
    }
    if (!error && remote_held) {
        error = -EBUSY;
    }
    if (!error) {
        remote_token = token;
        remote_held = true;
        remote_deadline = now + companion::PTT_LEASE_MS;
        atomic_inc(&ptt_press_sequence);
        atomic_set(&ptt_activity, 1);
    }
    k_spin_unlock(&remote_lock, key);
    k_mutex_unlock(&state_mutex);
    return error;
}

int radio_remote_ptt_keep(uint64_t session, uint32_t token) {
    const auto key = k_spin_lock(&remote_lock);
    const auto now = k_uptime_get();
    remote_expire(now);
    const bool valid =
        session && session == remote_session && token && token == remote_token && remote_held;
    if (valid) {
        remote_deadline = now + companion::PTT_LEASE_MS;
    }
    k_spin_unlock(&remote_lock, key);
    return valid ? 0 : -ESTALE;
}

int radio_remote_ptt_release(uint64_t session) {
    const auto key = k_spin_lock(&remote_lock);
    const bool valid = session && session == remote_session;
    if (valid) {
        // Preserve the last lease only for a bounded normal M17 end marker.
        remote_held = false;
        remote_released = true;
    }
    k_spin_unlock(&remote_lock, key);
    return valid ? 0 : -ESTALE;
}

bool radio_remote_ptt_requested() {
    const auto key = k_spin_lock(&remote_lock);
    remote_expire(k_uptime_get());
    const bool held = remote_held;
    k_spin_unlock(&remote_lock, key);
    return held;
}

int64_t radio_remote_ptt_deadline() {
    const auto key = k_spin_lock(&remote_lock);
    remote_expire(k_uptime_get());
    const auto deadline = remote_deadline;
    k_spin_unlock(&remote_lock, key);
    return deadline;
}

int radio_start(const RadioConfig &config, const Selection &selection) {
    k_mutex_lock(&state_mutex, K_FOREVER);
    k_msgq_purge(&commands);
    radio_remote_session(0);
    using_remote = false;
    atomic_clear(&ptt_pressed);
    atomic_clear(&ptt_released);
    atomic_clear(&ptt_activity);
    atomic_clear(&monitor_pressed);
    atomic_clear(&monitor_released);
    atomic_clear(&startup_chime_requested);
    atomic_clear(&peripheral_fault);
    atomic_clear(&power_off);
    const int result = controller.start(config, radio_power_requested(), selection);
    atomic_set(&controller_fault, controller.state().fault);
    k_mutex_unlock(&state_mutex);
    return result;
}

void radio_service() {
    k_mutex_lock(&state_mutex, K_FOREVER);
    const RadioState previous = controller.state();
    const bool stopped = atomic_cas(&power_off, 1, 0);
    if (stopped) {
        controller.set_power(false);
        k_msgq_purge(&commands);
    }
    controller.report_fault(atomic_get(&peripheral_fault));
    controller.set_power(radio_power_requested());
    const bool resumed = controller.state().power_active && (stopped || !previous.power_active);
    const bool interrupted = atomic_cas(&ptt_activity, 1, 0);
    controller.startup_chime(atomic_get(&startup_chime_requested) && !radio_ptt_requested());
    controller.poll();
    // Physical PTT takes priority. Cancel remote intent so its expiry/release
    // cannot stop a physical hold, and it cannot resume after physical release.
    const bool local_released = atomic_cas(&ptt_released, 1, 0);
    const bool local_held = atomic_get(&ptt_pressed) != 0;
    const auto key = k_spin_lock(&remote_lock);
    remote_expire(k_uptime_get());
    if (local_held || local_released) {
        remote_cancel(k_uptime_get());
    }
    const bool released = remote_released;
    remote_released = false;
    const bool held = remote_held;
    k_spin_unlock(&remote_lock, key);
    const bool next_remote = !local_held && held;
    // Preserve release edges through queue saturation and rapid release/repress.
    // Power resume still requires a released sample before another transmission.
    if (!resumed && (using_remote != next_remote ||
                     (using_remote ? released : local_released))) {
        if (using_remote) {
            controller.set_remote_ptt(false);
        } else {
            controller.set_ptt(false);
        }
    }
    using_remote = next_remote;
    if (using_remote) {
        controller.set_remote_ptt(held);
    } else {
        controller.set_ptt(local_held);
    }
    if (atomic_cas(&monitor_released, 1, 0) && !resumed) {
        controller.set_monitor(false);
    }
    const bool monitor_held = atomic_get(&monitor_pressed) != 0;
    if (interrupted) {
        controller.interrupt_monitor(monitor_held);
    } else {
        controller.set_monitor(monitor_held);
    }
    RadioCommand command;
    if (k_msgq_get(&commands, &command, K_NO_WAIT) == 0) {
        controller.execute(command);
    }
    atomic_set(&controller_fault, controller.state().fault);
    k_mutex_unlock(&state_mutex);
}

int radio_submit(const RadioCommand &command) {
    if (!radio_power_requested()) {
        return -EHOSTDOWN;
    }
    return k_msgq_put(&commands, &command, K_NO_WAIT);
}

void radio_ptt(bool pressed) {
    atomic_set(&ptt_pressed, pressed);
    if (pressed) {
        atomic_inc(&ptt_press_sequence);
        atomic_set(&ptt_activity, 1);
    }
    if (!pressed) {
        atomic_set(&ptt_released, 1);
    }
}

uint32_t radio_ptt_press_sequence() {
    return static_cast<uint32_t>(atomic_get(&ptt_press_sequence));
}

void radio_power(bool active) {
    atomic_set(&power_active, active);
    if (!active) {
        atomic_set(&power_off, 1);
        const auto key = k_spin_lock(&remote_lock);
        remote_cancel(k_uptime_get());
        k_spin_unlock(&remote_lock, key);
    }
}

bool radio_power_on_intent() {
    return atomic_get(&power_active) != 0;
}

bool radio_power_requested() {
    return atomic_get(&power_active) != 0 && atomic_get(&power_off) == 0;
}

void radio_monitor(bool pressed) {
    atomic_set(&monitor_pressed, pressed);
    if (pressed) {
        atomic_inc(&monitor_press_sequence);
    }
    if (!pressed) {
        atomic_set(&monitor_released, 1);
    }
}

uint32_t radio_monitor_press_sequence() {
    return atomic_get(&monitor_press_sequence);
}

bool radio_monitor_requested() {
    return atomic_get(&monitor_pressed) != 0;
}

void radio_report_fault(int error) {
    if (error) {
        atomic_cas(&peripheral_fault, 0, error < 0 ? error : -EIO);
        radio_ptt(false);
        const auto key = k_spin_lock(&remote_lock);
        remote_cancel(k_uptime_get());
        k_spin_unlock(&remote_lock, key);
    }
}

bool radio_ptt_requested() {
    return (atomic_get(&ptt_pressed) != 0 || radio_remote_ptt_requested()) &&
           atomic_get(&peripheral_fault) == 0 && radio_power_requested();
}

int radio_latched_fault() {
    const int error = atomic_get(&controller_fault);
    return error ? error : atomic_get(&peripheral_fault);
}

int radio_pending_fault() {
    return atomic_get(&peripheral_fault);
}

bool radio_idle_lock(bool inactive_only) {
    if (k_mutex_lock(&state_mutex, K_NO_WAIT)) {
        return false;
    }
    const auto phase = controller.state().phase;
    if ((inactive_only && (phase != RadioPhase::Inactive || radio_power_on_intent())) ||
        atomic_get(&peripheral_fault) || controller.state().fault || radio_ptt_requested() ||
        (phase != RadioPhase::Receiving && phase != RadioPhase::Diagnostics &&
         phase != RadioPhase::Inactive)) {
        k_mutex_unlock(&state_mutex);
        return false;
    }
    return true;
}

void radio_idle_unlock() {
    k_mutex_unlock(&state_mutex);
}

RadioState radio_snapshot() {
    k_mutex_lock(&state_mutex, K_FOREVER);
    const RadioState result = controller.state();
    k_mutex_unlock(&state_mutex);
    return result;
}

} // namespace ht

extern "C" void ht_radio_report_fault(int error) {
    ht::radio_report_fault(error);
}
