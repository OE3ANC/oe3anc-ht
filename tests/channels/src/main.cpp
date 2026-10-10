// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_records.hpp>
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>
#include "../fixtures/golden.hpp"

using namespace ht;
static Codeplug plug;
static Codeplug restored;
static uint8_t bytes[codeplug_record_max + 1];
static uint8_t saved[sizeof(Codeplug)];
static CodeplugManifest manifest;
static Bank bank;
static size_t written;
static uint32_t assigned;

static void reset(Codeplug &p) {
    // Unused records are outside the logical store; do not copy a 40 KiB object.
    p.channel_id_high_water = p.bank_id_high_water = 0;
    p.global = {};
    p.vfo = {};
    p.selection = {};
    p.channel_count = p.bank_count = 0;
}

static void before(void *) {
    reset(plug);
    reset(restored);
    manifest = {};
    bank = {};
    memset(bytes, 0xa5, sizeof(bytes));
    written = 999;
    assigned = 999;
}

static Channel channel(uint16_t number = 1) {
    Channel c;
    c.number = number;
    strcpy(c.name, "CITY REPEATER");
    c.configuration.rx_frequency_hz = 439075000;
    c.configuration.tx_frequency_hz = 431475000;
    c.configuration.rx_tone = {ToneKind::Ctcss, 885, false};
    c.configuration.tx_tone = {ToneKind::Dcs, 0023, true};
    return c;
}

static void full_store() {
    for (uint16_t number = 1; number <= channel_capacity; ++number) {
        zassert_ok(put_channel(plug, channel(number), assigned));
        zassert_equal(assigned, number);
    }
    strcpy(bank.name, "Full bank");
    bank.count = channel_capacity;
    for (size_t i = 0; i < channel_capacity; ++i) {
        bank.channel_ids[i] = channel_capacity - i; // Deliberately meaningful order.
    }
    for (size_t i = 0; i < bank_capacity; ++i) {
        zassert_ok(put_bank(plug, bank, assigned));
    }
    zassert_ok(select_operating(plug, {Operating::Memory, 1, 256}));
    zassert_ok(validate_codeplug(plug));
}

static void checksum() {
    sys_put_le32(crc32_ieee_update(crc32_ieee(bytes, 12), bytes + 16, written - 16), bytes + 12);
}

ZTEST(channels, test_stable_ids_edits_duplicates_and_failure_atomicity) {
    Channel c = channel();
    zassert_ok(put_channel(plug, c, assigned));
    zassert_equal(assigned, 1);
    c = *find_channel(plug, assigned);
    c.number = 9;
    strcpy(c.name, "Renamed");
    zassert_ok(put_channel(plug, c, assigned));
    zassert_equal(assigned, 1);
    zassert_equal(plug.channel_id_high_water, 1);
    zassert_is_null(find_channel_number(plug, 1));
    c.id = 0;
    c.number = 2;
    zassert_ok(put_channel(plug, c, assigned));
    zassert_equal(assigned, 2);
    memcpy(saved, &plug, sizeof(plug));
    assigned = 99;
    zassert_equal(put_channel(plug, c, assigned), -EEXIST);
    zassert_equal(assigned, 99);
    zassert_mem_equal(saved, &plug, sizeof(plug));
    zassert_ok(delete_channel(plug, 1));
    c.number = 1;
    zassert_ok(put_channel(plug, c, assigned));
    zassert_equal(assigned, 3);
    zassert_ok(validate_codeplug(plug));
}

ZTEST(channels, test_bank_membership_order_selection_and_deletion) {
    zassert_ok(put_channel(plug, channel(), assigned));
    zassert_ok(put_channel(plug, channel(2), assigned));
    strcpy(bank.name, "Local");
    bank.count = 2;
    bank.channel_ids[0] = 2;
    bank.channel_ids[1] = 1;
    zassert_ok(put_bank(plug, bank, assigned));
    zassert_ok(select_operating(plug, {Operating::Memory, assigned, 2}));
    zassert_ok(delete_channel(plug, 1));
    zassert_equal(plug.banks[0].count, 1);
    zassert_equal(plug.banks[0].channel_ids[0], 2);
    zassert_ok(delete_channel(plug, 2));
    zassert_equal(plug.selection.operating, Operating::Vfo);
    zassert_equal(plug.selection.channel_id, 0);
    zassert_equal(plug.banks[0].count, 0);
    zassert_ok(delete_bank(plug, assigned));
    zassert_equal(plug.selection.bank_id, 0);
    zassert_ok(validate_codeplug(plug));
    zassert_equal(select_operating(plug, {Operating::Memory, 0, 0}), -EINVAL);
    zassert_equal(select_operating(plug, {Operating::Vfo, 5, 0}), -ENOENT);
    zassert_equal(select_operating(plug, {static_cast<Operating>(2), 0, 0}), -EINVAL);
}

ZTEST(channels, test_invalid_references_and_bank_edit_selected_member_removal) {
    zassert_ok(put_channel(plug, channel(), assigned));
    zassert_ok(put_channel(plug, channel(2), assigned));
    strcpy(bank.name, "Local");
    bank.count = 1;
    bank.channel_ids[0] = 3;
    zassert_equal(put_bank(plug, bank, assigned), -ENOENT);
    bank.count = 2;
    bank.channel_ids[0] = bank.channel_ids[1] = 1;
    zassert_equal(put_bank(plug, bank, assigned), -EEXIST);
    zassert_equal(plug.bank_id_high_water, 0);
    bank.count = 1;
    zassert_ok(put_bank(plug, bank, assigned));
    zassert_equal(select_operating(plug, {Operating::Memory, assigned, 2}), -EINVAL);
    zassert_ok(select_operating(plug, {Operating::Memory, assigned, 1}));
    bank.id = assigned;
    bank.channel_ids[0] = 2;
    zassert_ok(put_bank(plug, bank, assigned));
    zassert_equal(plug.selection.bank_id, 0);
    zassert_equal(plug.selection.channel_id, 1);
    zassert_equal(plug.selection.operating, Operating::Memory);
    zassert_ok(validate_codeplug(plug));
}

ZTEST(channels, test_contract_boundaries_and_exhaustion) {
    Channel c = channel();
    c.configuration.tx_inhibit = true;
    c.configuration.tx_frequency_hz = 0; // RX-only still requires a valid TX frequency.
    zassert_equal(put_channel(plug, c, assigned), -EINVAL);
    c = channel();
    memset(c.name, 'X', sizeof(c.name));
    zassert_equal(put_channel(plug, c, assigned), -EINVAL);
    c = channel();
    c.configuration.tx_tone.value = 01000;
    zassert_equal(put_channel(plug, c, assigned), -EINVAL);
    c = channel();
    c.configuration.m17.can = 1; // Inactive mode cannot hide persistent settings.
    zassert_equal(put_channel(plug, c, assigned), -EINVAL);
    plug.channel_id_high_water = UINT32_MAX;
    zassert_equal(put_channel(plug, channel(), assigned), -EOVERFLOW);
    zassert_equal(plug.channel_count, 0);
    strcpy(bank.name, "Empty");
    plug.bank_id_high_water = UINT32_MAX;
    zassert_equal(put_bank(plug, bank, assigned), -EOVERFLOW);
    zassert_equal(plug.bank_count, 0);
    plug.channel_count = 257;
    zassert_equal(validate_codeplug(plug), -EINVAL);
    zassert_is_null(find_channel(plug, 1));
}

ZTEST(channels, test_maximum_capacity_and_binary_round_trip) {
    full_store();
    zassert_equal(put_channel(plug, channel(1), assigned), -EEXIST);
    zassert_equal(put_bank(plug, bank, assigned), -ENOSPC);
    zassert_ok(make_manifest(plug, manifest));
    zassert_ok(encode_manifest(manifest, 42, bytes, sizeof(bytes), written));
    zassert_equal(written, manifest_record_max);
    zassert_equal(bytes[written], 0xa5);
    CodeplugManifest decoded;
    zassert_ok(decode_manifest(bytes, written, 42, decoded));
    restored.channel_id_high_water = decoded.channel_id_high_water;
    restored.bank_id_high_water = decoded.bank_id_high_water;
    restored.global = decoded.global;
    restored.vfo = decoded.vfo;
    restored.selection = decoded.selection;
    restored.channel_count = decoded.channel_count;
    restored.bank_count = decoded.bank_count;
    for (size_t i = 0; i < plug.channel_count; ++i) {
        zassert_ok(encode_channel(plug.channels[i], 42, bytes, sizeof(bytes), written));
        zassert_equal(written, channel_record_size);
        zassert_ok(decode_channel(bytes, written, 42, restored.channels[i]));
        zassert_equal(restored.channels[i].id, decoded.channel_ids[i]);
    }
    for (size_t i = 0; i < plug.bank_count; ++i) {
        zassert_ok(encode_bank(plug.banks[i], 42, bytes, sizeof(bytes), written));
        zassert_equal(written, bank_record_max);
        zassert_ok(decode_bank(bytes, written, 42, restored.banks[i]));
        zassert_equal(restored.banks[i].id, decoded.bank_ids[i]);
        zassert_mem_equal(restored.banks[i].channel_ids, plug.banks[i].channel_ids, 1024);
    }
    zassert_ok(validate_codeplug(restored));
    printk("Codeplug RAM=%zu channel=%zu bank=%zu manifest=%zu scratch=%zu\n", sizeof(Codeplug),
           channel_record_size, bank_record_max, manifest_record_max, codeplug_record_max);
}

ZTEST(channels, test_independent_json_fixture_bytes) {
    zassert_ok(make_manifest(plug, manifest));
    zassert_ok(encode_manifest(manifest, 1, bytes, sizeof(bytes), written));
    zassert_equal(written, sizeof(empty_manifest));
    zassert_mem_equal(bytes, empty_manifest, written);
    Channel c = channel();
    c.id = 17;
    c.configuration.tx_tone.inverted = false; // examples/codeplug/fm-repeater.json
    zassert_ok(encode_channel(c, 1, bytes, sizeof(bytes), written));
    zassert_equal(written, sizeof(fm_channel));
    zassert_mem_equal(bytes, fm_channel, written);
    c.configuration = {};
    c.configuration.mode = Mode::M17;
    c.configuration.rx_frequency_hz = c.configuration.tx_frequency_hz = 433475000;
    c.configuration.m17.destination = Destination::Station;
    strcpy(c.configuration.m17.callsign, "OE1TEST");
    c.configuration.m17.can = 3;
    c.configuration.m17.rx_can_check = true;
    strcpy(c.name, "LOCAL M17");
    zassert_ok(encode_channel(c, 1, bytes, sizeof(bytes), written));
    zassert_mem_equal(bytes, m17_channel, sizeof(m17_channel));
}

ZTEST(channels, test_corruption_truncation_generation_and_output_preservation) {
    Channel c = channel();
    c.id = 1;
    Channel out = c;
    zassert_ok(encode_channel(c, 7, bytes, sizeof(bytes), written));
    for (size_t i = 0; i < written; ++i) {
        bytes[i] ^= 1;
        zassert_equal(decode_channel(bytes, written, 7, out), -EBADMSG);
        zassert_equal(out.id, 1);
        bytes[i] ^= 1;
    }
    for (size_t n = 0; n < written; ++n) {
        zassert_equal(decode_channel(bytes, n, 7, out), -EBADMSG);
    }
    zassert_equal(decode_channel(bytes, written + 1, 7, out), -EBADMSG);
    zassert_equal(decode_channel(bytes, written, 8, out), -EBADMSG);
    bytes[4] = 3;
    checksum();
    zassert_equal(decode_channel(bytes, written, 7, out), -ENOTSUP);
    bytes[4] = 1;
    bytes[50] = 2; // Operating TX inhibit must be a canonical boolean.
    checksum();
    zassert_equal(decode_channel(bytes, written, 7, out), -EBADMSG);
    zassert_equal(out.configuration.tx_frequency_hz, c.configuration.tx_frequency_hz);
    bytes[50] = 0;
    bytes[86] = 1;
    checksum(); // Reserved byte.
    zassert_equal(decode_channel(bytes, written, 7, out), -EBADMSG);
    written = 123;
    zassert_equal(encode_channel(c, 7, bytes, channel_record_size - 1, written), -ENOSPC);
    zassert_equal(written, 123);
    zassert_equal(encode_channel(c, 0, bytes, sizeof(bytes), written), -EINVAL);
    zassert_equal(written, 123);
}

ZTEST(channels, test_manifest_bank_malformed_payloads_with_valid_crc) {
    full_store();
    zassert_ok(make_manifest(plug, manifest));
    zassert_ok(encode_manifest(manifest, 5, bytes, sizeof(bytes), written));
    sys_put_le16(257, bytes + 24);
    checksum();
    zassert_equal(decode_manifest(bytes, written, 5, manifest), -EBADMSG);
    zassert_equal(manifest.channel_count, 256);
    zassert_ok(encode_bank(plug.banks[0], 5, bytes, sizeof(bytes), written));
    sys_put_le32(0, bytes + 47);
    checksum();
    zassert_equal(decode_bank(bytes, written, 5, bank), -EBADMSG);
    zassert_equal(bank.id, 0);
    zassert_ok(encode_bank(plug.banks[0], 5, bytes, sizeof(bytes), written));
    sys_put_le16(257, bytes + 45);
    checksum();
    zassert_equal(decode_bank(bytes, written, 5, bank), -EBADMSG);
    zassert_ok(encode_bank(plug.banks[0], 5, bytes, sizeof(bytes), written));
    bytes[30] = 'X';
    checksum(); // Nonzero string padding after "Full bank" NUL.
    zassert_equal(decode_bank(bytes, written, 5, bank), -EBADMSG);
}

ZTEST(channels, test_global_preferences_and_stored_palette_fallback) {
    strcpy(plug.global.local_callsign, "OE3ANC");
    plug.global.gain = 15;
    plug.global.transmit_limit_s = 60;
    plug.global.ui = {Theme::Darcula, Contrast::Maximum, false, 25, 60, 30};
    zassert_ok(make_manifest(plug, manifest));
    zassert_ok(encode_manifest(manifest, 2, bytes, sizeof(bytes), written));
    zassert_ok(decode_manifest(bytes, written, 2, manifest));
    zassert_equal(manifest.global.ui.theme, Theme::Darcula);
    zassert_equal(manifest.global.ui.contrast, Contrast::Maximum);
    zassert_equal(manifest.global.ui.brightness_percent, 25);
    zassert_false(manifest.global.ui.animations);
    zassert_equal(manifest.global.transmit_limit_s, 60);
    zassert_equal(manifest.global.gain, 15);
    zassert_mem_equal(manifest.global.local_callsign, plug.global.local_callsign, 10);
    bytes[40] = bytes[41] = 255;
    checksum();
    zassert_ok(decode_manifest(bytes, written, 2, manifest));
    zassert_equal(manifest.global.ui.theme, Theme::Midnight);
    zassert_equal(manifest.global.ui.contrast, Contrast::Normal);
    zassert_equal(manifest.global.gain, 15);
    manifest.global.ui.theme = static_cast<Theme>(255);
    zassert_equal(encode_manifest(manifest, 2, bytes, sizeof(bytes), written), -EINVAL);
}

ZTEST(channels, test_draft_checks_never_mutate_or_allocate) {
    Channel channel;
    strcpy(channel.name, "Draft");
    zassert_ok(check_channel_update(plug, channel));
    zassert_equal(plug.channel_count, 0);
    zassert_equal(plug.channel_id_high_water, 0);
    uint32_t id;
    zassert_ok(put_channel(plug, channel, id));
    zassert_equal(check_channel_update(plug, channel), -EEXIST);
    channel.id = id;
    channel.number = 2;
    strcpy(channel.name, "Edited draft");
    zassert_ok(check_channel_update(plug, channel));
    zassert_equal(plug.channels[0].number, 1);
    zassert_equal(strcmp(plug.channels[0].name, "Draft"), 0);
    bank = {};
    strcpy(bank.name, "Draft bank");
    bank.count = 1;
    bank.channel_ids[0] = id;
    zassert_ok(check_bank_update(plug, bank));
    zassert_equal(plug.bank_count, 0);
    zassert_equal(plug.bank_id_high_water, 0);
    zassert_false(bank_contains(bank, 0));
    zassert_true(bank_contains(bank, id));
    zassert_ok(check_selection(plug, {Operating::Memory, 0, id}));
    zassert_equal(plug.selection.operating, Operating::Vfo);
    zassert_equal(check_selection(plug, {Operating::Memory, 1, id}), -ENOENT);
    bank.channel_ids[0] = id + 1;
    zassert_equal(check_bank_update(plug, bank), -ENOENT);
    zassert_equal(plug.bank_count, 0);
}

ZTEST(channels, test_vfo_step_wire_defaults_and_validation) {
    for (auto step : vfo_steps_hz) {
        plug.global.vfo_step_hz = step;
        zassert_ok(make_manifest(plug, manifest));
        zassert_ok(encode_manifest(manifest, 1, bytes, sizeof(bytes), written));
        zassert_equal(bytes[4], 2);
        zassert_ok(decode_manifest(bytes, written, 1, manifest));
        zassert_equal(manifest.global.vfo_step_hz, step);
    }
    sys_put_le32(0, bytes + written - 5);
    checksum();
    const auto previous = manifest.global.vfo_step_hz;
    zassert_equal(decode_manifest(bytes, written, 1, manifest), -EBADMSG);
    zassert_equal(manifest.global.vfo_step_hz, previous);
    plug.global.vfo_step_hz = 1;
    zassert_equal(validate_codeplug(plug), -EINVAL);
}

ZTEST(channels, test_ctcss_level_manifest_versions_and_validation) {
    plug.global.fm_ctcss_level = 127;
    zassert_ok(make_manifest(plug, manifest));
    zassert_ok(encode_manifest(manifest, 1, bytes, sizeof(bytes), written));
    zassert_ok(decode_manifest(bytes, written, 1, manifest));
    zassert_equal(manifest.global.fm_ctcss_level, 127);
    bytes[written - 1] = 128;
    checksum();
    zassert_equal(decode_manifest(bytes, written, 1, manifest), -EBADMSG);
    zassert_equal(manifest.global.fm_ctcss_level, 127);
    // Public v1 ended after VFO step. Loading must preserve it and default the new field.
    --written;
    bytes[4] = 1;
    sys_put_le16(written, bytes + 6);
    checksum();
    zassert_ok(decode_manifest(bytes, written, 1, manifest));
    zassert_equal(manifest.global.fm_ctcss_level, 74);
    zassert_equal(manifest.global.vfo_step_hz, 12500);
    plug.global.fm_ctcss_level = 128;
    zassert_equal(validate_codeplug(plug), -EINVAL);
}

ZTEST_SUITE(channels, nullptr, nullptr, before, nullptr, nullptr);
