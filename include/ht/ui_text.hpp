// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ht {
enum class TextKind : uint8_t { Name, Callsign, Frequency, ChannelNumber };

// One copied draft, no LVGL/controller/storage pointers. UI-thread ownership.
class TextEditor {
  public:
    static constexpr int64_t tap_timeout_ms = 750;
    int begin(TextKind kind, const char *initial = "");
    int digit(char digit, int64_t now_ms);
    int literal(char character);
    void move(int direction);
    void erase();

    void finish() {
        active_digit_ = 255;
    }

    void advance(int64_t now_ms);

    const char *text() const {
        return text_;
    }

    size_t cursor() const {
        return cursor_;
    }

    size_t length() const {
        return length_;
    }

    bool pending() const {
        return active_digit_ != 255;
    }

    TextKind kind() const {
        return kind_;
    }

  private:
    char text_[25] = {};
    TextKind kind_ = TextKind::Name;
    uint8_t cursor_ = 0, length_ = 0;
    uint8_t active_digit_ = 255, tap_index_ = 0;
    int64_t last_tap_ms_ = 0;
    size_t capacity() const;
    int insert(char character);
    bool normalize(char &character, TextKind kind) const;
};

// MHz text to integer Hz; results remain unchanged on syntax/overflow errors.
// Band/capability validation is a separate controller/editor responsibility.
int ui_parse_frequency(const char *text, uint32_t &frequency_hz);
} // namespace ht
