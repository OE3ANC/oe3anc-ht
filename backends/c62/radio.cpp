// SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Power duties and audio mux behavior adapted from radio_C62.cpp/audio_c62.c.
#include "bk4819.h"
#include "rf_io.h"
#include <errno.h>
#include <ht/audio.hpp>
#include <ht/backend.hpp>
#ifdef CONFIG_HT_COMPANION
#include <ht/companion.h>
#endif
#include <ht/fm.hpp>
#include <ht/m17_mode.hpp>
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
static constexpr RadioCapabilities capabilities{
    {{136000000, 174000000}, {400000000, 480000000}}, 5000, true, true, false, true, true, true};
static RadioConfig configuration;
static FmSquelch squelch;
static bool io_ready;
static bool radio_ready;
static bool configured;
static bool receiving;
static bool transmitting;
static bool audible;
static bool digital_activity;
static bool monitor;
static int peripheral_error;
static Bk4819RxStatus rx_registers;

const RadioCapabilities &backend_capabilities() {
    return capabilities;
}

static void remember_error(int error) {
    if (error && !peripheral_error) {
        peripheral_error = error < 0 ? error : -EIO;
    }
}

void backend_stop() {
    // PA shutdown is independent of both audio queues and the SDK worker.
    if (radio_ready) {
        remember_error(bk4819_disable());
    }
    if (io_ready) {
        remember_error(c62_apc(0));
        remember_error(c62_speaker(false));
        remember_error(c62_receive_led(false));
    }
    if (radio_ready) {
        remember_error(bk4819_af(false));
    }
    audio_cancel();
    m17_cancel();
    receiving = transmitting = audible = digital_activity = false;
    rx_registers = {};
    monitor = false;
}

static int finish(int error) {
    if (error) {
        if (error != -ECANCELED) {
            remember_error(error);
        }
        backend_stop();
        if (error == -ECANCELED && peripheral_error) {
            return peripheral_error;
        }
    }
    return error;
}

static int health() {
    if (peripheral_error)
        return peripheral_error;
    const int pending = radio_pending_fault();
    if (pending)
        return pending;
    const int error = audio_status().error;
    return error ? error : m17_status().error;
}

int backend_init() {
    backend_stop();
    if (peripheral_error) {
        return peripheral_error;
    }
    configured = radio_ready = false;
    int error = c62_rf_io_init();
    io_ready = error == 0;
    if (!error) {
        error = bk4819_initialize();
        radio_ready = error == 0;
    }
    if (!error) {
        error = audio_start();
    }
    return finish(error);
}

static bk4819_tone driver_tone(const Tone &tone) {
    const auto kind = tone.kind == ToneKind::Ctcss ? BK4819_TONE_CTCSS
                      : tone.kind == ToneKind::Dcs ? BK4819_TONE_DCS
                                                   : BK4819_TONE_NONE;
    return {kind, tone.value, tone.inverted};
}

int backend_configure(const RadioConfig &config, const FmRxControls &controls) {
    int error = validate_config(config);
    if (!error && !valid_fm_rx_controls(controls)) {
        error = -EINVAL;
    }
    if (error) {
        return error; // Validation cannot disturb the current path.
    }
    if (!radio_ready) {
        return -ENODEV;
    }
    backend_stop();
    if (peripheral_error) {
        return peripheral_error;
    }
    const bk4819_config radio{config.rx_frequency_hz,      config.tx_frequency_hz,
                              config.tx_inhibit,           driver_tone(config.rx_tone),
                              driver_tone(config.tx_tone), config.bandwidth == Bandwidth::Wide,
                              config.mode == Mode::M17,    controls.weak_filter,
                              controls.af_dac_gain};
    error = bk4819_configure(&radio);
    configured = error == 0;
    if (!error) {
        configuration = config;
        squelch.configure(config);
    }
    return finish(error);
}

int backend_receive() {
    if (!configured) {
        return -ENODEV;
    }
    backend_stop();
    int error = health();
    if (!error)
        error = bk4819_receive();
    if (!error && configuration.mode == Mode::M17) {
        AudioRoutes routes;
        routes.input_rate[1] = 24000;
        routes.output[0] = AudioSource::Buffer;
        routes.output_rate[0] = 8000;
        AudioSession session;
        error = audio_route(routes, session);
        if (!error)
            error = bk4819_af(true); // Baseband remains open while searching for M17 sync.
        if (!error)
            error = m17_receive(session, configuration.callsign, configuration.m17);
        if (!error)
            error = health();
        if (!error)
            // PCM is gated by validated M17 frames; idle playback is silence.
            // Keep the amp on so activity loss cannot truncate queued speech.
            error = c62_speaker(true);
    }
    if (!error) {
        squelch.configure(configuration);
        receiving = true;
    }
    return finish(error);
}

int backend_transmit(int64_t &started_ms) {
    if (configured && configuration.tx_inhibit) {
        return -EPERM;
    }
    if (!configured) {
        return -ENODEV;
    }
    backend_stop();
    int error = health();
    if (!error && !radio_ptt_requested()) {
        error = -ECANCELED;
    }
    if (!error) {
        AudioRoutes routes;
        if (configuration.mode == Mode::M17) {
            routes.input_rate[0] = 8000;
            routes.output[1] = AudioSource::Buffer;
            routes.output_rate[1] = 48000;
        } else {
            routes.output[1] = AudioSource::Microphone;
        }
        AudioSession session;
        error = audio_route(routes, session);
        if (!error && !radio_ptt_requested())
            error = -ECANCELED;
        if (!error && configuration.mode == Mode::M17)
            error = m17_prepare_transmit(session, configuration.callsign, configuration.m17);
    }
    if (!error && !radio_ptt_requested()) {
        error = -ECANCELED;
    }
    // Provisional reference calibration: <=1 W, <=2.5 W, <=5 W.
    static constexpr uint8_t duty[2][3] = {{25, 40, 60}, {45, 55, 68}};
    const unsigned band = configuration.tx_frequency_hz <= 174000000 ? 0 : 1;
    const unsigned level = configuration.power_mw <= 1000   ? 0
                           : configuration.power_mw <= 2500 ? 1
                                                            : 2;
    if (!error) {
        error = c62_apc(duty[band][level]);
    }
    if (!error) {
        error = bk4819_transmit();
        if (!error) {
            started_ms = k_uptime_get();
        }
    }
    if (!error && !radio_ptt_requested()) {
        error = -ECANCELED;
    }
    if (!error && configuration.mode == Mode::M17)
        error = m17_begin_transmit();
    if (!error) {
        transmitting = true;
    }
    return finish(error);
}

int backend_monitor(bool enabled) {
    if (!receiving) {
        return -EBUSY;
    }
    if (configuration.mode != Mode::Fm) {
        return -ENOTSUP;
    }
    monitor = enabled;
    return 0; // The controller polls to apply the owned audio gate.
}

int backend_finish_transmit(int64_t limit_deadline_ms) {
    if (!transmitting || configuration.mode != Mode::M17)
        return 0;
    int error = m17_finish_transmit();
    const int64_t deadline = k_uptime_get() + 1500;
    while (!error) {
        error = health();
        if (error)
            break;
        if (!radio_power_requested()) {
            return -ECANCELED;
        }
#ifdef CONFIG_HT_COMPANION
        // Session cancellation can shorten the lease during an in-flight drain.
        if (ht_companion_enabled() && k_uptime_get() >= radio_remote_ptt_deadline()) {
            return -ETIME;
        }
#endif
        if (limit_deadline_ms && k_uptime_get() >= limit_deadline_ms) {
            return -ETIME; // Normal limit expiration, never a DSP/audio fault.
        }
        const auto state = m17_status();
        if (state.phase == M17Phase::Finished)
            return 0;
        if (state.phase != M17Phase::Transmitting) {
            error = -EPIPE;
            break;
        }
        if (k_uptime_get() >= deadline) {
            error = -ETIMEDOUT;
            break;
        }
        k_sleep(K_MSEC(5));
    }
    return finish(error);
}

static int receive_audio(bool enabled) {
    int error = c62_speaker(false);
    if (!error) {
        error = c62_receive_led(false);
    }
    if (!error) {
        error = bk4819_af(false);
    }
    audio_cancel();
    if (!error && enabled) {
        AudioRoutes routes;
        routes.output[0] = AudioSource::Radio;
        AudioSession session;
        error = audio_route(routes, session);
        if (!error) {
            error = bk4819_af(true);
        }
        if (!error) {
            error = c62_speaker(true);
        }
        if (!error) {
            error = c62_receive_led(true);
        }
    }
    if (!error) {
        audible = enabled;
    }
    return finish(error);
}

BackendStatus backend_status() {
    BackendStatus status;
    status.error = health();
    if (!status.error && receiving) {
        status.error = bk4819_rssi(&status.rssi_dbm);
        bool tone = false;
        if (!status.error && configuration.mode == Mode::Fm &&
            configuration.rx_tone.kind != ToneKind::None) {
            status.error = bk4819_tone_detected(&tone);
        }
        if (!status.error) {
            const auto digital = m17_status();
            if (configuration.mode == Mode::M17) {
                status.m17_quality = digital.quality;
            }
            const bool open = configuration.mode == Mode::M17
                                  ? digital.rx_active
                                  : (squelch.update(status.rssi_dbm, tone) || monitor);
            if (configuration.mode == Mode::M17) {
                if (open != digital_activity) {
                    status.error = c62_receive_led(open);
                    if (!status.error)
                        digital_activity = open;
                }
            } else if (open != audible) {
                status.error = receive_audio(open);
            }
            status.rx_active = !status.error && open;
            if (status.rx_active && configuration.mode == Mode::M17)
                memcpy(status.callsign, digital.callsign, sizeof(status.callsign));
        }
    }
    if (!status.error && receiving) {
        // Keep GPIO bus traffic out of the 10 ms hot path between samples.
        // The radio owner reads a fixed safe set; UI never accesses the bus.
        if (!rx_registers.valid || k_uptime_get() - rx_registers.sample_ms >= 250) {
            Bk4819RxStatus sample;
            for (unsigned i = 0; i < sizeof(bk4819_rx_addresses); ++i) {
                status.error = bk4819_read(bk4819_rx_addresses[i], &sample.values[i]);
                if (status.error) {
                    break;
                }
            }
            if (!status.error) {
                sample.sample_ms = k_uptime_get();
                sample.valid = true;
                rx_registers = sample;
            }
        }
        if (!status.error) {
            status.rx_registers = rx_registers;
        }
    }
    finish(status.error);
    return status;
}

int backend_read_register(uint8_t address, uint16_t &value) {
    if (receiving || transmitting) {
        return -EBUSY;
    }
    if (address > 0x7f) {
        return -EINVAL;
    }
    return finish(bk4819_read(address, &value));
}

int backend_write_register(uint8_t address, uint16_t value) {
    if (receiving || transmitting) {
        return -EBUSY;
    }
    if (address > 0x7f) {
        return -EINVAL;
    }
    if (address == 0x33 && (value & 0x001c)) {
        return -EPERM; // Diagnostics may not enable external PAs or the TX LED.
    }
    int error = bk4819_write(address, value);
    if (address == 0) {
        // A diagnostic chip reset must also leave external PA outputs inactive.
        const int unkey_error = bk4819_write(0x33, 0);
        if (!error) {
            error = unkey_error;
        }
    }
    return finish(error);
}
} // namespace ht
