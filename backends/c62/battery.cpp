// SPDX-License-Identifier: GPL-3.0-or-later
#include "battery_decode.hpp"
#include <errno.h>
#include <ht/battery_backend.hpp>
#include <zephyr/drivers/adc.h>

namespace ht {
#define BATTERY_NODE DT_NODELABEL(battery_monitor)
BUILD_ASSERT(DT_IO_CHANNELS_INPUT_BY_IDX(BATTERY_NODE, 0) == 1 &&
                 DT_IO_CHANNELS_INPUT_BY_IDX(BATTERY_NODE, 1) == 2,
             "C62 requires charger channel 1 and battery channel 2 in ascending order");
BUILD_ASSERT(DT_SAME_NODE(DT_IO_CHANNELS_CTLR_BY_IDX(BATTERY_NODE, 0), DT_NODELABEL(adc0)) &&
                 DT_SAME_NODE(DT_IO_CHANNELS_CTLR_BY_IDX(BATTERY_NODE, 1), DT_NODELABEL(adc0)),
             "C62 battery inputs must share ADC0");
static const adc_dt_spec charger = ADC_DT_SPEC_GET_BY_IDX(BATTERY_NODE, 0);
static const adc_dt_spec battery = ADC_DT_SPEC_GET_BY_IDX(BATTERY_NODE, 1);
static const BatteryCapabilities capabilities{true, true, true};

const BatteryCapabilities &battery_backend_capabilities() {
    return capabilities;
}

int battery_backend_init() {
    if (!device_is_ready(charger.dev) || !device_is_ready(battery.dev)) {
        return -ENODEV;
    }
    const int error = adc_channel_setup_dt(&charger);
    return error ? error : adc_channel_setup_dt(&battery);
}

int battery_backend_read(BatteryReading &reading) {
    uint16_t raw[2]; // ADC driver returns selected channels in ascending order.
    const adc_sequence sequence = {.channels = BIT(charger.channel_id) | BIT(battery.channel_id),
                                   .buffer = raw,
                                   .buffer_size = sizeof(raw),
                                   .resolution = 11};
    const int error = adc_read(battery.dev, &sequence);
    return error ? error
                 : c62_decode_battery(raw[0], raw[1], adc_ref_internal(battery.dev), reading);
}
} // namespace ht
