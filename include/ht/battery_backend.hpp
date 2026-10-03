// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/battery.hpp>

namespace ht {
const BatteryCapabilities &battery_backend_capabilities();
int battery_backend_init();
// Return an entire validated reading or an error. Do not retain output pointers.
int battery_backend_read(BatteryReading &reading);
} // namespace ht
