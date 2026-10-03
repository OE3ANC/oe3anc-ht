// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion.h>
#include <ht/companion_ptt.hpp>
#include <ht/emulator.hpp>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
using namespace ht;
using namespace ht::companion;

extern "C" int __wrap_ht_companion_read(uint8_t *, size_t) {
    return 0;
}

static Frame request, response;

static void enable() {
    const auto state = radio_snapshot();
    RadioCommand command;
    command.kind = CommandKind::CompanionMode;
    command.expected_generation = state.generation;
    command.expected_revision = state.configuration_revision;
    command.companion_enabled = true;
    zassert_ok(radio_submit(command));
    radio_service();
    zassert_ok(radio_snapshot().command_error);
    k_sleep(K_MSEC(10)); // Let the serial worker observe mode before owning a test session.
    radio_remote_session(42);
    radio_service();
}

static void before(void *) {
    radio_power(true);
    zassert_ok(radio_start({}));
    enable();
    request = {};
    request.session = 42;
    request.request = 2;
}

static int command(unsigned type, uint32_t token = 0) {
    request.type = type;
    request.size = type == MSG_PTT_KEEP ? 4 : 0;
    if (request.size) {
        sys_put_le32(token, request.payload);
    }
    handle_ptt(request, response);
    return response.payload[0];
}

static void press() {
    zassert_equal(command(MSG_PTT_PRESS), STATUS_OK);
    zassert_equal(response.size, 5);
    zassert_equal(sys_get_le32(response.payload + 1), request.request);
    radio_service();
    zassert_true(emulator_transmitting());
}

ZTEST(companion_ptt, test_source_and_queue_independent_release) {
    const auto sequence = radio_ptt_press_sequence();
    radio_ptt(true);
    radio_service();
    zassert_equal(radio_ptt_press_sequence(), sequence);
    zassert_false(emulator_transmitting());
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    press();
    zassert_false(radio_snapshot().monitor_active);
    zassert_equal(radio_ptt_press_sequence(), sequence + 1);
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(RadioCommand{}));
    }
    zassert_not_equal(radio_submit(RadioCommand{}), 0);
    zassert_equal(command(MSG_PTT_RELEASE), STATUS_OK);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_false(radio_snapshot().monitor_active); // Still held monitor cannot reactivate.
    zassert_equal(command(MSG_PTT_KEEP, 2), STATUS_STALE);
    zassert_equal(command(MSG_PTT_RELEASE), STATUS_OK);
    for (unsigned i = 0; i < 8; ++i) {
        radio_service();
    }
    radio_monitor(false);
    radio_service();
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
}

ZTEST(companion_ptt, test_lease_expires_without_serial_worker_cleanup) {
    press();
    k_sleep(K_MSEC(PTT_LEASE_MS));
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_ok(radio_snapshot().fault);
    zassert_equal(command(MSG_PTT_KEEP, 2), STATUS_STALE);
    request.request = 3;
    press();
}

ZTEST(companion_ptt, test_keep_token_and_session_cannot_reassert) {
    press();
    k_sleep(K_MSEC(300));
    zassert_equal(command(MSG_PTT_KEEP, 999), STATUS_STALE);
    zassert_equal(command(MSG_PTT_KEEP, 2), STATUS_OK);
    k_sleep(K_MSEC(300));
    radio_service();
    zassert_true(emulator_transmitting());
    radio_remote_session(43);
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(command(MSG_PTT_KEEP, 2), STATUS_STALE);
    zassert_equal(command(MSG_PTT_PRESS), STATUS_STALE);
    request.session = 43;
    request.request = 3;
    press();
    radio_remote_session(0);
    radio_service();
    zassert_false(emulator_transmitting());
}

ZTEST(companion_ptt, test_coalesced_release_and_fresh_press) {
    zassert_equal(command(MSG_PTT_PRESS), STATUS_OK); // Owner has not consumed it.
    zassert_equal(command(MSG_PTT_RELEASE), STATUS_OK);
    request.request = 3;
    zassert_equal(command(MSG_PTT_PRESS), STATUS_OK);
    radio_service();
    zassert_true(emulator_transmitting());
    zassert_equal(command(MSG_PTT_KEEP, 2), STATUS_STALE);
    zassert_equal(command(MSG_PTT_KEEP, 3), STATUS_OK);
}

ZTEST(companion_ptt, test_fault_and_coalesced_power_off_cancel_hold) {
    press();
    radio_power(false);
    radio_power(true);
    zassert_equal(command(MSG_PTT_KEEP, 2), STATUS_STALE);
    radio_service();
    zassert_false(emulator_transmitting());
    radio_service();
    zassert_false(emulator_transmitting());
    request.request = 3;
    press();
    radio_report_fault(-EIO);
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_false(emulator_transmitting());
    zassert_equal(command(MSG_PTT_KEEP, 3), STATUS_STALE);
    zassert_equal(command(MSG_PTT_PRESS), STATUS_FAILED);
}

ZTEST(companion_ptt, test_inhibit_diagnostics_and_m17_callsign_guards) {
    radio_remote_session(0);
    auto config = radio_snapshot().config;
    config.tx_inhibit = true;
    zassert_ok(radio_start(config));
    enable();
    zassert_equal(command(MSG_PTT_PRESS), STATUS_FAILED);
    config.tx_inhibit = false;
    config.mode = Mode::M17;
    zassert_ok(radio_start(config));
    enable();
    zassert_equal(command(MSG_PTT_PRESS), STATUS_INVALID);
    strcpy(config.callsign, "OE3ANC");
    zassert_ok(radio_start(config));
    enable();
    press();
    zassert_equal(command(MSG_PTT_RELEASE), STATUS_OK);
    radio_service();
    RadioCommand diagnostic;
    diagnostic.kind = CommandKind::EnterDiagnostics;
    zassert_ok(radio_submit(diagnostic));
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    zassert_equal(command(MSG_PTT_PRESS), STATUS_BUSY);
}

ZTEST(companion_ptt, test_normal_limit_still_requires_release_even_with_keeps) {
    RadioConfig config;
    config.transmit_limit_s = 60;
    zassert_ok(radio_start(config));
    enable();
    press();
    for (unsigned i = 0; i < 151; ++i) {
        k_sleep(K_MSEC(400));
        zassert_equal(command(MSG_PTT_KEEP, 2), STATUS_OK);
        radio_service();
    }
    zassert_false(emulator_transmitting());
    zassert_true(radio_snapshot().tx_timed_out);
    zassert_equal(command(MSG_PTT_PRESS), STATUS_BUSY);
    zassert_equal(command(MSG_PTT_RELEASE), STATUS_OK);
    radio_service();
    request.request = 3;
    press();
}

static void product(void *, const Frame &input, Frame &output) {
    handle_ptt(input, output);
}

ZTEST(companion_ptt, test_duplicate_wire_press_and_keep_do_not_renew) {
    Session session("dev-test", "emulator", CAP_PTT, product);
    Frame hello;
    hello.type = MSG_HELLO;
    hello.request = 1;
    hello.size = 17;
    hello.payload[0] = 8;
    memcpy(hello.payload + 1, "dev-test", 8);
    sys_put_le64(42, hello.payload + 9);
    zassert_true(session.handle(hello, k_uptime_get(), response));
    request.type = MSG_PTT_PRESS;
    request.size = 0;
    const auto sequence = radio_ptt_press_sequence();
    zassert_true(session.handle(request, k_uptime_get(), response));
    zassert_equal(response.payload[0], STATUS_OK);
    radio_service();
    k_sleep(K_MSEC(300));
    zassert_true(session.handle(request, k_uptime_get(), response));
    zassert_equal(radio_ptt_press_sequence(), sequence + 1);
    request.request = 3;
    request.type = MSG_PTT_KEEP;
    request.size = 4;
    sys_put_le32(2, request.payload);
    zassert_true(session.handle(request, k_uptime_get(), response));
    zassert_equal(response.payload[0], STATUS_OK);
    k_sleep(K_MSEC(300));
    zassert_true(session.handle(request, k_uptime_get(), response));
    k_sleep(K_MSEC(201));
    radio_service();
    zassert_false(emulator_transmitting());
    request.request = 4;
    zassert_true(session.handle(request, k_uptime_get(), response));
    zassert_equal(response.payload[0], STATUS_STALE);
}

ZTEST_SUITE(companion_ptt, nullptr, nullptr, before, nullptr, nullptr);
