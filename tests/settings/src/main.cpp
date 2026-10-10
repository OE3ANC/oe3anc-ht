// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include <ht/codeplug_records.hpp>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
static uint32_t generation;
static uint8_t bytes[codeplug_record_max];
static CodeplugManifest manifest;
static bool fail_durable;
static unsigned shutdown_delay_ms;
static bool defer_after_commit, off_during_save;
#ifdef CONFIG_HT_SETTINGS_NVS
static nvs_fs area;
extern "C" ssize_t __real_nvs_write(nvs_fs *, uint16_t, const void *, size_t);

extern "C" ssize_t __wrap_nvs_write(nvs_fs *fs, uint16_t id, const void *data, size_t length) {
    if (fail_durable && fs->offset == area.offset && id == 0x1fff) {
        return -EIO;
    }
    const ssize_t result = __real_nvs_write(fs, id, data, length);
    if (off_during_save && fs->offset == area.offset) {
        off_during_save = false;
        radio_power(false);
    }
    if (defer_after_commit && fs->offset == area.offset && id == 0x1fff && result >= 0) {
        defer_after_commit = false;
        return -EAGAIN;
    }
    if (shutdown_delay_ms && fs->offset == area.offset) {
        const auto delay = shutdown_delay_ms;
        shutdown_delay_ms = 0;
        k_sleep(K_MSEC(delay));
    }
    return result;
}
#else
static char root[] = "/tmp/ht-settings-XXXXXX";
static char path[160];
extern "C" int __real_fsync(int);

extern "C" int __wrap_fsync(int fd) {
    struct stat info;
    if (fail_durable && !fstat(fd, &info) && S_ISDIR(info.st_mode)) {
        errno = EIO;
        return -1; // Replacement is visible but durability is uncertain.
    }
    if (shutdown_delay_ms && !fstat(fd, &info) && S_ISREG(info.st_mode)) {
        const auto delay = shutdown_delay_ms;
        shutdown_delay_ms = 0;
        k_sleep(K_MSEC(delay));
    }
    if (off_during_save && !fstat(fd, &info) && S_ISREG(info.st_mode)) {
        off_during_save = false;
        radio_power(false);
    }
    const int result = __real_fsync(fd);
    if (!result && defer_after_commit && !fstat(fd, &info) && S_ISDIR(info.st_mode)) {
        defer_after_commit = false;
        errno = EAGAIN;
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

static void *setup() {
#ifndef CONFIG_HT_SETTINGS_NVS
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "test", 1));
    snprintf(path, sizeof(path), "%s/test.bin", root);
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
    fail_durable = defer_after_commit = off_during_save = false;
    shutdown_delay_ms = 0;
    radio_power(true);
    zassert_ok(radio_start({}));
#ifdef CONFIG_HT_SETTINGS_NVS
    area.ready = false;
    zassert_ok(nvs_mount(&area));
    zassert_ok(nvs_clear(&area));
#else
    unlink(path);
#endif
    RadioConfig config;
    zassert_equal(settings_start(config), -ENOENT);
    zassert_ok(radio_start(config));
    reset(plug);
    reset(loaded);
    bank = {};
    generation = 0;
}

static void configure(RadioConfig config, int64_t now = 0) {
    RadioCommand command;
    command.config = config;
    zassert_ok(radio_submit(command));
    radio_service();
    zassert_ok(radio_snapshot().command_error);
    settings_service(radio_snapshot(), now);
}

static void restart(RadioConfig &config) {
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_false(emulator_transmitting());
}

ZTEST(settings, test_split_inhibit_round_trip) {
    RadioConfig config;
    config.rx_frequency_hz = 439075000;
    config.tx_frequency_hz = 431475000;
    config.tx_inhibit = true;
    config.rx_tone = {ToneKind::Ctcss, 885, false};
    config.tx_tone = {ToneKind::Ctcss, 1000, false};
    strcpy(config.callsign, "OE3ANC");
    configure(config);
    settings_service(radio_snapshot(), 9999);
    uint32_t gen = 0;
    zassert_equal(codeplug_load(loaded, gen), -ENOENT);
    settings_service(radio_snapshot(), 10000);
    restart(config);
    zassert_equal(config.rx_frequency_hz, 439075000);
    zassert_equal(config.tx_frequency_hz, 431475000);
    zassert_true(config.tx_inhibit);
    zassert_equal(config.rx_tone.value, 885);
    zassert_equal(config.tx_tone.value, 1000);
    radio_ptt(true);
    radio_service();
    zassert_equal(radio_snapshot().ptt_error, -EPERM);
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
}

ZTEST(settings, test_autosave_waits_ten_seconds_after_last_change) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    settings_service(radio_snapshot(), 8999);
    zassert_true(settings_status().pending);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);

    config.rx_frequency_hz = config.tx_frequency_hz = 145600000;
    configure(config, 9000);
    // The old deadline and unchanged service ticks must not trigger a save.
    settings_service(radio_snapshot(), 10000);
    settings_service(radio_snapshot(), 18999);
    zassert_true(settings_status().pending);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);

    settings_service(radio_snapshot(), 19000);
    zassert_false(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145600000);
    settings_service(radio_snapshot(), 30000);
    zassert_equal(settings_status().generation, 1);
}

ZTEST(settings, test_revert_debounce_and_diagnostic_exclusion) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config);
    configure(RadioConfig{}, 500);
    settings_service(radio_snapshot(), 15000);
    zassert_false(settings_status().pending);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);
    configure(config, 20000);
    settings_service(radio_snapshot(), 30000);
    zassert_ok(codeplug_load(loaded, generation));
    const uint32_t previous = generation;
    RadioCommand command;
    command.kind = CommandKind::EnterDiagnostics;
    zassert_ok(radio_submit(command));
    radio_service();
    command.kind = CommandKind::WriteRegister;
    command.register_address = 0x40;
    command.register_value = 0xbeef;
    zassert_ok(radio_submit(command));
    radio_service();
    settings_service(radio_snapshot(), 40000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, previous);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
}

ZTEST(settings, test_full_codeplug_selection_and_temporary_tuning) {
    Channel c;
    strcpy(c.name, "Repeater");
    c.configuration.rx_frequency_hz = 439075000;
    c.configuration.tx_frequency_hz = 431475000;
    for (size_t i = 0; i < channel_capacity; ++i) {
        c.number = i + 1;
        uint32_t id;
        zassert_ok(put_channel(plug, c, id));
    }
    strcpy(bank.name, "Local");
    bank.count = channel_capacity;
    for (size_t i = 0; i < channel_capacity; ++i) {
        bank.channel_ids[i] = i + 1;
    }
    for (size_t i = 0; i < bank_capacity; ++i) {
        uint32_t id;
        zassert_ok(put_bank(plug, bank, id));
    }
    zassert_ok(select_operating(plug, {Operating::Memory, 1, 17}));
    plug.global.ui.theme = Theme::Nord;
    plug.global.ui.contrast = Contrast::High;
    plug.global.transmit_limit_s = 60;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    restart(config);
    zassert_equal(config.rx_frequency_hz, 439075000);
    zassert_equal(config.tx_frequency_hz, 431475000);
    zassert_equal(config.transmit_limit_s, 60);
    strcpy(config.callsign, "OE3ANC");
    config.transmit_limit_s = 120; // Global edit must not retune a memory into VFO.
    configure(config);
    settings_service(radio_snapshot(), 10000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.selection.operating, Operating::Memory);
    zassert_equal(loaded.selection.channel_id, 17);
    zassert_equal(loaded.vfo.rx_frequency_hz, 433500000);
    zassert_equal(loaded.channel_count, channel_capacity);
    zassert_equal(loaded.bank_count, bank_capacity);
    zassert_equal(loaded.global.ui.theme, Theme::Nord);
    zassert_equal(loaded.global.ui.contrast, Contrast::High);
    zassert_equal(loaded.global.transmit_limit_s, 120);
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 20000);
    config.rx_frequency_hz = 439075000;
    config.tx_frequency_hz = 431475000;
    configure(config, 25000); // Reverting tuning still intentionally enters VFO.
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 35000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.selection.operating, Operating::Vfo);
    zassert_equal(loaded.selection.channel_id, 17);
    zassert_equal(loaded.selection.bank_id, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, 439075000);
    zassert_equal(loaded.vfo.tx_frequency_hz, 431475000);
    zassert_equal(loaded.channels[16].configuration.tx_frequency_hz, 431475000);
    zassert_equal(loaded.channel_id_high_water, channel_capacity);
    zassert_equal(loaded.bank_id_high_water, bank_capacity);
}

ZTEST(settings, test_transmit_limit_is_global_debounced_and_persisted) {
    RadioConfig config;
    const uint16_t limits[] = {0, 60, 120, 180};
    for (size_t i = 0; i < 4; ++i) {
        config.transmit_limit_s = limits[i];
        configure(config, i * 20000);
        zassert_true(settings_status().pending);
        settings_service(radio_snapshot(), i * 20000 + 9999);
        zassert_true(settings_status().pending);
        settings_service(radio_snapshot(), i * 20000 + 10000);
        zassert_false(settings_status().pending);
        restart(config);
        zassert_equal(config.transmit_limit_s, limits[i]);
        zassert_ok(codeplug_load(loaded, generation));
        zassert_equal(loaded.global.transmit_limit_s, limits[i]);
    }
}

ZTEST(settings, test_monitor_never_changes_persisted_normal_settings) {
    RadioConfig config;
    config.rx_tone = {ToneKind::Ctcss, 1000, false};
    configure(config);
    settings_service(radio_snapshot(), 10000);
    zassert_ok(codeplug_load(loaded, generation));
    const uint32_t saved_generation = generation;
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    settings_service(radio_snapshot(), 20000);
    radio_monitor(false);
    radio_service();
    settings_service(radio_snapshot(), 30000);
    zassert_false(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, saved_generation);
    zassert_equal(loaded.vfo.rx_tone.value, 1000);
    zassert_equal(loaded.vfo.squelch, config.squelch);
}

static void corrupt_manifest(bool version) {
    size_t length = 0;
    uint32_t next = generation;
    zassert_ok(codeplug_store_open(false, next));
    zassert_ok(codeplug_store_read(StoreRecord::Manifest, 0, bytes, sizeof(bytes), length));
    bool published = false;
    zassert_ok(codeplug_store_close(false, published));
    if (version) {
        bytes[4] = 99;
        uint32_t crc = crc32_ieee(bytes, 12);
        crc = crc32_ieee_update(crc, bytes + 16, length - 16);
        sys_put_le32(crc, bytes + 12);
    } else {
        bytes[16] ^= 1;
    }
    zassert_ok(codeplug_store_open(true, next));
    // Update generation and CRC for a new committed envelope, then corrupt it.
    sys_put_le32(next, bytes + 8);
    uint32_t crc = crc32_ieee(bytes, 12);
    crc = crc32_ieee_update(crc, bytes + 16, length - 16);
    sys_put_le32(crc, bytes + 12);
    if (!version) {
        bytes[16] ^= 2;
    }
    zassert_ok(codeplug_store_write(StoreRecord::Manifest, 0, bytes, length));
    zassert_ok(codeplug_store_close(true, published));
    generation = next;
}

ZTEST(settings, test_damaged_or_unknown_store_is_read_only) {
    const bool versions[] = {false, true};
    for (bool version : versions) {
        if (version) {
            before(nullptr);
        }
        zassert_ok(codeplug_save(plug, generation));
        corrupt_manifest(version);
        RadioConfig config;
        const int error = version ? -ENOTSUP : -EBADMSG;
        zassert_equal(settings_start(config), error);
        zassert_true(settings_status().read_only);
        zassert_equal(config.rx_frequency_hz, 430000000);
        zassert_ok(radio_start(config));
        config.rx_frequency_hz = config.tx_frequency_hz = 145600000;
        configure(config);
        settings_service(radio_snapshot(), 10000);
        zassert_equal(codeplug_load(loaded, generation), error);
    }
}

ZTEST(settings, test_unsupported_stored_features_are_preserved) {
    plug.global.gain = 1; // Backend has no gain control; preserve unsupported data.
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    zassert_equal(settings_start(config), -ENOTSUP);
    zassert_true(settings_status().read_only);
    zassert_ok(radio_start(config));
    config.rx_frequency_hz = config.tx_frequency_hz = 145600000;
    configure(config);
    settings_service(radio_snapshot(), 10000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.global.gain, 1);
}

ZTEST(settings, test_dcs_load_polarity_only_edit_and_mixed_tones_round_trip) {
    plug.vfo.rx_tone = {ToneKind::Dcs, 0023, false};
    plug.vfo.tx_tone = {ToneKind::Dcs, 0754, true};
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    restart(config);
    zassert_true(same_tone(config.rx_tone, plug.vfo.rx_tone));
    zassert_true(same_tone(config.tx_tone, plug.vfo.tx_tone));
    config.rx_tone.inverted = true;
    configure(config, 100);
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 11000);
    restart(config);
    zassert_true(config.rx_tone.inverted);
    zassert_equal(settings_status().generation, 2);
    config.tx_tone = {ToneKind::Ctcss, 885, false};
    configure(config, 12000);
    settings_service(radio_snapshot(), 22000);
    restart(config);
    zassert_equal(config.rx_tone.kind, ToneKind::Dcs);
    zassert_equal(config.tx_tone.kind, ToneKind::Ctcss);
    zassert_equal(config.tx_tone.value, 885);
    config.rx_tone = {};
    configure(config, 23000);
    settings_service(radio_snapshot(), 33000);
    restart(config);
    zassert_true(same_tone(config.rx_tone, Tone{}));
}

ZTEST(settings, test_generation_conflict_protects_external_data) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config);
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 145600000;
    zassert_ok(codeplug_save(plug, generation));
    settings_service(radio_snapshot(), 10000);
    zassert_equal(settings_status().save_error, -ESTALE);
    zassert_true(settings_status().read_only);
    config.rx_frequency_hz = config.tx_frequency_hz = 145700000;
    configure(config, 20000);
    settings_service(radio_snapshot(), 50000);
    zassert_equal(settings_status().save_error, -ESTALE);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145600000);
}

ZTEST(settings, test_fault_snapshot_and_ptt_do_not_overwrite_saved_data) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config);
    radio_ptt(true);
    radio_service();
    settings_service(radio_snapshot(), 10000);
#ifdef CONFIG_HT_SETTINGS_NVS
    zassert_true(settings_status().pending);
    zassert_ok(settings_status().save_error);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);
#endif
    radio_ptt(false);
    radio_service();
    settings_service(radio_snapshot(), 10500);
    zassert_false(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    const uint32_t previous = generation;
    RadioState fault;
    fault.phase = RadioPhase::Fault;
    settings_service(fault, 30000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, previous);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
}

ZTEST(settings, test_missing_committed_channel_is_not_an_absent_database) {
    Channel c;
    strcpy(c.name, "Missing");
    uint32_t id;
    zassert_ok(put_channel(plug, c, id));
    zassert_ok(make_manifest(plug, manifest));
    uint32_t next = 0;
    zassert_ok(codeplug_store_open(true, next));
    size_t length;
    zassert_ok(encode_manifest(manifest, next, bytes, sizeof(bytes), length));
    zassert_ok(codeplug_store_write(StoreRecord::Manifest, 0, bytes, length));
    // Deliberately publish a damaged generation missing its referenced channel.
    bool published = false;
    zassert_ok(codeplug_store_close(true, published));
    zassert_true(published);
    RadioConfig config;
    zassert_equal(settings_start(config), -EBADMSG);
    zassert_true(settings_status().read_only);
    zassert_ok(radio_start(config));
    config.rx_frequency_hz = config.tx_frequency_hz = 145700000;
    configure(config);
    settings_service(radio_snapshot(), 10000);
    zassert_equal(codeplug_load(loaded, generation), -EBADMSG);
}

ZTEST(settings, test_m17_destination_can_configuration_and_reboot) {
    RadioConfig config;
    config.mode = Mode::M17;
    strcpy(config.callsign, "OE3ANC");
    config.m17.destination = Destination::Station;
    strcpy(config.m17.callsign, "OE1TEST");
    config.m17.can = 15;
    config.m17.rx_can_check = true;
    configure(config);
    settings_service(radio_snapshot(), 10000);
    restart(config);
    zassert_equal(config.mode, Mode::M17);
    zassert_equal(config.m17.destination, Destination::Station);
    zassert_equal(strcmp(config.m17.callsign, "OE1TEST"), 0);
    zassert_equal(config.m17.can, 15);
    zassert_true(config.m17.rx_can_check);
    config.m17.can = 7;
    configure(config, 20000);
    settings_service(radio_snapshot(), 30000);
    restart(config);
    zassert_equal(config.m17.can, 7);
    zassert_equal(strcmp(config.m17.callsign, "OE1TEST"), 0);
    config.m17 = {};
    configure(config, 40000);
    settings_service(radio_snapshot(), 50000);
    restart(config);
    zassert_equal(config.m17.destination, Destination::Broadcast);
    zassert_equal(config.m17.can, 0);
    zassert_false(config.m17.rx_can_check);
    zassert_equal(config.m17.callsign[0], 0);
}

ZTEST(settings, test_inactive_save_preserves_committed_config_without_rf_restart) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    zassert_true(settings_status().pending);
    radio_power(false);
    radio_service();
    const auto inactive = radio_snapshot();
    zassert_equal(inactive.phase, RadioPhase::Inactive);
    settings_service(inactive, 10000);
    zassert_false(settings_status().pending);
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    RadioConfig restored;
    zassert_ok(settings_start(restored));
    zassert_equal(restored.rx_frequency_hz, 145500000);
    zassert_ok(radio_start(restored));
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    radio_power(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
}

ZTEST(settings, test_shutdown_bypasses_debounce_and_captures_final_unobserved_config) {
    RadioCommand command;
    command.config.rx_frequency_hz = command.config.tx_frequency_hz = 145600000;
    zassert_ok(radio_submit(command));
    radio_service(); // settings owner has not observed RX
    const auto sequence = radio_snapshot().shutdown_sequence;
    radio_power(false);
    radio_service();
    zassert_equal(radio_snapshot().shutdown_sequence, sequence + 1);
    settings_service(radio_snapshot(), 100);
    zassert_false(settings_status().pending);
    zassert_false(settings_status().shutdown_pending);
    zassert_ok(settings_status().shutdown_error);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145600000);
    zassert_false(emulator_transmitting());
}

ZTEST(settings, test_cold_inactive_start_does_not_write_defaults) {
    radio_power(false);
    radio_service();
    RadioConfig config;
    zassert_equal(settings_start(config), -ENOENT);
    zassert_ok(radio_start(config));
    settings_service(radio_snapshot(), 100000);
    zassert_false(settings_status().pending);
    zassert_false(settings_status().shutdown_pending);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);
    radio_power(true);
    radio_service();
    settings_service(radio_snapshot(), 110000);
    zassert_false(settings_status().pending);
}

ZTEST(settings, test_shutdown_expiry_stops_retries_and_resume_restores_normal_save) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    settings_service(radio_snapshot(), 10000);
    config.rx_frequency_hz = config.tx_frequency_hz = 145600000;
    configure(config, 11000);
    radio_power(false);
    radio_service();
    shutdown_delay_ms = CONFIG_HT_SHUTDOWN_SAVE_BUDGET_MS + 10;
    settings_service(radio_snapshot(), 12000);
    zassert_true(settings_status().pending);
    zassert_false(settings_status().shutdown_pending);
    zassert_equal(settings_status().shutdown_error, -ETIMEDOUT);
    zassert_equal(settings_status().save_error, -ETIMEDOUT);
    const auto previous = settings_status().generation;
    settings_service(radio_snapshot(), 200000);
    zassert_equal(settings_status().generation, previous);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
    radio_power(true);
    radio_service();
    settings_service(radio_snapshot(), 210000);
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 219999);
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 220000);
    zassert_false(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 2);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145600000);
}

ZTEST(settings, test_shutdown_io_error_is_terminal_until_resume) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    radio_power(false);
    radio_service();
    fail_durable = true;
    settings_service(radio_snapshot(), 100);
    zassert_equal(settings_status().shutdown_error, -EIO);
    zassert_true(settings_status().pending);
    zassert_false(settings_status().shutdown_pending);
    const auto previous = settings_status().generation;
    fail_durable = false;
    settings_service(radio_snapshot(), 200000);
    zassert_equal(settings_status().generation, previous);
    zassert_true(settings_status().pending);
    radio_power(true);
    radio_service();
    settings_service(radio_snapshot(), 210000);
    settings_service(radio_snapshot(), 220000);
    zassert_false(settings_status().pending);
}

ZTEST(settings, test_shutdown_fault_and_readonly_preserve_committed_data) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    settings_service(radio_snapshot(), 10000);
    config.rx_frequency_hz = config.tx_frequency_hz = 145600000;
    configure(config, 11000);
    BackendStatus status;
    status.error = -EPIPE;
    emulator_inject(status);
    radio_service();
    zassert_ok(radio_pending_fault());
    zassert_equal(radio_latched_fault(), -EPIPE);
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 12000);
    zassert_equal(settings_status().shutdown_error, -ECANCELED);
    zassert_true(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
    before(nullptr);
    plug.global.gain = 1; // Backend has no gain control; preserve unsupported data.
    zassert_ok(codeplug_save(plug, generation));
    zassert_equal(settings_start(config), -ENOTSUP);
    zassert_ok(radio_start(config));
    config.rx_frequency_hz = config.tx_frequency_hz = 145700000;
    configure(config, 0);
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 100);
    zassert_equal(settings_status().shutdown_error, -ENOTSUP);
    zassert_true(settings_status().read_only);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.global.gain, 1);
}

ZTEST(settings, test_coalesced_off_on_cancels_shutdown_without_lost_sequence) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    const auto previous = radio_snapshot().shutdown_sequence;
    radio_power(false);
    radio_power(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_equal(radio_snapshot().shutdown_sequence, previous + 1);
    settings_service(radio_snapshot(), 100);
    zassert_equal(settings_status().shutdown_error, -ECANCELED);
    zassert_false(settings_status().shutdown_pending);
    zassert_true(settings_status().pending);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);
    settings_service(radio_snapshot(), 10000);
    zassert_false(settings_status().pending);
}

ZTEST(settings, test_late_shutdown_observation_does_not_extend_stop_deadline) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    radio_power(false);
    radio_service();
    const auto inactive = radio_snapshot();
    k_sleep(K_MSEC(CONFIG_HT_SHUTDOWN_SAVE_BUDGET_MS + 10));
    settings_service(inactive, 100);
    zassert_equal(settings_status().shutdown_error, -ETIMEDOUT);
    zassert_true(settings_status().pending);
    zassert_false(settings_status().shutdown_pending);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);
}

ZTEST(settings, test_ordinary_save_cancels_on_off_then_inactive_flushes) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    settings_service(radio_snapshot(), 10000);
    config.rx_frequency_hz = config.tx_frequency_hz = 145600000;
    configure(config, 11000);
    off_during_save = true;
    settings_service(radio_snapshot(), 21000);
    zassert_equal(settings_status().save_error, -ECANCELED);
    zassert_true(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
    radio_service();
    settings_service(radio_snapshot(), 21010);
    zassert_false(settings_status().pending);
    zassert_ok(settings_status().shutdown_error);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 2);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145600000);
}

ZTEST(settings, test_deferred_visible_commit_updates_status_generation_before_expiry) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    radio_power(false);
    radio_service();
    defer_after_commit = true;
    settings_service(radio_snapshot(), 100);
    zassert_equal(settings_status().generation, 1);
    zassert_true(settings_status().pending);
    zassert_true(settings_status().shutdown_pending);
    k_sleep(K_MSEC(CONFIG_HT_SHUTDOWN_SAVE_BUDGET_MS + 10));
    settings_service(radio_snapshot(), 200000);
    zassert_equal(settings_status().shutdown_error, -ETIMEDOUT);
    zassert_equal(settings_status().generation, 1);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    radio_power(true);
    radio_service();
    settings_service(radio_snapshot(), 210000);
    settings_service(radio_snapshot(), 220000);
    zassert_false(settings_status().pending);
    zassert_equal(settings_status().generation, 2);
}
#ifdef CONFIG_HT_SETTINGS_NVS
static K_SEM_DEFINE(reservation_ready, 0, 1);
static K_SEM_DEFINE(reservation_release, 0, 1);
K_THREAD_STACK_DEFINE(reservation_stack, 1024);
static struct k_thread reservation_thread;

static void hold_reservation(void *, void *, void *) {
    zassert_true(radio_idle_lock(true));
    k_sem_give(&reservation_ready);
    k_sem_take(&reservation_release, K_FOREVER);
    radio_idle_unlock();
}

ZTEST(settings, test_inactive_busy_flash_defers_only_inside_budget) {
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    radio_power(false);
    radio_service();
    const auto inactive = radio_snapshot();
    k_thread_create(&reservation_thread, reservation_stack,
                    K_THREAD_STACK_SIZEOF(reservation_stack), hold_reservation, nullptr, nullptr,
                    nullptr, 1, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&reservation_ready, K_MSEC(100)));
    settings_service(inactive, 100);
    zassert_true(settings_status().shutdown_pending);
    zassert_ok(settings_status().shutdown_error);
    k_sleep(K_MSEC(CONFIG_HT_SHUTDOWN_SAVE_BUDGET_MS + 10));
    settings_service(inactive, 200000);
    zassert_false(settings_status().shutdown_pending);
    zassert_equal(settings_status().shutdown_error, -ETIMEDOUT);
    k_sem_give(&reservation_release);
    zassert_ok(k_thread_join(&reservation_thread, K_MSEC(100)));
    settings_service(inactive, 300000);
    zassert_true(settings_status().pending);
    zassert_equal(codeplug_load(loaded, generation), -ENOENT);
}
#endif
static void recall_fixture() {
    Channel channel;
    strcpy(channel.name, "Repeater");
    channel.configuration.rx_frequency_hz = 439075000;
    channel.configuration.tx_frequency_hz = 431475000;
    uint32_t id;
    zassert_ok(put_channel(plug, channel, id));
    channel.number = 2;
    strcpy(channel.name, "Same RF, other identity");
    zassert_ok(put_channel(plug, channel, id));
    strcpy(bank.name, "Local");
    bank.count = 2;
    bank.channel_ids[0] = 2;
    bank.channel_ids[1] = 1;
    zassert_ok(put_bank(plug, bank, id));
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    restart(config);
}

ZTEST(settings, test_copied_channel_position_number_and_bank_order) {
    recall_fixture();
    uint16_t position = 99;
    zassert_ok(settings_channel_position(0, 1, position));
    zassert_equal(position, 0);
    zassert_ok(settings_channel_position(0, 2, position));
    zassert_equal(position, 1);
    zassert_ok(settings_channel_position(1, 2, position));
    zassert_equal(position, 0);
    zassert_ok(settings_channel_position(1, 1, position));
    zassert_equal(position, 1);
    zassert_equal(settings_channel_position(0, 0, position), -ENOENT);
    zassert_equal(position, 1);
    zassert_equal(settings_channel_position(999, 1, position), -ENOENT);
    zassert_equal(position, 1);
    zassert_equal(settings_channel_position(1, 999, position), -ENOENT);
    zassert_equal(position, 1);
    zassert_false(settings_status().operation_pending); // Readback has no RF/storage work.
}

ZTEST(settings, test_recall_copied_ram_revision_after_metadata_ack) {
    recall_fixture();
    const auto old_revision = settings_status().revision;
    UiPreferences preferences;
    settings_ui_preferences(preferences);
    preferences.contrast = Contrast::High;
    zassert_ok(settings_put_ui_preferences(preferences, 91, radio_snapshot(), old_revision));
    settings_service(radio_snapshot(), 0);
    radio_service();
    const auto expected = radio_snapshot(); // RF metadata authorization is already current.
    settings_service(expected, 1);
    zassert_false(settings_status().operation_pending);
    zassert_true(settings_status().revision != old_revision);
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 92, expected, old_revision));
    settings_service(expected, 2);
    zassert_false(settings_status().operation_pending);
    zassert_equal(settings_status().operation_error, -ESTALE);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    zassert_ok(
        settings_recall({Operating::Memory, 1, 1}, 93, expected, settings_status().revision));
    settings_service(expected, 3);
    radio_service();
    settings_service(radio_snapshot(), 4);
    zassert_ok(settings_status().operation_error);
    zassert_equal(radio_snapshot().selection.channel_id, 1);
}

static void recall_complete(const Selection &selection, uint32_t id, int64_t now = 0) {
    zassert_ok(settings_recall(selection, id, radio_snapshot()));
    zassert_true(settings_status().operation_pending);
    settings_service(radio_snapshot(), now);
    radio_service();
    settings_service(radio_snapshot(), now + 1);
    zassert_equal(settings_status().operation_id, id);
    zassert_false(settings_status().operation_pending);
    zassert_ok(settings_status().operation_error);
    zassert_true(same_selection(radio_snapshot().selection, selection));
    zassert_true(same_selection(settings_status().selection, selection));
}

ZTEST(settings, test_recall_identity_vfo_preservation_and_copied_readbacks) {
    recall_fixture();
    zassert_equal(settings_status().channel_count, 2);
    zassert_equal(settings_status().bank_count, 1);
    recall_complete({Operating::Memory, 1, 1}, 11);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 439075000);
    OperatingConfig vfo;
    settings_vfo(vfo);
    zassert_equal(vfo.rx_frequency_hz, 433500000);
    Channel copy;
    zassert_ok(settings_channel(1, copy));
    copy.configuration.squelch = 15;
    strcpy(copy.name, "Changed draft");
    zassert_ok(settings_channel(1, copy));
    zassert_equal(copy.configuration.squelch, 4);
    zassert_equal(strcmp(copy.name, "Repeater"), 0);
    copy.id = 99;
    zassert_equal(settings_channel(99, copy), -ENOENT);
    zassert_equal(copy.id, 99);
    static Bank copied_bank;
    zassert_ok(settings_bank(1, copied_bank));
    zassert_equal(copied_bank.channel_ids[0], 2);
    copied_bank.channel_ids[0] = 99;
    zassert_ok(settings_bank(1, copied_bank));
    zassert_equal(copied_bank.channel_ids[0], 2);
    zassert_equal(settings_bank(99, copied_bank), -ENOENT);

    settings_service(radio_snapshot(), 10010);
    const auto previous = settings_status().generation;
    recall_complete({Operating::Memory, 1, 2}, 12, 20000);
    // Same RF configuration still changes the selected identity and persists.
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 30010);
    zassert_equal(settings_status().generation, previous + 1);
    RadioConfig config;
    restart(config);
    zassert_true(same_selection(radio_snapshot().selection, {Operating::Memory, 1, 2}));
    recall_complete({Operating::Vfo, 1, 2}, 13, 40000);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 433500000);
    recall_complete({Operating::Memory, 1, 2}, 14, 50000);
    zassert_equal(radio_snapshot().config.tx_frequency_hz, 431475000);
}

ZTEST(settings, test_memory_quick_squelch_is_temporary_but_vfo_squelch_persists) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 21);
    settings_service(radio_snapshot(), 10010);
    const auto previous = settings_status().generation;
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config.squelch = 12;
    quick.config.rx_frequency_hz = 0; // Only the two quick-control fields apply.
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    radio_service();
    zassert_ok(radio_snapshot().command_error);
    settings_service(radio_snapshot(), 20000);
    zassert_equal(radio_snapshot().config.squelch, 12);
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    zassert_false(settings_status().pending);
    settings_service(radio_snapshot(), 30000);
    zassert_equal(settings_status().generation, previous);
    Channel channel;
    zassert_ok(settings_channel(1, channel));
    zassert_equal(channel.configuration.squelch, 4);
    recall_complete({Operating::Memory, 1, 1}, 22, 40000);
    zassert_equal(radio_snapshot().config.squelch, 4);
    auto config = radio_snapshot().config;
    strcpy(config.callsign, "OE3ANC");
    configure(config, 50000);
    settings_service(radio_snapshot(), 60000);
    zassert_equal(settings_status().selection.operating, Operating::Memory);
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 70000);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    zassert_equal(radio_snapshot().selection.channel_id, 1);
    quick.config.squelch = 9;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    radio_service();
    settings_service(radio_snapshot(), 80000);
    settings_service(radio_snapshot(), 90000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
    zassert_equal(loaded.vfo.squelch, 9);
    zassert_equal(loaded.channels[0].configuration.squelch, 4);
    zassert_equal(strcmp(loaded.global.local_callsign, "OE3ANC"), 0);
    recall_complete({Operating::Memory, 1, 1}, 23, 100000);
    OperatingConfig vfo;
    settings_vfo(vfo);
    zassert_equal(vfo.rx_frequency_hz, 145500000);
    zassert_equal(vfo.squelch, 9);
}

ZTEST(settings, test_recall_ack_survives_ordinary_commands_and_off_after_acceptance) {
    recall_fixture();
    Selection draft{Operating::Memory, 1, 1};
    zassert_ok(settings_recall(draft, 31, radio_snapshot()));
    draft = {}; // No retained caller pointer.
    zassert_equal(settings_recall(draft, 32, radio_snapshot()), -EBUSY);
    settings_service(radio_snapshot(), 0);
    radio_service();
    RadioCommand ordinary;
    ordinary.id = 500;
    ordinary.config = radio_snapshot().config;
    strcpy(ordinary.config.callsign, "OE3ANC");
    zassert_ok(radio_submit(ordinary));
    radio_service();
    zassert_equal(radio_snapshot().command_id, 500);
    settings_service(radio_snapshot(), 1);
    zassert_false(settings_status().operation_pending);
    zassert_ok(settings_status().operation_error);
    zassert_equal(settings_status().selection.channel_id, 1);
    zassert_equal(settings_status().selection.operating, Operating::Memory);
    settings_service(radio_snapshot(), 10010);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(strcmp(loaded.global.local_callsign, "OE3ANC"), 0);

    zassert_ok(settings_recall({Operating::Memory, 1, 2}, 33, radio_snapshot()));
    settings_service(radio_snapshot(), 20000);
    radio_service();
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 20010);
    zassert_false(settings_status().operation_pending);
    zassert_ok(settings_status().operation_error);
    zassert_equal(settings_status().selection.channel_id, 2);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.selection.channel_id, 2);
}

ZTEST(settings, test_recall_rejects_stale_state_queue_pressure_and_purged_commands) {
    recall_fixture();
    const auto old = radio_snapshot();
    auto config = old.config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    configure(config, 0);
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 41, old));
    settings_service(radio_snapshot(), 1);
    zassert_equal(settings_status().operation_error, -ESTALE);
    zassert_false(settings_status().operation_pending);
    zassert_equal(settings_status().selection.operating, Operating::Vfo);
    RadioCommand ordinary;
    ordinary.config = config;
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(ordinary));
    }
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 42, radio_snapshot()));
    settings_service(radio_snapshot(), 2);
    zassert_not_equal(settings_status().operation_error, 0);
    zassert_false(settings_status().operation_pending);
    for (unsigned i = 0; i < 8; ++i) {
        radio_service();
    }
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 43, radio_snapshot()));
    settings_service(radio_snapshot(), 3);
    radio_power(false);
    radio_service(); // Purges queued recall before execution.
    settings_service(radio_snapshot(), 4);
    zassert_equal(settings_status().operation_error, -ESTALE);
    zassert_false(settings_status().operation_pending);
    zassert_equal(settings_status().selection.operating, Operating::Vfo);
}

ZTEST(settings, test_recall_checks_revision_at_execution_and_retains_error_ack) {
    recall_fixture();
    const auto expected = radio_snapshot();
    RadioCommand ordinary;
    ordinary.id = 600;
    ordinary.config = expected.config;
    ordinary.config.rx_frequency_hz = ordinary.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(ordinary)); // Ahead of the owner's recall in RF queue.
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 51, expected));
    settings_service(expected, 0);
    radio_service();
    radio_service(); // Configure succeeds, stale recall fails.
    ordinary.id = 601;
    ordinary.config = radio_snapshot().config;
    zassert_ok(radio_submit(ordinary));
    radio_service();
    settings_service(radio_snapshot(), 1);
    zassert_equal(settings_status().operation_error, -ESTALE);
    zassert_false(settings_status().operation_pending);
    zassert_equal(settings_status().selection.operating, Operating::Vfo);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
}

ZTEST(settings, test_coalesced_memory_tuning_keeps_explicit_vfo_identity) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 72);
    settings_service(radio_snapshot(), 10010);
    RadioCommand tune;
    tune.config = radio_snapshot().config;
    const RadioConfig memory_config = tune.config;
    tune.config.rx_frequency_hz = tune.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(tune));
    radio_service();
    tune.config = memory_config;
    zassert_ok(radio_submit(tune));
    radio_service();
    // Both edits happened between settings ticks. Equal final RF settings must
    // still copy to VFO rather than silently infer that memory is selected.
    settings_service(radio_snapshot(), 20000);
    zassert_equal(settings_status().selection.operating, Operating::Vfo);
    settings_service(radio_snapshot(), 30000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.selection.operating, Operating::Vfo);
    zassert_equal(loaded.selection.channel_id, 1);
    zassert_equal(loaded.vfo.rx_frequency_hz, memory_config.rx_frequency_hz);
    zassert_equal(loaded.channels[0].configuration.rx_frequency_hz, 439075000);
}

ZTEST(settings, test_recall_invalid_references_tx_diagnostics_and_backend_failure) {
    recall_fixture();
    zassert_equal(settings_recall({}, 0, radio_snapshot()), -EINVAL);
    zassert_equal(settings_recall({Operating::Memory, 0, 0}, 61, radio_snapshot()), -EINVAL);
    zassert_ok(settings_recall({Operating::Memory, 1, 99}, 62, radio_snapshot()));
    settings_service(radio_snapshot(), 0);
    zassert_equal(settings_status().operation_error, -ENOENT);
    radio_ptt(true);
    radio_service();
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 63, radio_snapshot()));
    settings_service(radio_snapshot(), 1);
    zassert_equal(settings_status().operation_error, -EBUSY);
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    RadioCommand diagnostic;
    diagnostic.kind = CommandKind::EnterDiagnostics;
    zassert_ok(radio_submit(diagnostic));
    radio_service();
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 64, radio_snapshot()));
    settings_service(radio_snapshot(), 2);
    zassert_equal(settings_status().operation_error, -EBUSY);
    diagnostic.kind = CommandKind::ExitDiagnostics;
    zassert_ok(radio_submit(diagnostic));
    radio_service();
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 65, radio_snapshot()));
    settings_service(radio_snapshot(), 3);
    emulator_fail_next(-EIO);
    radio_service();
    settings_service(radio_snapshot(), 4);
    zassert_false(settings_status().operation_pending);
    zassert_equal(settings_status().operation_error, -EIO);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(settings_status().selection.operating, Operating::Vfo);
    zassert_false(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.selection.operating, Operating::Vfo);
}

static void finish_edit(uint32_t id, int error = 0, int64_t now = 0) {
    settings_service(radio_snapshot(), now);
    radio_service();
    settings_service(radio_snapshot(), now + 1);
    const auto status = settings_status();
    zassert_equal(status.operation_id, id);
    zassert_false(status.operation_pending);
    zassert_equal(status.operation_error, error);
    if (error) {
        zassert_equal(status.operation_object_id, 0);
    }
}

ZTEST(settings, test_channel_create_rename_renumber_duplicate_and_atomic_bank_add) {
    recall_fixture();
    const auto tuned = radio_snapshot().config.rx_frequency_hz;
    Channel draft;
    draft.number = 4;
    strcpy(draft.name, "Third");
    zassert_ok(settings_put_channel(draft, 101, radio_snapshot(), settings_status().revision, 1));
    strcpy(draft.name, "Caller changed"); // The owner retained a copy, not this pointer.
    finish_edit(101);
    zassert_equal(settings_status().operation_object_id, 3);
    zassert_false(settings_status().pending); // Explicit Save bypasses ordinary debounce.
    zassert_equal(radio_snapshot().config.rx_frequency_hz, tuned);
    zassert_ok(settings_channel_number(4, draft));
    zassert_equal(draft.id, 3);
    zassert_equal(strcmp(draft.name, "Third"), 0);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.count, 3);
    zassert_equal(bank.channel_ids[2], 3);
    uint16_t free = 99;
    zassert_ok(settings_free_channel_number(free));
    zassert_equal(free, 3);
    uint32_t revision;
    zassert_ok(settings_channel(1, draft, &revision));
    draft.number = 3;
    strcpy(draft.name, "Renamed");
    zassert_ok(settings_put_channel(draft, 102, radio_snapshot(), revision));
    finish_edit(102);
    zassert_equal(settings_status().operation_object_id, 1);
    zassert_ok(settings_channel_at(0, 0, draft));
    zassert_equal(draft.id, 2); // number 2 first
    zassert_ok(settings_channel_at(0, 1, draft));
    zassert_equal(draft.id, 1); // number 3
    draft.id = 0;
    draft.number = 5;
    strcpy(draft.name, "Duplicate");
    zassert_ok(settings_put_channel(draft, 103, radio_snapshot(), settings_status().revision));
    finish_edit(103);
    zassert_equal(settings_status().operation_object_id, 4);
    zassert_ok(settings_delete_channel(3, 104, radio_snapshot(), settings_status().revision));
    finish_edit(104);
    zassert_equal(settings_status().operation_object_id, 3);
    zassert_equal(settings_channel(3, draft), -ENOENT);
    draft.id = 0;
    draft.number = 4;
    zassert_ok(settings_put_channel(draft, 105, radio_snapshot(), settings_status().revision));
    finish_edit(105);
    zassert_equal(settings_status().operation_object_id, 5);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.channel_id_high_water, 5);
    zassert_equal(loaded.channel_count, 4);
    zassert_equal(loaded.banks[0].count, 2);
    RadioConfig config;
    restart(config);
    zassert_ok(settings_channel_number(3, draft));
    zassert_equal(draft.id, 1);
    zassert_equal(strcmp(draft.name, "Renamed"), 0);
    zassert_ok(settings_channel_at(1, 0, draft));
    zassert_equal(draft.id, 2);
    zassert_ok(settings_channel_at(1, 1, draft));
    zassert_equal(draft.id, 1);
}

ZTEST(settings, test_selected_channel_commits_only_after_rf_acceptance) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 110);
    Channel draft;
    uint32_t revision;
    zassert_ok(settings_channel(1, draft, &revision));
    strcpy(draft.name, "Edited");
    draft.configuration.rx_frequency_hz = 145500000;
    draft.configuration.tx_frequency_hz = 145600000;
    zassert_ok(settings_put_channel(draft, 111, radio_snapshot(), revision));
    settings_service(radio_snapshot(), 0);
    Channel visible;
    zassert_ok(settings_channel(1, visible));
    zassert_equal(strcmp(visible.name, "Repeater"), 0);
    radio_service(); // RF accepts; the settings owner has not committed yet.
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
    zassert_ok(settings_channel(1, visible));
    zassert_equal(visible.configuration.rx_frequency_hz, 439075000);
    settings_service(radio_snapshot(), 1);
    zassert_ok(settings_status().operation_error);
    zassert_ok(settings_channel(1, visible));
    zassert_equal(strcmp(visible.name, "Edited"), 0);
    zassert_equal(visible.configuration.tx_frequency_hz, 145600000);
    OperatingConfig vfo;
    settings_vfo(vfo);
    zassert_equal(vfo.rx_frequency_hz, 433500000);
    const auto committed = settings_status().generation;
    zassert_ok(settings_channel(1, draft, &revision));
    strcpy(draft.name, "Failing edit");
    draft.configuration.rx_frequency_hz = 145700000;
    zassert_ok(settings_put_channel(draft, 112, radio_snapshot(), revision));
    settings_service(radio_snapshot(), 2);
    emulator_fail_next(-EIO);
    radio_service();
    settings_service(radio_snapshot(), 3);
    zassert_equal(settings_status().operation_error, -EIO);
    zassert_equal(settings_status().operation_object_id, 0);
    zassert_ok(settings_channel(1, visible));
    zassert_equal(strcmp(visible.name, "Edited"), 0);
    zassert_equal(settings_status().generation, committed);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
}

ZTEST(settings, test_selected_channel_delete_restores_vfo_and_bank_delete_keeps_memories) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 120);
    zassert_ok(settings_delete_channel(1, 121, radio_snapshot(), settings_status().revision));
    finish_edit(121);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 433500000);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    zassert_equal(radio_snapshot().selection.channel_id, 0);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.count, 1);
    zassert_equal(bank.channel_ids[0], 2);
    recall_complete({Operating::Memory, 1, 2}, 122, 100);
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config.squelch = 12;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    radio_service();
    settings_service(radio_snapshot(), 101);
    zassert_ok(settings_delete_bank(1, 123, radio_snapshot(), settings_status().revision));
    finish_edit(123, 0, 102);
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    zassert_equal(radio_snapshot().selection.channel_id, 2);
    zassert_equal(radio_snapshot().selection.bank_id, 0);
    zassert_equal(radio_snapshot().config.squelch, 12); // Metadata edit did not retune.
    zassert_equal(settings_status().channel_count, 1);
    zassert_equal(settings_status().bank_count, 0);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.channel_id_high_water, 2);
    zassert_equal(loaded.bank_id_high_water, 1);
    zassert_equal(loaded.channels[0].configuration.squelch, 4);
}

ZTEST(settings, test_bank_create_rename_order_and_active_membership_repair) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 2}, 130);
    zassert_ok(settings_bank(1, bank));
    uint32_t revision = settings_status().revision;
    strcpy(bank.name, "Renamed bank");
    bank.channel_ids[0] = 1;
    bank.channel_ids[1] = 2;
    zassert_ok(settings_put_bank(bank, 131, radio_snapshot(), revision));
    finish_edit(131);
    zassert_equal(settings_status().operation_object_id, 1);
    Channel channel;
    zassert_ok(settings_channel_at(1, 0, channel));
    zassert_equal(channel.id, 1);
    zassert_ok(settings_channel_at(1, 1, channel));
    zassert_equal(channel.id, 2);
    zassert_ok(settings_bank_at(0, bank, &revision));
    zassert_equal(strcmp(bank.name, "Renamed bank"), 0);
    bank.count = 1;
    bank.channel_ids[0] = 1;
    zassert_ok(settings_put_bank(bank, 132, radio_snapshot(), revision));
    finish_edit(132);
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    zassert_equal(radio_snapshot().selection.channel_id, 2);
    zassert_equal(radio_snapshot().selection.bank_id, 0);
    bank.id = 0;
    strcpy(bank.name, "Other");
    zassert_ok(settings_put_bank(bank, 133, radio_snapshot(), settings_status().revision));
    bank.channel_ids[0] = 99; // Copied ordered payload.
    finish_edit(133);
    zassert_equal(settings_status().operation_object_id, 2);
    zassert_ok(settings_bank_at(1, bank));
    zassert_equal(bank.channel_ids[0], 1);
    zassert_equal(settings_bank_at(2, bank), -ENOENT);
    zassert_equal(settings_status().channel_count, 2);
}

ZTEST(settings, test_invalid_edits_and_stale_drafts_leave_store_and_counters_unchanged) {
    recall_fixture();
    Channel draft;
    strcpy(draft.name, "Collision");
    zassert_ok(settings_put_channel(draft, 141, radio_snapshot(), settings_status().revision));
    finish_edit(141, -EEXIST);
    draft.number = 3;
    zassert_ok(settings_put_channel(draft, 142, radio_snapshot(), settings_status().revision, 99));
    finish_edit(142, -ENOENT);
    draft.configuration.rx_frequency_hz = 300000000;
    zassert_ok(settings_put_channel(draft, 143, radio_snapshot(), settings_status().revision));
    finish_edit(143, -EINVAL);
    zassert_ok(settings_bank(1, bank));
    bank.channel_ids[1] = bank.channel_ids[0];
    zassert_ok(settings_put_bank(bank, 144, radio_snapshot(), settings_status().revision));
    finish_edit(144, -EEXIST);
    zassert_ok(settings_delete_channel(99, 145, radio_snapshot(), settings_status().revision));
    finish_edit(145, -ENOENT);
    zassert_ok(settings_delete_bank(99, 146, radio_snapshot(), settings_status().revision));
    finish_edit(146, -ENOENT);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(generation, 1);
    zassert_equal(loaded.channel_id_high_water, 2);
    uint32_t old_revision;
    zassert_ok(settings_channel(1, draft, &old_revision));
    zassert_ok(settings_bank(1, bank));
    strcpy(bank.name, "New name");
    zassert_ok(settings_put_bank(bank, 147, radio_snapshot(), settings_status().revision));
    finish_edit(147);
    strcpy(draft.name, "Stale draft");
    zassert_ok(settings_put_channel(draft, 148, radio_snapshot(), old_revision));
    finish_edit(148, -ESTALE);
    zassert_ok(settings_channel(1, draft));
    zassert_equal(strcmp(draft.name, "Repeater"), 0);
    // Explicit replacement edits the existing identity; collision never decides for the UI.
    strcpy(draft.name, "Confirmed replacement");
    zassert_ok(settings_put_channel(draft, 149, radio_snapshot(), settings_status().revision));
    finish_edit(149);
    zassert_equal(settings_status().operation_object_id, 1);
    zassert_equal(settings_put_channel(draft, 150, radio_snapshot(), 0), -EINVAL);
}

ZTEST(settings, test_fresh_recall_after_tuning_before_owner_tick_preserves_latest_vfo) {
    recall_fixture();
    RadioCommand tune;
    tune.config = radio_snapshot().config;
    tune.config.rx_frequency_hz = tune.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(tune));
    radio_service(); // The owner has not observed the VFO edit yet.
    zassert_ok(settings_recall({Operating::Memory, 1, 1}, 180, radio_snapshot()));
    finish_edit(180);
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 439075000);
    OperatingConfig vfo;
    settings_vfo(vfo);
    zassert_equal(vfo.rx_frequency_hz, 145500000);
}

ZTEST(settings, test_explicit_save_stays_immediate_after_following_global_edit) {
    recall_fixture();
    Channel draft;
    draft.number = 3;
    strcpy(draft.name, "New");
    zassert_ok(settings_put_channel(draft, 181, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 0);
    radio_service(); // Edit accepted; owner not notified yet.
    RadioCommand global;
    global.config = radio_snapshot().config;
    strcpy(global.config.callsign, "OE3ANC");
    zassert_ok(radio_submit(global));
    radio_service();
    settings_service(radio_snapshot(), 1);
    zassert_ok(settings_status().operation_error);
    zassert_false(settings_status().pending);
    zassert_equal(settings_status().generation, 2); // No new one-second delay.
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.channel_count, 3);
    zassert_equal(strcmp(loaded.global.local_callsign, "OE3ANC"), 0);
}

ZTEST(settings, test_name_only_active_channel_save_reapplies_stored_squelch) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 182);
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config.squelch = 12;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    radio_service();
    settings_service(radio_snapshot(), 0);
    Channel draft;
    zassert_ok(settings_channel(1, draft));
    strcpy(draft.name, "New name");
    zassert_ok(settings_put_channel(draft, 183, radio_snapshot(), settings_status().revision));
    finish_edit(183);
    zassert_equal(radio_snapshot().config.squelch, 4);
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    zassert_equal(radio_snapshot().selection.channel_id, 1);
}

ZTEST(settings, test_edit_rejects_tx_at_execution_and_off_purge_but_keeps_accepted_shutdown_edit) {
    recall_fixture();
    Channel draft;
    draft.number = 3;
    strcpy(draft.name, "New");
    zassert_ok(settings_put_channel(draft, 151, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 0);
    radio_ptt(true);
    radio_service(); // TX begins before the queued metadata authorization.
    settings_service(radio_snapshot(), 1);
    zassert_equal(settings_status().operation_error, -EBUSY);
    zassert_equal(settings_status().channel_count, 2);
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    zassert_ok(settings_put_channel(draft, 152, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 2);
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 3);
    zassert_equal(settings_status().operation_error, -ESTALE);
    zassert_equal(settings_status().channel_count, 2);
    radio_power(true);
    radio_service();
    settings_service(radio_snapshot(), 4);
    zassert_ok(settings_put_channel(draft, 153, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 5);
    radio_service(); // Accepted before switch-off.
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 6);
    zassert_ok(settings_status().operation_error);
    zassert_equal(settings_status().channel_count, 3);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.channel_count, 3);
}

ZTEST(settings, test_committed_edit_durability_error_stays_dirty_through_temporary_sql) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 160);
    Channel draft;
    zassert_ok(settings_channel(1, draft));
    strcpy(draft.name, "Unsynced");
    zassert_ok(settings_put_channel(draft, 161, radio_snapshot(), settings_status().revision));
    fail_durable = true;
    finish_edit(161);
    zassert_ok(settings_status().operation_error);
    zassert_true(settings_status().pending);
    zassert_equal(settings_status().save_error, -EIO);
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config.squelch = 12;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    radio_service();
    settings_service(radio_snapshot(), 2);
    zassert_true(
        settings_status().pending); // Cannot clear a dirty channel by comparing only active RF.
    fail_durable = false;
    settings_service(radio_snapshot(), 1000);
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 1001);
    zassert_false(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(strcmp(loaded.channels[0].name, "Unsynced"), 0);
    zassert_equal(loaded.channels[0].configuration.squelch, 4);
}

ZTEST(settings, test_full_capacity_browsing_ordered_bank_update_and_exhausted_ids) {
    Channel channel;
    strcpy(channel.name, "Full");
    uint32_t id;
    for (uint16_t number = channel_capacity; number; --number) {
        channel.number = number;
        zassert_ok(put_channel(plug, channel, id));
    }
    strcpy(bank.name, "Full bank");
    bank.count = channel_capacity;
    for (size_t i = 0; i < channel_capacity; ++i) {
        bank.channel_ids[i] = i + 1;
    }
    for (size_t i = 0; i < bank_capacity; ++i) {
        zassert_ok(put_bank(plug, bank, id));
    }
    plug.channel_id_high_water = plug.bank_id_high_water = UINT32_MAX;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    restart(config);
    zassert_ok(settings_channel_at(0, 0, channel));
    zassert_equal(channel.number, 1);
    zassert_ok(settings_channel_at(0, 128, channel));
    zassert_equal(channel.number, 129);
    zassert_ok(settings_channel_at(0, 255, channel));
    zassert_equal(channel.number, 256);
    channel.id = 99;
    uint32_t revision = 99;
    zassert_equal(settings_channel_at(0, 256, channel, &revision), -ENOENT);
    zassert_equal(channel.id, 99);
    zassert_equal(revision, 99);
    uint16_t number = 99;
    zassert_equal(settings_free_channel_number(number), -ENOSPC);
    zassert_equal(number, 99);
    zassert_ok(settings_bank(16, bank));
    for (size_t i = 0; i < channel_capacity; ++i) {
        bank.channel_ids[i] = channel_capacity - i;
    }
    zassert_ok(settings_put_bank(bank, 171, radio_snapshot(), settings_status().revision));
    finish_edit(171);
    zassert_ok(settings_channel_at(16, 0, channel));
    zassert_equal(channel.id, 256);
    zassert_ok(settings_channel_at(16, 255, channel));
    zassert_equal(channel.id, 1);
    bank.id = 0;
    bank.count = 0;
    zassert_ok(settings_put_bank(bank, 172, radio_snapshot(), settings_status().revision));
    finish_edit(172, -ENOSPC);
    zassert_ok(settings_delete_channel(1, 173, radio_snapshot(), settings_status().revision));
    finish_edit(173);
    channel.id = 0;
    channel.number = 256;
    zassert_ok(settings_put_channel(channel, 174, radio_snapshot(), settings_status().revision));
    finish_edit(174, -EOVERFLOW);
    zassert_ok(settings_delete_bank(1, 175, radio_snapshot(), settings_status().revision));
    finish_edit(175);
    bank.id = 0;
    zassert_ok(settings_put_bank(bank, 176, radio_snapshot(), settings_status().revision));
    finish_edit(176, -EOVERFLOW);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.channel_id_high_water, UINT32_MAX);
    zassert_equal(loaded.bank_id_high_water, UINT32_MAX);
}

static void check_ui(const UiPreferences &expected) {
    UiPreferences actual;
    settings_ui_preferences(actual);
    zassert_equal(actual.theme, expected.theme);
    zassert_equal(actual.contrast, expected.contrast);
    zassert_equal(actual.animations, expected.animations);
    zassert_equal(actual.brightness_percent, expected.brightness_percent);
    zassert_equal(actual.idle_s, expected.idle_s);
    zassert_equal(actual.dim_percent, expected.dim_percent);
}

ZTEST(settings, test_ui_preferences_all_palettes_contrasts_and_reboot) {
    recall_fixture();
    RadioCommand global;
    global.id = 201;
    global.config = radio_snapshot().config;
    strcpy(global.config.callsign, "OE3ANC");
    global.config.transmit_limit_s = 60;
    zassert_ok(radio_submit(global));
    radio_service();
    settings_service(radio_snapshot(), 0);
    UiPreferences draft;
    for (unsigned theme = 0; theme < ThemeCount; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            uint32_t revision;
            settings_ui_preferences(draft, &revision);
            draft.theme = static_cast<Theme>(theme);
            draft.contrast = static_cast<Contrast>(contrast);
            draft.animations = contrast != 2;
            draft.brightness_percent = 25 * (theme % 4 + 1);
            draft.idle_s = theme == 0 ? 0 : theme == 1 ? 15 : theme == 2 ? 30 : 60;
            draft.dim_percent = 10 * (contrast + 1);
            const auto before = radio_snapshot();
            zassert_ok(settings_put_ui_preferences(draft, 201, before, revision));
            finish_edit(201); // Same numeric ordinary command ID must not consume its ACK.
            check_ui(draft);
            zassert_equal(settings_status().operation_object_id, 0);
            zassert_equal(radio_snapshot().command_id, before.command_id);
            zassert_equal(radio_snapshot().command_error, before.command_error);
            zassert_false(settings_status().pending);
            RadioConfig config;
            restart(config);
            check_ui(draft);
            zassert_equal(strcmp(config.callsign, "OE3ANC"), 0);
            zassert_equal(config.transmit_limit_s, 60);
            zassert_equal(config.rx_frequency_hz, 433500000);
            zassert_ok(codeplug_load(loaded, generation));
            zassert_equal(loaded.channel_count, 2);
            zassert_equal(loaded.bank_count, 1);
        }
    }
}

ZTEST(settings, test_ui_apply_copies_draft_and_keeps_memory_monitor_and_transient_sql) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 202);
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config.squelch = 12;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    radio_service();
    settings_service(radio_snapshot(), 0);
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    const auto before = radio_snapshot();
    UiPreferences draft;
    uint32_t revision;
    settings_ui_preferences(draft, &revision);
    draft.theme = Theme::Nord;
    const auto expected = draft;
    zassert_ok(settings_put_ui_preferences(draft, 203, before, revision));
    draft.theme = Theme::Darcula;
    check_ui({}); // No publication before RF approval.
    finish_edit(203);
    check_ui(expected);
    const auto after = radio_snapshot();
    zassert_true(same_operating(after.config, before.config));
    zassert_true(same_selection(after.selection, before.selection));
    zassert_true(after.monitor_active);
    zassert_equal(after.config.squelch, 12);
    zassert_equal(emulator_tuned_frequency(), before.config.rx_frequency_hz);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.channels[0].configuration.squelch, 4);
    radio_monitor(false);
    radio_service();
    const auto stored_generation = settings_status().generation;
    settings_ui_preferences(draft, &revision);
    zassert_ok(settings_put_ui_preferences(draft, 204, radio_snapshot(), revision));
    finish_edit(204);
    zassert_equal(settings_status().generation, stored_generation);
    zassert_equal(settings_status().revision, revision); // No-op Apply is not a data mutation.
}

ZTEST(settings, test_ui_preferences_invalid_stale_busy_and_protected) {
    recall_fixture();
    UiPreferences draft;
    uint32_t revision;
    settings_ui_preferences(draft, &revision);
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        draft = {};
        switch (invalid) {
        case 0:
            draft.theme = static_cast<Theme>(255);
            break;
        case 1:
            draft.contrast = static_cast<Contrast>(255);
            break;
        case 2:
            draft.brightness_percent = 0;
            break;
        case 3:
            draft.idle_s = 1;
            break;
        case 4:
            draft.dim_percent = 100;
            break;
        }
        zassert_ok(settings_put_ui_preferences(draft, 205, radio_snapshot(), revision));
        finish_edit(205, -EINVAL);
        check_ui({});
        zassert_equal(settings_status().generation, 1);
        zassert_equal(settings_status().revision, revision);
    }
    draft = {};
    draft.theme = Theme::Nord;
    zassert_equal(settings_put_ui_preferences(draft, 0, radio_snapshot(), revision), -EINVAL);
    zassert_equal(settings_put_ui_preferences(draft, 206, radio_snapshot(), 0), -EINVAL);
    zassert_ok(settings_put_ui_preferences(draft, 206, radio_snapshot(), revision));
    zassert_equal(settings_put_ui_preferences(draft, 207, radio_snapshot(), revision), -EBUSY);
    zassert_equal(settings_recall({}, 207, radio_snapshot()), -EBUSY);
    finish_edit(206);
    draft.theme = Theme::Darcula;
    zassert_ok(settings_put_ui_preferences(draft, 208, radio_snapshot(), revision));
    finish_edit(208, -ESTALE);
    draft.theme = Theme::Nord;
    check_ui(draft);
    // Use a fresh committed store so this case isolates corruption protection.
    before(nullptr);
    zassert_ok(codeplug_save(plug, generation));
    corrupt_manifest(false);
    RadioConfig config;
    zassert_equal(settings_start(config), -EBADMSG);
    zassert_equal(
        settings_put_ui_preferences(draft, 209, radio_snapshot(), settings_status().revision),
        -EROFS);
    check_ui({});
}

ZTEST(settings, test_ui_apply_rejects_tx_diagnostics_stale_rf_fault_and_off) {
    recall_fixture();
    UiPreferences draft;
    draft.contrast = Contrast::High;
    zassert_ok(
        settings_put_ui_preferences(draft, 210, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 0);
    radio_ptt(true);
    radio_service();
    settings_service(radio_snapshot(), 1);
    zassert_equal(settings_status().operation_error, -EBUSY);
    check_ui({});
    radio_ptt(false);
    radio_service();
    const auto expected = radio_snapshot();
    RadioCommand tune;
    tune.config = expected.config;
    tune.config.rx_frequency_hz = tune.config.tx_frequency_hz = 145500000;
    configure(tune.config, 2);
    zassert_ok(settings_put_ui_preferences(draft, 211, expected, settings_status().revision));
    finish_edit(211, -ESTALE, 3);
    check_ui({});
    RadioCommand diagnostic;
    diagnostic.kind = CommandKind::EnterDiagnostics;
    zassert_ok(radio_submit(diagnostic));
    radio_service();
    zassert_ok(
        settings_put_ui_preferences(draft, 212, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 4);
    zassert_equal(settings_status().operation_error, -EBUSY);
    diagnostic.kind = CommandKind::ExitDiagnostics;
    zassert_ok(radio_submit(diagnostic));
    radio_service();
    zassert_ok(
        settings_put_ui_preferences(draft, 213, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 5);
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 6);
    zassert_equal(settings_status().operation_error, -ESTALE);
    check_ui({});
    radio_power(true);
    radio_service();
    radio_report_fault(-EIO);
    radio_service();
    zassert_ok(
        settings_put_ui_preferences(draft, 214, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 7);
    zassert_equal(settings_status().operation_error, -EIO);
    check_ui({});
}

ZTEST(settings, test_ui_accepted_apply_is_saved_on_off_and_survives_durability_retry) {
    recall_fixture();
    UiPreferences draft;
    draft.theme = Theme::SolarizedDark;
    zassert_ok(
        settings_put_ui_preferences(draft, 215, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 0);
    radio_service();
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 1);
    zassert_ok(settings_status().operation_error);
    check_ui(draft);
    zassert_false(settings_status().pending);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.global.ui.theme, Theme::SolarizedDark);
    radio_power(true);
    radio_service();
    settings_service(radio_snapshot(), 2);
    draft.contrast = Contrast::Maximum;
    zassert_ok(
        settings_put_ui_preferences(draft, 216, radio_snapshot(), settings_status().revision));
    fail_durable = true;
    finish_edit(216, 0, 3);
    check_ui(draft);
    zassert_true(settings_status().pending);
    zassert_equal(settings_status().save_error, -EIO);
    // An ordinary global update cannot wipe UI preferences or their pending dirty state.
    RadioConfig config = radio_snapshot().config;
    strcpy(config.callsign, "OE3ANC");
    configure(config, 5);
    check_ui(draft);
    zassert_true(settings_status().pending);
    fail_durable = false;
    settings_service(radio_snapshot(), 10050);
    zassert_false(settings_status().pending);
    restart(config);
    check_ui(draft);
    zassert_equal(strcmp(config.callsign, "OE3ANC"), 0);
}

ZTEST_SUITE(settings, nullptr, setup, before, nullptr, nullptr);

ZTEST(settings, test_vfo_step_values_persistence_noop_and_ordinary_global_edit) {
    recall_fixture();
    for (auto hz : vfo_steps_hz) {
        uint32_t revision;
        settings_vfo_step(&revision);
        const auto before = radio_snapshot();
        zassert_ok(settings_put_vfo_step(hz, 310, before, revision));
        finish_edit(310);
        zassert_equal(settings_vfo_step(), hz);
        zassert_false(settings_status().pending);
        zassert_equal(emulator_tuned_frequency(), before.config.rx_frequency_hz);
        zassert_true(same_selection(radio_snapshot().selection, before.selection));
        RadioConfig config;
        restart(config);
        zassert_equal(settings_vfo_step(), hz);
        zassert_ok(codeplug_load(loaded, generation));
        zassert_equal(loaded.global.vfo_step_hz, hz);
        const auto stored_generation = settings_status().generation;
        settings_vfo_step(&revision);
        zassert_ok(settings_put_vfo_step(hz, 311, radio_snapshot(), revision));
        finish_edit(311);
        zassert_equal(settings_status().generation, stored_generation);
        zassert_equal(settings_status().revision, revision);
    }
    RadioCommand global;
    global.config = radio_snapshot().config;
    strcpy(global.config.callsign, "OE3ANC");
    zassert_ok(radio_submit(global));
    radio_service();
    settings_service(radio_snapshot(), 0);
    settings_service(radio_snapshot(), 11000);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.global.vfo_step_hz, 100000);
    zassert_equal(strcmp(loaded.global.local_callsign, "OE3ANC"), 0);
}

ZTEST(settings, test_vfo_step_memory_monitor_sql_invalid_stale_and_shared_slot) {
    recall_fixture();
    recall_complete({Operating::Memory, 1, 1}, 312);
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config.squelch = 12;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    radio_service();
    settings_service(radio_snapshot(), 0);
    radio_monitor(true);
    radio_service();
    const auto before = radio_snapshot();
    uint32_t revision;
    settings_vfo_step(&revision);
    zassert_ok(settings_put_vfo_step(6250, 313, before, revision));
    zassert_equal(settings_put_vfo_step(5000, 314, before, revision), -EBUSY);
    finish_edit(313);
    zassert_equal(settings_vfo_step(), 6250);
    zassert_true(same_operating(radio_snapshot().config, before.config));
    zassert_true(same_selection(radio_snapshot().selection, before.selection));
    zassert_true(radio_snapshot().monitor_active);
    zassert_equal(radio_snapshot().config.squelch, 12);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.channels[0].configuration.squelch, 4);
    zassert_ok(settings_put_vfo_step(5000, 314, radio_snapshot(), revision));
    finish_edit(314, -ESTALE);
    const uint32_t invalid[] = {0, 1, UINT32_MAX};
    for (auto hz : invalid) {
        settings_vfo_step(&revision);
        zassert_ok(settings_put_vfo_step(hz, 315, radio_snapshot(), revision));
        finish_edit(315, -EINVAL);
        zassert_equal(settings_vfo_step(), 6250);
    }
    zassert_equal(settings_put_vfo_step(5000, 0, radio_snapshot(), revision), -EINVAL);
    zassert_equal(settings_put_vfo_step(5000, 315, radio_snapshot(), 0), -EINVAL);
}

// Compare all serialized fields, excluding inaccessible unused array slots and
// native struct padding. Keep both full snapshots and codec scratch off stack.
static uint8_t comparison_bytes[codeplug_record_max];

static void check_same_codeplug(const Codeplug &a, const Codeplug &b) {
    size_t left = 0, right = 0;
    zassert_ok(make_manifest(a, manifest));
    zassert_ok(encode_manifest(manifest, 1, bytes, sizeof(bytes), left));
    zassert_ok(make_manifest(b, manifest));
    zassert_ok(encode_manifest(manifest, 1, comparison_bytes, sizeof(comparison_bytes), right));
    zassert_equal(left, right);
    zassert_mem_equal(bytes, comparison_bytes, left);
    for (size_t i = 0; i < a.channel_count; ++i) {
        zassert_ok(encode_channel(a.channels[i], 1, bytes, sizeof(bytes), left));
        zassert_ok(
            encode_channel(b.channels[i], 1, comparison_bytes, sizeof(comparison_bytes), right));
        zassert_equal(left, right);
        zassert_mem_equal(bytes, comparison_bytes, left);
    }
    for (size_t i = 0; i < a.bank_count; ++i) {
        zassert_ok(encode_bank(a.banks[i], 1, bytes, sizeof(bytes), left));
        zassert_ok(encode_bank(b.banks[i], 1, comparison_bytes, sizeof(comparison_bytes), right));
        zassert_equal(left, right);
        zassert_mem_equal(bytes, comparison_bytes, left);
    }
}

ZTEST(settings, test_complete_snapshot_and_replacement_copy_before_controller_ack) {
    recall_fixture();
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    check_same_codeplug(plug, loaded);
    zassert_equal(captured.revision, settings_status().revision);
    zassert_equal(captured.generation, generation);
    const auto revision = captured.revision;
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 145500000;
    strcpy(plug.global.local_callsign, "OE3ANC");
    plug.global.vfo_step_hz = 6250;
    plug.global.ui.theme = Theme::Darcula;
    plug.bank_id_high_water = 99;
    plug.channel_id_high_water = 999;
    plug.banks[0].channel_ids[0] = 1;
    plug.banks[0].channel_ids[1] = 2;
    plug.selection = {Operating::Memory, 1, 2};
    zassert_ok(settings_replace_codeplug(plug, 401, radio_snapshot(), revision));
    // Caller storage is not retained. Restore it later from the committed copy.
    reset(plug);
    settings_copy_codeplug(loaded, captured);
    zassert_true(captured.operation_pending);
    zassert_equal(captured.operation_id, 401);
    zassert_equal(captured.revision, revision);
    zassert_equal(loaded.global.ui.theme, Theme::Midnight);
    zassert_equal(loaded.selection.operating, Operating::Vfo);
    const auto expected = radio_snapshot();
    zassert_equal(settings_recall({}, 402, expected), -EBUSY);
    zassert_equal(settings_replace_codeplug(loaded, 402, expected, revision), -EBUSY);
    settings_service(expected, 0);
    settings_copy_codeplug(loaded, captured);
    zassert_true(captured.operation_pending);
    zassert_equal(loaded.global.ui.theme, Theme::Midnight);
    zassert_ok(codeplug_load(plug, generation));
    check_same_codeplug(plug, loaded); // Storage and RAM still contain the old complete plug.
    radio_service();
    zassert_equal(radio_snapshot().selection.channel_id, 2);
    settings_service(radio_snapshot(), 1);
    settings_copy_codeplug(loaded, captured);
    zassert_false(captured.operation_pending);
    zassert_ok(captured.operation_error);
    zassert_false(captured.pending);
    zassert_equal(captured.revision, revision + 1);
    zassert_equal(captured.generation, generation + 1);
    zassert_equal(loaded.global.ui.theme, Theme::Darcula);
    zassert_equal(loaded.global.vfo_step_hz, 6250);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
    zassert_equal(loaded.channel_id_high_water, 999);
    zassert_equal(loaded.bank_id_high_water, 99);
    zassert_equal(loaded.banks[0].channel_ids[0], 1);
    zassert_equal(strcmp(loaded.global.local_callsign, "OE3ANC"), 0);
    zassert_true(same_selection(loaded.selection, radio_snapshot().selection));
    zassert_ok(codeplug_load(plug, generation));
    check_same_codeplug(plug, loaded);
    RadioConfig config;
    restart(config);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(plug, loaded);
    zassert_equal(config.rx_frequency_hz, 439075000);
}

ZTEST(settings, test_whole_replacement_invalid_and_unsupported_leave_every_record_unchanged) {
    recall_fixture();
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    const auto revision = captured.revision;
    // Validate every record, including ones that are not currently selected.
    plug.channels[1].configuration.rx_frequency_hz = 1;
    zassert_ok(settings_replace_codeplug(plug, 403, radio_snapshot(), revision));
    finish_edit(403, -EINVAL);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(loaded, plug);
    zassert_equal(captured.revision, revision);
    zassert_equal(captured.generation, generation);
    plug.global.gain = 1; // Structurally valid, unsupported on both targets.
    zassert_ok(settings_replace_codeplug(plug, 404, radio_snapshot(), revision));
    finish_edit(404, -ENOTSUP);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(loaded, plug);
    plug.banks[0].channel_ids[0] = 999;
    zassert_ok(settings_replace_codeplug(plug, 405, radio_snapshot(), revision));
    finish_edit(405, -ENOENT);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(loaded, plug);
    plug.channel_count = channel_capacity + 1;
    zassert_ok(settings_replace_codeplug(plug, 406, radio_snapshot(), revision));
    finish_edit(406, -EINVAL);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(loaded, plug);
    zassert_false(captured.pending);
    zassert_equal(captured.revision, revision);
    zassert_ok(codeplug_load(plug, generation));
    check_same_codeplug(loaded, plug);
}

ZTEST(settings, test_whole_replacement_stale_at_capture_owner_and_controller_execution) {
    recall_fixture();
    const auto revision = settings_status().revision;
    zassert_equal(settings_replace_codeplug(plug, 407, radio_snapshot(), 0), -EINVAL);
    zassert_equal(settings_replace_codeplug(plug, 0, radio_snapshot(), revision), -EINVAL);
    zassert_equal(settings_replace_codeplug(plug, 407, radio_snapshot(), revision + 1), -ESTALE);
    zassert_false(settings_status().operation_pending);
    auto expected = radio_snapshot();
    plug.global.ui.theme = Theme::Nord;
    zassert_ok(settings_replace_codeplug(plug, 407, expected, revision));
    RadioCommand ordinary;
    ordinary.config = expected.config;
    ordinary.config.rx_frequency_hz = ordinary.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(ordinary));
    radio_service();
    settings_service(radio_snapshot(), 0); // Reconciliation advances the RAM revision first.
    zassert_equal(settings_status().operation_error, -ESTALE);
    zassert_false(settings_status().operation_pending);
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    zassert_equal(loaded.global.ui.theme, Theme::Midnight);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145500000);
    zassert_equal(settings_replace_codeplug(plug, 408, radio_snapshot(), revision), -ESTALE);

    expected = radio_snapshot();
    ordinary.config = expected.config;
    ordinary.config.rx_frequency_hz = ordinary.config.tx_frequency_hz = 145600000;
    zassert_ok(radio_submit(ordinary)); // In front of replacement in the controller queue.
    zassert_ok(settings_replace_codeplug(plug, 408, expected, captured.revision));
    settings_service(expected, 1); // Submit using the still-current captured snapshot.
    radio_service();
    radio_service(); // Ordinary Configure, then stale replacement.
    settings_service(radio_snapshot(), 2);
    zassert_equal(settings_status().operation_error, -ESTALE);
    settings_copy_codeplug(loaded, captured);
    zassert_equal(loaded.global.ui.theme, Theme::Midnight);
    zassert_equal(loaded.vfo.rx_frequency_hz, 145600000);
    zassert_equal(loaded.channel_count, 2);
}

ZTEST(settings, test_whole_replacement_counter_reconciliation_and_exhaustion) {
    recall_fixture();
    // Persist a destination with deleted identities and exhausted counters.
    plug.channel_id_high_water = UINT32_MAX;
    plug.bank_id_high_water = UINT32_MAX;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    restart(config);
    reset(plug); // An older empty import must not reset destination allocation history.
    zassert_ok(settings_replace_codeplug(plug, 411, radio_snapshot(), settings_status().revision));
    finish_edit(411);
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    zassert_equal(loaded.channel_count, 0);
    zassert_equal(loaded.bank_count, 0);
    zassert_equal(loaded.channel_id_high_water, UINT32_MAX);
    zassert_equal(loaded.bank_id_high_water, UINT32_MAX);
    zassert_equal(plug.channel_id_high_water, 0); // Caller copy is never mutated.
    Channel draft;
    strcpy(draft.name, "New");
    zassert_ok(settings_put_channel(draft, 412, radio_snapshot(), captured.revision));
    finish_edit(412, -EOVERFLOW);
    strcpy(bank.name, "New");
    bank.id = bank.count = 0;
    zassert_ok(settings_put_bank(bank, 413, radio_snapshot(), captured.revision));
    finish_edit(413, -EOVERFLOW);
    zassert_ok(codeplug_load(plug, generation));
    check_same_codeplug(plug, loaded);
    restart(config);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(plug, loaded);
}

ZTEST(settings, test_whole_replacement_tx_off_fault_and_controller_failure_preserve_database) {
    recall_fixture();
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    plug.global.ui.theme = Theme::SolarizedDark;
    const auto revision = captured.revision;
    zassert_ok(settings_replace_codeplug(plug, 414, radio_snapshot(), revision));
    settings_service(radio_snapshot(), 0);
    radio_ptt(true);
    radio_service(); // Independent PTT is processed before the queued recall.
    settings_service(radio_snapshot(), 1);
    zassert_equal(settings_status().operation_error, -EBUSY);
    radio_ptt(false);
    radio_service();
    zassert_ok(settings_replace_codeplug(plug, 415, radio_snapshot(), revision));
    settings_service(radio_snapshot(), 2);
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 3);
    zassert_equal(settings_status().operation_error, -ESTALE);
    radio_power(true);
    radio_service();
    emulator_fail_next(-EIO);
    zassert_ok(settings_replace_codeplug(plug, 416, radio_snapshot(), revision));
    finish_edit(416, -EIO, 4);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_false(emulator_transmitting());
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(plug, loaded);
    zassert_equal(captured.revision, revision);
    zassert_equal(captured.generation, generation);
    zassert_false(captured.pending);
    zassert_ok(codeplug_load(plug, generation));
    check_same_codeplug(plug, loaded);
}

ZTEST(settings, test_whole_replacement_applied_and_durable_results_are_separate) {
    recall_fixture();
    const auto previous_generation = settings_status().generation;
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 145500000;
    plug.global.ui.theme = Theme::Nord;
    zassert_ok(settings_replace_codeplug(plug, 417, radio_snapshot(), settings_status().revision));
    fail_durable = true;
    finish_edit(417); // The operation is applied, while storage is failed/uncertain.
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    check_same_codeplug(plug, loaded);
    zassert_ok(captured.operation_error);
    zassert_true(captured.pending);
    zassert_equal(captured.save_error, -EIO);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
#ifdef CONFIG_HT_SETTINGS_NVS
    zassert_equal(captured.generation, previous_generation);
    zassert_ok(codeplug_load(loaded, generation));
    zassert_equal(loaded.global.ui.theme, Theme::Midnight);
#else
    // rename succeeded, directory fsync did not: visible, durability uncertain.
    zassert_equal(captured.generation, previous_generation + 1);
    zassert_ok(codeplug_load(loaded, generation));
    check_same_codeplug(plug, loaded);
#endif
    fail_durable = false;
    settings_service(radio_snapshot(), 1000);
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 1001);
    zassert_false(settings_status().pending);
    zassert_ok(settings_status().save_error);
    zassert_ok(codeplug_load(loaded, generation));
    check_same_codeplug(plug, loaded);
    RadioConfig config;
    restart(config);
    settings_copy_codeplug(loaded, captured);
    check_same_codeplug(plug, loaded);
}

ZTEST(settings, test_whole_replacement_full_capacity_mixed_modes_and_ordered_membership) {
    Channel channel;
    uint32_t id;
    strcpy(channel.name, "Full replacement");
    strcpy(plug.global.local_callsign, "OE3ANC");
    for (size_t i = 0; i < channel_capacity; ++i) {
        channel.number = i + 1;
        channel.configuration.mode = i % 2 ? Mode::M17 : Mode::Fm;
        channel.configuration.m17 = {};
        channel.configuration.rx_tone = {};
        channel.configuration.tx_tone = {};
        if (i % 2) {
            channel.configuration.m17.destination = Destination::Station;
            strcpy(channel.configuration.m17.callsign, "OE1TEST");
            channel.configuration.m17.can = i % 16;
            channel.configuration.m17.rx_can_check = true;
        } else {
            channel.configuration.rx_tone = {ToneKind::Dcs, 0023, false};
            channel.configuration.tx_tone = {ToneKind::Ctcss, 885, false};
        }
        zassert_ok(put_channel(plug, channel, id));
    }
    strcpy(bank.name, "All reversed");
    bank.count = channel_capacity;
    for (size_t i = 0; i < channel_capacity; ++i) {
        bank.channel_ids[i] = channel_capacity - i;
    }
    for (size_t i = 0; i < bank_capacity; ++i) {
        zassert_ok(put_bank(plug, bank, id));
    }
    plug.selection = {Operating::Memory, id, channel_capacity};
    const auto revision = settings_status().revision;
    zassert_ok(settings_replace_codeplug(plug, 418, radio_snapshot(), revision));
    finish_edit(418);
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    check_same_codeplug(plug, loaded);
    zassert_false(captured.pending);
    zassert_equal(captured.channel_count, channel_capacity);
    zassert_equal(captured.bank_count, bank_capacity);
    zassert_equal(captured.revision, revision + 1);
    zassert_equal(radio_snapshot().config.mode, Mode::M17);
    zassert_equal(radio_snapshot().config.m17.can, 15);
    zassert_ok(codeplug_load(loaded, generation));
    check_same_codeplug(plug, loaded);
    RadioConfig config;
    restart(config);
    settings_copy_codeplug(loaded, captured);
    check_same_codeplug(plug, loaded);
}

ZTEST(settings, test_whole_replacement_local_operation_diagnostics_and_protected_store) {
    recall_fixture();
    auto revision = settings_status().revision;
    UiPreferences ui;
    ui.theme = Theme::Nord;
    zassert_ok(settings_put_ui_preferences(ui, 419, radio_snapshot(), revision));
    zassert_equal(settings_replace_codeplug(plug, 420, radio_snapshot(), revision), -EBUSY);
    finish_edit(419);
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    revision = captured.revision;
    RadioCommand command;
    command.kind = CommandKind::EnterDiagnostics;
    zassert_ok(radio_submit(command));
    radio_service();
    zassert_ok(settings_replace_codeplug(plug, 420, radio_snapshot(), revision));
    finish_edit(420, -EBUSY);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(plug, loaded);
    zassert_equal(captured.revision, revision);
    zassert_false(captured.pending);

    before(nullptr);
    zassert_ok(codeplug_save(plug, generation));
    corrupt_manifest(false);
    RadioConfig config;
    zassert_equal(settings_start(config), -EBADMSG);
    zassert_ok(radio_start(config));
    settings_copy_codeplug(loaded, captured);
    plug.global.ui.theme = Theme::Darcula;
    zassert_equal(settings_replace_codeplug(plug, 421, radio_snapshot(), captured.revision),
                  -EROFS);
    settings_service(radio_snapshot(), 0);
    settings_copy_codeplug(plug, captured);
    check_same_codeplug(plug, loaded);
    zassert_false(captured.operation_pending);
    zassert_true(captured.read_only);
}

ZTEST(settings, test_whole_replacement_retained_ack_after_off_saves_complete_new_database) {
    recall_fixture();
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 145500000;
    plug.global.ui.theme = Theme::SolarizedDark;
    plug.selection = {Operating::Memory, 1, 2};
    zassert_ok(settings_replace_codeplug(plug, 422, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 0);
    radio_service(); // Controller accepts before off.
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 1);
    const auto result = settings_status();
    zassert_false(result.operation_pending);
    zassert_ok(result.operation_error);
    zassert_false(result.pending);
    zassert_ok(result.shutdown_error);
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    zassert_false(emulator_transmitting());
    SettingsStatus captured;
    settings_copy_codeplug(loaded, captured);
    check_same_codeplug(plug, loaded);
    zassert_ok(codeplug_load(loaded, generation));
    check_same_codeplug(plug, loaded);
}
