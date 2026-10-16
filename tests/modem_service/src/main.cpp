// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/audio_backend.h>
#include <ht/emulator.hpp>
#include <string.h>
#include <math.h>
#include <ht/modem_service.hpp>
#include <errno.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/atomic.h>
using namespace ht;
K_MSGQ_DEFINE(incoming, 1, 2048, 1);
K_MSGQ_DEFINE(outgoing, 1, 2048, 1);
static atomic_t serial_error;
static bool audio_capture, audio_playback;
static int64_t audio_next;
static int16_t played[96000];
static size_t played_count;
static int16_t capture_samples[160000];
static size_t capture_count, capture_position;
static unsigned drains;
static K_MUTEX_DEFINE(io_mutex);
extern "C" int __wrap_ht_companion_read(uint8_t *bytes, size_t size) {
    if (atomic_cas(&serial_error, 1, 0)) {
        return -EIO;
    }
    unsigned count = 0;
    while (count < size && !k_msgq_get(&incoming, bytes + count, K_NO_WAIT)) {
        ++count;
    }
    return count;
}

extern "C" int __wrap_ht_companion_write(const uint8_t *bytes, size_t size, uint32_t) {
    zassert_true(size <= 256);
    for (size_t i = 0; i < size; ++i) {
        zassert_ok(k_msgq_put(&outgoing, bytes + i, K_NO_WAIT));
    }
    return size;
}

extern "C" int ht_audio_backend_open() {
    return 0;
}

extern "C" int ht_audio_backend_set_running(bool input, bool output) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    audio_capture = input;
    audio_playback = output;
    audio_next = k_uptime_get() + 10;
    k_mutex_unlock(&io_mutex);
    return 0;
}

extern "C" int ht_audio_backend_poll(uint32_t session) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    if (k_uptime_get() < audio_next) {
        k_mutex_unlock(&io_mutex);
        return 0;
    }
    audio_next += 10;
    int16_t samples[960] = {};
    for (unsigned i = 0; audio_capture && i < 480 && capture_position < capture_count; ++i) {
        samples[2 * i + 1] = capture_samples[capture_position++];
    }
    int error = audio_capture ? ht_audio_capture(session, samples, 480, 2, 0, 1) : 0;
    if (!error && audio_playback) {
        error = ht_audio_playback(session, samples, 480);
        if (!error) {
            for (unsigned i = 0; i < 480 && played_count < 96000; ++i) {
                played[played_count++] = samples[2 * i + 1];
            }
        }
    }
    k_mutex_unlock(&io_mutex);
    return error;
}

extern "C" int ht_audio_backend_tail_ms(uint32_t *delay) {
    ++drains;
    *delay = 30;
    return 0;
}

static void serial(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        zassert_ok(k_msgq_put(&incoming, bytes + i, K_NO_WAIT));
    }
}

static RadioCommand enable_command() {
    const auto state = radio_snapshot();
    RadioCommand command;
    command.kind = CommandKind::Modem;
    command.modem_enabled = true;
    command.modem_settings.persistence = 255;
    command.expected_generation = state.generation;
    command.expected_revision = state.configuration_revision;
    command.modem_epoch = modem_epoch();
    command.modem_ptt_sequence = radio_ptt_press_sequence();
    return command;
}

static void enable() {
    zassert_ok(radio_submit(enable_command()));
    radio_service();
    zassert_ok(radio_snapshot().command_error);
    zassert_true(modem_enabled());
}

static void packet() {
    modem::KissMessage message;
    message.frame.size = 3;
    message.frame.bytes[0] = 1;
    modem_host(message, modem_epoch());
}

static void before(void *) {
    audio_cancel();
    capture_count = capture_position = 0;
    k_msgq_purge(&incoming);
    k_msgq_purge(&outgoing);
    radio_power(true);
    zassert_ok(radio_start({}));
    zassert_ok(audio_start());
    k_sleep(K_MSEC(30));
}

ZTEST(modem_service, test_opt_in_boot_and_standard_controls) {
    zassert_false(modem_enabled());
    enable();
    modem::KissMessage message;
    message.command = modem::KissCommand::TxDelay;
    message.frame.size = 1;
    message.frame.bytes[0] = 255;
    modem_host(message, modem_epoch());
    zassert_equal(modem_status().settings.txdelay, 255);
    message.command = modem::KissCommand::Return;
    modem_host(message, modem_epoch());
    radio_service();
    zassert_false(modem_enabled());
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
}

ZTEST(modem_service, test_queue_bound_carrier_and_duplex) {
    enable();
    const auto drops = modem_status().dropped;
    for (int i = 0; i < 8; ++i) {
        packet();
    }
    zassert_equal(modem_status().queued, 4);
    zassert_equal(modem_status().dropped - drops, 4);
    BackendStatus busy;
    busy.rx_active = true;
    emulator_inject(busy);
    radio_service();
    zassert_false(emulator_transmitting());
    modem::KissMessage message;
    message.command = modem::KissCommand::FullDuplex;
    message.frame.size = 1;
    message.frame.bytes[0] = 1;
    modem_host(message, modem_epoch());
    k_sleep(K_MSEC(110));
    radio_service();
    zassert_true(emulator_transmitting());
}

ZTEST(modem_service, test_tx_inhibit_and_configuration_flush) {
    RadioConfig config;
    config.tx_inhibit = true;
    zassert_ok(radio_start(config));
    enable();
    packet();
    radio_service();
    zassert_false(emulator_transmitting());
    RadioCommand command;
    command.config = config;
    command.config.tx_inhibit = false;
    zassert_ok(radio_submit(command));
    radio_service();
    zassert_false(modem_enabled());
    enable();
    radio_service();
    zassert_equal(modem_status().queued, 0);
    zassert_false(emulator_transmitting());
}

ZTEST(modem_service, test_physical_ptt_cancels_under_queue_pressure) {
    enable();
    packet();
    radio_service();
    zassert_true(emulator_transmitting());
    RadioCommand command;
    for (int i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(command));
    }
    radio_ptt(true);
    zassert_false(modem_tx_requested());
    radio_service();
    zassert_false(modem_enabled());
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    zassert_false(emulator_transmitting());
}

ZTEST(modem_service, test_tap_invalidates_pending_enable_without_serial_reset) {
    const auto epoch = modem_epoch();
    zassert_ok(radio_submit(enable_command()));
    radio_ptt(true);
    radio_ptt(false);
    radio_service();
    zassert_equal(modem_epoch(), epoch);
    zassert_false(modem_enabled());
    zassert_equal(radio_snapshot().command_error, -ESTALE);
}

ZTEST(modem_service, test_fault_power_and_stale_host_work) {
    enable();
    const auto old_epoch = modem_epoch();
    packet();
    radio_service();
    radio_power(false);
    zassert_false(modem_tx_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    radio_power(true);
    radio_service();
    enable();
    modem::KissMessage message;
    message.frame.size = 1;
    modem_host(message, old_epoch);
    zassert_equal(modem_status().queued, 0);
    packet();
    radio_service();
    radio_report_fault(-EIO);
    zassert_false(modem_tx_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
}

ZTEST(modem_service, test_serial_malformed_oversize_and_transport_recovery) {
    enable();
    k_sleep(K_MSEC(40));
    const auto rejected = modem_status().rejected;
    const uint8_t malformed[] = {0xc0, 0, 0xdb, 0x42, 0xc0};
    serial(malformed, sizeof(malformed));
    uint8_t oversized[335];
    memset(oversized, 1, sizeof(oversized));
    oversized[0] = oversized[334] = 0xc0;
    oversized[1] = 0;
    serial(oversized, sizeof(oversized));
    k_sleep(K_MSEC(30));
    zassert_equal(modem_status().queued, 0);
    zassert_equal(modem_status().rejected - rejected, 2);
    const uint8_t prefix[] = {0xc0, 0, 0x42};
    serial(prefix, sizeof(prefix));
    k_sleep(K_MSEC(20));
    atomic_set(&serial_error, 1);
    k_sleep(K_MSEC(20));
    const uint8_t suffix[] = {0x24, 0xc0};
    serial(suffix, sizeof(suffix));
    k_sleep(K_MSEC(20));
    zassert_equal(modem_status().queued, 0);
    const uint8_t valid[] = {0xc0, 0, 0x42, 0xc0};
    serial(valid, sizeof(valid));
    k_sleep(K_MSEC(20));
    zassert_equal(modem_status().queued, 1);
}

ZTEST(modem_service, test_serial_transition_parser_recovery_and_return_suffix) {
    enable();
    k_sleep(K_MSEC(40));
    const uint8_t prefix[] = {0xc0, 0, 0xdb};
    serial(prefix, sizeof(prefix));
    k_sleep(K_MSEC(10));
    modem_disable();
    radio_service();
    enable();
    k_sleep(K_MSEC(40));
    const uint8_t suffix[] = {0xdc, 1, 0xc0};
    serial(suffix, sizeof(suffix));
    k_sleep(K_MSEC(20));
    zassert_equal(modem_status().queued, 0);
    const uint8_t data[] = {0xc0, 0, 1, 0xdb, 0xdc, 2, 0xc0};
    serial(data, sizeof(data));
    k_sleep(K_MSEC(20));
    zassert_equal(modem_status().queued, 1);
    zassert_equal(k_msgq_num_used_get(&outgoing), 0);
    const uint8_t exit[] = {0xc0, 0xff, 0xc0, 0xc0, 0, 9, 0xc0};
    serial(exit, sizeof(exit));
    k_sleep(K_MSEC(20));
    zassert_false(modem_enabled());
    radio_service();
    enable();
    zassert_equal(modem_status().queued, 0);
}

ZTEST(modem_service, test_worker_cancel_restart_drain_and_frame_identity) {
    enable();
    auto settings = modem_status().settings;
    settings.txdelay = 100;
    modem_settings(settings);
    packet();
    zassert_true(modem_admit(false));
    AudioRoutes routes;
    routes.output[1] = AudioSource::Buffer;
    routes.output_rate[1] = 48000;
    AudioSession session;
    zassert_ok(audio_route(routes, session));
    zassert_ok(modem_transmit(session));
    k_sleep(K_MSEC(40));
    modem_disable();
    audio_cancel();
    modem_audio_stop();
    radio_service();
    enable();
    settings.txdelay = 0;
    settings.txtail = 0;
    modem_settings(settings);
    modem::KissMessage message;
    message.frame.size = 3;
    message.frame.bytes[0] = 0x42;
    message.frame.bytes[1] = 0x24;
    message.frame.bytes[2] = 0x81;
    modem_host(message, modem_epoch());
    zassert_true(modem_admit(false));
    k_mutex_lock(&io_mutex, K_FOREVER);
    played_count = 0;
    drains = 0;
    k_mutex_unlock(&io_mutex);
    zassert_ok(audio_route(routes, session));
    zassert_ok(modem_transmit(session));
    k_sleep(K_MSEC(300));
    zassert_true(modem_status().finished);
    zassert_true(drains > 0);
    modem::Receiver receiver;
    modem::Frame decoded;
    bool found = false;
    k_mutex_lock(&io_mutex, K_FOREVER);
    for (size_t i = 0; i < played_count; ++i) {
        if (receiver.sample(played[i], decoded)) {
            found = true;
            zassert_equal(decoded.size, 3);
            zassert_mem_equal(decoded.bytes, message.frame.bytes, 3);
        }
    }
    k_mutex_unlock(&io_mutex);
    zassert_true(found, "Restart must emit the new packet after cancellation, not old TX state");
}

ZTEST(modem_service, test_maximum_escaped_receive_serial_output) {
    enable();
    k_sleep(K_MSEC(40));
    modem::Frame frame;
    frame.size = modem::max_frame_size;
    memset(frame.bytes, 0xc0, frame.size);
    static modem::Transmitter transmitter;
    transmitter.reset();
    zassert_true(transmitter.start(frame, 3, 0));
    k_mutex_lock(&io_mutex, K_FOREVER);
    capture_count = transmitter.render(capture_samples, 160000);
    capture_position = 0;
    k_mutex_unlock(&io_mutex);
    AudioRoutes routes;
    routes.input_rate[1] = 48000;
    AudioSession session;
    zassert_ok(audio_route(routes, session));
    zassert_ok(modem_receive(session));
    k_sleep(K_SECONDS(6));
    zassert_ok(audio_status().error);
    zassert_ok(modem_status().error);
    zassert_true(modem_status().received > 0, "capture %u/%u",
                 unsigned(capture_position), unsigned(capture_count));
    uint8_t expected[663], actual[663];
    const auto length = modem::kiss_encode(frame.bytes, frame.size, expected, sizeof(expected));
    zassert_equal(length, 663);
    for (size_t i = 0; i < length; ++i) {
        zassert_ok(k_msgq_get(&outgoing, actual + i, K_NO_WAIT), "Missing RX byte %u", unsigned(i));
    }
    zassert_mem_equal(expected, actual, length);
}

// Frozen AX.25 UI: N0CALL-1>APRS:!4903.50N/07201.75W-Test.
// FCS 0x1874 independently calculated with reflected binascii.crc_hqx(0xffff).
// This synthesizer does not use the production CRC, HDLC, transmitter or KISS encoder.
static const uint8_t known_aprs[] = {
    0x82, 0xa0, 0xa4, 0xa6, 0x40, 0x40, 0xe0, 0x9c, 0x60, 0x86, 0x82, 0x98, 0x98, 0x63,
    0x03, 0xf0, 0x21, 0x34, 0x39, 0x30, 0x33, 0x2e, 0x35, 0x30, 0x4e, 0x2f, 0x30, 0x37,
    0x32, 0x30, 0x31, 0x2e, 0x37, 0x35, 0x57, 0x2d, 0x54, 0x65, 0x73, 0x74};

static void independent_capture(bool corrupt) {
    double phase = 1.723;
    bool mark = true;
    capture_count = capture_position = 0;
    auto bit = [&](bool value) {
        mark = value ? mark : !mark;
        for (unsigned i = 0; i < 40; ++i) {
            zassert_true(capture_count < ARRAY_SIZE(capture_samples));
            capture_samples[capture_count++] = int16_t(12000 * sin(phase));
            phase += 2 * 3.141592653589793 * (mark ? 1200 : 2200) / 48000;
        }
    };
    auto flags = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned b = 0; b < 8; ++b) {
                bit((0x7e >> b) & 1);
            }
        }
    };
    flags(40);
    unsigned ones = 0;
    for (unsigned i = 0; i < sizeof(known_aprs) + 2; ++i) {
        const uint8_t byte = i < sizeof(known_aprs) ? known_aprs[i]
                             : i == sizeof(known_aprs) ? (corrupt ? 0x75 : 0x74) : 0x18;
        for (unsigned b = 0; b < 8; ++b) {
            const bool value = (byte >> b) & 1;
            bit(value);
            ones = value ? ones + 1 : 0;
            if (ones == 5) {
                bit(false);
                ones = 0;
            }
        }
    }
    flags(8);
}

ZTEST(modem_service, test_independent_aprs_capture_to_kiss_and_bad_fcs) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const bool corrupt = attempt == 0;
        enable();
        k_sleep(K_MSEC(40)); // Let serial ownership transition settle before capture.
        const auto received = modem_status().received;
        k_mutex_lock(&io_mutex, K_FOREVER);
        independent_capture(corrupt);
        k_mutex_unlock(&io_mutex);
        AudioRoutes routes;
        routes.input_rate[1] = 48000;
        AudioSession session;
        zassert_ok(audio_route(routes, session));
        zassert_ok(modem_receive(session));
        // The fake backend emits one 480-sample block per service poll (20 ms).
        k_sleep(K_SECONDS(2));
        zassert_equal(capture_position, capture_count);
        zassert_ok(audio_status().error);
        zassert_ok(modem_status().error);
        zassert_equal(modem_status().received - received, corrupt ? 0 : 1,
                      "corrupt %u capture %u/%u received %u->%u", corrupt,
                      unsigned(capture_position), unsigned(capture_count), received,
                      modem_status().received);
        const size_t length = corrupt ? 0 : sizeof(known_aprs) + 3;
        zassert_equal(k_msgq_num_used_get(&outgoing), length);
        for (size_t i = 0; i < length; ++i) {
            uint8_t actual;
            zassert_ok(k_msgq_get(&outgoing, &actual, K_NO_WAIT));
            const uint8_t expected = i == 0 || i == length - 1 ? 0xc0
                                     : i == 1 ? 0 : known_aprs[i - 2];
            zassert_equal(actual, expected, "KISS byte %u", unsigned(i));
        }
        modem_disable();
        audio_cancel();
        modem_audio_stop();
        radio_service();
    }
}

ZTEST(modem_service, test_independent_packet_limit_stops_wedged_worker) {
    RadioConfig config;
    config.transmit_limit_s = 0;
    zassert_ok(radio_start(config));
    enable();
    packet();
    radio_service();
    zassert_true(emulator_transmitting());
    k_sleep(K_SECONDS(11));
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_false(modem_enabled());
}

static void listen_tx() {
    RadioConfig config;
    config.rx_frequency_hz = 145600000;
    config.tx_frequency_hz = 145000000;
    zassert_ok(radio_start(config));
    const auto state = radio_snapshot();
    RadioCommand command;
    command.kind = CommandKind::ReceiveOverride;
    command.receive_override = ReceiveOverride::ListenTx;
    command.expected_generation = state.generation;
    command.expected_revision = state.configuration_revision;
    command.selection = state.selection;
    zassert_ok(radio_submit(command));
    radio_service();
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(emulator_tuned_frequency(), config.tx_frequency_hz);
}

ZTEST(modem_service, test_listen_tx_restores_authoritative_rx_on_kiss_entry_and_exit) {
    listen_tx();
    enable();
    zassert_equal(radio_snapshot().receive_override, ReceiveOverride::Normal);
    zassert_equal(emulator_tuned_frequency(), 145600000);
    modem_disable();
    radio_service();
    zassert_false(modem_enabled());
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_equal(emulator_tuned_frequency(), 145600000);
}

ZTEST(modem_service, test_listen_tx_restore_failure_faults_and_disables_kiss) {
    listen_tx();
    zassert_ok(radio_submit(enable_command()));
    emulator_fail_next(-EIO);
    radio_service();
    zassert_equal(radio_snapshot().command_error, -EIO);
    zassert_equal(radio_snapshot().fault, -EIO);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_false(modem_enabled());
    zassert_false(emulator_transmitting());
}

ZTEST_SUITE(modem_service, nullptr, nullptr, before, nullptr, nullptr);
