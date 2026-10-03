// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../m17/vectors/golden.hpp"
#include "../../m17_demodulation/vectors/golden.hpp"
#include "bk4819.h"
#include "rf_io.h"
#include <errno.h>
#include <ht/audio.hpp>
#include <ht/audio_backend.h>
#include <ht/backend.hpp>
#include <ht/companion.h>
#include <ht/fm.hpp>
#include <ht/m17_mode.hpp>
#include <ht/m17_modem.hpp>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

using namespace ht;
static RadioController controller;
static uint16_t registers[128];
static uint8_t duty;
static bool speaker;
static bool green;
static bool fail_power;
static bool fail_speaker;
static bool fail_keyed_write;
static atomic_t audio_running;
static atomic_t stall_audio_stop;
static atomic_t delay_route;
static atomic_t ptt_released;
static atomic_t keyed_after_release;
static atomic_t pa_keyed;
static atomic_t pa_keys;
static atomic_t pa_unkeyed_at;
static atomic_t tail_calls;
static atomic_t fail_tail;
static atomic_t stall_tail;
static atomic_t drop_capture;
static bool check_m17_keying;
static K_SEM_DEFINE(tail_entered, 0, 1);
static K_SEM_DEFINE(tail_block, 0, 1);
static K_MUTEX_DEFINE(io_mutex);
static bool feed_rx;
static const int16_t *rx_samples = reference_adc;
static int16_t final_voice_adc[4800];
static size_t input_position, radio_nonzero, speaker_nonzero;
static int16_t radio_samples[48000];
static size_t radio_count;
static int16_t stereo_input[960], stereo_output[960];
static int64_t next_poll;
static K_SEM_DEFINE(audio_stop_block, 0, 1);
static K_SEM_DEFINE(route_preparing, 0, 1);
static struct k_thread release_thread;
K_THREAD_STACK_DEFINE(release_stack, 1024);

extern "C" int c62_rf_io_init(void) {
    duty = 0;
    speaker = green = false;
    return 0;
}

extern "C" int c62_apc(uint8_t value) {
    if (value && fail_power) {
        return -EIO;
    }
    duty = value;
    return 0;
}

extern "C" int c62_speaker(bool enabled) {
    if (enabled && fail_speaker) {
        return -EIO;
    }
    if (enabled) {
        zassert_equal(atomic_get(&audio_running), 3, "speaker enabled before audio readiness");
    }
    speaker = enabled;
    return 0;
}

extern "C" int c62_receive_led(bool enabled) {
    green = enabled;
    return 0;
}

extern "C" int bk4819_bus_init(void) {
    memset(registers, 0, sizeof(registers));
    return 0;
}

extern "C" int bk4819_read(uint8_t address, uint16_t *value) {
    *value = registers[address];
    return 0;
}

extern "C" int bk4819_write(uint8_t address, uint16_t value) {
    if (address == 0x33 && (value & 0x18)) {
        if (atomic_get(&ptt_released)) {
            atomic_inc(&keyed_after_release);
        }
        zassert_true(duty != 0, "PA keyed before APC preparation");
        zassert_equal(atomic_get(&audio_running), 3, "PA keyed before audio readiness");
        if (check_m17_keying)
            zassert_equal(m17_status().phase, M17Phase::Prepared,
                          "M17 samples started before RF keying completed");
    }
    if (fail_keyed_write && address == 0x30 && value == 0xc1fe) {
        zassert_not_equal(registers[0x33] & 0x18, 0);
        fail_keyed_write = false;
        return -EIO;
    }
    registers[address] = value;
    if (address == 0x33) {
        if ((value & 0x18) && !atomic_get(&pa_keyed))
            atomic_inc(&pa_keys);
        if (!(value & 0x18) && atomic_get(&pa_keyed)) {
            atomic_set(&pa_unkeyed_at, k_uptime_get());
        }
        atomic_set(&pa_keyed, (value & 0x18) != 0);
    }
    return 0;
}

extern "C" int ht_audio_backend_open(void) {
    return 0;
}

extern "C" int ht_audio_backend_set_running(bool capture, bool playback) {
    if (!capture && !playback && atomic_get(&stall_audio_stop)) {
        k_sem_take(&audio_stop_block, K_FOREVER);
    }
    if (capture && playback && atomic_get(&delay_route)) {
        k_sem_give(&route_preparing);
        k_sleep(K_MSEC(50));
    }
    atomic_set(&audio_running, (capture ? 1 : 0) | (playback ? 2 : 0));
    k_mutex_lock(&io_mutex, K_FOREVER);
    next_poll = k_uptime_get() + 10;
    k_mutex_unlock(&io_mutex);
    return 0;
}

extern "C" int ht_audio_backend_poll(uint32_t session) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    if (k_uptime_get() < next_poll) {
        k_mutex_unlock(&io_mutex);
        return 0;
    }
    next_poll += 10;
    int error = 0;
    const auto running = atomic_get(&audio_running);
    if ((running & 1) && !atomic_get(&drop_capture)) {
        memset(stereo_input, 0, sizeof(stereo_input));
        for (unsigned i = 0; i < 480; ++i) {
            if (feed_rx && input_position / 2 < 4800)
                stereo_input[2 * i + 1] = rx_samples[input_position / 2];
            ++input_position;
        }
        error = ht_audio_capture(session, stereo_input, 480, 2, 0, 1);
    }
    if (!error && (running & 2)) {
        error = ht_audio_playback(session, stereo_output, 480);
        if (!error)
            for (unsigned i = 0; i < 480; ++i) {
                radio_nonzero += stereo_output[2 * i + 1] != 0;
                if (radio_count < 48000) {
                    radio_samples[radio_count++] = stereo_output[2 * i + 1];
                }
                speaker_nonzero += stereo_output[2 * i] != 0;
            }
    }
    k_mutex_unlock(&io_mutex);
    return error;
}

extern "C" int ht_audio_backend_tail_ms(uint32_t *delay_ms) {
    zassert_true(atomic_get(&pa_keyed), "RF unkeyed before M17 tail completed");
    atomic_inc(&tail_calls);
    k_sem_give(&tail_entered);
    if (atomic_get(&stall_tail))
        k_sem_take(&tail_block, K_FOREVER);
    *delay_ms = 30;
    return atomic_get(&fail_tail) ? -EIO : 0;
}

static void before(void *) {
    radio_ptt(false);
    atomic_clear(&delay_route);
    atomic_clear(&ptt_released);
    atomic_clear(&keyed_after_release);
    k_mutex_lock(&io_mutex, K_FOREVER);
    feed_rx = false;
    rx_samples = reference_adc;
    input_position = radio_nonzero = speaker_nonzero = radio_count = 0;
    k_mutex_unlock(&io_mutex);
    atomic_clear(&tail_calls);
    atomic_clear(&pa_keys);
    check_m17_keying = false;
    zassert_ok(controller.start({}));
}

static void set_ptt(bool pressed) {
    radio_ptt(pressed);
    controller.set_ptt(pressed);
}

static void rssi(int dbm) {
    registers[0x67] = (dbm + 160) * 2;
}

static void stopped() {
    zassert_equal(registers[0x33] & 0x18, 0);
    zassert_equal(duty, 0);
    zassert_false(speaker);
    zassert_false(green);
}

static void configure(const RadioConfig &config) {
    RadioCommand command;
    command.config = config;
    zassert_ok(controller.execute(command));
}

ZTEST(c62_radio, test_receive_audio_follows_reference_squelch_hysteresis) {
    stopped();
    zassert_equal(controller.state().phase, RadioPhase::Receiving);
    // Level4 threshold = -110 dBm, strict opening >-109, closing <-111.
    rssi(-109);
    controller.poll();
    zassert_false(controller.state().rx_active);
    rssi(-108);
    controller.poll();
    zassert_true(controller.state().rx_active);
    zassert_true(speaker && green);
    zassert_equal(registers[0x47] & 0x0f00, 0x0100);
    rssi(-111);
    controller.poll();
    zassert_true(controller.state().rx_active);
    rssi(-112);
    controller.poll();
    zassert_false(controller.state().rx_active);
    stopped();
    zassert_equal(registers[0x47] & 0x0f00, 0);
}

ZTEST(c62_radio, test_tone_squelch_uses_detector_and_does_not_require_rf_threshold) {
    RadioConfig config;
    config.rx_tone = {ToneKind::Ctcss, 885, false};
    configure(config);
    rssi(-130);
    registers[0x0c] = 1U << 10;
    controller.poll();
    zassert_true(controller.state().rx_active);
    zassert_true(speaker);
    registers[0x0c] = 0;
    rssi(-70);
    controller.poll();
    zassert_false(controller.state().rx_active);
    stopped();
}

ZTEST(c62_radio, test_dcs_polarity_squelch_monitor_and_rx_restore_after_tx) {
    RadioConfig config;
    config.rx_tone = {ToneKind::Dcs, 0023, true};
    config.tx_tone = {ToneKind::Ctcss, 885, false};
    configure(config);
    rssi(-70);
    registers[0x0c] = 1U << 14;
    controller.poll();
    zassert_false(speaker); // Wrong polarity despite strong RF.
    controller.set_monitor(true);
    zassert_true(speaker && green);
    zassert_equal(controller.state().config.rx_tone.kind, ToneKind::Dcs);
    controller.set_monitor(false);
    zassert_false(speaker);
    registers[0x0c] = 1U << 15;
    rssi(-130);
    controller.poll();
    zassert_true(speaker && green);
    set_ptt(true);
    zassert_equal(controller.state().phase, RadioPhase::Transmitting);
    zassert_equal(registers[0x51] & 0xfc00, 0x9000);
    set_ptt(false);
    zassert_equal(controller.state().phase, RadioPhase::Receiving);
    zassert_equal(registers[0x51] & 0xfc00, 0);
    registers[0x0c] = 1U << 10;
    controller.poll();
    zassert_false(speaker);
    registers[0x0c] = 1U << 15;
    controller.poll();
    zassert_true(speaker);
    config.rx_tone = {ToneKind::Ctcss, 1000, false};
    config.tx_tone = {ToneKind::Dcs, 0754, true};
    configure(config);
    set_ptt(true);
    zassert_equal(registers[0x51] & 0xfc00, 0xa000);
    set_ptt(false);
    zassert_equal(registers[0x51] & 0xfc00, 0x1000);
    registers[0x0c] = 1U << 10;
    controller.poll();
    zassert_true(speaker);
}

ZTEST(c62_radio, test_monitor_bypasses_rssi_and_tone_without_changing_configuration) {
    RadioConfig config;
    config.squelch = 15;
    configure(config);
    rssi(-125);
    controller.poll();
    zassert_false(speaker);
    controller.set_monitor(true);
    zassert_true(speaker && green); // RSSI-only squelch is bypassed too.
    controller.set_monitor(false);
    zassert_false(speaker);
    config.rx_tone = {ToneKind::Ctcss, 1000, false};
    configure(config);
    rssi(-125);
    registers[0x0c] = 0;
    controller.poll();
    zassert_false(speaker);
    controller.set_monitor(true);
    zassert_true(controller.state().monitor_active);
    zassert_true(controller.state().rx_active);
    zassert_true(speaker && green);
    zassert_equal(registers[0x47] & 0x0f00, 0x0100);
    zassert_false(atomic_get(&pa_keyed));
    zassert_equal(duty, 0);
    zassert_equal(controller.state().config.squelch, 15);
    zassert_equal(controller.state().config.rx_tone.value, 1000);
    controller.set_monitor(false);
    zassert_false(speaker);
    zassert_false(controller.state().rx_active);
    controller.set_monitor(true);
    configure(config); // Even same-mode retuning consumes the monitor hold.
    controller.poll();
    zassert_false(controller.state().monitor_active);
    zassert_false(speaker);
    controller.set_monitor(true);
    zassert_false(speaker);
    controller.set_monitor(false);
    registers[0x0c] = 1U << 10;
    controller.poll();
    zassert_true(speaker); // Release restored normal tone gating, not a forced mute.
    controller.set_monitor(true);
    set_ptt(true);
    zassert_false(controller.state().monitor_active);
    zassert_false(speaker);
    set_ptt(false);
    controller.set_monitor(true);
    zassert_false(controller.state().monitor_active);
    config = {};
    config.mode = Mode::M17;
    strcpy(config.callsign, "OE3ANC");
    configure(config);
    controller.set_monitor(false);
    controller.set_monitor(true);
    zassert_false(controller.state().monitor_active);
    zassert_equal(backend_monitor(true), -ENOTSUP);
    zassert_equal(m17_status().phase, M17Phase::Receiving);
}

static void release_ptt(void *, void *, void *) {
    zassert_ok(k_sem_take(&route_preparing, K_MSEC(100)));
    atomic_set(&ptt_released, 1);
    radio_ptt(false);
}

ZTEST(c62_radio, test_release_during_audio_preparation_never_keys_and_does_not_fault) {
    zassert_ok(radio_start({}));
    k_sem_reset(&route_preparing);
    atomic_set(&delay_route, 1);
    radio_ptt(true);
    k_thread_create(&release_thread, release_stack, K_THREAD_STACK_SIZEOF(release_stack),
                    release_ptt, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
    radio_service();
    zassert_ok(k_thread_join(&release_thread, K_MSEC(100)));
    zassert_true(atomic_get(&ptt_released));
    zassert_equal(atomic_get(&keyed_after_release), 0);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_ok(radio_snapshot().fault);
    stopped();
    atomic_clear(&delay_route);
    atomic_clear(&ptt_released);
    radio_service(); // Consume release; a fresh press can transmit normally.
    radio_ptt(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Transmitting);
    radio_ptt(false);
    radio_service();
    stopped();
}

ZTEST(c62_radio, test_power_duties_and_receive_after_ptt_release) {
    const uint8_t expected[2][3] = {{25, 40, 60}, {45, 55, 68}};
    const uint32_t power[] = {1000, 2500, 5000};
    for (unsigned band = 0; band < 2; ++band) {
        for (unsigned level = 0; level < 3; ++level) {
            RadioConfig config;
            config.rx_frequency_hz = config.tx_frequency_hz = band ? 400000000 : 174000000;
            config.power_mw = power[level];
            config.tx_tone = {ToneKind::Ctcss, 2541, false};
            configure(config);
            set_ptt(true);
            zassert_equal(controller.state().phase, RadioPhase::Transmitting);
            zassert_equal(duty, expected[band][level]);
            zassert_equal(registers[0x33], band ? 0x000c : 0x0014);
            zassert_equal(registers[0x07], 5246);
            zassert_false(speaker);
            set_ptt(false);
            zassert_equal(controller.state().phase, RadioPhase::Receiving);
            stopped();
            zassert_equal(registers[0x33], band ? 0x20 : 0x40);
        }
    }
}

ZTEST(c62_radio, test_configuration_is_copied_and_invalid_callsign_is_rejected) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145000000;
    configure(config);
    config.rx_frequency_hz = config.tx_frequency_hz = 460000000;
    set_ptt(true);
    zassert_equal(registers[0x39], (145000000 / 10) >> 16);
    set_ptt(false);
    config.mode = Mode::M17;
    strcpy(config.callsign, "lower");
    RadioCommand command;
    command.config = config;
    zassert_equal(controller.execute(command), -EINVAL);
    zassert_equal(controller.state().config.rx_frequency_hz, 145000000);
    zassert_equal(controller.state().phase, RadioPhase::Receiving);
}

ZTEST(c62_radio, test_diagnostics_exclude_pa_writes_and_restore_normal_configuration) {
    RadioCommand command;
    command.kind = CommandKind::EnterDiagnostics;
    zassert_ok(controller.execute(command));
    stopped();
    set_ptt(true);
    stopped();
    set_ptt(false);
    command.kind = CommandKind::WriteRegister;
    command.register_address = 0x33;
    command.register_value = 0x0018;
    zassert_equal(controller.execute(command), -EPERM);
    stopped();
    command.register_address = 0x43;
    command.register_value = 0xabcd;
    zassert_ok(controller.execute(command));
    zassert_equal(registers[0x43], 0xabcd);
    command.kind = CommandKind::ExitDiagnostics;
    zassert_ok(controller.execute(command));
    zassert_equal(controller.state().phase, RadioPhase::Receiving);
    zassert_not_equal(registers[0x43], 0xabcd);
    stopped();
}

static RadioConfig digital_config() {
    RadioConfig config;
    config.mode = Mode::M17;
    strcpy(config.callsign, "OE3ANC");
    return config;
}

ZTEST(c62_radio, test_m17_rx_callsign_audio_gate_and_fm_restoration) {
    configure(digital_config());
    zassert_true(backend_capabilities().m17);
    zassert_equal(m17_status().phase, M17Phase::Receiving);
    zassert_equal(registers[0x47] & 0x0f00, 0x0900);
    zassert_true(speaker);
    k_mutex_lock(&io_mutex, K_FOREVER);
    feed_rx = true;
    input_position = 0;
    k_mutex_unlock(&io_mutex);
    bool active = false;
    const int64_t deadline = k_uptime_get() + 1500;
    while (k_uptime_get() < deadline) {
        controller.poll();
        zassert_ok(controller.state().fault);
        if (controller.state().rx_active) {
            active = true;
            zassert_true(speaker && green);
            zassert_equal(strcmp(controller.state().received_callsign, "OE3ANC"), 0);
        }
        k_sleep(K_MSEC(5));
    }
    zassert_true(active);
    zassert_false(controller.state().rx_active);
    zassert_true(speaker); // Queued voice/DSP latency must survive activity loss.
    zassert_false(green);
    zassert_equal(controller.state().received_callsign[0], '\0');
    k_mutex_lock(&io_mutex, K_FOREVER);
    zassert_true(speaker_nonzero > 0);
    feed_rx = false;
    k_mutex_unlock(&io_mutex);
    configure({});
    zassert_equal(m17_status().phase, M17Phase::Idle);
    rssi(-70);
    controller.poll();
    zassert_true(speaker && green);
    zassert_equal(registers[0x47] & 0x0f00, 0x0100);
}

ZTEST(c62_radio, test_m17_final_voice_keeps_speaker_on_after_activity_ends) {
    m17::Modulator modulator;
    m17::TxSamples samples;
    m17::Frame frame;
    for (unsigned block = 0; block < 4; ++block) {
        if (block < 2)
            m17::preamble(frame);
        else
            memcpy(frame.bytes, block == 2 ? lsf_frame : stream_5, 48);
        modulator.render(frame, samples);
        for (unsigned i = 0; i < 960; ++i)
            final_voice_adc[block * 960 + i] = samples.samples[2 * i] / 4;
    }
    memset(final_voice_adc + 3840, 0, 960 * sizeof(int16_t)); // EOT is deliberately absent.
    configure(digital_config());
    k_mutex_lock(&io_mutex, K_FOREVER);
    feed_rx = true;
    rx_samples = final_voice_adc;
    input_position = 0;
    k_mutex_unlock(&io_mutex);
    bool active = false;
    const int64_t deadline = k_uptime_get() + 1500;
    while (k_uptime_get() < deadline) {
        controller.poll();
        zassert_ok(controller.state().fault);
        active |= controller.state().rx_active;
        zassert_true(speaker, "RX activity must not mute queued final speech");
        k_sleep(K_MSEC(5));
    }
    zassert_true(active);
    zassert_false(controller.state().rx_active);
    zassert_false(green);
    k_mutex_lock(&io_mutex, K_FOREVER);
    zassert_true(speaker_nonzero > 0);
    k_mutex_unlock(&io_mutex);
    backend_stop();
    stopped();
}

ZTEST(c62_radio, test_m17_tx_preparation_drain_and_repeated_ptt) {
    configure(digital_config());
    check_m17_keying = true;
    for (unsigned i = 0; i < 4; ++i) {
        set_ptt(true);
        zassert_equal(controller.state().phase, RadioPhase::Transmitting);
        zassert_true(atomic_get(&pa_keyed));
        zassert_false(speaker);
        RadioCommand command;
        command.kind = CommandKind::Configure;
        zassert_equal(controller.execute(command), -EBUSY);
        k_sleep(K_MSEC(80));
        set_ptt(false);
        zassert_equal(controller.state().phase, RadioPhase::Receiving);
        zassert_false(atomic_get(&pa_keyed));
        zassert_equal(duty, 0);
        zassert_true(speaker);
        zassert_false(green);
        zassert_equal(m17_status().phase, M17Phase::Receiving);
        zassert_equal(atomic_get(&tail_calls), i + 1);
    }
    k_mutex_lock(&io_mutex, K_FOREVER);
    zassert_true(radio_nonzero > 0);
    k_mutex_unlock(&io_mutex);
}

ZTEST(c62_radio, test_m17_unset_callsign_and_diagnostics_remain_unkeyed) {
    RadioConfig config;
    config.mode = Mode::M17;
    configure(config);
    set_ptt(true);
    zassert_equal(controller.state().ptt_error, -EADDRNOTAVAIL);
    zassert_false(atomic_get(&pa_keyed));
    zassert_true(speaker);
    set_ptt(false);
    RadioCommand command;
    command.kind = CommandKind::EnterDiagnostics;
    zassert_ok(controller.execute(command));
    zassert_equal(m17_status().phase, M17Phase::Idle);
    set_ptt(true);
    stopped();
    set_ptt(false);
    command.kind = CommandKind::ExitDiagnostics;
    zassert_ok(controller.execute(command));
    zassert_equal(m17_status().phase, M17Phase::Receiving);
    zassert_false(atomic_get(&pa_keyed));
    zassert_true(speaker);
}

ZTEST(c62_radio, test_m17_release_repress_survives_full_command_queue) {
    zassert_ok(radio_start(digital_config()));
    radio_ptt(true);
    radio_service();
    zassert_equal(atomic_get(&pa_keys), 1);
    RadioCommand command;
    for (unsigned i = 0; i < 8; ++i)
        zassert_ok(radio_submit(command));
    zassert_not_equal(radio_submit(command), 0);
    radio_ptt(false);
    radio_ptt(true);
    radio_service();
    // Release must finish and unkey before the new press starts another stream.
    zassert_equal(atomic_get(&tail_calls), 1);
    zassert_equal(atomic_get(&pa_keys), 2);
    zassert_equal(radio_snapshot().phase, RadioPhase::Transmitting);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    radio_ptt(false);
    radio_service();
    stopped();
    zassert_equal(atomic_get(&tail_calls), 2);
    zassert_ok(radio_start({})); // Clear the remaining commands for other tests.
}

static void fault_during_tail(void *, void *, void *) {
    zassert_ok(k_sem_take(&tail_entered, K_MSEC(1000)));
    radio_report_fault(-EPIPE);
}

ZTEST(c62_radio, test_m17_release_during_preparation_remains_unkeyed) {
    zassert_ok(radio_start(digital_config()));
    k_sem_reset(&route_preparing);
    atomic_set(&delay_route, 1);
    radio_ptt(true);
    k_thread_create(&release_thread, release_stack, K_THREAD_STACK_SIZEOF(release_stack),
                    release_ptt, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
    radio_service();
    zassert_ok(k_thread_join(&release_thread, K_MSEC(100)));
    zassert_true(atomic_get(&ptt_released));
    zassert_equal(atomic_get(&keyed_after_release), 0);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_ok(radio_snapshot().fault);
    zassert_false(atomic_get(&pa_keyed));
    zassert_true(speaker);
    atomic_clear(&delay_route);
    atomic_clear(&ptt_released);
    radio_service();
    zassert_ok(radio_start({}));
}

ZTEST(c62_radio, test_z_failures_stop_tx_and_latch_until_reboot) {
    const char *fault = getenv("HT_C62_FAULT");
    if (fault && (!strcmp(fault, "m17-tail-error") || !strcmp(fault, "m17-tail-stall"))) {
        configure(digital_config());
        set_ptt(true);
        k_sem_reset(&tail_entered);
        if (!strcmp(fault, "m17-tail-stall")) {
            atomic_set(&stall_tail, 1);
            k_thread_create(&release_thread, release_stack, K_THREAD_STACK_SIZEOF(release_stack),
                            fault_during_tail, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
        } else {
            atomic_set(&fail_tail, 1);
        }
        set_ptt(false);
        if (!strcmp(fault, "m17-tail-stall")) {
            zassert_ok(k_thread_join(&release_thread, K_MSEC(100)));
            zassert_equal(controller.state().fault, -EPIPE);
            // The SDK is still blocked, but the PA is already off.
            stopped();
            atomic_clear(&stall_tail);
            k_sem_give(&tail_block);
        } else {
            zassert_equal(controller.state().fault, -EIO);
        }
    } else if (fault && !strcmp(fault, "m17-worker")) {
        configure(digital_config());
        set_ptt(true);
        atomic_set(&drop_capture, 1);
        for (unsigned i = 0; i < 100 && controller.state().phase != RadioPhase::Fault; ++i) {
            controller.poll();
            k_sleep(K_MSEC(10));
        }
        zassert_equal(controller.state().fault, -ETIMEDOUT);
    } else if (fault && strcmp(fault, "power") == 0) {
        fail_power = true;
        set_ptt(true);
    } else if (fault && strcmp(fault, "bus") == 0) {
        fail_keyed_write = true;
        set_ptt(true);
    } else if (fault && strcmp(fault, "monitor") == 0) {
        fail_speaker = true;
        controller.set_monitor(true);
        zassert_false(controller.state().monitor_active);
    } else if (fault && strcmp(fault, "speaker") == 0) {
        fail_speaker = true;
        rssi(-70);
        controller.poll();
    } else {
        set_ptt(true);
        zassert_equal(controller.state().phase, RadioPhase::Transmitting);
        atomic_set(&stall_audio_stop, 1);
        audio_report_error(-EPIPE);
        controller.poll(); // Must unkey even though DSP stop cannot complete.
    }
    zassert_equal(controller.state().phase, RadioPhase::Fault);
    zassert_not_equal(controller.state().fault, 0);
    stopped();
    set_ptt(false);
    set_ptt(true);
    stopped();
    zassert_equal(controller.state().phase, RadioPhase::Fault);
    zassert_not_equal(backend_init(), 0);
    atomic_set(&stall_audio_stop, 0);
    k_sem_give(&audio_stop_block);
}

ZTEST(c62_radio, test_split_frequency_uses_tx_band_for_apc_and_inhibit) {
    RadioConfig config;
    config.rx_frequency_hz = 439075000;
    config.tx_frequency_hz = 145125000;
    configure(config);
    set_ptt(true);
    zassert_equal(controller.state().phase, RadioPhase::Transmitting);
    zassert_equal(duty, 25);
    zassert_equal(registers[0x33], 0x0014);
    zassert_equal(((uint32_t)registers[0x39] << 16) | registers[0x38], 14512500);
    set_ptt(false);
    stopped();
    zassert_equal(registers[0x33], 0x0020);
    zassert_equal(((uint32_t)registers[0x39] << 16) | registers[0x38], 43907500);
    config.tx_inhibit = true;
    configure(config);
    set_ptt(true);
    zassert_equal(controller.state().ptt_error, -EPERM);
    zassert_equal(controller.state().phase, RadioPhase::Receiving);
    stopped();
    set_ptt(false);
}

ZTEST(c62_radio, test_transmit_timeout_unkeys_fm_and_m17_without_tail) {
    const Mode modes[] = {Mode::Fm, Mode::M17};
    for (Mode mode : modes) {
        auto config = digital_config();
        config.mode = mode;
        config.transmit_limit_s = 60;
        configure(config);
        set_ptt(true);
        zassert_true(atomic_get(&pa_keyed));
        zassert_equal(controller.state().tx_remaining_s, 60);
        k_sleep(K_SECONDS(60));
        controller.poll();
        zassert_equal(controller.state().phase, RadioPhase::Receiving);
        zassert_true(controller.state().tx_timed_out);
        zassert_ok(controller.state().fault);
        zassert_false(atomic_get(&pa_keyed));
        zassert_equal(duty, 0);
        zassert_equal(atomic_get(&tail_calls), 0, "Timeout must bypass M17 drain");
        set_ptt(true);
        zassert_false(atomic_get(&pa_keyed));
        set_ptt(false);
        set_ptt(true);
        zassert_true(atomic_get(&pa_keyed));
        set_ptt(false);
        // A fresh normal M17 release still uses its ordinary drain.
        if (mode == Mode::M17) {
            zassert_equal(atomic_get(&tail_calls), 1);
        }
    }
}

ZTEST(c62_radio, test_transmit_limit_bounds_m17_release_drain) {
    auto config = digital_config();
    config.transmit_limit_s = 60;
    configure(config);
    set_ptt(true);
    k_sleep(K_MSEC(59980));
    const int64_t release_ms = k_uptime_get();
    set_ptt(false);
    zassert_false(atomic_get(&pa_keyed));
    zassert_equal(duty, 0);
    zassert_equal(controller.state().phase, RadioPhase::Receiving);
    zassert_ok(controller.state().fault);
    zassert_false(controller.state().tx_timed_out); // PTT is already released.
    zassert_true(k_uptime_get() - release_ms < 200, "Do not wait for ordinary TX tail");
    set_ptt(true);
    zassert_true(atomic_get(&pa_keyed));
    set_ptt(false);
}

ZTEST(c62_radio, test_m17_backend_copies_destination_and_can_into_processing) {
    RadioConfig config;
    config.mode = Mode::M17;
    strcpy(config.callsign, "OE3ANC");
    config.m17.destination = Destination::Station;
    strcpy(config.m17.callsign, "OE1TEST");
    config.m17.can = 15;
    config.m17.rx_can_check = true;
    configure(config);
    k_mutex_lock(&io_mutex, K_FOREVER);
    feed_rx = true;
    input_position = 0;
    k_mutex_unlock(&io_mutex);
    // Independent RX waveform is CAN 0; configured CAN 15 must gate it.
    for (unsigned i = 0; i < 60; ++i) {
        controller.poll();
        zassert_false(controller.state().rx_active);
        zassert_equal(controller.state().received_callsign[0], 0);
        k_sleep(K_MSEC(5));
    }
    k_mutex_lock(&io_mutex, K_FOREVER);
    zassert_equal(speaker_nonzero, 0);
    feed_rx = false;
    radio_count = 0;
    k_mutex_unlock(&io_mutex);
    config.m17 = {}; // Caller draft is not retained by backend or worker.
    set_ptt(true);
    zassert_equal(controller.state().phase, RadioPhase::Transmitting);
    k_sleep(K_MSEC(230));
    set_ptt(false);
    zassert_equal(controller.state().phase, RadioPhase::Receiving);
    zassert_equal(registers[0x33] & 0x18, 0);
    zassert_equal(duty, 0);
    zassert_true(speaker); // M17 RX keeps its amp on; accepted PCM owns gating.
    m17::Demodulator demodulator;
    m17::Decoder decoder;
    m17::Frame frame;
    unsigned links = 0;
    k_mutex_lock(&io_mutex, K_FOREVER);
    for (size_t i = 0; i < radio_count; i += 2) {
        if (!demodulator.sample(-radio_samples[i] / 2, frame)) {
            continue;
        }
        const auto decoded = decoder.decode(frame);
        if (decoded.link_updated) {
            zassert_mem_equal(decoded.link.bytes, directed_can_links + 15 * 30, 30);
            ++links;
        }
    }
    k_mutex_unlock(&io_mutex);
    zassert_true(links > 0);
}

static void cycle_power_during_preparation(void *, void *, void *) {
    zassert_ok(k_sem_take(&route_preparing, K_SECONDS(1)));
    radio_power(false);
    radio_power(true); // Even a coalesced edge must cancel before keying.
}

ZTEST(c62_radio, test_power_cycle_during_preparation_never_keys) {
    radio_power(true);
    zassert_ok(radio_start({}));
    k_sem_reset(&route_preparing);
    atomic_set(&delay_route, 1);
    radio_ptt(true);
    k_thread_create(&release_thread, release_stack, K_THREAD_STACK_SIZEOF(release_stack),
                    cycle_power_during_preparation, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
    radio_service();
    zassert_ok(k_thread_join(&release_thread, K_MSEC(100)));
    zassert_equal(atomic_get(&pa_keys), 0);
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    zassert_ok(radio_snapshot().fault);
    stopped();
    atomic_clear(&delay_route);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_equal(atomic_get(&pa_keys), 0); // PTT stayed held through restart.
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Transmitting);
    radio_ptt(false);
    radio_service();
    stopped();
}

static void cycle_power_during_tail(void *, void *, void *) {
    zassert_ok(k_sem_take(&tail_entered, K_SECONDS(1)));
    radio_power(false);
    radio_power(true);
}

ZTEST(c62_radio, test_power_cycle_aborts_m17_drain_before_blocked_tail) {
    radio_power(true);
    zassert_ok(radio_start(digital_config()));
    radio_ptt(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Transmitting);
    k_sem_reset(&tail_entered);
    atomic_set(&stall_tail, 1);
    k_thread_create(&release_thread, release_stack, K_THREAD_STACK_SIZEOF(release_stack),
                    cycle_power_during_tail, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
    radio_ptt(false);
    const int64_t start = k_uptime_get();
    radio_service();
    zassert_ok(k_thread_join(&release_thread, K_MSEC(100)));
    zassert_true(k_uptime_get() - start < 500);
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    zassert_ok(radio_snapshot().fault);
    stopped(); // PA is off while the modeled SDK tail is still parked.
    atomic_clear(&stall_tail);
    k_sem_give(&tail_block);
    k_sleep(K_MSEC(50));
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_ok(radio_snapshot().fault);
    radio_ptt(false);
    radio_service();
}

// The serial worker stays idle while these fixtures own its source session.
extern "C" int __wrap_ht_companion_read(uint8_t *, size_t) {
    return 0;
}

static void remote_start(const RadioConfig &config) {
    radio_power(true);
    zassert_ok(radio_start(config));
    const auto state = radio_snapshot();
    RadioCommand command;
    command.kind = CommandKind::CompanionMode;
    command.expected_generation = state.generation;
    command.expected_revision = state.configuration_revision;
    command.companion_enabled = true;
    zassert_ok(radio_submit(command));
    radio_service();
    zassert_ok(radio_snapshot().command_error);
    k_sleep(K_MSEC(10));
    radio_remote_session(42);
    radio_service();
}

static void cancel_remote_during_preparation(void *, void *, void *) {
    zassert_ok(k_sem_take(&route_preparing, K_SECONDS(1)));
    radio_remote_session(0); // No controller mutex or ordinary queue needed.
}

ZTEST(c62_radio, test_remote_session_cancel_during_preparation_never_keys) {
    remote_start({});
    k_sem_reset(&route_preparing);
    atomic_set(&delay_route, 1);
    zassert_ok(radio_remote_ptt_press(42, 2));
    k_thread_create(&release_thread, release_stack, K_THREAD_STACK_SIZEOF(release_stack),
                    cancel_remote_during_preparation, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
    radio_service();
    zassert_ok(k_thread_join(&release_thread, K_MSEC(100)));
    zassert_equal(atomic_get(&pa_keys), 0);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_ok(radio_snapshot().fault);
    atomic_clear(&delay_route);
    stopped();
}

static void settle_remote_tail(void *cancel, void *, void *) {
    zassert_ok(k_sem_take(&tail_entered, K_SECONDS(1)));
    if (cancel) {
        radio_remote_session(0);
    }
    // Model SDK completion after independent PA stop. Waiting to free this
    // callback until radio_service returns would also park RX preparation.
    const auto deadline = k_uptime_get() + 1000;
    while (atomic_get(&pa_keyed) && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(5));
    }
    atomic_clear(&stall_tail);
    k_sem_give(&tail_block);
}

ZTEST(c62_radio, test_remote_m17_tail_lease_and_session_cancellation) {
    for (unsigned cancel = 0; cancel < 2; ++cancel) {
        remote_start(digital_config());
        zassert_ok(radio_remote_ptt_press(42, 2));
        radio_service();
        zassert_true(atomic_get(&pa_keyed));
        k_sem_reset(&tail_entered);
        atomic_set(&stall_tail, 1);
        k_thread_create(&release_thread, release_stack, K_THREAD_STACK_SIZEOF(release_stack),
                        settle_remote_tail, cancel ? reinterpret_cast<void *>(1) : nullptr, nullptr,
                        nullptr, 7, 0, K_NO_WAIT);
        const auto lease = radio_remote_ptt_deadline();
        zassert_ok(radio_remote_ptt_release(42));
        const auto start = k_uptime_get();
        radio_service();
        zassert_ok(k_thread_join(&release_thread, K_MSEC(100)));
        const auto off_ms = atomic_get(&pa_unkeyed_at);
        zassert_true(off_ms >= start && off_ms <= lease + 10);
        if (cancel) {
            zassert_true(off_ms - start < 500);
        }
        zassert_false(atomic_get(&pa_keyed));
        zassert_equal(duty, 0);
        zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
        zassert_ok(radio_snapshot().fault);
        zassert_equal(radio_remote_ptt_keep(42, 2), -ESTALE);
        atomic_clear(&stall_tail);
        k_sem_give(&tail_block);
        k_sleep(K_MSEC(50));
    }
}

ZTEST_SUITE(c62_radio, nullptr, nullptr, before, nullptr, nullptr);
