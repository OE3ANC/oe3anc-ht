// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/battery.hpp>

namespace ht {
// Pure conversion boundary for reference-domain and meter checks.
int c62_decode_battery(uint16_t charger_raw, uint16_t battery_raw, uint16_t reference_mv,
                       BatteryReading &reading);
} // namespace ht
