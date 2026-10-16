// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/audio.hpp>
#include <ht/modem.hpp>

namespace ht {
struct ModemSettings {
    uint8_t txdelay = 30, persistence = 63, slot_time = 10, txtail = 0;
    bool full_duplex = false;
    uint8_t level = 37; // Percent of signed PCM full scale; bench adjustment required.
};
struct ModemStatus {
    ModemSettings settings;
    uint32_t received = 0, transmitted = 0, dropped = 0, rejected = 0;
    unsigned queued = 0;
    bool enabled = false, transmitting = false, finished = false;
    int error = 0;
};
#ifdef CONFIG_HT_MODEM
// Disable is a producer-safe cancellation path; controller alone enables/routes RF.
void modem_disable();
// Separate cancellation ticket also invalidates an entry queued while disabled.
uint32_t modem_entry_ticket();
void modem_cancel_entry();
bool modem_enabled();
uint32_t modem_epoch();
void modem_enable();
void modem_settings(const ModemSettings &settings);
ModemStatus modem_status();
void modem_host(const modem::KissMessage &message, uint32_t epoch);
void modem_rejected();
bool modem_output(modem::Frame &frame, uint32_t epoch);
// Controller only: reserve one packet after carrier/persistence admission.
bool modem_admit(bool carrier);
bool modem_tx_requested();
void modem_complete();
// Backend only. Stop invalidates processing, but keeps the admitted packet for TX setup.
void modem_audio_stop();
int modem_receive(AudioSession session);
int modem_transmit(AudioSession session);
#else
inline void modem_disable() {
}
inline bool modem_enabled() {
    return false;
}

inline bool modem_tx_requested() {
    return false;
}

inline void modem_audio_stop() {
}
#endif
} // namespace ht
