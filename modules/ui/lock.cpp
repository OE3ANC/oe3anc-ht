// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <zephyr/kernel.h>

namespace ht {
static struct k_spinlock gesture_lock;
static KeypadGesture gesture;
static bool physical_star, remote_star;
static int64_t remote_deadline;

// Caller holds gesture_lock. Expire the remote level here too: even a stalled
// serial worker cannot cause the UI owner to commit a stale virtual hold.
static void star_level(int64_t now) {
    if (remote_star && now >= remote_deadline) {
        remote_star = false;
    }
    const bool pressed = physical_star || remote_star;
    if (pressed && !gesture.pressed) {
        ++gesture.press_sequence;
        gesture.pressed_at = now;
    }
    gesture.pressed = pressed;
    if (physical_star && remote_star) {
        gesture.cancelled_press = gesture.press_sequence;
    }
}

void ui_keypad_star(bool pressed, int64_t now_ms, bool cancelled) {
    const auto key = k_spin_lock(&gesture_lock);
    star_level(now_ms);
    physical_star = pressed;
    star_level(now_ms);
    if (cancelled) {
        gesture.cancelled_press = gesture.press_sequence;
    }
    k_spin_unlock(&gesture_lock, key);
}

void ui_keypad_remote_star(bool pressed, int64_t now_ms, int64_t deadline_ms) {
    const auto key = k_spin_lock(&gesture_lock);
    star_level(now_ms);
    remote_star = pressed;
    remote_deadline = deadline_ms;
    star_level(now_ms);
    k_spin_unlock(&gesture_lock, key);
}

void ui_keypad_cancel_gesture() {
    const auto key = k_spin_lock(&gesture_lock);
    gesture.cancelled_press = gesture.press_sequence;
    k_spin_unlock(&gesture_lock, key);
}

KeypadGesture ui_keypad_gesture() {
    const auto key = k_spin_lock(&gesture_lock);
    star_level(k_uptime_get());
    const auto copy = gesture;
    k_spin_unlock(&gesture_lock, key);
    return copy;
}

static bool consume_gesture(uint32_t sequence) {
    const auto key = k_spin_lock(&gesture_lock);
    star_level(k_uptime_get());
    // Commit against the live producer level. A release/cancellation after the
    // owner's earlier snapshot must not turn a short press into a hold.
    const bool held = gesture.pressed && gesture.press_sequence == sequence &&
                      gesture.cancelled_press != sequence;
    if (held) {
        gesture.cancelled_press = sequence;
    }
    k_spin_unlock(&gesture_lock, key);
    return held;
}

void UiModel::cancel_lock() {
    const auto copy = ui_keypad_gesture();
    lock_press_sequence_ = copy.press_sequence;
    lock_ptt_sequence_ = radio_ptt_press_sequence();
    lock_since_ = -1;
}

void UiModel::advance_lock(int64_t now) {
    const auto copy = ui_keypad_gesture();
    if (screen_ != UiScreen::Home || !state_.power_active || state_.fault ||
        state_.phase != RadioPhase::Receiving || radio_latched_fault() || radio_ptt_requested() ||
        copy.cancelled_press == copy.press_sequence ||
        lock_ptt_sequence_ != radio_ptt_press_sequence() || now < copy.pressed_at) {
        cancel_lock();
        return;
    }
    if (copy.press_sequence != lock_press_sequence_) {
        lock_press_sequence_ = copy.press_sequence;
        lock_since_ = copy.pressed ? copy.pressed_at : -1;
        if (dimmed_) {
            wake_light(now);
            lock_since_ = -1;
        }
    }
    if (!copy.pressed) {
        lock_since_ = -1;
    }
    if (lock_since_ >= 0 && now - lock_since_ >= 1000 && consume_gesture(copy.press_sequence)) {
        keypad_locked_ = !keypad_locked_;
        lock_since_ = -1;
        wake_light(now);
    }
}

bool UiModel::lock_input(const UiInput &input, bool consumed_wake) {
    if (input.key == UiKey::Release || input.key == UiKey::Quit ||
        (input.key == UiKey::Ptt && input.pressed) ||
        (input.pressed && input.key != UiKey::Star && input.key != UiKey::Ptt &&
         input.key != UiKey::Monitor)) {
        cancel_lock();
    }
    if (input.key == UiKey::Star && consumed_wake) {
        cancel_lock();
    }
    return keypad_locked_ && input.key != UiKey::Ptt && input.key != UiKey::Monitor &&
           input.key != UiKey::Release && input.key != UiKey::Quit;
}
} // namespace ht
