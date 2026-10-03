// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_records.hpp>
#include <ht/codeplug_storage.hpp>
#include <errno.h>
#include <ht/radio.hpp>
#include <zephyr/kernel.h>

namespace ht {
#ifdef CONFIG_BOARD_C62
#define STORE_RAM __attribute__((section(".psram_section")))
#else
#define STORE_RAM
#endif
static uint8_t scratch[codeplug_record_max] STORE_RAM;
static CodeplugManifest manifest STORE_RAM;

int codeplug_save_check(const CodeplugSavePolicy &policy) {
    if (policy.deadline_ms < 0 || (policy.radio_state != SaveRadioState::Any &&
                                   policy.radio_state != SaveRadioState::Active &&
                                   policy.radio_state != SaveRadioState::Inactive)) {
        return -EINVAL;
    }
    if (policy.deadline_ms && k_uptime_get() >= policy.deadline_ms) {
        return -ETIMEDOUT;
    }
    if (policy.radio_state != SaveRadioState::Any && radio_latched_fault()) {
        return -ECANCELED;
    }
    if ((policy.radio_state == SaveRadioState::Inactive && radio_power_on_intent()) ||
        (policy.radio_state == SaveRadioState::Active && !radio_power_requested())) {
        return -ECANCELED;
    }
    return 0;
}

int codeplug_load(Codeplug &p, uint32_t &generation) {
    uint32_t loaded = 0;
    int error = codeplug_store_open(false, loaded);
    if (error) {
        return error;
    }

    size_t length = 0;
    error = codeplug_store_read(StoreRecord::Manifest, 0, scratch, sizeof(scratch), length);
    if (!error) {
        error = decode_manifest(scratch, length, loaded, manifest);
    }
    if (!error) {
        p.channel_id_high_water = manifest.channel_id_high_water;
        p.bank_id_high_water = manifest.bank_id_high_water;
        p.global = manifest.global;
        p.vfo = manifest.vfo;
        p.selection = manifest.selection;
        p.channel_count = manifest.channel_count;
        p.bank_count = manifest.bank_count;
    }
    for (size_t i = 0; !error && i < manifest.channel_count; ++i) {
        error = codeplug_store_read(StoreRecord::Channel, i, scratch, sizeof(scratch), length);
        if (!error) {
            error = decode_channel(scratch, length, loaded, p.channels[i]);
        }
        if (!error && p.channels[i].id != manifest.channel_ids[i]) {
            error = -EBADMSG;
        }
    }
    for (size_t i = 0; !error && i < manifest.bank_count; ++i) {
        error = codeplug_store_read(StoreRecord::Bank, i, scratch, sizeof(scratch), length);
        if (!error) {
            error = decode_bank(scratch, length, loaded, p.banks[i]);
        }
        if (!error && p.banks[i].id != manifest.bank_ids[i]) {
            error = -EBADMSG;
        }
    }
    if (!error && validate_codeplug(p)) {
        error = -EBADMSG;
    }

    bool published = false;
    const int close_error = codeplug_store_close(false, published);
    if (!error) {
        error = close_error;
    }
    // Open succeeded: a missing referenced record is damaged committed data,
    // never an absent database eligible for initialization with defaults.
    if (error == -ENOENT) {
        error = -EBADMSG;
    }
    if (!error) {
        generation = loaded;
    }
    return error;
}

int codeplug_save(const Codeplug &p, uint32_t &generation, const CodeplugSavePolicy &policy) {
    int error = codeplug_save_check(policy);
    if (!error) {
        error = make_manifest(p, manifest);
    }
    if (error) {
        return error;
    }

    uint32_t next = generation;
    error = codeplug_store_open(true, next, policy);
    if (error) {
        return error;
    }

    size_t length = 0;
    error = encode_manifest(manifest, next, scratch, sizeof(scratch), length);
    if (!error) {
        error = codeplug_store_write(StoreRecord::Manifest, 0, scratch, length);
    }
    for (size_t i = 0; !error && i < p.channel_count; ++i) {
        error = codeplug_save_check(policy);
        if (!error) {
            error = encode_channel(p.channels[i], next, scratch, sizeof(scratch), length);
        }
        if (!error) {
            error = codeplug_store_write(StoreRecord::Channel, i, scratch, length);
        }
    }
    for (size_t i = 0; !error && i < p.bank_count; ++i) {
        error = codeplug_save_check(policy);
        if (!error) {
            error = encode_bank(p.banks[i], next, scratch, sizeof(scratch), length);
        }
        if (!error) {
            error = codeplug_store_write(StoreRecord::Bank, i, scratch, length);
        }
    }
    if (!error) {
        error = codeplug_save_check(policy);
    }

    // Publication may succeed even if close reports a later failure. Keep the
    // caller's generation aligned with the data that became authoritative.
    bool published = false;
    const int close_error = codeplug_store_close(!error, published);
    if (published) {
        generation = next;
    }
    return error ? error : close_error ? close_error : codeplug_save_check(policy);
}
} // namespace ht
