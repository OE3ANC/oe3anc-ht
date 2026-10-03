// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include <ht/backend.hpp>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <ht/ui.hpp>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zephyr/ztest.h>
using namespace ht;
static UiModel model;
static Codeplug plug;
static Bank bank;
static lv_color_t buffer[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static char root[] = "/tmp/ht-ui-quick-XXXXXX", path[160];
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
static bool fail_durable;
static bool enable_gain;
extern "C" const RadioCapabilities &__real__ZN2ht20backend_capabilitiesEv();

extern "C" const RadioCapabilities &__wrap__ZN2ht20backend_capabilitiesEv() {
    static RadioCapabilities capabilities;
    capabilities = __real__ZN2ht20backend_capabilitiesEv();
    capabilities.gain = enable_gain;
    return capabilities;
}

extern "C" int __real_fsync(int fd);

extern "C" int __wrap_fsync(int fd) {
    if (fail_durable) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    unsigned offset = 0;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            frame[y * 160 + x] = pixels[offset++].full;
        }
    }
    lv_disp_flush_ready(drv);
}

static void *setup() {
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "channels", 1));
    snprintf(path, sizeof(path), "%s/channels.bin", root);
    zassert_ok(setenv("SDL_VIDEODRIVER", "dummy", 1));
    lv_init();
    zassert_ok(ui_backend_start());
    lv_disp_draw_buf_init(&draw_buffer, buffer, nullptr, 160 * 16);
    lv_disp_drv_init(&driver);
    driver.hor_res = 160;
    driver.ver_res = 128;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = flush;
    display = lv_disp_drv_register(&driver);
    zassert_not_null(display);
    zassert_ok(ui_view_start(lv_disp_get_scr_act(display)));
    zassert_ok(settings_storage_init());
    return nullptr;
}

static void load(unsigned count = 6) {
    fail_durable = false;
    enable_gain = false;
    radio_power(true);
    zassert_ok(radio_start({}));
    unlink(path);
    plug.channel_count = plug.bank_count = 0;
    plug.channel_id_high_water = plug.bank_id_high_water = 0;
    plug.global = {};
    plug.vfo = {};
    plug.selection = {};
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 145500001;
    strcpy(plug.global.local_callsign, "OE3ANC");
    for (unsigned i = count; i > 0; --i) {
        Channel channel;
        uint32_t id;
        channel.number = i;
        snprintf(channel.name, sizeof(channel.name), "CHANNEL %03u", i);
        channel.configuration.rx_frequency_hz = channel.configuration.tx_frequency_hz =
            433000000 + i * 12500;
        if (i == 3) {
            channel.configuration.mode = Mode::M17;
        }
        zassert_ok(put_channel(plug, channel, id));
    }
    bank = {};
    strcpy(bank.name, "Local");
    if (count >= 4) {
        bank.count = 3;
        bank.channel_ids[0] = find_channel_number(plug, 4)->id;
        bank.channel_ids[1] = find_channel_number(plug, 1)->id;
        bank.channel_ids[2] = find_channel_number(plug, 3)->id;
    }
    uint32_t id;
    zassert_ok(put_bank(plug, bank, id));
    bank = {};
    strcpy(bank.name, "Empty");
    zassert_ok(put_bank(plug, bank, id));
    if (count == 256) {
        for (unsigned i = 3; i <= 16; ++i) {
            bank = {};
            snprintf(bank.name, sizeof(bank.name), "BANK %02u", i);
            bank.count = count;
            for (unsigned j = 0; j < count; ++j) {
                bank.channel_ids[j] = plug.channels[j].id;
            }
            zassert_ok(put_bank(plug, bank, id));
        }
    }
    uint32_t generation = 0;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
    ui_view_update(model);
    lv_tick_inc(200);
    lv_timer_handler();
}

static void before(void *) {
    load();
}

static void key(UiKey key) {
    model.input({key});
}

static void tick() {
    settings_service(radio_snapshot(), 0);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
    const auto storage = settings_status();
    model.storage_status(storage.load_error, storage.save_error, storage.pending);
}

static void menu(const char *name) {
    key(UiKey::Enter);
    bool found = false;
    for (unsigned i = 0; i < 20 && !found; ++i) {
        char lines[8][32];
        model.lines(lines);
        for (const auto &line : lines) {
            found |= line[0] == '>' && strstr(line, name);
        }
        if (!found) {
            key(UiKey::Down);
        }
    }
    zassert_true(found);
    key(UiKey::Enter);
}

static void frame_out(const char *name) {
    ui_view_update(model);
    lv_tick_inc(200);
    lv_timer_handler();
    lv_refr_now(display);
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    zassert_equal(memory.total_size, 32768);
    zassert_true(memory.free_size > 4096);
    minimum_free = MIN(minimum_free, memory.free_size);
    minimum_largest = MIN(minimum_largest, memory.free_biggest_size);
    maximum_fragmentation = MAX(maximum_fragmentation, memory.frag_pct);
    zassert_equal(lv_mem_test(), LV_RES_OK);
    const char *directory = getenv("HT_UI_QUICK_FRAMES");
    if (!directory) {
        return;
    }
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/%s.ppm", directory, name);
    FILE *file = fopen(filename, "wb");
    zassert_not_null(file);
    fprintf(file, "P6\n160 128\n255\n");
    for (uint16_t pixel : frame) {
        const uint8_t rgb[] = {static_cast<uint8_t>(((pixel >> 11) & 31) * 255 / 31),
                               static_cast<uint8_t>(((pixel >> 5) & 63) * 255 / 63),
                               static_cast<uint8_t>((pixel & 31) * 255 / 31)};
        zassert_equal(fwrite(rgb, 1, 3, file), 3);
    }
    zassert_ok(fclose(file));
}

static void open() {
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::QuickControls);
}

static void reboot() {
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
}

ZTEST(ui_quick, test_vfo_draft_cancel_apply_bounds_and_reboot) {
    const auto frequency = emulator_tuned_frequency();
    open();
    frame_out("quick-vfo");
    key(UiKey::Up);
    zassert_equal(radio_snapshot().config.squelch, 4);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.squelch, 4);
    open();
    for (unsigned i = 0; i < 20; ++i) {
        key(UiKey::Up);
    }
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[2], "Squelch: 15"));
    for (unsigned i = 0; i < 20; ++i) {
        key(UiKey::Down);
    }
    model.lines(lines);
    zassert_not_null(strstr(lines[2], "Squelch: 0"));
    key(UiKey::Up);
    key(UiKey::Up);
    frame_out("quick-preview");
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    frame_out("quick-pending");
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.squelch, 2);
    zassert_equal(emulator_tuned_frequency(), frequency);
    zassert_true(settings_status().pending);
    settings_service(radio_snapshot(), 1100);
    reboot();
    zassert_equal(radio_snapshot().config.squelch, 2);
}

ZTEST(ui_quick, test_menu_discovery_and_squelch_entry_use_draft_return_to_menu) {
    menu("Quick controls");
    zassert_equal(model.screen(), UiScreen::QuickControls);
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Menu);
    key(UiKey::Back);
    menu("Squelch:");
    zassert_equal(model.screen(), UiScreen::QuickControls);
    zassert_equal(radio_snapshot().config.squelch, 5);
    key(UiKey::Down);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(radio_snapshot().config.squelch, 5);
}

ZTEST(ui_quick, test_memory_squelch_is_temporary_and_recall_restores_saved_channel) {
    key(UiKey::Left);
    tick();
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    settings_service(radio_snapshot(), 1100);
    const auto generation = settings_status().generation;
    const auto selected = radio_snapshot().selection;
    const auto frequency = emulator_tuned_frequency();
    open();
    key(UiKey::Up);
    frame_out("quick-memory");
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.squelch, 5);
    zassert_true(same_selection(radio_snapshot().selection, selected));
    zassert_equal(emulator_tuned_frequency(), frequency);
    zassert_false(settings_status().pending);
    settings_service(radio_snapshot(), 2200);
    zassert_equal(settings_status().generation, generation);
    Channel channel;
    zassert_ok(settings_channel(selected.channel_id, channel));
    zassert_equal(channel.configuration.squelch, 4);
    zassert_ok(settings_recall(selected, 711, radio_snapshot()));
    tick();
    zassert_equal(radio_snapshot().config.squelch, 4);
}

ZTEST(ui_quick, test_supported_gain_field_switching_global_persistence_and_m17) {
    enable_gain = true;
    open();
    const char *actions[4];
    model.quick_actions(actions);
    zassert_equal(strcmp(actions[2], "P1 Field"), 0);
    zassert_equal(model.form_cursor(), 1);
    key(UiKey::Left);
    zassert_equal(model.form_cursor(), 0);
    key(UiKey::Up);
    frame_out("quick-gain-fixture");
    key(UiKey::Left);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.gain, 1);
    zassert_equal(radio_snapshot().config.squelch, 3);
    settings_service(radio_snapshot(), 1100);
    reboot();
    zassert_equal(radio_snapshot().config.gain, 1);
    RadioCommand command;
    command.config = radio_snapshot().config;
    command.config.mode = Mode::M17;
    zassert_ok(radio_submit(command));
    tick();
    open();
    model.quick_actions(actions);
    zassert_equal(strcmp(actions[2], ""), 0);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.gain, 0);
    zassert_equal(radio_snapshot().config.squelch, 3);
    zassert_equal(radio_snapshot().config.mode, Mode::M17);
}

ZTEST(ui_quick, test_unsupported_gain_and_m17_squelch_never_submit) {
    open();
    const char *actions[4];
    model.quick_actions(actions);
    zassert_equal(strcmp(actions[2], ""), 0);
    key(UiKey::Left);
    zassert_equal(model.form_cursor(), 0);
    key(UiKey::Back);
    RadioCommand command;
    command.config = radio_snapshot().config;
    command.config.mode = Mode::M17;
    zassert_ok(radio_submit(command));
    tick();
    open();
    zassert_false(model.quick_available());
    frame_out("quick-unavailable");
    model.quick_actions(actions);
    zassert_equal(strcmp(actions[0], ""), 0);
    key(UiKey::Up);
    key(UiKey::Enter);
    zassert_false(model.command_pending());
    zassert_equal(radio_snapshot().config.squelch, 4);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Home);
}

ZTEST(ui_quick, test_ptt_coalesced_press_focus_fault_power_restart_cancel) {
    open();
    key(UiKey::Up);
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    zassert_equal(radio_snapshot().config.squelch, 4);
    open();
    model.input({UiKey::Release});
    zassert_equal(model.screen(), UiScreen::Home);
    open();
    radio_ptt(true);
    radio_service();
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Back);
    zassert_equal(model.error(), -EBUSY);
    radio_ptt(false);
    tick();
    open();
    radio_power(false);
    radio_service();
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    radio_power(true);
    tick();
    open();
    zassert_ok(radio_start({}));
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    open();
    radio_report_fault(-EIO);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().fault, -EIO);
}

ZTEST(ui_quick, test_stale_draft_and_queued_apply_cannot_modify_changed_selection) {
    open();
    key(UiKey::Up);
    RadioCommand change;
    change.id = 981;
    change.config = radio_snapshot().config;
    change.config.rx_frequency_hz = change.config.tx_frequency_hz = 145600000;
    zassert_ok(radio_submit(change));
    radio_service();
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(model.error(), -ESTALE);
    open();
    key(UiKey::Up);
    change.config.rx_frequency_hz = change.config.tx_frequency_hz = 145700000;
    zassert_ok(radio_submit(change));
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    tick();
    tick();
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(radio_snapshot().config.squelch, 4);
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(emulator_tuned_frequency(), 145700000);
}

ZTEST(ui_quick, test_queue_pressure_release_and_cancelled_submitted_apply_do_not_reopen) {
    open();
    key(UiKey::Up);
    RadioCommand command;
    command.config = radio_snapshot().config;
    unsigned count = 0;
    while (!radio_submit(command)) {
        ++count;
    }
    zassert_true(count > 0);
    key(UiKey::Enter);
    zassert_equal(model.error(), -ENOMSG);
    frame_out("quick-busy");
    zassert_false(model.command_pending());
    radio_ptt(true);
    radio_service();
    model.sync(radio_snapshot());
    radio_ptt(false);
    radio_service();
    zassert_false(emulator_transmitting());
    for (unsigned i = 0; i < count; ++i) {
        radio_service();
    }
    model.sync(radio_snapshot());
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    model.input({UiKey::Release});
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    zassert_equal(radio_snapshot().config.squelch,
                  5); // Already submitted Apply may finish; the draft stays closed.
}

ZTEST(ui_quick, test_noop_keeps_monitor_and_storage_failure_reports_dirty_retry) {
    radio_monitor(true);
    tick();
    zassert_true(radio_snapshot().monitor_active);
    const auto revision = radio_snapshot().configuration_revision;
    open();
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().configuration_revision, revision);
    zassert_true(radio_snapshot().monitor_active);
    zassert_false(settings_status().pending);
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_false(radio_snapshot().monitor_active);
    fail_durable = true;
    settings_service(radio_snapshot(), 1100);
    zassert_equal(settings_status().save_error, -EIO);
    zassert_true(settings_status().pending);
    zassert_equal(radio_snapshot().config.squelch, 5);
    fail_durable = false;
    settings_service(radio_snapshot(), 2200);
    reboot();
    zassert_equal(radio_snapshot().config.squelch, 5);
}

ZTEST_SUITE(ui_quick, nullptr, setup, before, nullptr, nullptr);

ZTEST(ui_quick, test_repeated_palette_field_navigation_is_bounded_and_motion_off) {
    for (unsigned theme = 0; theme < 4; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            auto ui = plug.global.ui;
            ui.theme = static_cast<Theme>(theme);
            ui.contrast = static_cast<Contrast>(contrast);
            ui.animations = false;
            zassert_ok(
                settings_put_ui_preferences(ui, 880, radio_snapshot(), settings_status().revision));
            tick();
            enable_gain = true;
            for (unsigned i = 0; i < 10; ++i) {
                open();
                key(UiKey::Left);
                key(UiKey::Up);
                key(UiKey::Left);
                key(UiKey::Down);
                frame_out("quick-theme-fixture");
                key(UiKey::Back);
            }
        }
    }
    printk("Quick UI heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}
