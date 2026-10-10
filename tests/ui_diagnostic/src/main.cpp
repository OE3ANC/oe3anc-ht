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
static char root[] = "/tmp/ht-ui-diagnostic-XXXXXX", path[160];
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
    const auto *directory = getenv("HT_UI_DIAGNOSTIC_FRAMES");
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
    zassert_ok(setenv("HT_PROFILE", "diagnostic", 1));
    snprintf(path, sizeof(path), "%s/diagnostic.bin", root);
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
    Channel channel;
    channel.number = 7;
    strcpy(channel.name, "DIAGNOSTIC BASELINE");
    channel.configuration.rx_frequency_hz = 439075000;
    channel.configuration.tx_frequency_hz = 431475000;
    channel.configuration.mode = mode;
    channel.configuration.power_mw = 2500;
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
    emulator_inject_battery({7200, false, true}, 0);
    zassert_ok(battery_sample());
}

static void before(void *) {
    load();
}

static void tick() {
    settings_service(radio_snapshot(), 0);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
}

static void key(UiKey value) {
    model.input({value});
}

static void type(const char *value) {
    for (; *value; ++value) {
        model.input({UiKey::Character, *value});
    }
}

static void open() {
    key(UiKey::Enter);
    for (unsigned i = 0; i < 32; ++i) {
        UiListPage page;
        model.menu_page(page);
        if (!strcmp(page.rows[page.cursor % 4].name, "BK4819 diagnostics")) {
            key(UiKey::Enter);
            break;
        }
        key(UiKey::Down);
    }
    zassert_true(model.command_pending());
    tick();
    zassert_equal(model.screen(), UiScreen::Diagnostics);
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    zassert_false(emulator_transmitting());
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

static void address(const char *value) {
    key(UiKey::Enter);
    type(value);
    key(UiKey::Enter);
}

static void value(const char *text) {
    key(UiKey::Down);
    key(UiKey::Enter);
    type(text);
    key(UiKey::Enter);
}

ZTEST(ui_diagnostic, test_entry_paged_navigation_physical_helpers_and_idle) {
    open();
    render("diagnostic");
    label("OK Edit");
    label("BACK Exit");
    zassert_equal(lv_obj_get_child_cnt(lv_disp_get_scr_act(display)), 1); // No legacy widgets.
    const auto idle = flush_count;
    ui_view_update(model);
    lv_refr_now(display);
    zassert_equal(flush_count, idle);
    UiListPage page;
    model.diagnostic_page(page);
    zassert_equal(page.count, 5);
    key(UiKey::Left);
    key(UiKey::Right);
    model.diagnostic_page(page);
    zassert_equal(page.cursor, 0);
    key(UiKey::Up);
    model.diagnostic_page(page);
    zassert_equal(page.cursor, 4);
    render("diagnostic-exit");
    label("OK Restore");
    key(UiKey::Down);
    model.diagnostic_page(page);
    zassert_equal(page.cursor, 0);
    key(UiKey::Down);
    ui_view_update(model);
    zassert_true(lv_anim_count_running() > 0);
    lv_tick_inc(70);
    lv_timer_handler();
    ui_view_update(model);
    lv_tick_inc(75);
    lv_timer_handler();
    zassert_equal(lv_anim_count_running(), 0);
}

ZTEST(ui_diagnostic, test_complete_physical_hex_write_read_and_no_persistence) {
    open();
    const auto saved = settings_status();
    const auto original = radio_snapshot();
    key(UiKey::Enter);
    zassert_true(model.diagnostic_editing());
    render("hex-empty");
    label("P2 Add");
    label("BACK Cancel");
    key(UiKey::Enter);
    zassert_equal(model.error(), -EINVAL);
    zassert_true(model.diagnostic_editing());
    for (unsigned i = 0; i < 4; ++i) {
        key(UiKey::Up);
    }
    key(UiKey::Right);
    for (unsigned i = 0; i < 4; ++i) {
        key(UiKey::Down);
    }
    key(UiKey::Right);
    zassert_equal(strcmp(model.diagnostic_text(), "40"), 0);
    render("hex-address");
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Enter);
    for (unsigned i = 0; i < 11; ++i) {
        key(UiKey::Up);
    }
    key(UiKey::Right);
    for (unsigned i = 0; i < 3; ++i) {
        key(UiKey::Up);
    }
    key(UiKey::Right);
    key(UiKey::Right);
    key(UiKey::Up);
    key(UiKey::Right);
    zassert_equal(strcmp(model.diagnostic_text(), "BEEF"), 0);
    key(UiKey::Left);
    key(UiKey::Right);
    render("hex-value");
    const auto idle = flush_count;
    ui_view_update(model);
    lv_refr_now(display);
    zassert_equal(flush_count, idle);
    key(UiKey::Right);
    zassert_equal(model.error(), -ENOSPC);
    zassert_equal(strcmp(model.diagnostic_text(), "BEEF"), 0);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    render("register-pending");
    tick();
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().register_value, 0xbeef);
    render("register-read");
    zassert_equal(settings_status().revision, saved.revision);
    zassert_false(settings_status().pending);
    zassert_true(same_operating(radio_snapshot().config, original.config));
    zassert_true(same_selection(radio_snapshot().selection, original.selection));
}

ZTEST(ui_diagnostic, test_invalid_hex_address_bounds_and_cancel_keep_fields) {
    open();
    key(UiKey::Enter);
    type("80");
    key(UiKey::Enter);
    zassert_equal(model.error(), -ERANGE);
    zassert_true(model.diagnostic_editing());
    render("address-range");
    key(UiKey::Back);
    UiListPage page;
    model.diagnostic_page(page);
    zassert_equal(strcmp(page.rows[0].name, "Address 0x00"), 0);
    key(UiKey::Enter);
    type("G");
    zassert_equal(model.error(), -EINVAL);
    zassert_equal(strcmp(model.diagnostic_text(), ""), 0);
    type("7f");
    key(UiKey::Enter);
    model.diagnostic_page(page);
    zassert_equal(strcmp(page.rows[0].name, "Address 0x7F"), 0);
    value("abcd");
    key(UiKey::Enter);
    type("ffff");
    key(UiKey::Back);
    model.diagnostic_page(page);
    zassert_equal(strcmp(page.rows[1].name, "Value 0xABCD"), 0);
    render("hex-cancelled");
}

ZTEST(ui_diagnostic, test_held_ptt_cannot_queue_write_that_applies_after_release) {
    open();
    address("40");
    value("beef");
    key(UiKey::Down);
    key(UiKey::Down);
    radio_ptt(true);
    tick();
    zassert_false(emulator_transmitting());
    const char *actions[4];
    model.diagnostic_actions(actions);
    for (auto action : actions) {
        zassert_equal(strcmp(action, ""), 0);
    }
    key(UiKey::Enter);
    zassert_false(model.command_pending());
    render("diagnostic-ptt-held");
    radio_ptt(false);
    tick();
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().register_value, 0);
}

ZTEST(ui_diagnostic, test_focus_coalesced_ptt_fault_and_off_discard_hex) {
    open();
    key(UiKey::Enter);
    type("40");
    model.input({UiKey::Release});
    zassert_false(model.diagnostic_editing());
    key(UiKey::Enter);
    type("40");
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Enter);
    zassert_false(model.diagnostic_editing());
    UiListPage page;
    model.diagnostic_page(page);
    zassert_equal(strcmp(page.rows[0].name, "Address 0x00"), 0);
    key(UiKey::Enter);
    type("40");
    radio_report_fault(-EPIPE);
    tick();
    zassert_false(model.diagnostic_editing());
    render("diagnostic-fault");
    label("RADIO FAULT");
    load();
    open();
    key(UiKey::Enter);
    type("40");
    radio_power(false);
    tick();
    zassert_false(model.diagnostic_editing());
    render("diagnostic-inactive");
    label("RADIO INACTIVE");
}

ZTEST(ui_diagnostic, test_register_errors_restore_failures_and_memory_fm_m17_retained) {
    open();
    key(UiKey::Down);
    key(UiKey::Down);
    emulator_fail_next(-EINVAL);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.error(), -EINVAL);
    render("register-error");
    label("Register error -22");
    key(UiKey::Enter);
    tick();
    zassert_ok(model.error());
    key(UiKey::Up);
    key(UiKey::Enter);
    type("beef");
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Down);
    emulator_fail_next(-EIO);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.error(), -EIO);
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().register_value, 0);
    emulator_fail_next(-EIO);
    key(UiKey::Back);
    tick();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    render("restore-fault");
    label("RADIO FAULT");
    const Mode modes[] = {Mode::Fm, Mode::M17};
    for (auto mode : modes) {
        load(true, mode);
        const auto original = radio_snapshot();
        const auto saved = settings_status();
        open();
        address("40");
        value("beef");
        key(UiKey::Down);
        key(UiKey::Down);
        key(UiKey::Enter);
        tick();
        key(UiKey::Back);
        tick();
        zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
        zassert_true(same_selection(radio_snapshot().selection, original.selection));
        zassert_true(same_operating(radio_snapshot().config, original.config));
        zassert_equal(settings_status().revision, saved.revision);
        zassert_false(settings_status().pending);
        open();
        address("40");
        key(UiKey::Down);
        key(UiKey::Down);
        key(UiKey::Enter);
        tick();
        zassert_equal(radio_snapshot().register_value, 0);
        key(UiKey::Back);
        tick();
    }
}

ZTEST(ui_diagnostic, test_all_themes_contrasts_editor_geometry_and_bounded_heap) {
    for (unsigned theme = 0; theme < ThemeCount; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            load(false, Mode::Fm, static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            open();
            char name[48];
            snprintf(name, sizeof(name), "diagnostic-theme-%u-%u", theme, contrast);
            render(name);
            address("40");
            key(UiKey::Down);
            key(UiKey::Enter);
            type("beef");
            snprintf(name, sizeof(name), "hex-theme-%u-%u", theme, contrast);
            render(name);
            auto *content = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
            lv_obj_t *entry = nullptr;
            for (unsigned i = 0; i < lv_obj_get_child_cnt(content); ++i) {
                auto *object = lv_obj_get_child(content, i);
                if (lv_obj_check_type(object, &lv_textarea_class)) {
                    entry = object;
                }
            }
            zassert_not_null(entry);
            zassert_false(lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN));
            zassert_equal(lv_obj_get_style_text_font(entry, 0), &lv_font_montserrat_22);
            zassert_equal(lv_textarea_get_cursor_pos(entry), 4);
            key(UiKey::Back);
            key(UiKey::Back);
            tick();
            ui_view_update(model);
            label("MENU");
            label("OK Select");
        }
    }
    printk("Diagnostic heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST_SUITE(ui_diagnostic, nullptr, setup, before, nullptr, nullptr);
