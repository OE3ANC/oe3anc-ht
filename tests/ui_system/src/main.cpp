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
static char root[] = "/tmp/ht-ui-system-XXXXXX", path[160];
static lv_color_t pixels[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
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
    const auto *directory = getenv("HT_UI_SYSTEM_FRAMES");
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
    zassert_ok(setenv("HT_PROFILE", "system", 1));
    snprintf(path, sizeof(path), "%s/system.bin", root);
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
    RadioState state;
    state.power_active = false;
    state.phase = RadioPhase::Inactive;
    model.sync(state);
    UiStatus view;
    zassert_true(model.system_state(view));
    zassert_equal(strcmp(view.rows[2], "Battery unknown"), 0);
    zassert_equal(strcmp(view.rows[1], "Switch input unknown"), 0);
    render("inactive-unknown");
    emulator_inject_battery({}, -EIO);
    zassert_equal(battery_sample(), -EIO);
    zassert_true(model.system_state(view));
    zassert_equal(strcmp(view.rows[2], "Battery unknown"), 0);
    zassert_not_null(strstr(view.detail, "read error"));
    zassert_equal(view.color, UiStatusColor::Red);
    render("inactive-unknown-error");
    return nullptr;
}

static void load(Theme theme = Theme::Midnight, Contrast contrast = Contrast::Normal) {
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

static void no_helpers() {
    auto *content = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
    zassert_false(lv_obj_has_flag(content, LV_OBJ_FLAG_HIDDEN));
    unsigned count = 0;
    for (unsigned i = 0; i < lv_obj_get_child_cnt(content); ++i) {
        auto *object = lv_obj_get_child(content, i);
        if (lv_obj_check_type(object, &lv_label_class) && lv_obj_get_y(object) >= 102) {
            zassert_equal(strcmp(lv_label_get_text(object), ""), 0);
            ++count;
        }
    }
    zassert_equal(count, 4);
}

static void inactive() {
    auto state = radio_snapshot();
    state.power_active = false;
    state.phase = RadioPhase::Inactive;
    state.tx_warning = true;
    state.tx_timed_out = true;
    model.sync(state);
}

ZTEST(ui_system, test_actual_tx_fault_immediate_priority_release_and_no_hidden_actions) {
    model.input({UiKey::Enter});
    ui_view_update(model);
    model.input({UiKey::Down});
    ui_view_update(model);
    zassert_true(lv_anim_count_running() > 0);
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    radio_report_fault(-EPIPE);
    radio_service();
    model.sync(radio_snapshot());
    ui_view_update(model);
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(lv_anim_count_running(), 0);
    render("fault-tx");
    no_helpers();
    label("RADIO FAULT");
    label("Latched error -32");
    label("Restart radio to recover");
    const auto idle = flush_count;
    ui_view_update(model);
    lv_refr_now(display);
    zassert_equal(flush_count, idle);
    const auto revision = radio_snapshot().configuration_revision;
    const UiKey keys[] = {UiKey::Enter, UiKey::Back, UiKey::Left, UiKey::Right, UiKey::Up,
                          UiKey::Down,  UiKey::Star, UiKey::Hash, UiKey::Digit};
    for (auto key : keys) {
        model.input({key, '1'});
    }
    zassert_equal(radio_snapshot().configuration_revision, revision);
    zassert_false(model.command_pending());
    model.input({UiKey::Release});
    radio_ptt(false);
    radio_service();
    zassert_false(radio_ptt_requested());
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
}

ZTEST(ui_system, test_inactive_preserves_fault_and_cannot_offer_switch_recovery) {
    radio_report_fault(-EIO);
    radio_service();
    radio_power(false);
    radio_service();
    model.sync(radio_snapshot());
    UiStatus view;
    zassert_true(model.system_state(view));
    zassert_equal(strcmp(view.title, "RADIO INACTIVE"), 0);
    zassert_equal(strcmp(view.rows[1], "Fault survives switch-on"), 0);
    zassert_equal(strcmp(view.rows[3], "Restart radio to recover"), 0);
    zassert_not_null(strstr(view.detail, "Fault latched"));
    zassert_equal(view.color, UiStatusColor::Red);
    render("inactive-fault");
    no_helpers();
    radio_power(true);
    radio_service();
    model.sync(radio_snapshot());
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    render("fault-resume");
    label("RADIO FAULT");
}

ZTEST(ui_system, test_cached_cradle_switch_stale_error_and_confirmation_truth) {
    emulator_inject_battery({7234, true, false}, 0);
    zassert_ok(battery_sample());
    inactive();
    UiStatus view;
    zassert_true(model.system_state(view));
    zassert_equal(strcmp(view.detail, "Charger present"), 0);
    zassert_equal(strcmp(view.rows[1], "Power switch off"), 0);
    zassert_equal(strcmp(view.rows[2], "Battery 7234 mV"), 0);
    zassert_equal(strcmp(view.rows[3], "Switch on to resume"), 0);
    const auto tuned = emulator_tuned_frequency();
    render("inactive-cradle");
    no_helpers();
    emulator_inject_battery({}, -EIO);
    zassert_equal(battery_sample(), -EIO);
    zassert_true(model.system_state(view));
    zassert_equal(strcmp(view.rows[1], "Switch input unknown"), 0);
    zassert_equal(strcmp(view.rows[2], "Last Battery 7234 mV"), 0);
    zassert_equal(view.color, UiStatusColor::Red);
    render("inactive-read-error");
    emulator_inject_battery({7251, false, false}, 0);
    zassert_ok(battery_sample());
    k_sleep(K_MSEC(301));
    zassert_true(model.system_state(view));
    zassert_equal(strcmp(view.detail, "Last Charger absent"), 0);
    zassert_equal(view.color, UiStatusColor::Amber);
    render("inactive-stale");
    emulator_inject_battery({7251, false, true}, 0);
    zassert_ok(battery_sample());
    zassert_true(model.system_state(view));
    zassert_equal(strcmp(view.rows[1], "Confirming switch-on"), 0);
    zassert_equal(strcmp(view.rows[3], "Wait for confirmation"), 0);
    render("inactive-confirming");
    zassert_equal(emulator_tuned_frequency(), tuned);
}

ZTEST(ui_system, test_preview_discard_applied_theme_and_normal_layout_restoration) {
    model.input({UiKey::Enter});
    for (unsigned i = 0; i < 32; ++i) {
        UiListPage page;
        model.menu_page(page);
        if (!strcmp(page.rows[page.cursor % 4].name, "Appearance")) {
            model.input({UiKey::Enter});
            break;
        }
        model.input({UiKey::Down});
    }
    zassert_equal(model.screen(), UiScreen::Appearance);
    model.input({UiKey::Down});
    zassert_equal(model.preferences().theme, Theme::Nord);
    radio_report_fault(-EFAULT);
    radio_service();
    model.sync(radio_snapshot());
    zassert_equal(model.preferences().theme, Theme::Midnight);
    render("fault-preview-discarded");
    no_helpers();
    load();
    render("home-restored");
    label("OK Menu");
    label("BACK Quick");
    UiStatus view;
    zassert_false(model.system_state(view));
    model.input({UiKey::Enter});
    render("menu-restored");
    label("OK Select");
    zassert_equal(lv_obj_get_style_text_font(label("Channels"), 0), &lv_font_montserrat_10);
}

ZTEST(ui_system, test_all_theme_contrast_fault_inactive_bounded_and_idle) {
    for (unsigned theme = 0; theme < ThemeCount; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            load(static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            auto state = radio_snapshot();
            state.phase = RadioPhase::Fault;
            state.fault = -EPIPE;
            state.tx_warning = true;
            state.tx_timed_out = true;
            model.sync(state);
            char name[40];
            snprintf(name, sizeof(name), "fault-theme-%u-%u", theme, contrast);
            render(name);
            no_helpers();
            const auto colors =
                ui_palette(static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            zassert_equal(lv_obj_get_style_text_color(label("RADIO FAULT"), 0).full,
                          lv_color_hex(colors.red).full);
            emulator_inject_battery({7200, true, false}, 0);
            zassert_ok(battery_sample());
            state.power_active = false;
            state.fault = 0;
            state.phase = RadioPhase::Inactive;
            model.sync(state);
            snprintf(name, sizeof(name), "inactive-theme-%u-%u", theme, contrast);
            render(name);
            no_helpers();
            const auto idle = flush_count;
            ui_view_update(model);
            lv_refr_now(display);
            zassert_equal(flush_count, idle);
        }
    }
    printk("System-state heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST_SUITE(ui_system, nullptr, setup, before, nullptr, nullptr);
