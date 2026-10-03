// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ht {
enum class ToneKind : uint8_t { None = 0, Ctcss = 1, Dcs = 2 };

struct Tone {
    ToneKind kind = ToneKind::None;
    uint16_t value = 0;    // CTCSS tenths Hz, or 9-bit DCS (display as three octal digits).
    bool inverted = false; // DCS only; RX and TX polarity are independent.
};

inline bool valid_tone(const Tone &tone) {
    switch (tone.kind) {
    case ToneKind::None:
        return !tone.value && !tone.inverted;
    case ToneKind::Ctcss:
        return tone.value >= 670 && tone.value <= 2541 && !tone.inverted;
    case ToneKind::Dcs:
        return tone.value <= 0777;
    }
    return false;
}

inline bool same_tone(const Tone &a, const Tone &b) {
    return a.kind == b.kind && a.value == b.value && a.inverted == b.inverted;
}
} // namespace ht
