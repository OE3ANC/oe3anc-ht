// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ht {
enum class UiKey : uint8_t {
    Up,
    Down,
    Left,
    Right,
    Enter,
    Back,
    Erase,
    Star,
    Hash,
    Character, // Literal host text.
    Digit,     // Physical keypad multi-tap; numeric in frequency/diagnostics.
    Ptt,
    Release,
    Quit,
    Monitor
};

struct UiInput {
    UiKey key;
    char character = 0;
    bool pressed = true;
    int64_t timestamp_ms = -1;      // Producer uptime; -1 uses UI observation time.
    uint32_t remote_generation = 0; // Zero for local input; assigned by remote admission.
};

// All ordinary front-key producers share this nonblocking 16-entry FIFO.
// It owns copies in accepted enqueue order; overflow returns -EBUSY. Only the
// UI owner drains it. Independent PTT/monitor/release/quit must bypass it and
// are rejected here. Star level/release/cancellation remain loss-independent.
int ui_queue_input(const UiInput &input);
bool ui_take_input(UiInput &input);
// Serial worker sets the exact active session; zero invalidates remote inputs.
// Releases/clear bypass FIFO capacity. Held inputs expire after 500 ms unless
// KEEP carries the exact held mask; neither polling nor CPS renews this lease.
void ui_remote_session(uint64_t session);
int ui_remote_key(UiInput input, unsigned wire_key);
int ui_remote_keep(uint32_t held_mask);
void ui_remote_clear();
} // namespace ht
