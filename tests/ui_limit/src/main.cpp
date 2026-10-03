// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
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
static Codeplug plug, restored;
static char root[] = "/tmp/ht-ui-limit-XXXXXX", path[160];
static lv_color_t pixels[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static bool fail_durable;
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
extern "C" int __real_fsync(int fd);

extern "C" int __wrap_fsync(int fd) {
    if (fail_durable) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors) {
    unsigned i = 0;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            frame[y * 160 + x] = colors[i++].full;
        }
    }
    lv_disp_flush_ready(drv);
}

static void *setup() {
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "limit", 1));
    zassert_ok(setenv("SDL_VIDEODRIVER", "dummy", 1));
    snprintf(path, sizeof(path), "%s/limit.bin", root);
    zassert_ok(settings_storage_init());
    lv_init();
    zassert_ok(ui_backend_start());
    lv_disp_draw_buf_init(&draw_buffer, pixels, nullptr, 160 * 16);
    lv_disp_drv_init(&driver);
    driver.hor_res = 160;
    driver.ver_res = 128;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = flush;
    display = lv_disp_drv_register(&driver);
    zassert_not_null(display);
    zassert_ok(ui_view_start(lv_disp_get_scr_act(display)));
    return nullptr;
}

static void load(bool memory = false, Mode mode = Mode::Fm) {
    fail_durable = false;
    unlink(path);
    radio_power(true);
    plug.global = {};
    plug.vfo = {};
    plug.selection = {};
    plug.channel_count = plug.bank_count = 0;
    plug.channel_id_high_water = plug.bank_id_high_water = 0;
    plug.global.ui.idle_s = 0;
    strcpy(plug.global.local_callsign, "OE3ANC");
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 430000001;
    plug.vfo.mode = mode;
    Channel channel;
    channel.number = 7;
    strcpy(channel.name, "REPEATER");
    channel.configuration.rx_frequency_hz = 145500001;
    channel.configuration.tx_frequency_hz = 145000001;
    channel.configuration.mode = mode;
    uint32_t id;
    zassert_ok(put_channel(plug, channel, id));
    if (memory) {
        plug.selection = {Operating::Memory, 0, id};
    }
    uint32_t generation = 0;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    Selection selected;
    zassert_ok(settings_start(config, selected));
    zassert_ok(radio_start(config, selected));
    model = {};
    model.sync(radio_snapshot());
}

static void before(void *) {
    load();
}

static void key(UiKey key) {
    model.input({key});
}

static void tick(int64_t now = 0) {
    settings_service(radio_snapshot(), now);
    radio_service();
    settings_service(radio_snapshot(), now);
    model.sync(radio_snapshot());
    const auto store = settings_status();
    model.storage_status(store.load_error, store.save_error, store.pending);
}

static void open(const char *name = "TX limit") {
    if (model.screen() == UiScreen::Menu) {
        key(UiKey::Back);
    }
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Enter);
    for (unsigned i = 0; i < 32; ++i) {
        char lines[8][32];
        model.lines(lines);
        bool found = false;
        for (const auto &line : lines) {
            found |= line[0] == '>' && strstr(line, name);
        }
        if (found) {
            key(UiKey::Enter);
            return;
        }
        key(UiKey::Down);
    }
    zassert_unreachable("Menu entry not found");
}

static void render(const char *name) {
    ui_view_update(model);
    lv_tick_inc(200);
    lv_timer_handler();
    lv_refr_now(display);
    lv_mem_monitor_t heap;
    lv_mem_monitor(&heap);
    zassert_equal(heap.total_size, 32768);
    zassert_true(heap.free_size > 4096);
    zassert_equal(lv_mem_test(), LV_RES_OK);
    minimum_free = MIN(minimum_free, heap.free_size);
    minimum_largest = MIN(minimum_largest, heap.free_biggest_size);
    maximum_fragmentation = MAX(maximum_fragmentation, heap.frag_pct);
    const auto *directory = getenv("HT_UI_LIMIT_FRAMES");
    if (!directory) {
        return;
    }
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(file_path, "wb");
    zassert_not_null(file);
    fprintf(file, "P6\n160 128\n255\n");
    for (uint16_t pixel : frame) {
        const unsigned r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
        const unsigned char rgb[] = {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)),
                                     uint8_t((b << 3) | (b >> 2))};
        zassert_equal(fwrite(rgb, 1, 3, file), 3);
    }
    zassert_ok(fclose(file));
}

ZTEST(ui_limit, test_all_draft_values_cancel_and_explicit_persistent_apply) {
    const uint16_t limits[] = {180, 0, 60, 120};
    const char *names[] = {"limit-180", "limit-off", "limit-60", "limit-120"};
    open();
    zassert_equal(model.screen(), UiScreen::TransmitLimit);
    const auto revision = settings_status().revision;
    for (unsigned i = 0; i < 4; ++i) {
        char lines[8][32], expected[32];
        model.lines(lines);
        if (limits[i]) {
            snprintf(expected, sizeof(expected), "%u seconds", limits[i]);
        } else {
            strcpy(expected, "Off");
        }
        zassert_equal(strcmp(lines[2], expected), 0);
        render(names[i]);
        zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
        zassert_equal(settings_status().revision, revision);
        key(UiKey::Up);
    }
    key(UiKey::Down);
    key(UiKey::Back);
    tick();
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    render("limit-pending");
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    tick();
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 0);
    zassert_true(settings_status().pending);
    tick(999);
    zassert_true(settings_status().pending);
    tick(1000);
    zassert_false(settings_status().pending);
    RadioConfig config;
    Selection selected;
    zassert_ok(settings_start(config, selected));
    zassert_equal(config.transmit_limit_s, 0);
}

ZTEST(ui_limit, test_global_limit_preserves_memory_temporary_squelch_and_vfo) {
    load(true);
    auto before = radio_snapshot();
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config = before.config;
    quick.config.squelch = 9;
    quick.expected_generation = before.generation;
    quick.expected_revision = before.configuration_revision;
    quick.selection = before.selection;
    zassert_ok(radio_submit(quick));
    tick();
    radio_monitor(true);
    tick();
    before = radio_snapshot();
    open();
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    const auto after = radio_snapshot();
    zassert_equal(after.config.transmit_limit_s, 120);
    zassert_true(same_selection(before.selection, after.selection));
    zassert_true(same_operating(before.config, after.config));
    zassert_true(after.monitor_active);
    zassert_equal(after.config.squelch, 9);
    tick(1000);
    uint32_t generation;
    zassert_ok(codeplug_load(restored, generation));
    zassert_equal(restored.global.transmit_limit_s, 120);
    zassert_equal(restored.vfo.rx_frequency_hz, 430000001);
    zassert_equal(restored.channels[0].configuration.squelch,
                  plug.channels[0].configuration.squelch);
    zassert_equal(restored.channels[0].configuration.tx_frequency_hz, 145000001);
    zassert_equal(strcmp(restored.global.local_callsign, "OE3ANC"), 0);
}

ZTEST(ui_limit, test_noop_and_m17_edits_keep_stream_configuration_and_selection) {
    load(true, Mode::M17);
    const auto before = radio_snapshot();
    const auto revision = settings_status().revision;
    open();
    key(UiKey::Enter);
    tick();
    zassert_ok(model.error());
    zassert_equal(radio_snapshot().configuration_revision, before.configuration_revision);
    zassert_equal(settings_status().revision, revision);
    zassert_false(settings_status().pending);
    open();
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    const auto after = radio_snapshot();
    zassert_equal(after.config.transmit_limit_s, 120);
    zassert_true(same_operating(before.config, after.config));
    zassert_true(same_selection(before.selection, after.selection));
    zassert_equal(after.config.mode, Mode::M17);
}

ZTEST(ui_limit, test_ptt_focus_fault_power_and_restart_cancel_drafts) {
    open();
    key(UiKey::Down);
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    tick();
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    open();
    model.input({UiKey::Release});
    zassert_equal(model.screen(), UiScreen::Home);
    open();
    radio_ptt(true);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    tick();
    open();
    radio_power(false);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    radio_power(true);
    tick();
    open();
    zassert_ok(radio_start(radio_snapshot().config));
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    open();
    radio_report_fault(-EIO);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
}

ZTEST(ui_limit, test_queued_apply_guards_stale_state_and_never_reopens_after_cancel) {
    open();
    key(UiKey::Down);
    RadioCommand tune;
    tune.id = 800;
    tune.config = radio_snapshot().config;
    tune.config.rx_frequency_hz = tune.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(tune));
    key(UiKey::Enter);
    tick();
    tick();
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    open();
    key(UiKey::Down);
    key(UiKey::Enter);
    model.input({UiKey::Release});
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 120);
    open();
    key(UiKey::Down);
    key(UiKey::Enter);
    model.input({UiKey::Ptt});
    radio_ptt(true);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 120);
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    tick();
    zassert_false(emulator_transmitting());
}

ZTEST(ui_limit, test_queue_busy_and_storage_failure_are_explicit_and_retryable) {
    open();
    RadioCommand filler;
    filler.config = radio_snapshot().config;
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(filler));
    }
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_equal(model.error(), -ENOMSG);
    zassert_false(model.command_pending());
    render("limit-queue");
    for (unsigned i = 0; i < 8; ++i) {
        tick();
    }
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    if (model.screen() != UiScreen::Home) {
        key(UiKey::Back);
    }
    open();
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    fail_durable = true;
    tick(1000);
    zassert_equal(settings_status().save_error, -EIO);
    zassert_true(settings_status().pending);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 120);
    fail_durable = false;
    tick(2000);
    zassert_false(settings_status().pending);
    uint32_t generation;
    zassert_ok(codeplug_load(restored, generation));
    zassert_equal(restored.global.transmit_limit_s, 120);
}

ZTEST(ui_limit, test_theme_contrast_forms_and_font_restore) {
    for (unsigned theme = 0; theme < 4; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            UiPreferences prefs;
            uint32_t revision;
            settings_ui_preferences(prefs, &revision);
            prefs.theme = Theme(theme);
            prefs.contrast = Contrast(contrast);
            zassert_ok(settings_put_ui_preferences(prefs, 900 + theme * 3 + contrast,
                                                   radio_snapshot(), revision));
            tick();
            open();
            ui_view_update(model);
            lv_tick_inc(200);
            lv_timer_handler();
            lv_refr_now(display);
            if (theme == 1 && contrast == 1) {
                render("limit-nord-high");
            }
            if (theme == 3 && contrast == 2) {
                render("limit-darcula-max");
            }
            auto *content = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
            bool found = false;
            for (unsigned i = 0; i < lv_obj_get_child_cnt(content); ++i) {
                auto *child = lv_obj_get_child(content, i);
                if (lv_obj_check_type(child, &lv_label_class) &&
                    !strcmp(lv_label_get_text(child), "180 seconds")) {
                    zassert_equal(lv_obj_get_style_text_font(child, 0), &lv_font_montserrat_14);
                    found = true;
                }
            }
            zassert_true(found);
            key(UiKey::Back);
            open("Appearance");
            ui_view_update(model);
            for (unsigned i = 0; i < lv_obj_get_child_cnt(content); ++i) {
                auto *child = lv_obj_get_child(content, i);
                if (lv_obj_check_type(child, &lv_label_class) &&
                    !strcmp(lv_label_get_text(child), "Midnight")) {
                    zassert_equal(lv_obj_get_style_text_font(child, 0), &lv_font_montserrat_10);
                }
            }
            key(UiKey::Back);
            key(UiKey::Back);
        }
    }
}

static void finish(void *) {
    printk("Limit UI heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST_SUITE(ui_limit, nullptr, setup, before, nullptr, finish);
