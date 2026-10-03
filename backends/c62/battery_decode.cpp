// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2020-2026 OpenRTX Contributors (reference conversion).
// Reference: OpenRTX-c62 756da1d6c6d76fccc5175840db45931dd6fbb557.
#include "battery_decode.hpp"
#include <errno.h>
#include <zephyr/drivers/adc.h>

namespace ht {
int c62_decode_battery(uint16_t charger_raw, uint16_t battery_raw, uint16_t reference_mv,
                       BatteryReading &reading) {
    // The reference uses an offset-coded domain, not unsigned 11-bit counts.
    // Reject negative/out-of-domain voltages; never wrap into an on decision.
    if (!reference_mv || reference_mv > 5000 || charger_raw > 4095 || battery_raw < 2048 ||
        battery_raw > 4095) {
        return -ERANGE;
    }
    int32_t millivolts = int32_t(battery_raw) - 2048;
    const int error = adc_raw_to_millivolts(reference_mv, ADC_GAIN_1, 11, &millivolts);
    if (error) {
        return error;
    }
    millivolts *= 3; // Reference 200 kOhm / 100 kOhm divider, provisional.
    BatteryReading next;
    next.millivolts = uint32_t(millivolts);
    next.charger_input = charger_raw > 2100;
    next.switch_on = millivolts > 3000; // Circuit detection, not low-battery cutoff.
    reading = next;
    return 0;
}
} // namespace ht
