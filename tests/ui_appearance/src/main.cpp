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
    radio_ptt(false);
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

static void field(unsigned index, unsigned steps = 1) {
    while (model.form_cursor() != index) {
        key(model.form_cursor() > index ? UiKey::Up : UiKey::Down);
    }
    key(UiKey::Enter);
    while (steps--) {
        key(UiKey::Down);
    }
    key(UiKey::Enter);
}

static void apply() {
    key(UiKey::Right); // Apply page.
    key(UiKey::Enter);
    tick();
}

ZTEST(ui_appearance, test_explicit_field_accept_cancel_and_form_discard) {
    open();
    key(UiKey::Enter);
    key(UiKey::Down);
    zassert_equal(model.preferences().theme, Theme::Nord);
    key(UiKey::Left);
    key(UiKey::Right);
    zassert_equal(model.preferences().theme, Theme::Nord);
    key(UiKey::Back);
    zassert_equal(model.preferences().theme, Theme::Midnight);
    field(0);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Discard);
    key(UiKey::Enter); // Continue.
    zassert_equal(model.screen(), UiScreen::Appearance);
    key(UiKey::Back);
    key(UiKey::Up);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(model.preferences().theme, Theme::Midnight);
}

ZTEST(ui_appearance, test_apply_persists_all_preferences_without_rf_retune) {
    const auto tuned = emulator_tuned_frequency();
    open();
    field(0);
    field(1);
    field(2);
    field(3);
    apply();
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(emulator_tuned_frequency(), tuned);
    RadioConfig config;
    zassert_ok(settings_start(config));
    UiPreferences saved;
    settings_ui_preferences(saved);
    zassert_equal(saved.theme, Theme::Nord);
    zassert_equal(saved.contrast, Contrast::High);
    zassert_false(saved.animations);
    zassert_false(saved.startup_animation);
}

ZTEST(ui_appearance, test_durable_failure_retains_editor_and_retry) {
    open();
    field(0);
    fail_durable = true;
    apply();
    zassert_equal(model.screen(), UiScreen::Appearance);
    zassert_equal(model.error(), -EIO);
    zassert_equal(model.preferences().theme, Theme::Nord);
    fail_durable = false;
    key(UiKey::Enter); // Retry same Apply.
    tick(1001);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_false(settings_status().pending);
}

ZTEST(ui_appearance, test_ptt_preserves_preview_and_fault_cancels) {
    open();
    field(0);
    radio_ptt(true);
    tick();
    zassert_equal(model.screen(), UiScreen::Appearance);
    UiHome home;
    model.home(home);
    zassert_true(home.transmitting);
    radio_ptt(false);
    tick();
    zassert_equal(model.preferences().theme, Theme::Nord);
    RadioState fault = radio_snapshot();
    fault.fault = -EIO;
    fault.phase = RadioPhase::Fault;
    model.sync(fault);
    zassert_equal(model.preferences().theme, Theme::Midnight);
}

ZTEST(ui_appearance, test_all_palettes_contrasts_use_bounded_widgets) {
    open();
    for (unsigned theme = 0; theme < ThemeCount; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            render();
            char name[48];
            snprintf(name, sizeof(name), "theme-%u-contrast-%u", theme, contrast);
            dump(name);
            zassert_equal(unsigned(model.preferences().theme), theme);
            zassert_equal(unsigned(model.preferences().contrast), contrast);
            field(1);
        }
        field(0);
    }
    zassert_true(minimum_free > 4096);
    zassert_equal(lv_mem_test(), LV_RES_OK);
}

ZTEST_SUITE(ui_appearance, nullptr, setup, before, nullptr, nullptr);
