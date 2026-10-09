// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/backend.hpp>
#ifdef CONFIG_HT_COMPANION
#include <ht/companion.h>
#endif
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
bool same_operating(const RadioConfig &a, const RadioConfig &b) {
    return a.rx_frequency_hz == b.rx_frequency_hz && a.tx_frequency_hz == b.tx_frequency_hz &&
           a.tx_inhibit == b.tx_inhibit && a.power_mw == b.power_mw && a.mode == b.mode &&
           a.bandwidth == b.bandwidth && a.squelch == b.squelch &&
           (a.mode == Mode::M17
                ? a.m17.destination == b.m17.destination && a.m17.can == b.m17.can &&
                      a.m17.rx_can_check == b.m17.rx_can_check &&
                      !strncmp(a.m17.callsign, b.m17.callsign, sizeof(a.m17.callsign))
                : same_tone(a.rx_tone, b.rx_tone) && same_tone(a.tx_tone, b.tx_tone));
}

int validate_config(const RadioConfig &config) {
    const auto &caps = backend_capabilities();
    const uint32_t frequencies[] = {config.rx_frequency_hz, config.tx_frequency_hz};
    for (uint32_t frequency : frequencies) {
        bool in_band = false;
        for (const auto &band : caps.bands) {
            in_band |= frequency >= band.min_hz && frequency <= band.max_hz;
        }
        if (!in_band) {
            return -EINVAL;
        }
    }
    if (config.power_mw == 0 || config.power_mw > caps.max_power_mw || config.squelch > 15 ||
        config.gain > 15 || !valid_transmit_limit(config.transmit_limit_s) ||
        (config.mode != Mode::Fm && config.mode != Mode::M17) ||
        (config.bandwidth != Bandwidth::Narrow && config.bandwidth != Bandwidth::Wide)) {
        return -EINVAL;
    }
    if (config.mode == Mode::M17 && !valid_m17_settings(config.m17)) {
        return -EINVAL;
    }
    if ((config.mode == Mode::M17 && !caps.m17) || (config.gain && !caps.gain)) {
        return -ENOTSUP;
    }
    const Tone tones[] = {config.rx_tone, config.tx_tone};
    for (const auto &tone : tones) {
        if (!valid_tone(tone)) {
            return -EINVAL;
        }
        if ((tone.kind == ToneKind::Ctcss && !caps.ctcss) ||
            (tone.kind == ToneKind::Dcs && !caps.dcs)) {
            return -ENOTSUP;
        }
    }
    // An unset local callsign is valid for reception, but never for M17 TX.
    if (config.callsign[0] && !valid_callsign(config.callsign)) {
        return -EINVAL;
    }
    return 0;
}

int RadioController::fail(int error) {
    backend_stop();
    state_.monitor_active = false;
    tx_deadline_ms_ = 0;
    state_.tx_remaining_s = 0;
    state_.tx_warning = state_.tx_timed_out = false;
    state_.phase = state_.power_active ? RadioPhase::Fault : RadioPhase::Inactive;
    state_.fault = error < 0 ? error : -EIO;
    state_.rx_active = false;
    state_.rx_registers = {};
    memset(state_.received_callsign, 0, sizeof(state_.received_callsign));
    return state_.fault;
}

void RadioController::report_fault(int error) {
    if (error && !state_.fault) {
        fail(error);
    }
}

int RadioController::receive() {
    state_.monitor_active = false;
    tx_deadline_ms_ = 0;
    state_.tx_remaining_s = 0;
    state_.tx_warning = false;
    const int error = backend_receive();
    if (error) {
        return fail(error);
    }
    state_.phase = RadioPhase::Receiving;
    state_.rx_active = false;
    state_.rx_registers = {};
    state_.m17_quality = {};
    memset(state_.received_callsign, 0, sizeof(state_.received_callsign));
    return 0;
}

int RadioController::start(const RadioConfig &config, bool power_active,
                           const Selection &selection) {
    const uint32_t generation = state_.generation + 1;
    const uint32_t shutdown_sequence = state_.shutdown_sequence;
    const int64_t shutdown_ms = state_.shutdown_ms;
    state_ = {};
    state_.shutdown_sequence = shutdown_sequence;
    state_.shutdown_ms = shutdown_ms;
    state_.generation = generation;
    state_.power_active = power_active;
    ptt_ = false;
    remote_ptt_ = false;
    ptt_armed_ = power_active;
    monitor_pressed_ = false;
    tx_deadline_ms_ = 0;
    backend_stop();
    int error = valid_selection(selection) ? validate_config(config) : -EINVAL;
#ifdef CONFIG_HT_COMPANION
    if (!error) {
        error = ht_companion_set_enabled(false);
    }
#endif
    if (error) {
        return fail(error);
    }
    state_.config = config;
    state_.selection = selection;
    if (!power_active) {
        state_.phase = RadioPhase::Inactive;
        return 0;
    }
    error = backend_init();
    if (error) {
        return fail(error);
    }
    if (!radio_power_requested()) {
        set_power(false);
        return 0;
    }
    return configure(config);
}

int RadioController::configure(const RadioConfig &config) {
    const int validation = validate_config(config);
    if (validation) {
        return validation;
    }
    state_.monitor_active = false;
    backend_stop();
    const int error = backend_configure(config);
    if (error) {
        return fail(error);
    }
    const int result = receive();
    if (result == 0) {
        state_.config = config;
        ++state_.configuration_revision;
    }
    return result;
}

int RadioController::execute(const RadioCommand &command) {
    int error = 0;
    if (!state_.power_active) {
        error = -EHOSTDOWN;
    } else if (state_.phase == RadioPhase::Fault) {
        error = state_.fault;
    } else {
        switch (command.kind) {
        case CommandKind::Configure: {
            const bool changed = !same_operating(state_.config, command.config);
            error = state_.phase == RadioPhase::Receiving ? configure(command.config) : -EBUSY;
            if (!error && changed) {
                state_.selection.operating = Operating::Vfo;
            }
            break;
        }
        case CommandKind::Recall:
        case CommandKind::Edit:
            if (command.expected_generation != state_.generation ||
                command.expected_revision != state_.configuration_revision) {
                error = -ESTALE;
            } else if (!command.id || !valid_selection(command.selection)) {
                error = -EINVAL;
            } else {
                error = state_.phase != RadioPhase::Receiving ? -EBUSY
                        : command.kind == CommandKind::Recall ? configure(command.config)
                                                              : 0;
                if (!error) {
                    state_.selection = command.selection;
                    if (command.kind == CommandKind::Edit) {
                        ++state_.configuration_revision;
                    }
                }
            }
            break;
        case CommandKind::QuickControls: {
            if (command.expected_generation != state_.generation ||
                command.expected_revision != state_.configuration_revision ||
                !same_selection(command.selection, state_.selection)) {
                error = -ESTALE;
                break;
            }
            RadioConfig config = state_.config;
            config.gain = command.config.gain;
            config.squelch = command.config.squelch;
            error = config.mode != Mode::Fm && config.squelch != state_.config.squelch ? -ENOTSUP
                    : state_.phase != RadioPhase::Receiving                            ? -EBUSY
                    : config.gain == state_.config.gain && config.squelch == state_.config.squelch
                        ? 0
                        : configure(config);
            break;
        }
        case CommandKind::TransmitLimit:
            if (command.expected_generation != state_.generation ||
                command.expected_revision != state_.configuration_revision ||
                !same_selection(command.selection, state_.selection)) {
                error = -ESTALE;
            } else if (!valid_transmit_limit(command.config.transmit_limit_s)) {
                error = -EINVAL;
            } else if (state_.phase != RadioPhase::Receiving || ptt_) {
                error = -EBUSY;
            } else if (state_.config.transmit_limit_s != command.config.transmit_limit_s) {
                state_.config.transmit_limit_s = command.config.transmit_limit_s;
                ++state_.configuration_revision;
            }
            break;
        case CommandKind::CompanionMode:
#ifdef CONFIG_HT_COMPANION
            if (command.expected_generation != state_.generation ||
                command.expected_revision != state_.configuration_revision) {
                error = -ESTALE;
            } else if (state_.phase != RadioPhase::Receiving || ptt_) {
                error = -EBUSY;
            } else if (state_.companion_mode && !command.companion_enabled &&
                       !command.companion_disconnected) {
                error = -EPERM;
            } else if (command.companion_enabled != state_.companion_mode) {
                error = ht_companion_set_enabled(command.companion_enabled);
                if (!error) {
                    state_.companion_mode = command.companion_enabled;
                    ptt_armed_ = false;
                    ++state_.configuration_revision;
                }
            }
#else
            error = -ENOTSUP;
#endif
            break;
        case CommandKind::EnterDiagnostics:
            if (state_.phase != RadioPhase::Receiving || ptt_) {
                error = -EBUSY;
            } else if (!backend_capabilities().registers) {
                error = -ENOTSUP;
            } else {
                state_.monitor_active = false;
                backend_stop();
                state_.phase = RadioPhase::Diagnostics;
                state_.rx_active = false;
                memset(state_.received_callsign, 0, sizeof(state_.received_callsign));
            }
            break;
        case CommandKind::ExitDiagnostics:
            if (state_.phase != RadioPhase::Diagnostics || ptt_) {
                error = -EBUSY;
            } else {
                error = backend_init();
                if (error) {
                    fail(error);
                } else {
                    error = configure(state_.config);
                }
            }
            break;
        case CommandKind::ReadRegister:
        case CommandKind::WriteRegister:
            if (state_.phase != RadioPhase::Diagnostics) {
                error = -EBUSY;
            } else if (command.register_address > 0x7f) {
                error = -EINVAL;
            } else if (command.kind == CommandKind::ReadRegister) {
                error = backend_read_register(command.register_address, state_.register_value);
            } else {
                error = backend_write_register(command.register_address, command.register_value);
            }
            break;
        default:
            error = -EINVAL;
        }
    }
    if (command.kind == CommandKind::Recall || command.kind == CommandKind::Edit) {
        state_.recall_id = command.id;
        state_.recall_generation = state_.generation;
        state_.recall_error = error;
    } else {
        state_.command_id = command.id;
        state_.command_error = error;
    }
    return error;
}

void RadioController::set_ptt(bool pressed) {
    if (state_.companion_mode) {
        return;
    }
    apply_ptt(pressed, false);
}

void RadioController::set_remote_ptt(bool pressed) {
    if (!state_.companion_mode) {
        return;
    }
    apply_ptt(pressed, true);
}

void RadioController::apply_ptt(bool pressed, bool remote) {
    // An on transition disarms TX even if the physical level was already held.
    // A released sample while active is required before a subsequent press.
    if (!pressed && state_.power_active) {
        ptt_armed_ = true;
    }
    if (pressed == ptt_) {
        return;
    }
    ptt_ = pressed;
    if (pressed) {
        remote_ptt_ = remote;
    }
    if (pressed) {
        cancel_monitor();
    }
    state_.ptt_error = 0;
    if (!pressed) {
        state_.tx_timed_out = false;
    }
    if (!pressed && state_.phase == RadioPhase::Transmitting) {
        const auto lease_deadline = remote_ptt_ ? radio_remote_ptt_deadline() : 0;
        const auto deadline =
            lease_deadline && (!tx_deadline_ms_ || lease_deadline < tx_deadline_ms_)
                ? lease_deadline
                : tx_deadline_ms_;
        const int error = backend_finish_transmit(deadline);
        if (error == -ETIME && remote_ptt_) {
            backend_stop();
            receive();
            return;
        }
        if (error == -ECANCELED && !radio_power_requested()) {
            set_power(false);
            return;
        }
        if (error == -ETIME && tx_deadline_ms_ && k_uptime_get() >= tx_deadline_ms_) {
            expire_transmit();
            return;
        }
        if (error) {
            fail(error);
            return;
        }
        backend_stop();
        receive();
    } else if (pressed && state_.phase == RadioPhase::Receiving) {
        if (!ptt_armed_) {
            state_.ptt_error = -EPERM;
            return;
        }
        if (state_.config.tx_inhibit) {
            state_.ptt_error = -EPERM;
            return;
        }
        if (state_.config.mode == Mode::M17 && !valid_callsign(state_.config.callsign)) {
            state_.ptt_error = -EADDRNOTAVAIL;
            return;
        }
        backend_stop();
        int64_t started_ms = 0;
        const int error = backend_transmit(started_ms);
        if (error == -ECANCELED) {
            backend_stop();
            if (!radio_power_requested()) {
                set_power(false);
            } else {
                receive();
            } // Released during preparation; not a hardware fault.
        } else if (error) {
            fail(error);
        } else {
            state_.phase = RadioPhase::Transmitting;
            tx_deadline_ms_ =
                state_.config.transmit_limit_s
                    ? started_ms + static_cast<int64_t>(state_.config.transmit_limit_s) * 1000
                    : 0;
            state_.tx_remaining_s = state_.config.transmit_limit_s;
            state_.rx_active = false;
            memset(state_.received_callsign, 0, sizeof(state_.received_callsign));
        }
    }
}

void RadioController::set_power(bool active) {
    if (active == state_.power_active) {
        return;
    }
    state_.power_active = active;
    ++state_.generation; // Cancel UI drafts/queued commands even on a coalesced cycle.
    if (!active) {
        ++state_.shutdown_sequence;
    }
    ptt_armed_ = false;
    backend_stop(); // PA/audio stop before any reinitialization or storage work.
    if (!active) {
        state_.shutdown_ms = k_uptime_get();
    }
    state_.monitor_active = false;
    monitor_pressed_ = true; // Auxiliary hold also needs a fresh release/repress.
    tx_deadline_ms_ = 0;
    state_.tx_remaining_s = 0;
    state_.tx_warning = state_.tx_timed_out = false;
    state_.rx_active = false;
    memset(state_.received_callsign, 0, sizeof(state_.received_callsign));
    if (!active) {
        state_.phase = RadioPhase::Inactive;
        return;
    }
    if (state_.fault) {
        fail(state_.fault);
        return;
    }
    const int error = backend_init();
    if (error) {
        fail(error);
    } else if (!radio_power_requested()) {
        set_power(false);
    } else {
        configure(state_.config);
    }
}

void RadioController::cancel_monitor() {
    if (!state_.monitor_active) {
        return;
    }
    state_.monitor_active = false;
    const int error = backend_monitor(false);
    if (error) {
        fail(error);
    } else {
        poll();
    } // Restore normal RSSI/tone gating immediately.
}

void RadioController::set_monitor(bool pressed) {
    if (pressed == monitor_pressed_) {
        return;
    }
    monitor_pressed_ = pressed; // Consume held level even when activation is excluded.
    if (!pressed) {
        cancel_monitor();
    } else if (!ptt_ && state_.phase == RadioPhase::Receiving && state_.config.mode == Mode::Fm &&
               backend_capabilities().fm_monitor) {
        const int error = backend_monitor(true);
        if (error) {
            fail(error);
        } else {
            state_.monitor_active = true;
            poll();
        }
    }
}

void RadioController::interrupt_monitor(bool held) {
    monitor_pressed_ = held;
    cancel_monitor();
}

void RadioController::expire_transmit() {
    // Forced stop bypasses queued protocol termination, just as fault shutdown
    // does. Keep the sampled PTT level held so only release/repress can rekey.
    backend_stop();
    if (!receive()) {
        state_.tx_timed_out = ptt_;
        state_.ptt_error = ptt_ ? -ETIMEDOUT : 0;
    }
}

void RadioController::poll() {
    if (state_.phase == RadioPhase::Fault || state_.phase == RadioPhase::Inactive) {
        return;
    }
    // Lease expiry is sampled by the controller, independent of a browser release event.
    if (state_.phase == RadioPhase::Transmitting && remote_ptt_ && !radio_remote_ptt_requested()) {
        set_remote_ptt(false);
        if (state_.phase != RadioPhase::Receiving) {
            return;
        }
    }
    const auto status = backend_status();
    if (status.error) {
        fail(status.error);
        return;
    }
    if (state_.phase == RadioPhase::Transmitting && tx_deadline_ms_) {
        const int64_t remaining_ms = tx_deadline_ms_ - k_uptime_get();
        if (remaining_ms <= 0) {
            expire_transmit();
            return;
        }
        state_.tx_remaining_s = (remaining_ms + 999) / 1000;
        state_.tx_warning = remaining_ms <= 10000;
    }
    state_.rssi_dbm = status.rssi_dbm;
    state_.rx_registers = status.rx_registers;
    state_.rx_active = state_.phase == RadioPhase::Receiving && status.rx_active;
    state_.m17_quality = state_.phase == RadioPhase::Receiving && state_.config.mode == Mode::M17
                             ? status.m17_quality
                             : m17::ReceiveStatistics{};
    memset(state_.received_callsign, 0, sizeof(state_.received_callsign));
    if (state_.rx_active && state_.config.mode == Mode::M17 && valid_callsign(status.callsign)) {
        memcpy(state_.received_callsign, status.callsign, sizeof(status.callsign));
    }
}
} // namespace ht
