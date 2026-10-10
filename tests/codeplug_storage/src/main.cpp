// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_records.hpp>
#include <ht/codeplug_storage.hpp>
#include <ht/settings.hpp>
#include <ht/emulator.hpp>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>
#ifdef CONFIG_HT_SETTINGS_NVS
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#endif

using namespace ht;
static Codeplug plug;
static Codeplug loaded;
static Bank bank;
static uint8_t bytes[codeplug_record_max];
static uint32_t generation;
static int fail_after = -1;
static int calls;
static bool fail_after_commit;
static unsigned delay_record_ms, delay_commit_ms;
static bool resume_during_record, fault_during_record;
#ifndef CONFIG_HT_SETTINGS_NVS
static bool status_fault_during_record;
#endif
static void record_hook() {
    if (delay_record_ms) {
        const auto delay = delay_record_ms;
        delay_record_ms = 0;
        k_sleep(K_MSEC(delay));
    }
    if (resume_during_record) {
        resume_during_record = false;
        radio_power(false);
        radio_power(true);
    }
    if (fault_during_record) {
        fault_during_record = false;
        radio_report_fault(-EPIPE);
    }
#ifndef CONFIG_HT_SETTINGS_NVS
    if (status_fault_during_record) {
        status_fault_during_record = false;
        BackendStatus status;
        status.error = -EPIPE;
        emulator_inject(status);
        radio_service();
        zassert_equal(radio_snapshot().fault, -EPIPE);
        zassert_ok(radio_pending_fault());
    }
#endif
}
#ifdef CONFIG_HT_SETTINGS_NVS
static nvs_fs area;
static bool request_ptt;
extern "C" ssize_t __real_nvs_write(nvs_fs *, uint16_t, const void *, size_t);

extern "C" ssize_t __wrap_nvs_write(nvs_fs *fs, uint16_t id, const void *data, size_t length) {
    if (fs->offset == area.offset) {
        if (fail_after >= 0 && calls++ == fail_after) {
            return -EIO;
        }
        const ssize_t result = __real_nvs_write(fs, id, data, length);
        if (id == 0x1fff && delay_commit_ms) {
            const auto delay = delay_commit_ms;
            delay_commit_ms = 0;
            k_sleep(K_MSEC(delay));
        } else {
            record_hook();
        }
        if (request_ptt) {
            radio_ptt(true);
            request_ptt = false;
        }
        if (fail_after_commit && id == 0x1fff && result >= 0) {
            return -EIO;
        }
        return result;
    }
    return __real_nvs_write(fs, id, data, length);
}
#else
static char root[] = "/tmp/ht-codeplug-XXXXXX";
static char path[160];
static char lock_path[160];
static bool fail_file_sync;
static bool fail_directory_sync;
static bool fail_rename;
static unsigned delay_file_sync_ms;
extern "C" ssize_t __real_write(int, const void *, size_t);

extern "C" ssize_t __wrap_write(int fd, const void *data, size_t length) {
    struct stat info;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && fail_after >= 0 && calls++ == fail_after) {
        errno = EIO;
        return -1;
    }
    const ssize_t result = __real_write(fd, data, length);
    if (!fstat(fd, &info) && S_ISREG(info.st_mode)) {
        record_hook();
    }
    return result;
}

extern "C" int __real_fsync(int);

extern "C" int __wrap_fsync(int fd) {
    struct stat info;
    if (!fstat(fd, &info) && ((fail_file_sync && S_ISREG(info.st_mode)) ||
                              (fail_directory_sync && S_ISDIR(info.st_mode)))) {
        errno = EIO;
        return -1;
    }
    if (!fstat(fd, &info)) {
        unsigned &delay = S_ISDIR(info.st_mode) ? delay_commit_ms : delay_file_sync_ms;
        if (delay) {
            const auto wait = delay;
            delay = 0;
            k_sleep(K_MSEC(wait));
        }
    }
    return __real_fsync(fd);
}

extern "C" int __real_rename(const char *, const char *);

extern "C" int __wrap_rename(const char *from, const char *to) {
    if (fail_rename) {
        errno = EIO;
        return -1;
    }
    const int result = __real_rename(from, to);
    if (fail_after_commit && result == 0) {
        errno = EIO;
        return -1;
    }
    return result;
}
#endif

static void reset(Codeplug &p) {
    p.channel_id_high_water = p.bank_id_high_water = 0;
    p.global = {};
    p.vfo = {};
    p.selection = {};
    p.channel_count = p.bank_count = 0;
}

static void remount() {
#ifdef CONFIG_HT_SETTINGS_NVS
    area.ready = false;
    zassert_ok(nvs_mount(&area));
#endif
    zassert_ok(settings_storage_init());
}

static void *setup() {
    uint32_t unused = 0;
    zassert_equal(codeplug_load(loaded, unused), -ENODEV);
#ifndef CONFIG_HT_SETTINGS_NVS
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "test", 1));
    snprintf(path, sizeof(path), "%s/test.bin", root);
    snprintf(lock_path, sizeof(lock_path), "%s/test.bin.lock", root);
#endif
    zassert_ok(settings_storage_init());
#ifdef CONFIG_HT_SETTINGS_NVS
    const flash_area *storage = nullptr;
    zassert_ok(flash_area_open(FIXED_PARTITION_ID(storage_partition), &storage));
    area.flash_device = flash_area_get_device(storage);
    area.offset = storage->fa_off + 32 * 1024;
    flash_area_close(storage);
    area.sector_size = 4096;
    area.sector_count = 64;
    zassert_ok(nvs_mount(&area));
#endif
    return nullptr;
}

static void before(void *) {
    fail_after = -1;
    calls = 0;
    fail_after_commit = false;
    delay_record_ms = delay_commit_ms = 0;
    resume_during_record = fault_during_record = false;
    radio_power(true);
    radio_ptt(false);
    zassert_ok(radio_start({}));
#ifdef CONFIG_HT_SETTINGS_NVS
    request_ptt = false;
    zassert_ok(nvs_clear(&area));
    remount();
#else
    fail_file_sync = fail_directory_sync = fail_rename = false;
    delay_file_sync_ms = 0;
    status_fault_during_record = false;
    zassert_ok(setenv("HT_PROFILE", "test", 1));
    zassert_ok(settings_storage_init());
    unlink(path);
#endif
    reset(plug);
    reset(loaded);
    bank = {};
    generation = 0;
}

static void fill(size_t count = 2, size_t banks = 2) {
    Channel c;
    strcpy(c.name, "Original");
    for (size_t i = 0; i < count; ++i) {
        c.number = i + 1;
        uint32_t id;
        zassert_ok(put_channel(plug, c, id));
    }
    strcpy(bank.name, "Local");
    bank.count = count;
    for (size_t i = 0; i < count; ++i) {
        bank.channel_ids[i] = count - i;
    }
    for (size_t i = 0; i < banks; ++i) {
        uint32_t id;
        zassert_ok(put_bank(plug, bank, id));
    }
    if (count) {
        zassert_ok(select_operating(plug, {Operating::Memory, banks ? 1U : 0U, 1}));
    }
}

static void check_old(uint32_t expected) {
    uint32_t found = 77;
    zassert_ok(codeplug_load(loaded, found));
    zassert_equal(found, expected);
    zassert_equal(strcmp(loaded.channels[0].name, "Original"), 0);
    zassert_equal(loaded.banks[0].channel_ids[0], loaded.channel_count);
    zassert_equal(loaded.selection.operating, Operating::Memory);
}

ZTEST(codeplug_storage, test_full_capacity_round_trip) {
    fill(256, 16);
    plug.global.ui = {Theme::Nord, Contrast::High, false, 50, 30, 20};
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(generation, 1);
    remount();
    check_old(1);
    zassert_equal(loaded.channel_count, 256);
    zassert_equal(loaded.bank_count, 16);
    zassert_equal(loaded.global.ui.theme, Theme::Nord);
    zassert_ok(validate_codeplug(loaded));
#ifndef CONFIG_HT_SETTINGS_NVS
    struct stat info;
    zassert_ok(stat(path, &info));
    zassert_equal(info.st_size, 40596);
#endif
}

ZTEST(codeplug_storage, test_every_small_transaction_write_boundary_preserves_old_generation) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    strcpy(plug.channels[0].name, "New");
    plug.banks[0].channel_ids[0] = 1;
    plug.banks[0].channel_ids[1] = 2;
    for (int boundary = 0; boundary < 5; ++boundary) { // Manifest + two channels + two banks.
        calls = 0;
        fail_after = boundary;
        zassert_equal(codeplug_save(plug, generation), -EIO);
        zassert_equal(generation, 1);
        fail_after = -1;
        remount();
        check_old(1);
    }
#ifdef CONFIG_HT_SETTINGS_NVS
    fail_after = 5;
    calls = 0; // All data written, final commit fails.
    zassert_equal(codeplug_save(plug, generation), -EIO);
    fail_after = -1;
    remount();
    check_old(1);
#endif
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(generation, 2);
    remount();
    uint32_t found = 0;
    zassert_ok(codeplug_load(loaded, found));
    zassert_equal(found, 2);
    zassert_equal(strcmp(loaded.channels[0].name, "New"), 0);
    zassert_equal(loaded.banks[0].channel_ids[0], 1);
}

ZTEST(codeplug_storage, test_generation_conflict_exhaustion_and_corruption) {
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);
    zassert_ok(codeplug_save(plug, generation));
    uint32_t stale = 0;
    zassert_equal(codeplug_save(plug, stale), -ESTALE);
    zassert_equal(stale, 0);
    // Modify the persisted empty generation to the maximum valid generation.
#ifdef CONFIG_HT_SETTINGS_NVS
    area.ready = false;
    zassert_ok(nvs_mount(&area)); // Refresh after writes through the backend's mount.
    const ssize_t n = nvs_read(&area, 0x2000, bytes, sizeof(bytes));
    zassert_equal(n, 100);
    sys_put_le32(UINT32_MAX, bytes + 8);
    sys_put_le32(crc32_ieee_update(crc32_ieee(bytes, 12), bytes + 16, n - 16), bytes + 12);
    zassert_equal(nvs_write(&area, 0x2000, bytes, n), n);
    uint8_t commit[16] = {'H', 'T', 'C', 'C', 1, 0, 0, 0};
    sys_put_le32(UINT32_MAX, commit + 8);
    sys_put_le32(crc32_ieee(commit, 12), commit + 12);
    zassert_equal(nvs_write(&area, 0x1fff, commit, sizeof(commit)), sizeof(commit));
#else
    int fd = open(path, O_RDWR);
    zassert_true(fd >= 0);
    zassert_equal(read(fd, bytes, 100), 100);
    sys_put_le32(UINT32_MAX, bytes + 8);
    sys_put_le32(crc32_ieee_update(crc32_ieee(bytes, 12), bytes + 16, 84), bytes + 12);
    zassert_equal(lseek(fd, 0, SEEK_SET), 0);
    zassert_equal(write(fd, bytes, 100), 100);
    zassert_ok(close(fd));
#endif
    remount();
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, UINT32_MAX);
    zassert_equal(codeplug_save(plug, generation), -EOVERFLOW);
    bytes[27] ^= 1;
#ifdef CONFIG_HT_SETTINGS_NVS
    zassert_equal(nvs_write(&area, 0x2000, bytes, 100), 100);
#else
    fd = open(path, O_WRONLY);
    zassert_true(fd >= 0);
    zassert_equal(write(fd, bytes, 100), 100);
    zassert_ok(close(fd));
#endif
    uint32_t unchanged = 88;
    zassert_equal(codeplug_load(loaded, unchanged), -EBADMSG);
    zassert_equal(unchanged, 88);
}

ZTEST(codeplug_storage, test_full_capacity_interrupted_staging_preserves_complete_old_store) {
    fill(256, 16);
    zassert_ok(codeplug_save(plug, generation));
    strcpy(plug.channels[0].name, "New full store");
    const int boundaries[] = {0, 1, 128, 256, 257, 272};
    for (int boundary : boundaries) {
        calls = 0;
        fail_after = boundary;
        zassert_equal(codeplug_save(plug, generation), -EIO);
        zassert_equal(generation, 1);
        fail_after = -1;
        remount();
        check_old(1);
        zassert_equal(loaded.channel_count, 256);
        zassert_equal(loaded.bank_count, 16);
        zassert_equal(loaded.banks[15].count, 256);
    }
#ifdef CONFIG_HT_SETTINGS_NVS
    calls = 0;
    fail_after = 273;
    zassert_equal(codeplug_save(plug, generation), -EIO);
    fail_after = -1;
    remount();
    check_old(1);
#endif
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(generation, 2);
    remount();
    uint32_t found = 0;
    zassert_ok(codeplug_load(loaded, found));
    zassert_equal(strcmp(loaded.channels[0].name, "New full store"), 0);
}

ZTEST(codeplug_storage, test_visible_commit_error_advances_generation_and_can_retry) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    fail_after_commit = true;
    zassert_equal(codeplug_save(plug, generation), -EIO);
    fail_after_commit = false;
    zassert_equal(generation, 2);
    remount();
    check_old(2);
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(generation, 3);
}

#ifdef CONFIG_HT_SETTINGS_NVS
ZTEST(codeplug_storage, test_full_capacity_gc_repeated_replacement_and_remount) {
    fill(256, 16);
    for (uint32_t i = 1; i <= 12; ++i) {
        plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 433500000 + i * 10;
        zassert_ok(codeplug_save(plug, generation));
        zassert_equal(generation, i);
    }
    remount();
    check_old(12);
    zassert_equal(loaded.vfo.rx_frequency_hz, 433500120);
    printk(
        "NVS full-capacity replacement: 12 generations, >475 KiB record writes in 256 KiB ring\n");
}

ZTEST(codeplug_storage, test_flash_reservation_and_ptt_during_staging) {
    fill();
    radio_ptt(true);
    zassert_equal(codeplug_save(plug, generation), -EAGAIN);
    zassert_equal(generation, 0);
    radio_ptt(false);
    radio_service();
    zassert_ok(codeplug_save(plug, generation));
    request_ptt = true;
    zassert_equal(codeplug_save(plug, generation), -EAGAIN);
    zassert_equal(generation, 1);
    radio_ptt(false);
    radio_service();
    remount();
    check_old(1);
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(generation, 2);
}

ZTEST(codeplug_storage, test_full_codeplug_garbage_collection_and_remount) {
    fill(256, 16);
    for (unsigned i = 0; i < 8; ++i) {
        plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 430000000 + i * 10;
        zassert_ok(codeplug_save(plug, generation));
    }
    remount();
    check_old(8);
    zassert_equal(loaded.vfo.rx_frequency_hz, 430000070);
}
#else
ZTEST(codeplug_storage, test_sync_and_rename_failures_preserve_and_report_durability) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    fail_file_sync = true;
    zassert_equal(codeplug_save(plug, generation), -EIO);
    fail_file_sync = false;
    zassert_equal(generation, 1);
    check_old(1);
    fail_rename = true;
    zassert_equal(codeplug_save(plug, generation), -EIO);
    fail_rename = false;
    zassert_equal(generation, 1);
    check_old(1);
    fail_directory_sync = true;
    zassert_equal(codeplug_save(plug, generation), -EIO);
    fail_directory_sync = false;
    zassert_equal(generation, 2);
    check_old(2);
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(generation, 3);
}

ZTEST(codeplug_storage, test_truncation_trailing_data_and_profile_isolation) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    zassert_ok(setenv("HT_PROFILE", "other", 1));
    zassert_ok(settings_storage_init());
    uint32_t other = 0;
    zassert_equal(codeplug_load(loaded, other), -ENOENT);
    zassert_ok(codeplug_save(plug, other));
    zassert_ok(setenv("HT_PROFILE", "test", 1));
    zassert_ok(settings_storage_init());
    check_old(1);
    int fd = open(path, O_WRONLY | O_APPEND);
    zassert_true(fd >= 0);
    zassert_equal(write(fd, "X", 1), 1);
    zassert_ok(close(fd));
    zassert_equal(codeplug_load(loaded, generation), -EBADMSG);
    zassert_equal(generation, 1);
    zassert_ok(truncate(path, 10));
    zassert_equal(codeplug_load(loaded, generation), -EBADMSG);
}

ZTEST(codeplug_storage, test_another_process_cannot_lock_running_profile) {
    const pid_t child = fork();
    zassert_true(child >= 0);
    if (!child) {
        const int fd = open(lock_path, O_RDWR);
        const bool refused = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0 &&
                             (errno == EWOULDBLOCK || errno == EAGAIN);
        if (fd >= 0) {
            close(fd);
        }
        _exit(refused ? 0 : 1);
    }
    int status = -1;
    zassert_equal(waitpid(child, &status, 0), child);
    zassert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif
static CodeplugSavePolicy inactive_policy(unsigned budget = 20) {
    radio_power(false);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    return {k_uptime_get() + budget, SaveRadioState::Inactive};
}

ZTEST(codeplug_storage, test_shutdown_deadline_before_open_retains_committed_generation) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    const auto policy = inactive_policy();
    k_sleep(K_MSEC(30));
    zassert_equal(codeplug_save(plug, generation, policy), -ETIMEDOUT);
    zassert_equal(generation, 1);
    remount();
    check_old(1);
}

ZTEST(codeplug_storage, test_shutdown_deadline_between_full_capacity_records_aborts_session) {
    fill(256, 16);
    zassert_ok(codeplug_save(plug, generation));
    strcpy(plug.channels[255].name, "Changed");
    const auto policy = inactive_policy();
    delay_record_ms = 30;
    zassert_equal(codeplug_save(plug, generation, policy), -ETIMEDOUT);
    zassert_equal(generation, 1);
    remount();
    check_old(1);
    zassert_equal(strcmp(loaded.channels[255].name, "Original"), 0);
    radio_power(true);
    radio_service();
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(generation, 2);
    remount();
    check_old(2);
    zassert_equal(strcmp(loaded.channels[255].name, "Changed"), 0);
}

ZTEST(codeplug_storage, test_shutdown_resume_or_pending_fault_cancels_unpublished_records) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    auto policy = inactive_policy();
    resume_during_record = true;
    zassert_equal(codeplug_save(plug, generation, policy), -ECANCELED);
    zassert_equal(generation, 1);
    remount();
    check_old(1);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    policy = inactive_policy();
    fault_during_record = true;
    zassert_equal(codeplug_save(plug, generation, policy), -ECANCELED);
    zassert_equal(generation, 1);
    remount();
    check_old(1);
}

ZTEST(codeplug_storage, test_shutdown_late_publication_reconciles_visible_generation) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 145600000;
    auto policy = inactive_policy();
#ifndef CONFIG_HT_SETTINGS_NVS
    delay_file_sync_ms = 30; // expiry after staging fsync must precede rename
    zassert_equal(codeplug_save(plug, generation, policy), -ETIMEDOUT);
    zassert_equal(generation, 1);
    remount();
    check_old(1);
    policy = {k_uptime_get() + 20, SaveRadioState::Inactive};
#endif
    delay_commit_ms = 30; // actual NVS commit or Linux directory sync already ran
    zassert_equal(codeplug_save(plug, generation, policy), -ETIMEDOUT);
    zassert_equal(generation, 2);
    remount();
    check_old(2);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145600000);
}
#ifndef CONFIG_HT_SETTINGS_NVS
ZTEST(codeplug_storage, test_active_save_cancels_on_controller_origin_status_fault) {
    fill();
    zassert_ok(codeplug_save(plug, generation));
    status_fault_during_record = true;
    const CodeplugSavePolicy policy{0, SaveRadioState::Active};
    zassert_equal(codeplug_save(plug, generation, policy), -ECANCELED);
    zassert_equal(generation, 1);
    remount();
    check_old(1);
    zassert_equal(radio_latched_fault(), -EPIPE);
    zassert_ok(radio_pending_fault());
    zassert_ok(radio_start({}));
    zassert_ok(radio_latched_fault());
}
#endif
ZTEST_SUITE(codeplug_storage, nullptr, setup, before, nullptr, nullptr);
