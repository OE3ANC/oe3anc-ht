// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/m17_settings.hpp>
#include <ht/m17.hpp>
#include <ht/selection.hpp>
#include <ht/tone.hpp>
#include <stdint.h>

namespace ht {
enum class Mode : uint8_t { Fm, M17 };
enum class Bandwidth : uint8_t { Narrow, Wide };
enum class RadioPhase : uint8_t { Starting, Receiving, Transmitting, Diagnostics, Fault, Inactive };

inline bool valid_transmit_limit(uint16_t seconds) {
    return seconds == 0 || seconds == 60 || seconds == 120 || seconds == 180;
}

struct RadioConfig {
    uint32_t rx_frequency_hz = 430000000;
    uint32_t tx_frequency_hz = 430000000;
    bool tx_inhibit = false;
    uint32_t power_mw = 1000;
    Mode mode = Mode::Fm;
    Bandwidth bandwidth = Bandwidth::Wide;
    uint8_t squelch = 4;
    uint8_t gain = 0;
    uint16_t transmit_limit_s = 180; // Global normal setting; zero disables the limit.
    Tone rx_tone;
    Tone tx_tone;
    char callsign[10] = {}; // Local station, independent of m17 destination.
    M17Settings m17;
};

// Read-only BK4819 RX observations, grouped into eight-register status pages.
// Never read interrupt/FIFO registers here: status must not consume events.
constexpr uint8_t bk4819_rx_addresses[] = {
    0x30, 0x33, 0x38, 0x39, 0x43, 0x47, 0x48, 0x37, // RX path, tuning, filters/audio.
    0x10, 0x11, 0x12, 0x13, 0x14, 0x49, 0x7b, 0x7e, // RF gain table and AGC.
    0x0c, 0x63, 0x65, 0x67, 0x4d, 0x4e, 0x4f, 0x78, // Signal and hardware squelch.
};

struct Bk4819RxStatus {
    uint16_t values[sizeof(bk4819_rx_addresses)] = {};
    int64_t sample_ms = 0;
    bool valid = false; // Only meaningful while Receiving; emulator leaves unavailable.
};

struct RadioState {
    uint32_t generation = 0;             // Changes on restart or inactive/active transition.
    uint32_t configuration_revision = 0; // Successful configure or owner edit authorization.
    Selection selection;
    // Separate retained acknowledgement for the settings owner's one operation.
    // Ordinary commands/events cannot overwrite it. Restart clears it.
    uint32_t recall_id = 0;
    uint32_t recall_generation = 0;
    int recall_error = 0;
    uint32_t shutdown_sequence = 0; // Counts off transitions; survives explicit restart.
    int64_t shutdown_ms = 0;        // Uptime when latest backend stop completed.
    RadioConfig config;
    RadioPhase phase = RadioPhase::Starting;
    bool power_active = true; // Normal operation allowed; faults survive inactivity.
    bool rx_active = false;
    bool monitor_active = false; // Transient FM RX override, never a setting.
    bool companion_mode = false; // Temporary UART ownership; physical PTT disabled.
    int16_t rssi_dbm = -127;
    Bk4819RxStatus rx_registers;
    char received_callsign[10] = {};
    m17::ReceiveStatistics m17_quality;
    int fault = 0;
    int command_error = 0;
    int ptt_error = 0;
    uint16_t tx_remaining_s = 0; // Zero when not keyed or limit disabled.
    bool tx_warning = false;     // Final ten seconds of a limited transmission.
    bool tx_timed_out = false;   // Visible while held; release is required to retry.
    uint32_t command_id = 0;
    uint16_t register_value = 0;
};

enum class CommandKind : uint8_t {
    Configure,
    Recall,        // Settings owner only: validated identity + complete configuration.
    Edit,          // Settings owner only: authorize metadata edit/identity, no RF retune.
    QuickControls, // Only gain/SQL; requires expected lifecycle/revision/selection.
    EnterDiagnostics,
    ExitDiagnostics,
    ReadRegister,
    WriteRegister,
    TransmitLimit, // Global limit only; guarded, without RF/audio reconfiguration.
    CompanionMode
};

struct RadioCommand {
    CommandKind kind = CommandKind::Configure;
    uint32_t id = 0;
    RadioConfig config;
    Selection selection;
    uint32_t expected_generation = 0;
    uint32_t expected_revision = 0;
    uint8_t register_address = 0;
    uint16_t register_value = 0;
    bool companion_enabled = false;
    bool companion_disconnected = false; // Local exit confirmation, not an idle-line guess.
};

int validate_config(const RadioConfig &config);
bool same_operating(const RadioConfig &a, const RadioConfig &b);

// Single-owner state machine. Methods are called only by the radio service.
class RadioController {
  public:
    int start(const RadioConfig &config, bool power_active = true, const Selection &selection = {});
    int execute(const RadioCommand &command);
    void set_ptt(bool pressed);
    void set_remote_ptt(bool pressed);
    void set_power(bool active);
    void set_monitor(bool pressed);
    // Consume auxiliary hold on PTT activity even if the PTT tap was coalesced.
    // This cancels RX monitor without synthesizing a transmit edge.
    void interrupt_monitor(bool held);
    void poll();
    void report_fault(int error);

    const RadioState &state() const {
        return state_;
    }

  private:
    RadioState state_;
    bool ptt_ = false;
    bool remote_ptt_ = false;
    bool ptt_armed_ = true;
    bool monitor_pressed_ = false;
    int64_t tx_deadline_ms_ = 0;
    void apply_ptt(bool pressed, bool remote);
    int configure(const RadioConfig &config);
    int receive();
    int fail(int error);
    void expire_transmit();
    void cancel_monitor();
};

// Call start/service from one owner thread. Producers may submit from other
// threads. submit returns queue acceptance, not command completion; inspect
// command_id/command_error in a snapshot.
// Settings-owned Recall/Edit instead use the retained recall_* acknowledgement;
// they never overwrite ordinary command results.
int radio_start(const RadioConfig &config, const Selection &selection = {});
void radio_service();
int radio_submit(const RadioCommand &command);
void radio_ptt(bool pressed);
// Companion intent has its own non-queued slot and bounded lease. Only the
// radio owner calls RF; release/session cancellation never waits for its mutex.
void radio_remote_session(uint64_t session);
int radio_remote_ptt_press(uint64_t session, uint32_t token);
int radio_remote_ptt_keep(uint64_t session, uint32_t token);
int radio_remote_ptt_release(uint64_t session);
bool radio_remote_ptt_requested();
// Also retained after release to bound M17 termination; cancellation expires it.
int64_t radio_remote_ptt_deadline();
// Producer press counter (modulo 2^32), independent of queue/phase/TX success.
// Read-only UI interruption observation; never consumes/replays PTT. Not reset
// by radio_start. Compare against a marker captured when a draft/gesture begins.
uint32_t radio_ptt_press_sequence();
// Switch intent bypasses ordinary queues. An off edge survives a rapid on.
// Backends observe it during TX preparation/drain without taking state locks.
void radio_power(bool active);
bool radio_power_requested();
// Raw producer level, including a newer on intent hidden by a pending off edge.
// Storage cancellation uses it; TX must keep using the gated query above.
bool radio_power_on_intent();
// Auxiliary hold input; release bypasses ordinary command/UI queue capacity.
void radio_monitor(bool pressed);
// Independent UI wake observation, including coalesced side-button taps.
uint32_t radio_monitor_press_sequence();
bool radio_monitor_requested();
// Peripheral/UI threads report faults without waiting for state or queue space.
// The controller performs shutdown on its next service tick.
void radio_report_fault(int error);
// Atomic producer level gated by pending faults, available during backend waits.
// A backend must check it again before keying after blocking TX preparation.
bool radio_ptt_requested();
// Atomic fault observation during a bounded backend completion wait. No state
// mutex is acquired; a fault must abort protocol draining and unkey RF.
int radio_pending_fault();
// Atomic published controller fault, or a pending peripheral fault. Storage
// uses this broader observation; no state mutex is acquired.
int radio_latched_fault();
// Flash backends reserve RX/diagnostic/inactive state before operations which mask
// interrupts. Nonblocking: false means defer. The acquiring thread must
// unlock after I/O and must not call mutex-taking radio APIs while holding it.
// Atomic intent/fault queries remain safe. inactive_only also requires stopped
// controller state and excludes switch-on intent before each shutdown write.
bool radio_idle_lock(bool inactive_only = false);
void radio_idle_unlock();
RadioState radio_snapshot();
} // namespace ht
