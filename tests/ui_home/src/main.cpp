// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/battery.hpp>
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
static Codeplug plug;
static char root[] = "/tmp/ht-ui-home-XXXXXX", path[160];
static lv_color_t pixels[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static uint32_t channel_id, bank_id;
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
static unsigned flush_count;

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors) {
    ++flush_count;
    unsigned i = 0;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            frame[y * 160 + x] = colors[i++].full;
        }
    }
    lv_disp_flush_ready(drv);
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
    const auto *directory = getenv("HT_UI_HOME_FRAMES");
    if (!directory) {
        return;
    }
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(file_path, "wb");
    zassert_not_null(file);
    fprintf(file, "P6\n160 128\n255\n");
    for (uint16_t pixel : frame) {
        const unsigned r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
        const unsigned char rgb[] = {static_cast<unsigned char>((r << 3) | (r >> 2)),
                                     static_cast<unsigned char>((g << 2) | (g >> 4)),
                                     static_cast<unsigned char>((b << 3) | (b >> 2))};
        zassert_equal(fwrite(rgb, 1, 3, file), 3);
    }
    zassert_ok(fclose(file));
}

static void *setup() {
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "home", 1));
    snprintf(path, sizeof(path), "%s/home.bin", root);
    zassert_ok(settings_storage_init());
    lv_init();
    lv_disp_draw_buf_init(&draw_buffer, pixels, nullptr, 160 * 16);
    lv_disp_drv_init(&driver);
    driver.hor_res = 160;
    driver.ver_res = 128;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = flush;
    display = lv_disp_drv_register(&driver);
    zassert_not_null(display);
    zassert_ok(ui_view_start(lv_disp_get_scr_act(display)));
    zassert_equal(battery_snapshot().freshness, BatteryFreshness::Unknown);
    RadioState state;
    state.phase = RadioPhase::Receiving;
    model.sync(state);
    UiHome home;
    model.home(home);
    zassert_equal(strcmp(home.battery, "? V"), 0);
    render("battery-unknown");
    return nullptr;
}

static void load(bool memory = false, Mode mode = Mode::Fm, Theme theme = Theme::Midnight,
                 Contrast contrast = Contrast::Normal) {
    unlink(path);
    radio_power(true);
    radio_ptt(false);
    radio_monitor(false);
    plug.global = {};
    plug.vfo = {};
    plug.selection = {};
    plug.channel_count = plug.bank_count = 0;
    plug.channel_id_high_water = plug.bank_id_high_water = 0;
    plug.global.ui.idle_s = 0;
    plug.global.ui.theme = theme;
    plug.global.ui.contrast = contrast;
    strcpy(plug.global.local_callsign, "OE3ANC");
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 430000001;
    plug.vfo.mode = mode;
    Channel channel;
    channel.number = 7;
    strcpy(channel.name, "LOCAL REPEATER");
    channel.configuration.rx_frequency_hz = 145500001;
    channel.configuration.tx_frequency_hz = 144900001;
    channel.configuration.mode = mode;
    zassert_ok(put_channel(plug, channel, channel_id));
    Bank bank;
    strcpy(bank.name, "Local");
    bank.count = 1;
    bank.channel_ids[0] = channel_id;
    zassert_ok(put_bank(plug, bank, bank_id));
    if (memory) {
        plug.selection = {Operating::Memory, bank_id, channel_id};
    }
    uint32_t generation = 0;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    Selection selected;
    zassert_ok(settings_start(config, selected));
    zassert_ok(radio_start(config, selected));
    model = {};
    model.sync(radio_snapshot());
    emulator_inject_battery({7200, false, true}, 0);
    zassert_ok(battery_sample());
}

static void before(void *) {
    load();
}

static void tick(int64_t now = 0) {
    settings_service(radio_snapshot(), now);
    radio_service();
    settings_service(radio_snapshot(), now);
    model.sync(radio_snapshot());
    const auto store = settings_status();
    model.storage_status(store.load_error, store.save_error, store.pending);
}

static lv_obj_t *label(const char *value) {
    auto *content = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
    for (unsigned i = 0; i < lv_obj_get_child_cnt(content); ++i) {
        auto *object = lv_obj_get_child(content, i);
        if (lv_obj_check_type(object, &lv_label_class) &&
            !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) &&
            !strcmp(lv_label_get_text(object), value)) {
            return object;
        }
    }
    zassert_unreachable("Missing label: %s", value);
    return nullptr;
}

ZTEST(ui_home, test_vfo_exact_frequency_geometry_and_actual_ptt) {
    UiHome home;
    model.home(home);
    zassert_equal(strcmp(home.frequency, "430.000001"), 0);
    zassert_equal(strcmp(home.actions[0], "OK Menu"), 0);
    zassert_equal(strcmp(home.actions[1], "BACK Quick"), 0);
    zassert_equal(strcmp(home.actions[2], "P1 Memory"), 0);
    zassert_equal(strcmp(home.actions[3], "P2 Save"), 0);
    render("vfo-exact");
    zassert_equal(lv_obj_get_style_text_font(label(home.frequency), 0), &lv_font_montserrat_22);
    const auto idle_flushes = flush_count;
    ui_view_update(model);
    lv_refr_now(display);
    zassert_equal(flush_count, idle_flushes, "Unchanged Home must not redraw");
    zassert_equal(lv_obj_get_x(label("OK Menu")), 8);
    zassert_equal(lv_obj_get_x(label("BACK Quick")), 82);
    zassert_equal(lv_obj_get_y(label("P1 Memory")), 115);
    zassert_equal(lv_obj_get_y(label("P2 Save")), 115);
    radio_ptt(true);
    tick();
    model.home(home);
    zassert_true(home.transmitting);
    zassert_equal(strcmp(home.activity, "Transmitting"), 0);
    zassert_is_null(home.actions[0]);
    render("vfo-tx");
    model.input({UiKey::Enter});
    zassert_equal(model.screen(), UiScreen::Home);
    radio_ptt(false);
    tick();
    zassert_false(emulator_transmitting());
    auto state = radio_snapshot();
    state.config.rx_frequency_hz = 433500000;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.frequency, "433.500"), 0);
    state.config.rx_frequency_hz = 433506250;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.frequency, "433.50625"), 0);
}

ZTEST(ui_home, test_memory_metadata_split_rx_only_and_missing_identity) {
    load(true);
    UiHome home;
    model.home(home);
    zassert_equal(strcmp(home.identity, "MEM 007"), 0);
    zassert_equal(strcmp(home.name, "LOCAL REPEATER"), 0);
    zassert_equal(strcmp(home.context, "MHz / Local"), 0);
    zassert_not_null(strstr(home.settings, "SPLIT"));
    zassert_equal(strcmp(home.actions[3], "P2 Edit"), 0);
    render("memory-rx");
    radio_ptt(true);
    tick();
    model.home(home);
    zassert_equal(strcmp(home.frequency, "144.900001"), 0);
    render("memory-tx");
    radio_ptt(false);
    tick();
    auto state = radio_snapshot();
    state.config.tx_inhibit = true;
    model.sync(state);
    model.home(home);
    zassert_not_null(strstr(home.settings, "RX ONLY"));
    render("receive-only");
    state.selection.channel_id = UINT32_MAX;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.name, "Channel unavailable"), 0);
    char name[25] = "untouched", bank[25] = "untouched";
    uint16_t number = 23;
    zassert_equal(settings_selection_labels(state.selection, name, number, bank), -ENOENT);
    zassert_equal(strcmp(name, "untouched"), 0);
    zassert_equal(strcmp(bank, "untouched"), 0);
    zassert_equal(number, 23);
}

ZTEST(ui_home, test_rssi_smoothing_clear_clock_and_no_mutation) {
    const auto revision = radio_snapshot().configuration_revision;
    auto state = radio_snapshot();
    state.rx_active = true;
    state.rssi_dbm = -80;
    model.sync(state);
    UiHome home;
    for (int64_t now = 0; now < 400; now += 80) {
        model.advance(now);
        model.home(home);
        zassert_equal(home.bars, now / 80 + 1);
        model.advance(now + 79);
        model.home(home);
        zassert_equal(home.bars, now / 80 + 1);
    }
    render("rssi-full");
    state.rssi_dbm = -121;
    model.sync(state);
    model.advance(400);
    model.home(home);
    zassert_equal(home.bars, 4);
    model.advance(10000);
    model.home(home);
    zassert_equal(home.bars, 3); // no multi-step catch-up
    state.rx_active = false;
    model.sync(state);
    model.home(home);
    zassert_equal(home.bars, 0);
    zassert_equal(strcmp(home.activity, "Listening"), 0);
    state.rx_active = true;
    state.rssi_dbm = -110;
    model.sync(state);
    model.advance(10010);
    model.home(home);
    zassert_equal(home.bars, 1);
    model.advance(0);
    model.home(home);
    zassert_equal(home.bars, 2);
    state.phase = RadioPhase::Transmitting;
    model.sync(state);
    model.home(home);
    zassert_equal(home.bars, 0);
    zassert_equal(radio_snapshot().configuration_revision, revision);
    zassert_false(settings_status().pending);
}

ZTEST(ui_home, test_m17_callsign_can_filter_and_monitor) {
    load(true, Mode::M17);
    auto state = radio_snapshot();
    state.rx_active = true;
    state.rssi_dbm = -90;
    state.config.rx_frequency_hz = 145000000;
    state.config.m17.can = 15;
    state.config.m17.rx_can_check = true;
    state.config.m17.destination = Destination::Station;
    strcpy(state.config.m17.callsign, "OE1TEST");
    strcpy(state.received_callsign, "OE3VOICE");
    state.m17_quality.locked = state.m17_quality.sampled = true;
    state.m17_quality.sample_ms = k_uptime_get();
    state.m17_quality.errors = 3;
    state.m17_quality.lost = 2;
    state.m17_quality.rejected = 1;
    model.sync(state);
    model.advance(0);
    UiHome home;
    model.home(home);
    zassert_equal(strcmp(home.activity, "RX OE3VOICE"), 0);
    zassert_equal(strcmp(home.context, "BER~1.10% L2 B1"), 0);
    zassert_not_null(strstr(home.settings, "CAN15F OE1TEST"));
    render("m17-rx");
    state.m17_quality.lost = 10000;
    state.m17_quality.rejected = UINT32_MAX;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.context, "BER~1.10% L9999+ B9999+"), 0);
    state.m17_quality.sample_ms = k_uptime_get() - 500;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.context, "BER~-- / no fresh RX"), 0);
    state.phase = RadioPhase::Transmitting;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.context, "TX / RX stats --"), 0);
    state.phase = RadioPhase::Receiving;
    state.rx_active = false;
    model.sync(state);
    model.home(home);
    zassert_is_null(strstr(home.activity, "OE3VOICE"));
    load();
    state = radio_snapshot();
    state.monitor_active = true;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.activity, "FM monitor / held"), 0);
    render("monitor");
}

ZTEST(ui_home, test_battery_fresh_charger_failure_staleness_and_recovery) {
    UiHome home;
    model.home(home);
    zassert_equal(strcmp(home.battery, "7.20V"), 0);
    emulator_inject_battery({8000, true, true}, 0);
    zassert_ok(battery_sample());
    model.home(home);
    zassert_equal(strcmp(home.battery, "8.00+"), 0);
    zassert_equal(home.battery_color, UiStatusColor::Accent);
    render("charger");
    emulator_inject_battery({0, false, false}, -EIO);
    zassert_equal(battery_sample(), -EIO);
    model.home(home);
    zassert_equal(strcmp(home.battery, "8.00!"), 0);
    zassert_equal(home.battery_color, UiStatusColor::Amber);
    render("battery-stale");
    zassert_true(radio_snapshot().power_active);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    emulator_inject_battery({7300, false, true}, 0);
    zassert_ok(battery_sample());
    model.home(home);
    zassert_equal(strcmp(home.battery, "7.30V"), 0);
    k_sleep(K_MSEC(301));
    model.home(home);
    zassert_equal(strcmp(home.battery, "7.30!"), 0);
    zassert_ok(battery_sample());
}

ZTEST(ui_home, test_warning_timeout_overlay_fault_priority_and_restore) {
    model.input({UiKey::Enter});
    zassert_equal(model.screen(), UiScreen::Menu);
    auto state = radio_snapshot();
    state.phase = RadioPhase::Transmitting;
    state.tx_warning = true;
    state.tx_remaining_s = 10;
    model.sync(state);
    UiHome home;
    model.home(home);
    zassert_true(home.visible);
    zassert_equal(strcmp(home.activity, "TX ends in 10 s"), 0);
    render("tx-warning");
    const auto warning_flushes = flush_count;
    ui_view_update(model);
    lv_refr_now(display);
    zassert_equal(flush_count, warning_flushes);
    model.input({UiKey::Back});
    zassert_equal(model.screen(), UiScreen::Menu);
    state.phase = RadioPhase::Receiving;
    state.tx_warning = false;
    state.tx_timed_out = true;
    model.sync(state);
    model.home(home);
    zassert_equal(strcmp(home.activity, "Timeout / release PTT"), 0);
    zassert_equal(home.status, UiStatusColor::Red);
    render("tx-timeout");
    model.input({UiKey::Back});
    zassert_equal(model.screen(), UiScreen::Menu);
    state.phase = RadioPhase::Fault;
    state.fault = -EPIPE;
    model.sync(state);
    model.home(home);
    zassert_false(home.visible);
    render("fault");
    auto *screen = lv_disp_get_scr_act(display);
    zassert_equal(lv_obj_get_child_cnt(screen), 1); // Only the shared modern root remains.
    zassert_false(lv_obj_has_flag(lv_obj_get_child(screen, -1), LV_OBJ_FLAG_HIDDEN));
    state.power_active = false;
    model.sync(state);
    model.home(home);
    zassert_false(home.visible);
    render("inactive");
    load();
    model.input({UiKey::Hash});
    ui_view_update(model);
    // Every shared Home label must restore its editor/list typography and geometry.
    model.input({UiKey::Back});
    ui_view_update(model);
    model.home(home);
    zassert_true(home.visible);
}

ZTEST(ui_home, test_storage_feedback_themes_lock_and_layout_restoration) {
    model.storage_status(0, -EIO, true);
    UiHome home;
    model.home(home);
    zassert_not_null(strstr(home.context, "Unsaved"));
    zassert_equal(home.context_color, UiStatusColor::Red);
    render("storage-error");
    model.storage_status(-ENOENT, 0, false);
    model.home(home);
    zassert_equal(strcmp(home.context, "First-run defaults"), 0);
    zassert_equal(home.context_color, UiStatusColor::Muted);
    model.storage_status(-ENOENT, -ENOENT, true);
    model.home(home);
    zassert_not_null(strstr(home.context, "Unsaved"));
    zassert_equal(home.context_color, UiStatusColor::Red);
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[6], "Storage error -2"));
    model.storage_status(0, 0, true);
    model.home(home);
    zassert_equal(strcmp(home.context, "Settings pending"), 0);
    render("storage-pending");
    model.storage_status(0, 0, false);
    model.home(home);
    zassert_equal(strcmp(home.activity, "Listening"), 0);
    const auto now = k_uptime_get();
    ui_keypad_star(true, now);
    model.advance(now);
    model.advance(now + 1000);
    model.home(home);
    zassert_true(home.locked);
    zassert_equal(strcmp(home.actions[0], "Hold * 1s"), 0);
    render("locked");
    ui_keypad_star(false, now + 1001);
    const auto locked_flushes = flush_count;
    ui_view_update(model);
    lv_refr_now(display);
    zassert_equal(flush_count, locked_flushes);
    for (unsigned theme = 0; theme < 4; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            load(false, Mode::Fm, static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            char name[32];
            snprintf(name, sizeof(name), "theme-%u-%u", theme, contrast);
            render(name);
            const auto colors =
                ui_palette(static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            zassert_equal(lv_obj_get_style_bg_color(lv_disp_get_scr_act(display), 0).full,
                          lv_color_hex(colors.background).full);
            zassert_equal(lv_obj_get_style_text_color(label("430.000001"), 0).full,
                          lv_color_hex(colors.white).full);
            zassert_true(lv_txt_get_width("430.000001", 10, &lv_font_montserrat_22, 0,
                                          LV_TEXT_FLAG_NONE) <= 148);
        }
    }
}

static void finish(void *) {
    unlink(path);
    rmdir(root);
    printk("Home heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST(ui_home, test_documentation_khz_frames) {
    const char *names[] = {"docs-vfo", "docs-memory", "docs-m17"};
    for (unsigned i = 0; i < 3; ++i) {
        load(i != 0, i == 2 ? Mode::M17 : Mode::Fm);
        auto state = radio_snapshot();
        state.config.rx_frequency_hz -= state.config.rx_frequency_hz % 1000;
        state.config.tx_frequency_hz -= state.config.tx_frequency_hz % 1000;
        if (i == 2) {
            state.rx_active = true;
            state.rssi_dbm = -90;
            state.config.m17.can = 15;
            state.config.m17.rx_can_check = true;
            state.config.m17.destination = Destination::Station;
            strcpy(state.config.m17.callsign, "OE1TEST");
            strcpy(state.received_callsign, "OE3VOICE");
        }
        model.sync(state);
        model.advance(0);
        UiHome home;
        model.home(home);
        zassert_equal(strcmp(home.frequency, i == 0 ? "430.000" : "145.500"), 0);
        render(names[i]);
    }
}

ZTEST_SUITE(ui_home, nullptr, setup, before, nullptr, finish);
