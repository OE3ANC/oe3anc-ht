// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/codeplug_records.hpp>
#include <ht/companion_codeplug.hpp>
#include <zephyr/sys/byteorder.h>

namespace ht {
namespace companion {
// Single companion thread owns this scratch, separate from storage's manifest.
#ifdef CONFIG_BOARD_C62
static CodeplugManifest manifest __attribute__((section(".psram_section")));
#else
static CodeplugManifest manifest;
#endif
static_assert(CPS_MAX_BYTES == manifest_record_max + channel_capacity * channel_record_size +
                                   bank_capacity * bank_record_max,
              "Paired CPS limit drift");

int encode_codeplug(const Codeplug &plug, uint8_t *bytes, size_t capacity, size_t &written) {
    if (!bytes) {
        return -EINVAL;
    }
    int error = make_manifest(plug, manifest);
    size_t part = 0, cursor = 0;
    if (!error) {
        error = encode_manifest(manifest, 1, bytes, capacity, part);
    }
    if (!error) {
        cursor += part;
    }
    for (size_t i = 0; !error && i < plug.channel_count; ++i) {
        error = encode_channel(plug.channels[i], 1, bytes + cursor, capacity - cursor, part);
        if (!error) {
            cursor += part;
        }
    }
    for (size_t i = 0; !error && i < plug.bank_count; ++i) {
        error = encode_bank(plug.banks[i], 1, bytes + cursor, capacity - cursor, part);
        if (!error) {
            cursor += part;
        }
    }
    if (!error) {
        written = cursor;
    }
    return error;
}

int decode_codeplug(const uint8_t *bytes, size_t length, Codeplug &plug) {
    if (!bytes || length > CPS_MAX_BYTES) {
        return -EINVAL;
    }
    size_t cursor = 0;
    auto next = [&](unsigned kind, size_t &size) {
        if (length - cursor < codeplug_envelope_size) {
            return -EBADMSG;
        }
        const auto *record = bytes + cursor;
        size = sys_get_le16(record + 6);
        if (size < codeplug_envelope_size || size > codeplug_record_max || size > length - cursor ||
            record[4] != 1 || record[5] != kind) {
            return -EBADMSG;
        }
        return 0;
    };
    size_t size = 0;
    int error = next(1, size);
    // CPS is strict: stored palette fallback do not
    // silently reinterpret a companion's unknown enums or schema.
    if (!error && (size < 99 || bytes[40] >= ThemeCount || bytes[41] > 2)) {
        error = -EBADMSG;
    }
    if (!error) {
        error = decode_manifest(bytes, size, 1, manifest);
    }
    if (!error) {
        cursor += size;
        plug.channel_id_high_water = manifest.channel_id_high_water;
        plug.bank_id_high_water = manifest.bank_id_high_water;
        plug.global = manifest.global;
        plug.vfo = manifest.vfo;
        plug.selection = manifest.selection;
        plug.channel_count = manifest.channel_count;
        plug.bank_count = manifest.bank_count;
    }
    for (size_t i = 0; !error && i < plug.channel_count; ++i) {
        error = next(2, size);
        if (!error) {
            error = decode_channel(bytes + cursor, size, 1, plug.channels[i]);
        }
        if (!error && plug.channels[i].id != manifest.channel_ids[i]) {
            error = -EBADMSG;
        }
        if (!error) {
            cursor += size;
        }
    }
    for (size_t i = 0; !error && i < plug.bank_count; ++i) {
        error = next(3, size);
        if (!error) {
            error = decode_bank(bytes + cursor, size, 1, plug.banks[i]);
        }
        if (!error && plug.banks[i].id != manifest.bank_ids[i]) {
            error = -EBADMSG;
        }
        if (!error) {
            cursor += size;
        }
    }
    if (!error && (cursor != length || validate_codeplug(plug))) {
        error = -EBADMSG;
    }
    return error;
}
} // namespace companion
} // namespace ht
