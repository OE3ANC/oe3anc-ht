// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>

namespace ht {
// Shared application/protocol settings; no Zephyr, vendor or UI dependencies.
enum class Destination : uint8_t { Broadcast = 0, Station = 1 };

struct M17Settings {
    Destination destination = Destination::Broadcast;
    char callsign[10] = {}; // Station destination; empty for broadcast.
    uint8_t can = 0;
    bool rx_can_check = false;
};

inline bool valid_callsign(const char (&callsign)[10]) {
    size_t length = 0;
    while (length < sizeof(callsign) && callsign[length]) {
        ++length;
    }
    if (length == 0 || length > 9) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const char c = callsign[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '/' ||
              c == '.')) {
            return false;
        }
    }
    return strcmp(callsign, "ALL") != 0 && strcmp(callsign, "INVALID") != 0;
}

inline bool valid_m17_settings(const M17Settings &settings) {
    if (settings.can > 15) {
        return false;
    }
    switch (settings.destination) {
    case Destination::Broadcast:
        return settings.callsign[0] == 0;
    case Destination::Station:
        return valid_callsign(settings.callsign);
    }
    return false;
}
} // namespace ht
