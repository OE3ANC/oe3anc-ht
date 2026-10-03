// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <ht/ui.hpp>
#include "../../../backends/linux/sdl.hpp"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zephyr/ztest.h>
using namespace ht;
static UiModel model;
static Codeplug plug;
static char root[] = "/tmp/ht-ui-lock-XXXXXX";
static char settings_path[160];
static lv_color_t pixels[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static int64_t now;
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;

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
    zassert_ok(setenv("HT_PROFILE", "lock", 1));
    zassert_ok(setenv("SDL_VIDEODRIVER", "dummy", 1));
    snprintf(settings_path, sizeof(settings_path), "%s/lock.bin", root);
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

static void before(void *) {
    ui_keypad_star(false, k_uptime_get());
    ui_keypad_cancel_gesture();
    radio_power(true);
    plug.global = {};
    plug.vfo = {};
    plug.selection = {};
    unlink(settings_path);
    plug.global.ui.idle_s = 0;
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 430000000;
    uint32_t generation = 0;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
    now = k_uptime_get();
}

static void tick() {
    radio_service();
    model.sync(radio_snapshot());
    model.advance(now);
}

static void key(UiKey key, bool pressed = true) {
    model.input({key, 0, pressed, now});
}

static void star(bool pressed) {
    ui_keypad_star(pressed, now);
    key(UiKey::Star, pressed);
    model.advance(now);
}

static void hold() {
    star(true);
    now += 1000;
    model.advance(now);
}

static void realtime_hold() {
    now = k_uptime_get();
    star(true);
    k_sleep(K_SECONDS(1));
    now = k_uptime_get();
    model.advance(now);
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
    const auto *directory = getenv("HT_UI_LOCK_FRAMES");
    if (!directory) {
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb");
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

ZTEST(ui_lock, test_exact_hold_short_press_repeat_and_fresh_unlock) {
    render("unlocked");
    star(true);
    now += 999;
    model.advance(now);
    zassert_false(model.keypad_locked());
    star(false);
    now += 2000;
    model.advance(now);
    zassert_false(model.keypad_locked());
    hold();
    zassert_true(model.keypad_locked());
    render("locked");
    now += 5000;
    ui_keypad_star(true, now);
    model.advance(now);
    zassert_true(model.keypad_locked());
    star(false);
    hold();
    zassert_false(model.keypad_locked());
    render("unlocked-again");
}

ZTEST(ui_lock, test_all_front_controls_block_without_rf_or_settings_edits) {
    hold();
    zassert_true(model.keypad_locked());
    star(false);
    const auto revision = settings_status().revision;
    const auto rf_revision = radio_snapshot().configuration_revision;
    const UiKey buttons[] = {UiKey::Up,        UiKey::Down, UiKey::Left, UiKey::Right,
                             UiKey::Enter,     UiKey::Back, UiKey::Hash, UiKey::Erase,
                             UiKey::Character, UiKey::Digit};
    for (const auto button : buttons) {
        model.input({button, '4'});
        tick();
        zassert_equal(model.screen(), UiScreen::Home);
        zassert_true(model.keypad_locked());
        zassert_equal(emulator_tuned_frequency(), 430000000);
    }
    zassert_equal(settings_status().revision, revision);
    zassert_equal(radio_snapshot().configuration_revision, rf_revision);
    hold();
    star(false);
    key(UiKey::Up);
    tick();
    zassert_equal(emulator_tuned_frequency(), 430012500);
}

ZTEST(ui_lock, test_ptt_monitor_and_faults_remain_live) {
    hold();
    star(false);
    key(UiKey::Monitor);
    radio_monitor(true);
    tick();
    zassert_true(radio_snapshot().monitor_active);
    zassert_true(model.keypad_locked());
    render("locked-monitor");
    key(UiKey::Monitor, false);
    radio_monitor(false);
    tick();
    zassert_false(radio_snapshot().monitor_active);
    key(UiKey::Ptt);
    radio_ptt(true);
    tick();
    zassert_true(emulator_transmitting());
    render("locked-tx");
    key(UiKey::Ptt, false);
    radio_ptt(false);
    tick();
    zassert_false(emulator_transmitting());
    radio_report_fault(-EIO);
    tick();
    zassert_true(model.keypad_locked());
    render("locked-fault");
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[0], "RADIO FAULT"), 0);
    hold();
    zassert_true(model.keypad_locked());
}

ZTEST(ui_lock, test_focus_ptt_navigation_and_fault_cancel_until_new_press) {
    const UiKey cancellations[] = {UiKey::Release, UiKey::Ptt, UiKey::Enter, UiKey::Up};
    for (const auto cancel : cancellations) {
        star(true);
        now += 500;
        model.advance(now);
        key(cancel);
        now += 1000;
        model.advance(now);
        zassert_false(model.keypad_locked());
        if (model.screen() == UiScreen::Menu) {
            key(UiKey::Back);
        }
        star(false);
    }
    star(true);
    radio_ptt(true);
    radio_ptt(false);
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    star(false);
    tick();
    hold();
    zassert_true(model.keypad_locked());
    star(false);
    star(true);
    radio_report_fault(-EIO);
    now += 1001;
    model.advance(now);
    zassert_true(model.keypad_locked());
}

ZTEST(ui_lock, test_loss_independent_level_cancellation_and_startup_watermark) {
    // No queued Star event is required, and a missed release cannot become a hold.
    ui_keypad_star(true, now);
    model.advance(now);
    ui_keypad_star(false, now + 20);
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    ui_keypad_star(true, now);
    model.advance(now);
    ui_keypad_cancel_gesture();
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    ui_keypad_star(false, now);
    ui_keypad_star(true, now);
    model.advance(now);
    now += 1000;
    model.advance(now);
    zassert_true(model.keypad_locked());
    // A lifecycle restart starts unlocked and refuses the already-held Star.
    zassert_ok(radio_start({}));
    model.sync(radio_snapshot());
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    star(false);
    hold();
    zassert_true(model.keypad_locked());
    radio_power(false);
    tick();
    zassert_false(model.keypad_locked());
    radio_power(true);
    tick();
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    star(false);
    hold();
    zassert_true(model.keypad_locked());
}

ZTEST(ui_lock, test_editor_hold_and_clock_reversal_do_not_toggle) {
    key(UiKey::Hash);
    zassert_equal(model.screen(), UiScreen::Frequency);
    hold();
    zassert_false(model.keypad_locked());
    key(UiKey::Back);
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    star(false);
    star(true);
    model.advance(now - 1);
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    star(false);
    hold();
    zassert_true(model.keypad_locked());
}

ZTEST(ui_lock, test_dimmed_star_wakes_only_then_fresh_hold_locks) {
    UiPreferences prefs;
    uint32_t revision;
    settings_ui_preferences(prefs, &revision);
    prefs.idle_s = 15;
    zassert_ok(settings_put_ui_preferences(prefs, 100, radio_snapshot(), revision));
    settings_service(radio_snapshot(), 0);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
    k_sleep(K_SECONDS(15));
    now = k_uptime_get();
    model.advance(now);
    zassert_true(model.dimmed());
    realtime_hold();
    zassert_false(model.dimmed());
    zassert_false(model.keypad_locked());
    star(false);
    realtime_hold();
    zassert_true(model.keypad_locked());
    star(false);
    k_sleep(K_SECONDS(15));
    now = k_uptime_get();
    model.advance(now);
    zassert_true(model.dimmed());
    key(UiKey::Up);
    zassert_false(model.dimmed());
    zassert_true(model.keypad_locked());
    zassert_equal(emulator_tuned_frequency(), 430000000);
    // The same wake policy applies when a C62 Star press misses the queue.
    k_sleep(K_SECONDS(15));
    now = k_uptime_get();
    model.advance(now);
    zassert_true(model.dimmed());
    ui_keypad_star(true, now);
    model.advance(now);
    zassert_false(model.dimmed());
    now += 1000;
    model.advance(now);
    zassert_true(model.keypad_locked());
}

static void push(SDL_Keycode key, SDL_Scancode code, bool pressed, unsigned mods, unsigned window) {
    SDL_Event event{};
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = window;
    event.key.keysym.sym = key;
    event.key.keysym.scancode = code;
    event.key.keysym.mod = mods;
    zassert_equal(SDL_PushEvent(&event), 1);
}

ZTEST(ui_lock, test_sdl_shift_release_identity_repeat_and_window_focus) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
    }
    push(SDLK_8, SDL_SCANCODE_8, true, KMOD_SHIFT, 1);
    UiInput input;
    zassert_true(ui_backend_input(input));
    zassert_equal(input.key, UiKey::Star);
    model.input(input);
    now = k_uptime_get();
    model.advance(now);
    const auto press = ui_keypad_gesture().press_sequence;
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = 1;
    event.key.repeat = 1;
    event.key.keysym.sym = SDLK_8;
    event.key.keysym.scancode = SDL_SCANCODE_8;
    event.key.keysym.mod = KMOD_SHIFT;
    zassert_equal(SDL_PushEvent(&event), 1);
    zassert_false(ui_backend_input(input));
    zassert_false(ui_backend_input(input));
    zassert_equal(ui_keypad_gesture().press_sequence, press);
    push(SDLK_8, SDL_SCANCODE_8, false, 0, 2); // Modifiers/focus changed before release.
    zassert_true(ui_backend_input(input) || ui_backend_input(input));
    zassert_equal(input.key, UiKey::Star);
    zassert_false(input.pressed);
    model.input(input);
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    push(SDLK_KP_MULTIPLY, SDL_SCANCODE_KP_MULTIPLY, true, 0, 1);
    zassert_true(ui_backend_input(input) || ui_backend_input(input));
    model.input(input);
    now = k_uptime_get();
    model.advance(now);
    event = {};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = 1;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    zassert_equal(SDL_PushEvent(&event), 1);
    zassert_true(ui_backend_input(input) || ui_backend_input(input));
    zassert_equal(input.key, UiKey::Release);
    model.input(input);
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
    push(SDLK_KP_MULTIPLY, SDL_SCANCODE_KP_MULTIPLY, true, 0,
         2); // Developer window cannot lock radio.
    zassert_false(ui_backend_input(input));
    now += 1001;
    model.advance(now);
    zassert_false(model.keypad_locked());
}

static void finish(void *) {
    printk("Lock UI heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST_SUITE(ui_lock, nullptr, setup, before, nullptr, finish);
