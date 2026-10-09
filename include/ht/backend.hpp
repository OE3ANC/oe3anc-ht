// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/radio.hpp>

namespace ht {
struct FrequencyBand {
    uint32_t min_hz;
    uint32_t max_hz;
};

struct RadioCapabilities {
    FrequencyBand bands[2];
    uint32_t max_power_mw;
    bool m17;
    bool ctcss;
    bool gain;
    bool registers;
    bool fm_monitor;
    bool dcs; // Both polarities, independent RX/TX.
};

struct BackendStatus {
    bool rx_active = false;
    int16_t rssi_dbm = -127;
    char callsign[10] = {};
    m17::ReceiveStatistics m17_quality;
    Bk4819RxStatus rx_registers;
    int error = 0;
};

// Implemented by exactly one compile-time backend. Only the radio service
// calls these functions; observations from other contexts are synchronized
// inside the backend. No backend may retain borrowed configuration pointers.
const RadioCapabilities &backend_capabilities();
int backend_init();
int backend_configure(const RadioConfig &config);
int backend_receive();
// Transient FM RX gating only. stop/configure must discard this override.
int backend_monitor(bool enabled);
// On success return the uptime timestamp of RF keying, after preparation.
int backend_transmit(int64_t &started_ms);
// Normal release only: complete queued protocol termination while RF is keyed.
// Must be bounded and observe pending faults without the service state mutex.
// Fault shutdown bypasses this operation and calls stop immediately.
// A nonzero uptime deadline limits normal drain. Return -ETIME on deadline,
// without latching a peripheral error; caller immediately stops and returns RX.
int backend_finish_transmit(int64_t deadline_ms);
void backend_stop(); // Idempotent: unkey before cancelling any audio work.
BackendStatus backend_status();
int backend_read_register(uint8_t address, uint16_t &value);
int backend_write_register(uint8_t address, uint16_t value);
} // namespace ht
