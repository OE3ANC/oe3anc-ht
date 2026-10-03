// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ht {
constexpr int64_t battery_freshness_ms = 300;
enum class BatteryFreshness : uint8_t { Unknown, Fresh, Stale };

struct BatteryCapabilities {
    bool voltage;
    bool charger_input;
    bool power_switch;
};

struct BatteryReading {
    uint32_t millivolts = 0;
    bool charger_input = false; // Presence indication, not charge/full control.
    bool switch_on = false;     // Target-derived; never inferred by shared code.
};

struct BatterySnapshot {
    BatteryReading reading;
    BatteryFreshness freshness = BatteryFreshness::Unknown;
    int error = 0;         // Last sampling/setup error; never converted to zero volts.
    int64_t sample_ms = 0; // Last successful reading's uptime.
    int64_t attempt_ms = 0;
};

const BatteryCapabilities &battery_capabilities();
// One sampling owner calls this, initially during startup and subsequently
// from the battery worker. Snapshot consumers never perform ADC work.
int battery_sample();
BatterySnapshot battery_snapshot();
} // namespace ht
