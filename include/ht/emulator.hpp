// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/backend.hpp>
#include <ht/battery.hpp>

namespace ht {
// Developer controls affect the simulated peripheral, never controller state.
void emulator_inject(const BackendStatus &status);
// Voltage, charger and switch are independent fake inputs. Errors are held
// until changed; simulation does not use the C62 voltage-switch threshold.
void emulator_inject_battery(const BatteryReading &reading, int error = 0);
// Copy current development inputs, including cold-start environment overrides.
BatteryReading emulator_battery_input(int &error);
// Inject one failed setup/configure/RX/TX/monitor/register operation for tests.
void emulator_fail_next(int error);
bool emulator_transmitting();
uint32_t emulator_tuned_frequency(); // Current simulated synthesizer frequency, Hz.
void emulator_request_reset();
bool emulator_take_reset_request(); // Consumed only by the radio owner thread.
} // namespace ht
