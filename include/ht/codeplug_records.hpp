// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/codeplug.hpp>

namespace ht {
// Manifest writes version 2 and reads public version 1 with default CTCSS level 74.
// Channel/bank records and JSON schema use version 1. Integers are little-endian;
// no struct layout or padding is serialized. Generation zero is not committed.
constexpr size_t codeplug_envelope_size = 16;
constexpr size_t operating_wire_size = 40;
constexpr size_t channel_record_size =
    codeplug_envelope_size + 4 + 2 + radio_name_size + operating_wire_size;
constexpr size_t bank_record_max =
    codeplug_envelope_size + 4 + radio_name_size + 2 + 4 * channel_capacity;
constexpr size_t manifest_record_max =
    codeplug_envelope_size + 84 + 4 * (channel_capacity + bank_capacity);
constexpr size_t codeplug_record_max = manifest_record_max;
static_assert(bank_record_max <= codeplug_record_max, "Record scratch must hold a full bank");
static_assert(codeplug_record_max <= 4064, "C62 pinned NVS sector payload limit");

// Manifest binds the ordered records of one generation, not an independently
// mutable selection record. A store publishes it only after all records persist.
struct CodeplugManifest {
    uint32_t channel_id_high_water = 0;
    uint32_t bank_id_high_water = 0;
    GlobalSettings global;
    OperatingConfig vfo;
    Selection selection;
    uint16_t channel_count = 0;
    uint8_t bank_count = 0;
    uint32_t channel_ids[channel_capacity] = {};
    uint32_t bank_ids[bank_capacity] = {};
};

int make_manifest(const Codeplug &codeplug, CodeplugManifest &manifest);
// Encode requires validated record fields and enough output capacity. Decode
// rejects CRC/version/kind/generation/length/noncanonical fields, except unknown
// stored theme/contrast IDs fall back to Midnight/Normal. Outputs and
// written size are unchanged on error. Use static scratch for serialized bytes.
int encode_manifest(const CodeplugManifest &, uint32_t generation, uint8_t *, size_t capacity,
                    size_t &written);
int decode_manifest(const uint8_t *, size_t length, uint32_t generation, CodeplugManifest &);
int encode_channel(const Channel &, uint32_t generation, uint8_t *, size_t capacity,
                   size_t &written);
int decode_channel(const uint8_t *, size_t length, uint32_t generation, Channel &);
int encode_bank(const Bank &, uint32_t generation, uint8_t *, size_t capacity, size_t &written);
int decode_bank(const uint8_t *, size_t length, uint32_t generation, Bank &);
} // namespace ht
