// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion.h>
#include <ht/companion_protocol.hpp>
#include <ht/emulator.hpp>
#include <ht/release.hpp>
#include <string.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
using namespace ht;
using namespace ht::companion;

K_MSGQ_DEFINE(incoming, 1, 512, 1);
K_MSGQ_DEFINE(outgoing, sizeof(Frame), 4, 4);
static atomic_t input_error;
static uint64_t nonce_base;

extern "C" int __wrap_ht_companion_read(uint8_t *bytes, size_t size) {
    if (atomic_cas(&input_error, 1, 0)) {
        return -EIO;
    }
    unsigned count = 0;
    while (count < size && !k_msgq_get(&incoming, bytes + count, K_NO_WAIT)) {
        ++count;
    }
    return count;
}

extern "C" int __wrap_ht_companion_write(const uint8_t *bytes, size_t size, uint32_t) {
    Frame frame;
    zassert_true(size >= 2 && decode(bytes + 1, size - 2, frame));
    zassert_ok(k_msgq_put(&outgoing, &frame, K_NO_WAIT));
    return size;
}

static Frame hello(uint64_t nonce = 0) {
    if (!nonce) {
        nonce = nonce_base;
    }
    Frame frame;
    frame.type = MSG_HELLO;
    frame.request = 1;
    const auto length = strlen(HT_RELEASE_IDENTITY);
    frame.size = length + 9;
    frame.payload[0] = length;
    memcpy(frame.payload + 1, HT_RELEASE_IDENTITY, length);
    sys_put_le64(nonce, frame.payload + 1 + length);
    return frame;
}

static void send(const Frame &frame, bool corrupt = false) {
    uint8_t bytes[MAX_ENCODED];
    const auto length = encode(frame, bytes, sizeof(bytes));
    zassert_true(length > 2);
    if (corrupt) {
        bytes[length - 2] ^= 0x80;
    }
    for (size_t i = 0; i < length; ++i) {
        zassert_ok(k_msgq_put(&incoming, bytes + i, K_NO_WAIT));
    }
}

static Frame reply() {
    Frame frame;
    zassert_ok(k_msgq_get(&outgoing, &frame, K_MSEC(100)));
    return frame;
}

static Frame exchange(const Frame &frame) {
    send(frame);
    return reply();
}

static void before(void *) {
    nonce_base += 100;
    k_msgq_purge(&incoming);
    k_msgq_purge(&outgoing);
    atomic_set(&input_error, 1); // Revoke any session from the previous test.
    radio_power(true);
    zassert_ok(radio_start({}));
    k_sleep(K_MSEC(20));
}

ZTEST(companion_connection, test_hello_and_close_do_not_suppress_physical_ptt) {
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    zassert_equal(exchange(hello()).payload[0], STATUS_OK);
    radio_service();
    zassert_true(emulator_transmitting());
    Frame close;
    close.type = MSG_CLOSE;
    close.request = 2;
    close.session = nonce_base;
    zassert_equal(exchange(close).payload[0], STATUS_OK);
    radio_service();
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(exchange(hello(nonce_base + 1)).payload[0], STATUS_OK);
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    atomic_set(&input_error, 1);
    k_sleep(K_MSEC(20));
    radio_service();
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    zassert_false(emulator_transmitting());
}

ZTEST(companion_connection, test_noise_crc_and_other_messages_cannot_enter) {
    auto frame = hello();
    send(frame, true);
    frame.flags = FLAG_RESPONSE;
    send(frame);
    frame = hello();
    frame.request = 0;
    send(frame);
    k_sleep(K_MSEC(50));
    zassert_equal(k_msgq_num_used_get(&outgoing), 0);
    zassert_equal(exchange(hello()).payload[0], STATUS_OK);
}

ZTEST(companion_connection, test_hello_during_diagnostics_preserves_ui_command_result) {
    RadioCommand command;
    command.id = 123;
    command.kind = CommandKind::EnterDiagnostics;
    zassert_ok(radio_submit(command));
    radio_service();
    zassert_equal(exchange(hello()).payload[0], STATUS_OK);
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    zassert_equal(radio_snapshot().command_id, 123);
    zassert_ok(radio_snapshot().command_error);
}

ZTEST(companion_connection, test_mismatch_does_not_authorize_remote_ptt) {
    auto frame = hello();
    frame.minor = MINOR - 1;
    zassert_equal(exchange(frame).payload[0], STATUS_MISMATCH);
    frame = {};
    frame.type = MSG_PTT_PRESS;
    frame.request = 2;
    frame.session = nonce_base;
    zassert_equal(exchange(frame).payload[0], STATUS_SESSION);
    zassert_false(emulator_transmitting());
}

ZTEST(companion_connection, test_errors_and_expiry_revoke_remote_hold) {
    zassert_equal(exchange(hello()).payload[0], STATUS_OK);
    radio_service(); // Supply the initial release to the controller.
    Frame press;
    press.type = MSG_PTT_PRESS;
    press.request = 2;
    press.session = nonce_base;
    zassert_equal(exchange(press).payload[0], STATUS_OK);
    radio_service();
    zassert_true(emulator_transmitting());
    atomic_set(&input_error, 1);
    for (unsigned i = 0; i < 10 && radio_remote_ptt_requested(); ++i) {
        k_sleep(K_MSEC(10));
    }
    zassert_false(radio_remote_ptt_requested(), "Serial worker must revoke before lease expiry");
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(exchange(hello(nonce_base + 1)).payload[0], STATUS_OK);
    k_sleep(K_MSEC(LEASE_MS + 10));
    press.session = nonce_base + 1;
    zassert_equal(exchange(press).payload[0], STATUS_SESSION);
}

ZTEST_SUITE(companion_connection, nullptr, nullptr, before, nullptr, nullptr);
