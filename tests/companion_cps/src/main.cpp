// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/codeplug_storage.hpp>
#include <ht/companion_codeplug.hpp>
#include <ht/companion_cps.hpp>
#include <ht/emulator.hpp>
#include <ht/ui_companion.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
using namespace ht;
using namespace ht::companion;
static Cps cps;
static unsigned handled;
static bool local_busy, fail_save;

namespace ht {
int ui_replace_codeplug(const Codeplug &plug, uint32_t id, const RadioState &state,
                        uint32_t revision) {
    return local_busy ? -EBUSY : settings_replace_codeplug(plug, id, state, revision);
}
} // namespace ht

extern "C" int __real_fsync(int);

extern "C" int __wrap_fsync(int fd) {
    struct stat info;
    if (fail_save && !fstat(fd, &info) && S_ISDIR(info.st_mode)) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static void product(void *, const Frame &request, Frame &response) {
    ++handled;
    cps.handle(request, response);
}

static Session session("dev-test", "c62", CAP_CPS, product);
static Frame request, response, prior_response;
static Codeplug plug, snapshot;
static uint8_t bytes[CPS_MAX_BYTES], read_bytes[CPS_MAX_BYTES];
static char directory[] = "/tmp/ht-companion-cps-XXXXXX", path[160];
static uint32_t sequence;
static int64_t now;

static void *setup() {
    zassert_not_null(mkdtemp(directory));
    zassert_ok(setenv("HT_SETTINGS_DIR", directory, 1));
    zassert_ok(setenv("HT_PROFILE", "cps", 1));
    snprintf(path, sizeof(path), "%s/cps.bin", directory);
    return nullptr;
}

static void before(void *) {
    unlink(path);
    local_busy = fail_save = false;
    handled = sequence = 0;
    now = 0;
    radio_power(true);
    zassert_ok(radio_start({}));
    RadioConfig config;
    zassert_equal(settings_start(config), -ENOENT);
    zassert_ok(radio_start(config));
    session = Session("dev-test", "c62", CAP_CPS, product);
    cps.reset();
    SettingsStatus status;
    settings_copy_codeplug(plug, status);
}

static void send(uint8_t type, size_t size = 0, uint8_t expected = STATUS_OK) {
    request.type = type;
    request.request = ++sequence;
    request.size = size;
    request.flags = FLAG_REQUEST;
    request.major = MAJOR;
    request.minor = MINOR;
    request.session = session.identity();
    now += 100;
    zassert_true(session.handle(request, now, response));
    zassert_equal(response.payload[0], expected);
}

static void hello() {
    request.payload[0] = 8;
    memcpy(request.payload + 1, "dev-test", 8);
    request.payload[9] = 42;
    memset(request.payload + 10, 0, 7);
    send(MSG_HELLO, 17);
    zassert_equal(session.identity(), 42);
}

static uint32_t read() {
    send(MSG_CPS_READ);
    zassert_equal(response.size, 31);
    const auto token = sys_get_le32(response.payload + 1),
               length = sys_get_le32(response.payload + 17);
    const auto checksum = sys_get_le32(response.payload + 21);
    for (uint32_t offset = 0; offset < length;) {
        const auto count = length - offset < CPS_CHUNK_BYTES ? length - offset : CPS_CHUNK_BYTES;
        sys_put_le32(token, request.payload);
        sys_put_le32(offset, request.payload + 4);
        sys_put_le16(count, request.payload + 8);
        send(MSG_CPS_READ_CHUNK, 10);
        zassert_equal(response.size, 9 + count);
        memcpy(read_bytes + offset, response.payload + 9, count);
        offset += count;
    }
    zassert_equal(crc32(read_bytes, length), checksum);
    zassert_ok(decode_codeplug(read_bytes, length, snapshot));
    return token;
}

static uint32_t upload(uint32_t read_token, bool retry = false, bool bad_crc = false) {
    size_t length = 0;
    zassert_ok(encode_codeplug(plug, bytes, sizeof(bytes), length));
    sys_put_le32(read_token, request.payload);
    sys_put_le32(length, request.payload + 4);
    sys_put_le32(crc32(bytes, length) ^ unsigned(bad_crc), request.payload + 8);
    send(MSG_CPS_WRITE, 12);
    const auto token = sys_get_le32(response.payload + 1);
    for (uint32_t offset = 0; offset < length;) {
        const auto count = length - offset < CPS_CHUNK_BYTES ? length - offset : CPS_CHUNK_BYTES;
        sys_put_le32(token, request.payload);
        sys_put_le32(offset, request.payload + 4);
        memcpy(request.payload + 8, bytes + offset, count);
        send(MSG_CPS_WRITE_CHUNK, 8 + count);
        if (retry) {
            const auto called = handled;
            zassert_true(session.handle(request, now + 1, prior_response));
            zassert_equal(handled, called);
            zassert_mem_equal(prior_response.payload, response.payload, response.size);
        }
        offset += count;
    }
    return token;
}

static void commit(uint32_t token, uint8_t expected = STATUS_OK) {
    sys_put_le32(token, request.payload);
    send(MSG_CPS_COMMIT, 4, expected);
}

static void settle() {
    settings_service(radio_snapshot(), 0);
    radio_service();
    settings_service(radio_snapshot(), 1);
}

static void status(uint32_t token, uint8_t state) {
    sys_put_le32(token, request.payload);
    send(MSG_CPS_STATUS, 4);
    zassert_equal(response.size, 16);
    zassert_equal(response.payload[5], state);
}

ZTEST(companion_cps, test_full_read_write_retry_durable_and_retained_result) {
    hello();
    const auto baseline = read();
    Channel channel;
    uint32_t id;
    strcpy(channel.name, "Maximum CPS");
    for (size_t i = 0; i < channel_capacity; ++i) {
        channel.number = i + 1;
        zassert_ok(put_channel(plug, channel, id));
    }
    static Bank bank;
    strcpy(bank.name, "Reverse");
    bank.count = channel_capacity;
    for (size_t i = 0; i < channel_capacity; ++i)
        bank.channel_ids[i] = channel_capacity - i;
    for (size_t i = 0; i < bank_capacity; ++i)
        zassert_ok(put_bank(plug, bank, id));
    plug.selection = {Operating::Memory, 16, 256};
    plug.global.ui.theme = Theme::Nord;
    const auto token = upload(baseline, true);
    commit(token);
    const auto accepted = settings_status().operation_id, calls = handled;
    zassert_true(session.handle(request, now + 1, prior_response));
    zassert_equal(handled, calls);
    zassert_equal(settings_status().operation_id, accepted);
    status(token, CPS_STATE_ACCEPTED);
    settle();
    status(token, CPS_STATE_DURABLE);
    zassert_equal(sys_get_le32(response.payload + 6), 2);
    zassert_equal(sys_get_le32(response.payload + 10), 1);
    // A following local operation must not erase the remote result.
    zassert_ok(settings_put_vfo_step(6250, accepted, radio_snapshot(), settings_status().revision));
    settle();
    status(token, CPS_STATE_DURABLE);
    zassert_equal(sys_get_le32(response.payload + 6), 2);
    read();
    zassert_equal(snapshot.channel_count, 256);
    zassert_equal(snapshot.bank_count, 16);
    zassert_equal(snapshot.banks[0].channel_ids[0], 256);
    zassert_equal(snapshot.global.ui.theme, Theme::Nord);
    zassert_equal(snapshot.global.vfo_step_hz, 6250);
    zassert_equal(snapshot.selection.channel_id, 256);
    zassert_true(now > LEASE_MS);
    zassert_equal(session.identity(), 42); // Valid chunks renew the lease.
}

ZTEST(companion_cps, test_partial_cancel_corruption_and_expiry_never_publish) {
    hello();
    auto baseline = read();
    plug.global.ui.theme = Theme::Darcula;
    auto token = upload(baseline);
    sys_put_le32(token, request.payload);
    send(MSG_CPS_CANCEL, 4);
    zassert_false(settings_status().operation_pending);
    zassert_equal(settings_status().revision, 1);
    baseline = read();
    token = upload(baseline, false, true);
    commit(token, STATUS_INVALID);
    zassert_false(settings_status().operation_pending);
    zassert_equal(settings_status().revision, 1);
    baseline = read();
    token = upload(baseline);
    sys_put_le32(token, request.payload);
    sys_put_le32(0, request.payload + 4);
    send(MSG_CPS_WRITE_CHUNK, 9, STATUS_STALE); // Out-of-order chunk cannot overwrite staging.
    zassert_true(session.expire(now + LEASE_MS));
    cps.reset();
    commit(token, STATUS_SESSION);
    zassert_equal(settings_status().revision, 1);
    zassert_false(settings_status().operation_pending);
}

ZTEST(companion_cps, test_snapshot_does_not_mix_local_edits_during_read) {
    hello();
    send(MSG_CPS_READ);
    const auto token = sys_get_le32(response.payload + 1),
               length = sys_get_le32(response.payload + 17);
    const auto checksum = sys_get_le32(response.payload + 21);
    UiPreferences ui;
    ui.theme = Theme::Nord;
    zassert_ok(settings_put_ui_preferences(ui, 901, radio_snapshot(), settings_status().revision));
    settle();
    zassert_true(length < CPS_CHUNK_BYTES);
    sys_put_le32(token, request.payload);
    sys_put_le32(0, request.payload + 4);
    sys_put_le16(length, request.payload + 8);
    send(MSG_CPS_READ_CHUNK, 10);
    zassert_equal(crc32(response.payload + 9, length), checksum);
    zassert_ok(decode_codeplug(response.payload + 9, length, snapshot));
    zassert_equal(snapshot.global.ui.theme, Theme::Midnight);
    sys_put_le32(token, request.payload);
    sys_put_le32(length, request.payload + 4);
    sys_put_le32(checksum, request.payload + 8);
    send(MSG_CPS_WRITE, 12, STATUS_STALE);
}

ZTEST(companion_cps, test_stale_write_requires_fresh_read_and_preserves_old_database) {
    hello();
    const auto baseline = read();
    zassert_ok(settings_put_vfo_step(6250, 900, radio_snapshot(), settings_status().revision));
    settle();
    sys_put_le32(baseline, request.payload);
    sys_put_le32(99, request.payload + 4);
    sys_put_le32(0, request.payload + 8);
    send(MSG_CPS_WRITE, 12, STATUS_STALE);
    zassert_false(settings_status().operation_pending);
    plug.global.ui.theme = Theme::Darcula;
    const auto token = upload(read());
    RadioCommand command;
    command.config = radio_snapshot().config;
    command.config.rx_frequency_hz = command.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(command));
    radio_service();
    commit(token);
    settle();
    status(token, CPS_STATE_FAILED);
    zassert_equal(response.payload[14], STATUS_STALE);
    read();
    zassert_equal(snapshot.global.ui.theme, Theme::Midnight);
    zassert_equal(snapshot.vfo.rx_frequency_hz, 145500000);
}

ZTEST(companion_cps, test_local_draft_and_target_failures_preserve_data) {
    hello();
    plug.global.ui.theme = Theme::Darcula;
    auto token = upload(read());
    local_busy = true;
    commit(token, STATUS_BUSY);
    zassert_false(settings_status().operation_pending);
    local_busy = false;
    plug.global.gain = 1;
    token = upload(read());
    commit(token);
    settle();
    status(token, CPS_STATE_FAILED);
    zassert_equal(response.payload[14], STATUS_UNSUPPORTED);
    read();
    zassert_equal(snapshot.global.gain, 0);
    zassert_equal(snapshot.global.ui.theme, Theme::Midnight);
}

ZTEST(companion_cps, test_applied_dirty_status_retry_and_commit_cannot_cancel) {
    hello();
    plug.global.ui.theme = Theme::Nord;
    const auto token = upload(read());
    commit(token);
    sys_put_le32(token, request.payload);
    send(MSG_CPS_CANCEL, 4, STATUS_BUSY);
    fail_save = true;
    settle();
    status(token, CPS_STATE_APPLIED);
    zassert_equal(response.payload[14], STATUS_OK);
    zassert_equal(response.payload[15], STATUS_FAILED);
    fail_save = false;
    settings_service(radio_snapshot(), 1001);
    status(token, CPS_STATE_DURABLE);
    zassert_equal(response.payload[15], STATUS_OK);
}

ZTEST(companion_cps, test_mismatched_session_never_enters_product_handler) {
    send(MSG_CPS_READ, 0, STATUS_SESSION);
    zassert_equal(handled, 0);
    hello();
    request.session = 123;
    request.type = MSG_CPS_READ;
    request.size = 0;
    request.request = ++sequence;
    zassert_true(session.handle(request, ++now, response));
    zassert_equal(response.payload[0], STATUS_SESSION);
    zassert_equal(handled, 0);
}

ZTEST_SUITE(companion_cps, nullptr, setup, before, nullptr, nullptr);
