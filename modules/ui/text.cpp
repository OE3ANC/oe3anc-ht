// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui_text.hpp>
#include <errno.h>
#include <string.h>

namespace ht {
size_t TextEditor::capacity() const {
    return kind_ == TextKind::Name        ? 24
           : kind_ == TextKind::Callsign  ? 9
           : kind_ == TextKind::Frequency ? 11
                                          : 3;
}

bool TextEditor::normalize(char &character, TextKind kind) const {
    if (kind == TextKind::Name) {
        return character >= ' ' && character <= '~';
    }
    if (kind == TextKind::Callsign && character >= 'a' && character <= 'z') {
        character -= 'a' - 'A';
    }
    return (character >= '0' && character <= '9') ||
           (character == '.' && kind != TextKind::ChannelNumber) ||
           (kind == TextKind::Callsign &&
            ((character >= 'A' && character <= 'Z') || character == '-' || character == '/'));
}

int TextEditor::begin(TextKind kind, const char *initial) {
    if (!initial || kind > TextKind::ChannelNumber) {
        return -EINVAL;
    }
    char checked[25];
    size_t length = 0;
    while (length < sizeof(checked) && initial[length]) {
        ++length;
    }
    const size_t limit = kind == TextKind::Name        ? 24
                         : kind == TextKind::Callsign  ? 9
                         : kind == TextKind::Frequency ? 11
                                                       : 3;
    if (length > limit) {
        return -ENOSPC;
    }
    for (size_t i = 0; i < length; ++i) {
        checked[i] = initial[i];
        if (!normalize(checked[i], kind)) {
            return -EINVAL;
        }
    }
    checked[length] = 0;
    memcpy(text_, checked, length + 1);
    kind_ = kind;
    length_ = cursor_ = length;
    finish();
    return 0;
}

int TextEditor::insert(char character) {
    if (length_ == capacity()) {
        return -ENOSPC;
    }
    memmove(text_ + cursor_ + 1, text_ + cursor_, length_ - cursor_ + 1);
    text_[cursor_++] = character;
    ++length_;
    return 0;
}

int TextEditor::literal(char character) {
    if (!normalize(character, kind_)) {
        return -EINVAL;
    }
    finish();
    return insert(character);
}

void TextEditor::advance(int64_t now_ms) {
    if (pending() && (now_ms < last_tap_ms_ || now_ms - last_tap_ms_ >= tap_timeout_ms)) {
        finish();
    }
}

int TextEditor::digit(char digit, int64_t now_ms) {
    if (digit < '0' || digit > '9' || now_ms < 0) {
        return -EINVAL;
    }
    if (kind_ == TextKind::Frequency || kind_ == TextKind::ChannelNumber) {
        return literal(digit);
    }
    static const char *groups[] = {" 0",   ".,?!-/1", "ABC2",  "DEF3", "GHI4",
                                   "JKL5", "MNO6",    "PQRS7", "TUV8", "WXYZ9"};
    static const char *call_groups[] = {"0",    ".-/1", "ABC2",  "DEF3", "GHI4",
                                        "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9"};
    advance(now_ms);
    const unsigned index = digit - '0';
    const char *group = (kind_ == TextKind::Callsign ? call_groups : groups)[index];
    if (active_digit_ == index) {
        tap_index_ = (tap_index_ + 1) % strlen(group);
        text_[cursor_ - 1] = group[tap_index_];
    } else {
        finish();
        const int error = insert(group[0]);
        if (error) {
            return error;
        }
        active_digit_ = index;
        tap_index_ = 0;
    }
    last_tap_ms_ = now_ms;
    return 0;
}

void TextEditor::move(int direction) {
    finish();
    if (direction < 0 && cursor_) {
        --cursor_;
    } else if (direction > 0 && cursor_ < length_) {
        ++cursor_;
    }
}

void TextEditor::erase() {
    finish();
    if (cursor_) {
        memmove(text_ + cursor_ - 1, text_ + cursor_, length_ - cursor_ + 1);
        --cursor_;
        --length_;
    }
}

int ui_parse_frequency(const char *text, uint32_t &frequency) {
    if (!text) {
        return -EINVAL;
    }
    uint32_t whole = 0, fraction = 0, scale = 1000000;
    bool decimal = false, digit = false;
    for (const char *p = text; *p; ++p) {
        if (*p == '.' && !decimal) {
            decimal = true;
        } else if (*p >= '0' && *p <= '9') {
            digit = true;
            if (decimal) {
                if (scale == 1) {
                    return -EINVAL;
                }
                scale /= 10;
                fraction += (*p - '0') * scale;
            } else {
                whole = whole * 10 + (*p - '0'); // whole is bounded before the next multiplication.
                if (whole > UINT32_MAX / 1000000) {
                    return -ERANGE;
                }
            }
        } else {
            return -EINVAL;
        }
    }
    if (!digit) {
        return -EINVAL;
    }
    const uint64_t result = uint64_t(whole) * 1000000 + fraction;
    if (result > UINT32_MAX) {
        return -ERANGE;
    }
    frequency = result;
    return 0;
}

} // namespace ht
