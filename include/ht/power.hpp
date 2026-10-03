// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/battery.hpp>
#include <ht/radio.hpp>

namespace ht {
// Main composition calls prepare before any normal radio/audio/UI startup.
int power_prepare();
// The battery sampling owner supplies completed cached readings only.
void power_sample(const BatterySnapshot &snapshot, int64_t now_ms);
// Confirmed switch level, independent of an unconsumed radio off edge.
bool power_switch_on();
// Radio owner applies target lights after controller shutdown/resume.
// No LVGL operations; returns a target error for the fault bridge.
int power_apply_outputs(const RadioState &state);
} // namespace ht
