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
#include <sys/stat.h>
#include <zephyr/ztest.h>
using namespace ht;
static UiModel model;
static Codeplug plug;
static char root[] = "/tmp/ht-ui-motion-XXXXXX", path[160];
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

static void update();

static void render(const char *name) {
    update();
    lv_refr_now(display);
    lv_mem_monitor_t heap;
    lv_mem_monitor(&heap);
    zassert_equal(heap.total_size, 32768);
    zassert_true(heap.free_size > 4096);
    zassert_equal(lv_mem_test(), LV_RES_OK);
    minimum_free = MIN(minimum_free, heap.free_size);
    minimum_largest = MIN(minimum_largest, heap.free_biggest_size);
    maximum_fragmentation = MAX(maximum_fragmentation, heap.frag_pct);
    const auto *directory = getenv("HT_UI_MOTION_FRAMES");
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
    zassert_ok(setenv("HT_PROFILE", "motion", 1));
    snprintf(path, sizeof(path), "%s/motion.bin", root);
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

static lv_obj_t *page() {
    return lv_obj_get_child(lv_disp_get_scr_act(display), -1);
}

static void update() {
    ui_view_update(model);
    lv_obj_update_layout(page());
}

static void advance(unsigned ms) {
    lv_tick_inc(ms);
    lv_timer_handler();
    lv_obj_update_layout(page());
}

static void load(bool motion = true) {
    unlink(path);
    radio_power(true);
    radio_ptt(false);
    radio_monitor(false);
    emulator_inject({});
    plug.global = {};
    plug.vfo = {};
    plug.selection = {};
    plug.channel_count = plug.bank_count = 0;
    plug.channel_id_high_water = plug.bank_id_high_water = 0;
    plug.global.ui.idle_s = 0;
    plug.global.ui.animations = motion;
    strcpy(plug.global.local_callsign, "OE3ANC");
    Channel channel;
    channel.number = 7;
    strcpy(channel.name, "MOTION BASELINE");
    uint32_t id;
    zassert_ok(put_channel(plug, channel, id));
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
    update();
    advance(200);
}

static void before(void *) {
    load();
}

static void tick() {
    settings_service(radio_snapshot(), 0);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
    update();
}

static void key(UiKey value) {
    model.input({value});
    update();
}

static void menu(const char *name) {
    if (model.screen() == UiScreen::Home) {
        key(UiKey::Enter);
    }
    zassert_equal(model.screen(), UiScreen::Menu);
    for (unsigned i = 0; i < 32; ++i) {
        UiListPage list;
        model.menu_page(list);
        if (!strcmp(list.rows[list.cursor % 4].name, name)) {
            key(UiKey::Enter);
            return;
        }
        key(UiKey::Down);
    }
    zassert_unreachable("Missing menu item: %s", name);
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

ZTEST(ui_motion, test_page_movement_completes_without_refresh_restart_or_idle_flush) {
    zassert_equal(lv_anim_count_running(), 0);
    zassert_equal(lv_obj_get_y(page()), 0);
    key(UiKey::Enter);
    label("MENU");
    zassert_equal(lv_obj_get_y(page()), 3);
    zassert_equal(lv_anim_count_running(), 1);
    render("menu-start");
    advance(40);
    update();
    zassert_true(lv_obj_get_y(page()) < 3);
    render("menu-40ms");
    advance(40);
    update();
    render("menu-80ms");
    advance(65);
    update();
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    render("menu-settled");
    const auto idle = flush_count;
    update();
    lv_refr_now(display);
    zassert_equal(flush_count, idle);
    key(UiKey::Back);
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
}

ZTEST(ui_motion, test_rapid_navigation_replaces_page_animation_and_text_is_immediate) {
    key(UiKey::Enter);
    menu("Appearance");
    label("APPEARANCE");
    zassert_equal(lv_obj_get_y(page()), 3);
    zassert_equal(lv_anim_count_running(), 1);
    render("appearance-start");
    advance(30);
    key(UiKey::Back);
    label("MENU");
    zassert_equal(lv_obj_get_y(page()), 3);
    zassert_equal(lv_anim_count_running(), 1);
    advance(145);
    update();
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Back);
    key(UiKey::Hash);
    label("VFO FREQUENCY");
    model.input({UiKey::Character, '4'});
    update();
    const auto *entry = [&]() -> lv_obj_t * {
        for (unsigned i = 0; i < lv_obj_get_child_cnt(page()); ++i) {
            auto *object = lv_obj_get_child(page(), i);
            if (lv_obj_check_type(object, &lv_textarea_class)) {
                return object;
            }
        }
        return nullptr;
    }();
    zassert_not_null(entry);
    zassert_equal(strcmp(lv_textarea_get_text(entry), "4"), 0);
    render("frequency-start");
    zassert_equal(lv_anim_count_running(), 1);
    advance(145);
    update();
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Back);
    menu("BK4819 diagnostics");
    tick();
    key(UiKey::Enter);
    zassert_true(model.diagnostic_editing());
    label("REGISTER ADDRESS");
}

ZTEST(ui_motion, test_motion_off_and_live_toggle_are_immediate) {
    load(false);
    key(UiKey::Enter);
    key(UiKey::Down);
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    menu("Appearance");
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Right);
    key(UiKey::Down);
    zassert_true(lv_anim_count_running() > 0);
    key(UiKey::Right);
    zassert_equal(lv_anim_count_running(), 0);
    zassert_equal(lv_obj_get_y(page()), 0);
    render("motion-off");
    key(UiKey::Back);
    zassert_equal(lv_anim_count_running(), 0);
}

ZTEST(ui_motion, test_first_run_defaults_allow_motion_but_real_storage_errors_snap) {
    model.storage_status(-ENOENT, 0, false);
    zassert_true(model.motion_allowed());
    key(UiKey::Enter);
    zassert_equal(lv_obj_get_style_y(page(), LV_PART_MAIN), 3);
    model.storage_status(-ENOENT, -ENOENT, true);
    update();
    zassert_false(model.motion_allowed());
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    model.storage_status(-EIO, 0, false);
    zassert_false(model.motion_allowed());
    model.storage_status(0, 0, false);
    key(UiKey::Back);
    key(UiKey::Enter);
    zassert_equal(lv_obj_get_style_y(page(), LV_PART_MAIN), 3);
}

ZTEST(ui_motion, test_actual_first_run_save_enoent_keeps_error_origin) {
    zassert_ok(unlink(path));
    RadioConfig config;
    Selection selected;
    zassert_equal(settings_start(config, selected), -ENOENT);
    zassert_ok(radio_start(config, selected));
    model = {};
    model.sync(radio_snapshot());
    const auto initial = settings_status();
    model.storage_status(initial.load_error, initial.save_error, initial.pending);
    zassert_true(model.motion_allowed());
    char lock_path[160];
    snprintf(lock_path, sizeof(lock_path), "%s/motion.bin.lock", root);
    zassert_ok(unlink(lock_path));
    zassert_ok(rmdir(root));
    RadioCommand command;
    command.kind = CommandKind::Configure;
    command.config = radio_snapshot().config;
    command.config.rx_frequency_hz += 12500;
    command.config.tx_frequency_hz += 12500;
    const int submit_error = radio_submit(command);
    radio_service();
    settings_service(radio_snapshot(), 0);
    settings_service(radio_snapshot(), 1001);
    const auto failed = settings_status();
    const int restore_error = mkdir(root, 0700);
    zassert_ok(restore_error);
    zassert_ok(submit_error);
    zassert_equal(failed.load_error, -ENOENT);
    zassert_equal(failed.save_error, -ENOENT);
    zassert_true(failed.pending);
    zassert_equal(failed.generation, 0);
    model.sync(radio_snapshot());
    model.storage_status(failed.load_error, failed.save_error, failed.pending);
    UiHome home;
    model.home(home);
    printf("ACTUAL_SAVE load=%d save=%d pending=%d motion=%d context=%s\n", failed.load_error,
           failed.save_error, failed.pending, model.motion_allowed(), home.context);
    zassert_false(model.motion_allowed());
    zassert_equal(home.context_color, UiStatusColor::Red);
    zassert_not_equal(strcmp(home.context, "First-run defaults"), 0);
}

ZTEST(ui_motion, test_focus_and_coalesced_side_activity_snap_without_owner_tick) {
    key(UiKey::Enter);
    zassert_equal(lv_obj_get_y(page()), 3);
    model.input({UiKey::Release});
    update();
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Back);
    key(UiKey::Enter);
    radio_ptt(true);
    radio_ptt(false);
    update();
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    zassert_false(emulator_transmitting());
    key(UiKey::Back);
    key(UiKey::Enter);
    radio_monitor(true);
    radio_monitor(false);
    update();
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Back);
    menu("Appearance");
    model.input({UiKey::Ptt});
    update();
    label("MENU");
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
}

ZTEST(ui_motion, test_pending_layout_cannot_move_fault_after_cancellation) {
    model.input({UiKey::Enter});
    ui_view_update(model); // Page style changes before layout resolves.
    zassert_equal(lv_obj_get_style_y(page(), LV_PART_MAIN), 3);
    radio_report_fault(-EPIPE);
    radio_service();
    model.sync(radio_snapshot());
    ui_view_update(model);
    zassert_equal(lv_obj_get_style_y(page(), LV_PART_MAIN), 0);
    zassert_equal(lv_anim_count_running(), 0);
    advance(200);
    render("pending-layout-fault");
    zassert_equal(lv_obj_get_y(page()), 0);
    const auto idle = flush_count;
    update();
    lv_refr_now(display);
    zassert_equal(flush_count, idle);
}

ZTEST(ui_motion, test_rx_monitor_tx_warning_timeout_and_queue_pressure_release) {
    key(UiKey::Enter);
    emulator_inject({true, -80});
    tick();
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Down);
    zassert_equal(lv_anim_count_running(), 0);
    render("live-rx");
    emulator_inject({});
    tick();
    key(UiKey::Back);
    key(UiKey::Enter);
    radio_monitor(true);
    tick();
    zassert_true(radio_snapshot().monitor_active);
    zassert_equal(lv_anim_count_running(), 0);
    radio_monitor(false);
    tick();
    key(UiKey::Back);
    key(UiKey::Enter);
    radio_ptt(true);
    tick();
    zassert_true(emulator_transmitting());
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    render("tx-fixed");
    RadioCommand command;
    command.kind = CommandKind::Configure;
    command.config = radio_snapshot().config;
    unsigned queued = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (radio_submit(command)) {
            break;
        }
        ++queued;
    }
    zassert_true(queued > 0 && queued < 32);
    radio_ptt(false);
    tick();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    auto state = radio_snapshot();
    state.phase = RadioPhase::Transmitting;
    state.tx_warning = true;
    state.tx_remaining_s = 7;
    model.sync(state);
    update();
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    label("TX ends in 7 s");
    render("tx-warning");
    state.phase = RadioPhase::Receiving;
    state.tx_warning = false;
    state.tx_timed_out = true;
    model.sync(state);
    update();
    label("Timeout / release PTT");
    zassert_equal(lv_anim_count_running(), 0);
    render("tx-timeout");
}

ZTEST(ui_motion, test_fault_off_error_and_hex_layout_cancel_or_replace_movement) {
    key(UiKey::Enter);
    radio_report_fault(-EPIPE);
    tick();
    label("RADIO FAULT");
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    render("fault-fixed");
    load();
    key(UiKey::Enter);
    radio_power(false);
    tick();
    label("RADIO INACTIVE");
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    render("inactive-fixed");
    load();
    menu("BK4819 diagnostics");
    tick();
    zassert_equal(model.screen(), UiScreen::Diagnostics);
    key(UiKey::Enter);
    zassert_true(model.diagnostic_editing());
    zassert_equal(lv_obj_get_y(page()), 3);
    render("hex-start");
    model.input({UiKey::Character, '8'});
    model.input({UiKey::Character, '0'});
    key(UiKey::Enter);
    zassert_equal(model.error(), -ERANGE);
    zassert_equal(lv_obj_get_y(page()), 0);
    zassert_equal(lv_anim_count_running(), 0);
    label("Address must be 00-7F");
    render("error-fixed");
}

ZTEST(ui_motion, test_repeated_navigation_and_selection_bound_allocations) {
    const auto objects = lv_obj_get_child_cnt(page());
    unsigned settled_free = 0;
    for (unsigned i = 0; i < 100; ++i) {
        key(UiKey::Enter);
        advance(40);
        key(UiKey::Down);
        zassert_true(lv_anim_count_running() <= 2);
        lv_mem_monitor_t heap;
        lv_mem_monitor(&heap);
        zassert_true(heap.free_size > 8192);
        minimum_free = MIN(minimum_free, heap.free_size);
        minimum_largest = MIN(minimum_largest, heap.free_biggest_size);
        maximum_fragmentation = MAX(maximum_fragmentation, heap.frag_pct);
        advance(40);
        key(UiKey::Back);
        advance(145);
        zassert_equal(model.screen(), UiScreen::Home);
        zassert_equal(lv_obj_get_y(page()), 0);
        zassert_equal(lv_anim_count_running(), 0);
        zassert_equal(lv_obj_get_child_cnt(page()), objects);
        lv_mem_monitor(&heap);
        zassert_equal(lv_mem_test(), LV_RES_OK);
        if (i) {
            zassert_equal(heap.free_size, settled_free);
        } else {
            settled_free = heap.free_size;
        }
        zassert_true(heap.free_size > 8192);
        minimum_free = MIN(minimum_free, heap.free_size);
        minimum_largest = MIN(minimum_largest, heap.free_biggest_size);
        maximum_fragmentation = MAX(maximum_fragmentation, heap.frag_pct);
    }
    printk("Motion heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST_SUITE(ui_motion, nullptr, setup, before, nullptr, nullptr);
