// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ht {
enum class Operating : uint8_t { Vfo = 0, Memory = 1 };

struct Selection {
    Operating operating = Operating::Vfo;
    uint32_t bank_id = 0;    // Zero is the derived All channels view.
    uint32_t channel_id = 0; // Remembered memory, also retained while in VFO.
};

inline bool valid_selection(const Selection &s) {
    return s.operating == Operating::Vfo || (s.operating == Operating::Memory && s.channel_id);
}

inline bool same_selection(const Selection &a, const Selection &b) {
    return a.operating == b.operating && a.bank_id == b.bank_id && a.channel_id == b.channel_id;
}
} // namespace ht
