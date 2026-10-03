// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/audio.hpp>
#include <ht/m17_settings.hpp>

namespace ht {
enum class M17Phase : uint8_t {
    Idle,
    Starting,
    Receiving,
    Prepared,
    Transmitting,
    Finished,
    Fault
};

struct M17Status {
    M17Phase phase = M17Phase::Idle;
    int error = 0;
    bool rx_active = false;
    char callsign[10] = {};
};

// One controller owns these operations and the RF/audio routes. RX requires
// Radio capture at 24 kHz and buffered Speaker playback at 8 kHz. TX requires
// Microphone capture at 8 kHz and buffered Radio playback at 48 kHz.
// The worker owns Codec2 and all protocol state; no RF or vendor calls occur.
// Start replaces previous processing, copies the callsign/settings and waits at most
// one second for preparation. An unset RX callsign receives broadcast only.
// Cancel/route a fresh audio session before replacing an operation, so any
// in-flight sample copy from the previous job cannot reach the new route.
int m17_receive(AudioSession session, const char *local_callsign, const M17Settings &settings = {});
int m17_prepare_transmit(AudioSession session, const char *local_callsign,
                         const M17Settings &settings = {});
// Prepare does not send samples. Key RF first, then begin. Normal PTT release
// requests a final voice frame, EOT and playback drain. Poll for Finished before
// unkeying. Fault/forced stop instead unkeys immediately and cancels audio.
int m17_begin_transmit();
int m17_finish_transmit();
// Invalidates processing without waiting for Codec2 or touching audio/RF.
// The owner also cancels its audio session to wake blocked sample transfers.
void m17_cancel();
// Processing errors and a stalled worker latch until reboot, including after
// cancel. The controller polls this snapshot and owns fault-driven shutdown.
M17Status m17_status();
} // namespace ht
