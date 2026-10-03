// SPDX-License-Identifier: GPL-3.0-or-later
#include <SDL.h>
#include <errno.h>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <ht/ui.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zephyr/ztest.h>
using namespace ht;
static UiModel model;
static lv_color_t buffer[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static char root[] = "/tmp/ht-ui-appearance-XXXXXX", path[160];
static bool fail_durable;
static uint32_t minimum_free = 32768, minimum_largest = 32768;
static unsigned maximum_fragmentation;
extern "C" int __real_fsync(int);

extern "C" int __wrap_fsync(int fd) {
    struct stat info;
    if (fail_durable && !fstat(fd, &info) && S_ISDIR(info.st_mode)) {
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
    zassert_ok(setenv("HT_PROFILE", "appearance", 1));
    snprintf(path, sizeof(path), "%s/appearance.bin", root);
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
    return nullptr;
}

static void before(void *) {
    fail_durable = false;
    unlink(path);
    radio_power(true);
    zassert_ok(radio_start({}));
    RadioConfig config;
    zassert_equal(settings_start(config), -ENOENT);
    zassert_ok(radio_start(config));
    model = {};
    model.sync(radio_snapshot());
    ui_view_update(model);
    lv_tick_inc(200);
    lv_timer_handler();
}

static void key(UiKey key) {
    model.input({key});
}

static void tick(int64_t now = 0) {
    settings_service(radio_snapshot(), now);
    radio_service();
    settings_service(radio_snapshot(), now + 1);
    model.sync(radio_snapshot());
    const auto status = settings_status();
    model.storage_status(status.load_error, status.save_error, status.pending);
}

static void open() {
    if (model.screen() != UiScreen::Home) {
        key(UiKey::Back);
    }
    key(UiKey::Enter);
    bool selected = false;
    for (unsigned step = 0; step < 12 && !selected; ++step) {
        char lines[8][32];
        model.lines(lines);
        for (const auto &line : lines) {
            selected |= strstr(line, "> Appearance") != nullptr;
        }
        if (!selected) {
            key(UiKey::Down);
        }
    }
    zassert_true(selected);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Appearance);
}

static void render() {
    ui_view_update(model);
    lv_tick_inc(200);
    lv_timer_handler();
    lv_tick_inc(30);
    lv_timer_handler();
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    if (memory.free_size < minimum_free) {
        minimum_free = memory.free_size;
    }
    if (memory.free_biggest_size < minimum_largest) {
        minimum_largest = memory.free_biggest_size;
    }
    if (memory.frag_pct > maximum_fragmentation) {
        maximum_fragmentation = memory.frag_pct;
    }
}

static void dump(const char *name) {
    const char *directory = getenv("HT_UI_APPEARANCE_FRAMES");
    if (!directory) {
        return;
    }
    char output[512];
    snprintf(output, sizeof(output), "%s/%s.ppm", directory, name);
    FILE *file = fopen(output, "wb");
    zassert_not_null(file);
    fprintf(file, "P6\n160 128\n255\n");
    for (uint16_t pixel : frame) {
        const unsigned r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
        const unsigned char rgb[] = {static_cast<unsigned char>(r * 255 / 31),
                                     static_cast<unsigned char>(g * 255 / 63),
                                     static_cast<unsigned char>(b * 255 / 31)};
        zassert_equal(fwrite(rgb, 1, 3, file), 3);
    }
    zassert_ok(fclose(file));
}

ZTEST(ui_appearance, test_preview_cancel_wrap_and_physical_secondary_roles) {
    open();
    const auto revision = settings_status().revision;
    key(UiKey::Up);
    zassert_equal(model.preferences().theme, Theme::Darcula);
    key(UiKey::Down);
    zassert_equal(model.preferences().theme, Theme::Midnight);
    key(UiKey::Down);
    key(UiKey::Left);
    key(UiKey::Right);
    zassert_equal(model.preferences().theme, Theme::Nord);
    zassert_equal(model.preferences().contrast, Contrast::High);
    zassert_false(model.preferences().animations);
    UiPreferences applied;
    settings_ui_preferences(applied);
    zassert_equal(applied.theme, Theme::Midnight);
    zassert_equal(settings_status().revision, revision);
    zassert_false(settings_status().pending);
    zassert_false(settings_status().operation_pending);
    zassert_equal(access(path, F_OK), -1);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(model.preferences().theme, Theme::Midnight);
    zassert_equal(model.preferences().contrast, Contrast::Normal);
    zassert_true(model.preferences().animations);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Appearance);
    key(UiKey::Left);
    key(UiKey::Left);
    key(UiKey::Left);
    zassert_equal(model.preferences().contrast, Contrast::Normal);
}

ZTEST(ui_appearance, test_explicit_apply_ack_durability_restart_and_no_retune) {
    open();
    key(UiKey::Down);
    key(UiKey::Left);
    key(UiKey::Right);
    const auto before = radio_snapshot();
    key(UiKey::Enter);
    zassert_true(model.appearance_pending());
    zassert_equal(model.screen(), UiScreen::Appearance);
    UiPreferences applied;
    settings_ui_preferences(applied);
    zassert_equal(applied.theme, Theme::Midnight);
    key(UiKey::Down);
    zassert_equal(model.preferences().theme, Theme::Nord);
    zassert_ok(model.error());
    tick();
    zassert_false(model.appearance_pending());
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_ok(model.error());
    zassert_equal(model.preferences().theme, Theme::Nord);
    zassert_equal(model.preferences().contrast, Contrast::High);
    zassert_false(model.preferences().animations);
    zassert_true(same_operating(radio_snapshot().config, before.config));
    zassert_true(same_selection(radio_snapshot().selection, before.selection));
    zassert_equal(emulator_tuned_frequency(), before.config.rx_frequency_hz);
    zassert_equal(radio_snapshot().command_id, before.command_id);
    zassert_false(settings_status().pending);
    RadioConfig config;
    zassert_ok(settings_start(config));
    zassert_ok(radio_start(config));
    model = {};
    model.sync(radio_snapshot());
    zassert_equal(model.preferences().theme, Theme::Nord);
    zassert_equal(model.preferences().contrast, Contrast::High);
    zassert_false(model.preferences().animations);
}

ZTEST(ui_appearance, test_interruptions_cancel_preview_without_storage_or_blocking_ptt) {
    for (unsigned event = 0; event < 5; ++event) {
        model = {};
        model.sync(radio_snapshot());
        open();
        key(UiKey::Down);
        key(UiKey::Left);
        if (event == 0) {
            model.input({UiKey::Ptt});
            radio_ptt(true);
            radio_service();
            model.sync(radio_snapshot());
        }
        if (event == 1) {
            model.input({UiKey::Release});
        }
        if (event == 2) {
            model.input({UiKey::Quit});
        }
        if (event == 3) {
            RadioState fault = radio_snapshot();
            fault.phase = RadioPhase::Fault;
            fault.fault = -EPIPE;
            model.sync(fault);
        }
        if (event == 4) {
            radio_power(false);
            radio_service();
            model.sync(radio_snapshot());
        }
        zassert_not_equal(model.screen(), UiScreen::Appearance);
        zassert_equal(model.preferences().theme, Theme::Midnight);
        zassert_equal(model.preferences().contrast, Contrast::Normal);
        zassert_false(settings_status().pending);
        zassert_false(settings_status().operation_pending);
        if (event == 0) {
            zassert_true(emulator_transmitting());
            radio_ptt(false);
            radio_service();
        }
        if (event == 4) {
            radio_power(true);
            radio_service();
        }
    }
}

ZTEST(ui_appearance, test_direct_hardware_ptt_path_rejected_and_coalesced_presses_cancel) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        radio_ptt(false);
        radio_service();
        RadioConfig config;
        if (scenario == 0) {
            config.tx_inhibit = true;
        }
        if (scenario == 1) {
            config.mode = Mode::M17;
        }
        zassert_ok(radio_start(config));
        model = {};
        model.sync(radio_snapshot());
        open();
        key(UiKey::Down);
        key(UiKey::Left);
        const auto marker = radio_ptt_press_sequence();
        radio_ptt(true); // Physical controls send this directly, with no UiInput.
        zassert_not_equal(radio_ptt_press_sequence(), marker);
        if (scenario >= 2) {
            radio_ptt(false);
        } // Tap coalesced before owner tick.
        if (scenario == 3) {
            key(UiKey::Enter); // A queued Apply key before UI/radio sync must not submit.
        } else {
            radio_service();
            model.sync(radio_snapshot());
            zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
        }
        zassert_equal(model.screen(), UiScreen::Menu);
        zassert_equal(model.preferences().theme, Theme::Midnight);
        zassert_equal(model.preferences().contrast, Contrast::Normal);
        zassert_false(settings_status().operation_pending);
        zassert_false(settings_status().pending);
        if (scenario < 2) {
            // Reopening while rejected PTT is still held cannot start a new preview.
            key(UiKey::Enter);
            zassert_equal(model.screen(), UiScreen::Menu);
            zassert_equal(model.error(), -EBUSY);
        }
    }
    radio_ptt(false);
    radio_service();
}

ZTEST(ui_appearance, test_stale_or_busy_apply_keeps_draft_and_durability_error_keeps_applied) {
    open();
    key(UiKey::Down);
    RadioCommand config;
    config.config = radio_snapshot().config;
    strcpy(config.config.callsign, "OE3ANC");
    zassert_ok(radio_submit(config));
    tick(); // RAM revision changed after Appearance began.
    key(UiKey::Enter);
    tick();
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(model.screen(), UiScreen::Appearance);
    zassert_equal(model.preferences().theme, Theme::Nord);
    UiPreferences applied;
    settings_ui_preferences(applied);
    zassert_equal(applied.theme, Theme::Midnight);
    key(UiKey::Back);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Enter);
    fail_durable = true;
    tick();
    zassert_ok(model.error());
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(model.preferences().theme, Theme::Nord);
    zassert_equal(settings_status().save_error, -EIO);
    zassert_true(settings_status().pending);
    fail_durable = false;
    tick(1001);
    zassert_false(settings_status().pending);
    key(UiKey::Enter); // Reopen selected Appearance.
    zassert_ok(settings_recall({}, 999, radio_snapshot()));
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_equal(model.error(), -EBUSY);
    zassert_false(model.appearance_pending());
    zassert_equal(model.screen(), UiScreen::Appearance);
    tick(1002);
    key(UiKey::Back);
    zassert_equal(model.preferences().theme, Theme::Nord);
}

ZTEST(ui_appearance, test_focus_loss_event_cancels_preview_and_release_bypasses_queue_pressure) {
    open();
    key(UiKey::Down);
    key(UiKey::Left);
    RadioCommand command;
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(command));
    }
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = 1;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    zassert_equal(SDL_PushEvent(&event), 1);
    UiInput input;
    bool released = false;
    while (ui_backend_input(input)) {
        model.input(input);
        if (input.key == UiKey::Release) {
            radio_ptt(false);
            radio_monitor(false);
            released = true;
        }
    }
    zassert_true(released);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(model.preferences().theme, Theme::Midnight);
    zassert_equal(model.preferences().contrast, Contrast::Normal);
    radio_service();
    zassert_false(emulator_transmitting());
}

ZTEST(ui_appearance, test_real_lvgl_twelve_variants_bounded_heap_and_immediate_cancel) {
    open();
    for (unsigned theme = 0; theme < 4; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            zassert_equal(model.preferences().theme, static_cast<Theme>(theme));
            zassert_equal(model.preferences().contrast, static_cast<Contrast>(contrast));
            render();
            const auto colors =
                ui_palette(static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            zassert_equal(frame[0], lv_color_hex(colors.background).full);
            // Selected row has its accent marker, and actual OK-left/BACK-right footer text.
            zassert_equal(frame[(42 + 15 * theme) * 160 + 5], lv_color_hex(colors.accent).full);
            const auto screen = lv_disp_get_scr_act(display);
            const auto appearance = lv_obj_get_child(screen, -1);
            zassert_false(lv_obj_has_flag(appearance, LV_OBJ_FLAG_HIDDEN));
            unsigned primary = 0, secondary = 0;
            for (unsigned child = 0; child < lv_obj_get_child_cnt(appearance); ++child) {
                auto *object = lv_obj_get_child(appearance, child);
                if (!lv_obj_check_type(object, &lv_label_class)) {
                    continue;
                }
                const char *value = lv_label_get_text(object);
                if (!strcmp(value, "OK Apply")) {
                    zassert_equal(lv_obj_get_x(object), 8);
                    ++primary;
                }
                if (!strcmp(value, "BACK Cancel")) {
                    zassert_equal(lv_obj_get_x(object), 82);
                    ++primary;
                }
                if (!strcmp(value, "P1 Contrast") || !strcmp(value, "P2 Motion")) {
                    ++secondary;
                }
            }
            zassert_equal(primary, 2);
            zassert_equal(secondary, 2);
            char name[48];
            snprintf(name, sizeof(name), "appearance-%u-%u", theme, contrast);
            dump(name);
            key(UiKey::Left);
        }
        key(UiKey::Down);
    }
    for (unsigned i = 0; i < 100; ++i) {
        key(UiKey::Down);
        render();
    }
    // Cancel in mid-animation: immediate applied colors, hidden preview, no live animation.
    key(UiKey::Down);
    ui_view_update(model);
    zassert_true(lv_anim_count_running() > 0);
    model.input({UiKey::Ptt});
    ui_view_update(model);
    zassert_equal(lv_anim_count_running(), 0);
    zassert_equal(lv_obj_get_style_bg_color(lv_disp_get_scr_act(display), LV_PART_MAIN).full,
                  lv_color_hex(ui_palette(Theme::Midnight, Contrast::Normal).background).full);
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    zassert_equal(memory.total_size, 32768);
    zassert_true(memory.free_size > 8192);
    zassert_equal(lv_mem_test(), LV_RES_OK);
    printf("Appearance LVGL: min-free=%u min-largest=%u max-fragmentation=%u%%\n",
           unsigned(minimum_free), unsigned(minimum_largest), maximum_fragmentation);
}

ZTEST(ui_appearance, test_motion_off_moves_immediately_and_fault_cancels_running_transition) {
    open();
    render();
    key(UiKey::Down);
    ui_view_update(model);
    zassert_true(lv_anim_count_running() > 0);
    key(UiKey::Right);
    ui_view_update(model);
    zassert_equal(lv_anim_count_running(), 0);
    const auto appearance = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
    auto *selected = lv_obj_get_child(appearance, 0);
    lv_obj_update_layout(appearance);
    zassert_equal(lv_obj_get_y(selected), 54);
    key(UiKey::Down);
    ui_view_update(model);
    lv_obj_update_layout(appearance);
    zassert_equal(lv_obj_get_y(selected), 69);
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Right);
    key(UiKey::Down);
    ui_view_update(model);
    zassert_true(lv_anim_count_running() > 0);
    radio_report_fault(-EPIPE);
    radio_service();
    model.sync(radio_snapshot());
    ui_view_update(model);
    zassert_equal(lv_anim_count_running(), 0);
    zassert_false(lv_obj_has_flag(appearance, LV_OBJ_FLAG_HIDDEN));
    zassert_true(lv_obj_has_flag(selected, LV_OBJ_FLAG_HIDDEN));
    zassert_equal(model.preferences().theme, Theme::Midnight);
    const auto screen = lv_disp_get_scr_act(display);
    zassert_equal(lv_obj_get_child_cnt(screen), 1); // Only the shared modern root remains.
    lv_obj_t *fault_title = nullptr;
    for (unsigned i = 0; i < lv_obj_get_child_cnt(appearance); ++i) {
        auto *object = lv_obj_get_child(appearance, i);
        if (lv_obj_check_type(object, &lv_label_class) &&
            !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) &&
            !strcmp(lv_label_get_text(object), "RADIO FAULT")) {
            fault_title = object;
        }
    }
    zassert_not_null(fault_title);
    zassert_equal(lv_obj_get_style_text_color(fault_title, LV_PART_MAIN).full,
                  lv_color_hex(ui_palette(Theme::Midnight, Contrast::Normal).red).full);
}

ZTEST(ui_appearance, test_contrast_foregrounds_brighten_surfaces_and_fallback_stay_dark) {
    for (unsigned theme = 0; theme < 4; ++theme) {
        const auto normal = ui_palette(static_cast<Theme>(theme), Contrast::Normal);
        const auto high = ui_palette(static_cast<Theme>(theme), Contrast::High);
        const auto maximum = ui_palette(static_cast<Theme>(theme), Contrast::Maximum);
        zassert_equal(normal.background, high.background);
        zassert_equal(normal.background, maximum.background);
        zassert_equal(normal.panel, maximum.panel);
        zassert_equal(normal.selection, maximum.selection);
        const uint32_t originals[] = {normal.white, normal.muted, normal.accent,
                                      normal.line,  normal.amber, normal.red};
        const uint32_t highs[] = {high.white, high.muted, high.accent,
                                  high.line,  high.amber, high.red};
        const uint32_t maxima[] = {maximum.white, maximum.muted, maximum.accent,
                                   maximum.line,  maximum.amber, maximum.red};
        for (unsigned role = 0; role < 6; ++role) {
            for (unsigned shift = 0; shift <= 16; shift += 8) {
                zassert_true(((highs[role] >> shift) & 255) >= ((originals[role] >> shift) & 255));
                zassert_true(((maxima[role] >> shift) & 255) >= ((highs[role] >> shift) & 255));
            }
        }
        zassert_equal(maximum.white, 0xffffff);
        zassert_equal(maximum.muted, 0xffffff);
    }
    const auto safe = ui_palette(static_cast<Theme>(255), static_cast<Contrast>(255));
    zassert_equal(strcmp(safe.name, "Midnight"), 0);
    zassert_equal(safe.background, 0x0b111a);
    zassert_equal(strcmp(ui_contrast_name(static_cast<Contrast>(255)), "Normal"), 0);
}

ZTEST_SUITE(ui_appearance, nullptr, setup, before, nullptr, nullptr);
