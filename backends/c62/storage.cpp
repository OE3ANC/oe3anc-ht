// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/settings.hpp>
#include <ht/codeplug_storage.hpp>
#include <ht/codeplug_records.hpp>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

namespace ht {
static bool ready;
static nvs_fs store;
static bool session;
// Keep the established codeplug flash extent; factory data starts at 0x3B0000.
static constexpr uint16_t commit_id = 0x1fff;
static constexpr uint16_t slot_base[2] = {0x2000, 0x2200};
static constexpr size_t store_offset = 32 * 1024;
static constexpr size_t store_bytes = 256 * 1024;
#ifdef CONFIG_BOARD_C62
BUILD_ASSERT(store_offset + store_bytes <= DT_REG_SIZE(DT_NODELABEL(storage_partition)),
             "Codeplug storage exceeds reserved partition");
BUILD_ASSERT(64 * 4096 == store_bytes, "NVS geometry must match its reserved extent");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(storage_partition)) <= 0x3b0000 &&
                 store_offset + store_bytes <=
                     0x3b0000 - DT_REG_ADDR(DT_NODELABEL(storage_partition)),
             "Codeplug NVS must end before factory calibration at 0x3B0000");
#endif
static bool writing;
static uint8_t slot;
static uint32_t session_generation;
static CodeplugSavePolicy session_policy;

static int read_commit(uint32_t &generation, uint8_t &active) {
    uint8_t bytes[16];
    const ssize_t count = nvs_read(&store, commit_id, bytes, sizeof(bytes));
    if (count < 0) {
        return static_cast<int>(count);
    }
    if (count != sizeof(bytes) || memcmp(bytes, "HTCC", 4) ||
        crc32_ieee(bytes, 12) != sys_get_le32(bytes + 12) || bytes[5] > 1 || bytes[6] || bytes[7] ||
        !sys_get_le32(bytes + 8)) {
        return -EBADMSG;
    }
    if (bytes[4] != 1) {
        return -ENOTSUP;
    }
    generation = sys_get_le32(bytes + 8);
    active = bytes[5];
    return 0;
}

static uint16_t record_id(StoreRecord kind, size_t index) {
    switch (kind) {
    case StoreRecord::Manifest:
        return index == 0 ? slot_base[slot] : 0;
    case StoreRecord::Channel:
        return index < channel_capacity ? slot_base[slot] + 1 + index : 0;
    case StoreRecord::Bank:
        return index < bank_capacity ? slot_base[slot] + 1 + channel_capacity + index : 0;
    }
    return 0;
}

static int write_value(uint16_t id, const uint8_t *bytes, size_t length) {
    int error = codeplug_save_check(session_policy);
    if (error) {
        return error;
    }
    if (!radio_idle_lock(session_policy.radio_state == SaveRadioState::Inactive)) {
        return -EAGAIN;
    }
    error = codeplug_save_check(session_policy);
    if (!error) {
        const ssize_t count = nvs_write(&store, id, bytes, length);
        error = count < 0                                            ? static_cast<int>(count)
                : count == 0 || static_cast<size_t>(count) == length ? 0
                                                                     : -EIO;
        if (!error) {
            error = codeplug_save_check(session_policy);
        }
    }
    radio_idle_unlock();
    return error;
}

int settings_storage_init() {
    if (session) {
        return -EBUSY;
    }
    ready = false;
    const flash_area *area = nullptr;
    int error = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
    if (error) {
        return error;
    }
    const device *flash = flash_area_get_device(area);
    if (!flash || !device_is_ready(flash)) {
        error = -ENODEV;
    } else if (store_offset + store_bytes > area->fa_size) {
        error = -ENOSPC;
    } else {
        store = {};
        store.flash_device = flash;
        store.offset = area->fa_off + store_offset;
        store.sector_size = 4096;
        store.sector_count = 64;
        error = nvs_mount(&store);
    }
    flash_area_close(area);
    ready = !error;
    return error;
}

int codeplug_store_open(bool save, uint32_t &generation, const CodeplugSavePolicy &policy) {
    if (!ready || !store.ready) {
        return -ENODEV;
    }
    if (session) {
        return -EBUSY;
    }
    const int policy_error = save ? codeplug_save_check(policy) : 0;
    if (policy_error) {
        return policy_error;
    }
    uint32_t current = 0;
    uint8_t active = 1;
    const int error = read_commit(current, active);
    if (error && !(save && error == -ENOENT)) {
        return error;
    }
    if (save) {
        if (current != generation) {
            return -ESTALE;
        }
        if (current == UINT32_MAX) {
            return -EOVERFLOW;
        }
        slot = active ^ 1;
        session_generation = current + 1;
    } else {
        slot = active;
        session_generation = current;
    }
    session_policy = save ? policy : CodeplugSavePolicy{};
    writing = save;
    session = true;
    generation = session_generation;
    return 0;
}

int codeplug_store_read(StoreRecord kind, size_t index, uint8_t *bytes, size_t capacity,
                        size_t &length) {
    if (!session || writing) {
        return -ENODEV;
    }
    const uint16_t id = record_id(kind, index);
    if (!id || !bytes || !capacity) {
        return -EINVAL;
    }
    const ssize_t count = nvs_read(&store, id, bytes, capacity);
    if (count < 0) {
        return static_cast<int>(count);
    }
    if (static_cast<size_t>(count) > capacity) {
        return -EBADMSG;
    }
    length = count;
    return 0;
}

int codeplug_store_write(StoreRecord kind, size_t index, const uint8_t *bytes, size_t length) {
    if (!session || !writing) {
        return -ENODEV;
    }
    const uint16_t id = record_id(kind, index);
    if (!id || !bytes || !length || length > codeplug_record_max) {
        return -EINVAL;
    }
    return write_value(id, bytes, length);
}

int codeplug_store_close(bool commit, bool &published) {
    published = false;
    if (!session) {
        return -ENODEV;
    }
    int error = 0;
    if (writing && commit) {
        uint8_t bytes[16] = {'H', 'T', 'C', 'C', 1, slot, 0, 0};
        sys_put_le32(session_generation, bytes + 8);
        sys_put_le32(crc32_ieee(bytes, 12), bytes + 12);
        error = write_value(commit_id, bytes, sizeof(bytes));
        if (!error) {
            published = true;
        } else {
            // An I/O error can follow a physically completed commit. Reconcile
            // visible state so a durability retry does not use a stale version.
            uint32_t current = 0;
            uint8_t active = 0;
            published =
                !read_commit(current, active) && current == session_generation && active == slot;
        }
    }
    session = writing = false;
    return error;
}
} // namespace ht
