// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion.h>
#include <ht/companion_contract.hpp>
#include <ht/ui.hpp>
#include <zephyr/kernel.h>

namespace ht {
K_MSGQ_DEFINE(front_inputs, sizeof(UiInput), 16, 4);
static struct k_spinlock input_lock;

static bool companion_enabled() {
#ifdef CONFIG_HT_COMPANION
    return ht_companion_enabled();
#else
    return false;
#endif
}

static uint64_t remote_session;
static uint32_t generation = 1, held;
static int64_t deadline;

// Caller holds input_lock. Lock ordering is input_lock -> gesture_lock; the
// gesture owner never acquires input_lock. Neither lock encloses model/RF work.
static void clear_remote(int64_t now) {
    held = 0;
    deadline = 0;
    ui_keypad_remote_star(false, now, 0);
    if (generation != UINT32_MAX) {
        ++generation;
    }
}

static void expire(int64_t now) {
    if (deadline && (now >= deadline || !companion_enabled())) {
        clear_remote(now);
    }
}

int ui_queue_input(const UiInput &input) {
    if (input.key > UiKey::Digit || input.remote_generation) {
        return -EINVAL;
    }
    const auto key = k_spin_lock(&input_lock);
    const int error = k_msgq_put(&front_inputs, &input, K_NO_WAIT) ? -EBUSY : 0;
    k_spin_unlock(&input_lock, key);
    return error;
}

bool ui_take_input(UiInput &input) {
    const auto key = k_spin_lock(&input_lock);
    expire(k_uptime_get());
    bool found = false;
    while (k_msgq_get(&front_inputs, &input, K_NO_WAIT) == 0) {
        if (!input.remote_generation || (remote_session && deadline && generation != UINT32_MAX &&
                                         input.remote_generation == generation)) {
            found = true;
            break;
        }
    }
    k_spin_unlock(&input_lock, key);
    return found;
}

void ui_remote_session(uint64_t session) {
    const auto key = k_spin_lock(&input_lock);
    if (session != remote_session) {
        clear_remote(k_uptime_get());
        remote_session = session;
    }
    k_spin_unlock(&input_lock, key);
}

void ui_remote_clear() {
    const auto key = k_spin_lock(&input_lock);
    clear_remote(k_uptime_get());
    k_spin_unlock(&input_lock, key);
}

int ui_remote_key(UiInput input, unsigned wire_key) {
    if (wire_key > companion::UI_KEY_DIGIT_9 || input.key > UiKey::Digit) {
        return -EINVAL;
    }
    const auto key = k_spin_lock(&input_lock);
    const int64_t now = k_uptime_get();
    expire(now);
    int error = 0;
    const uint32_t bit = uint32_t(1) << wire_key;
    if (!remote_session || !companion_enabled() || generation == UINT32_MAX) {
        error = -ESTALE;
    } else if (!input.pressed) {
        held &= ~bit;
        if (input.key == UiKey::Star) {
            ui_keypad_remote_star(false, now, 0);
        }
    } else if (held & bit) {
        error = -EINVAL;
    } else {
        input.timestamp_ms = now;
        input.remote_generation = generation;
        if (k_msgq_put(&front_inputs, &input, K_NO_WAIT)) {
            error = -EBUSY;
        } else {
            held |= bit;
            deadline = now + companion::UI_KEYS_LEASE_MS;
            if (held & (uint32_t(1) << companion::UI_KEY_STAR)) {
                ui_keypad_remote_star(true, now, deadline);
            }
            if (held & ~(uint32_t(1) << companion::UI_KEY_STAR)) {
                ui_keypad_cancel_gesture();
            }
        }
    }
    k_spin_unlock(&input_lock, key);
    return error;
}

int ui_remote_keep(uint32_t mask) {
    const auto key = k_spin_lock(&input_lock);
    const int64_t now = k_uptime_get();
    expire(now);
    int error = 0;
    if (!remote_session || !deadline || !held || generation == UINT32_MAX) {
        error = -ESTALE;
    } else if (mask != held) {
        error = -EINVAL;
    } else {
        deadline = now + companion::UI_KEYS_LEASE_MS;
        if (held & (uint32_t(1) << companion::UI_KEY_STAR)) {
            ui_keypad_remote_star(true, now, deadline);
            if (held & ~(uint32_t(1) << companion::UI_KEY_STAR)) {
                ui_keypad_cancel_gesture();
            }
        }
    }
    k_spin_unlock(&input_lock, key);
    return error;
}
} // namespace ht
