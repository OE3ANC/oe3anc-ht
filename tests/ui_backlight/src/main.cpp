// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include "../../../backends/linux/sdl.hpp"
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
static char root[] = "/tmp/ht-ui-backlight-XXXXXX", path[160];
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
static bool fail_durable;
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
    for (unsigned i = 0; i < 32 && !found; ++i) {
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
    const char *directory = getenv("HT_UI_BACKLIGHT_FRAMES");
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
    menu("Backlight");
    zassert_equal(model.screen(), UiScreen::Backlight);
}

static void advance(unsigned ms) {
    k_sleep(K_MSEC(ms));
    model.advance(k_uptime_get());
}

static void reboot() {
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
}

ZTEST(ui_backlight, test_draft_cancel_apply_field_choices_preserve_other_preferences_reboot) {
    open();
    frame_out("backlight-default");
    key(UiKey::Up); //100->25
    key(UiKey::Left);
    key(UiKey::Down); //15->Never
    key(UiKey::Left);
    key(UiKey::Up); //10->20
    frame_out("backlight-preview");
    zassert_equal(model.backlight_percent(), 100);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Menu);
    UiPreferences ui;
    settings_ui_preferences(ui);
    zassert_equal(ui.brightness_percent, 100);
    key(UiKey::Enter);
    key(UiKey::Up);
    key(UiKey::Left);
    key(UiKey::Down);
    key(UiKey::Left);
    key(UiKey::Up);
    key(UiKey::Enter);
    zassert_true(model.backlight_pending());
    frame_out("backlight-pending");
    tick();
    zassert_equal(model.screen(), UiScreen::Menu);
    settings_ui_preferences(ui);
    zassert_equal(ui.brightness_percent, 25);
    zassert_equal(ui.idle_s, 0);
    zassert_equal(ui.dim_percent, 20);
    zassert_equal(ui.theme, Theme::Midnight);
    zassert_equal(ui.contrast, Contrast::Normal);
    zassert_true(ui.animations);
    zassert_equal(model.backlight_percent(), 25);
    reboot();
    zassert_equal(model.backlight_percent(), 25);
    advance(61000);
    zassert_false(model.dimmed());
}

ZTEST(ui_backlight, test_idle_dim_boundary_first_key_consumed_then_normal_navigation) {
    const auto start = k_uptime_get();
    model.advance(start + 14999);
    zassert_false(model.dimmed());
    model.advance(start + 15000);
    zassert_true(model.dimmed());
    zassert_equal(model.backlight_percent(), 10);
    const auto frequency = emulator_tuned_frequency();
    key(UiKey::Up);
    zassert_false(model.dimmed());
    zassert_false(model.command_pending());
    zassert_equal(emulator_tuned_frequency(), frequency);
    key(UiKey::Up);
    tick();
    zassert_equal(emulator_tuned_frequency(), frequency + 12500);
    advance(15000);
    zassert_true(model.dimmed());
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Menu);
    advance(15000);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Menu); //wake only
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Home);
}

ZTEST(ui_backlight, test_ptt_side_taps_held_inputs_fault_and_power_are_independent_of_front_keys) {
    advance(15000);
    radio_ptt(true);
    radio_ptt(false);
    model.advance(k_uptime_get());
    zassert_false(model.dimmed());
    advance(15000);
    radio_monitor(true);
    radio_monitor(false);
    model.advance(k_uptime_get());
    zassert_false(model.dimmed());
    advance(15000);
    radio_ptt(true);
    model.advance(k_uptime_get());
    zassert_equal(model.backlight_percent(), 100);
    radio_service();
    model.sync(radio_snapshot());
    advance(16000);
    zassert_false(model.dimmed());
    radio_ptt(false);
    tick();
    radio_monitor(true);
    tick();
    advance(16000);
    zassert_false(model.dimmed());
    radio_monitor(false);
    tick();
    advance(15000);
    zassert_true(model.dimmed());
    radio_report_fault(-EIO);
    model.advance(k_uptime_get());
    zassert_false(model.dimmed());
    tick();
    advance(16000);
    zassert_equal(model.backlight_percent(), 100);
    radio_power(false);
    radio_service();
    model.sync(radio_snapshot());
    model.advance(k_uptime_get());
    zassert_equal(model.backlight_percent(), 0);
    radio_power(true);
    tick();
    model.advance(k_uptime_get());
    zassert_equal(model.backlight_percent(), 100);
    zassert_equal(radio_snapshot().fault, -EIO);
}

ZTEST(ui_backlight, test_every_idle_choice_dim_clamp_and_explicit_key_wake) {
    const uint8_t choices[] = {15, 30, 60};
    for (uint8_t idle : choices) {
        auto ui = plug.global.ui;
        ui.brightness_percent = 50;
        ui.dim_percent = 30;
        ui.idle_s = idle;
        zassert_ok(
            settings_put_ui_preferences(ui, 890, radio_snapshot(), settings_status().revision));
        tick();
        key(UiKey::Star);
        advance(unsigned(idle) * 1000);
        zassert_true(model.dimmed());
        zassert_equal(model.backlight_percent(), 30);
        key(UiKey::Hash);
        zassert_equal(model.screen(), UiScreen::Home);
        key(UiKey::Hash);
        zassert_equal(model.screen(), UiScreen::Frequency);
        key(UiKey::Back);
    }
    auto ui = plug.global.ui;
    ui.brightness_percent = 25;
    ui.dim_percent = 30;
    zassert_ok(settings_put_ui_preferences(ui, 892, radio_snapshot(), settings_status().revision));
    tick();
    key(UiKey::Star);
    advance(15000);
    zassert_false(model.dimmed());
    zassert_equal(model.backlight_percent(), 25);
    key(UiKey::Hash);
    zassert_equal(model.screen(), UiScreen::Frequency); // Same brightness needs no wake key.
}

ZTEST(ui_backlight, test_ptt_focus_stale_owner_failure_and_submitted_apply_does_not_reopen) {
    open();
    key(UiKey::Up);
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    open();
    model.input({UiKey::Release});
    zassert_equal(model.screen(), UiScreen::Home);
    open();
    auto ui = plug.global.ui;
    ui.contrast = Contrast::High;
    zassert_ok(settings_put_ui_preferences(ui, 891, radio_snapshot(), settings_status().revision));
    tick();
    key(UiKey::Enter);
    tick();
    zassert_equal(model.error(), -ESTALE);
    frame_out("backlight-stale");
    key(UiKey::Back);
    key(UiKey::Back);
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    model.input({UiKey::Release});
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(model.backlight_percent(), 25);
    settings_ui_preferences(ui);
    zassert_equal(ui.contrast, Contrast::High);
    open();
    key(UiKey::Up);
    fail_durable = true;
    key(UiKey::Enter);
    tick();
    zassert_equal(model.backlight_percent(), 50);
    zassert_true(settings_status().pending);
    zassert_equal(settings_status().save_error, -EIO);
    fail_durable = false;
    settings_service(radio_snapshot(), 1100);
    reboot();
    zassert_equal(model.backlight_percent(), 50);
}

ZTEST(ui_backlight, test_sdl_brightness_modulates_radio_texture_and_can_restore_without_a_flush) {
    SdlDisplay radio, panel;
    zassert_ok(sdl_open(radio, "brightness test", 160, 128, 1));
    zassert_ok(sdl_open(panel, "panel test", 240, 128, 1));
    zassert_ok(sdl_backlight(radio, 10));
    uint8_t r, g, b;
    zassert_ok(SDL_GetTextureColorMod(radio.texture, &r, &g, &b));
    zassert_equal(r, 25);
    zassert_equal(g, r);
    zassert_equal(b, r);
    zassert_ok(SDL_GetTextureColorMod(panel.texture, &r, &g, &b));
    zassert_equal(r, 255);
    zassert_ok(sdl_backlight(radio, 100));
    zassert_ok(SDL_GetTextureColorMod(radio.texture, &r, &g, &b));
    zassert_equal(r, 255);
    zassert_equal(sdl_backlight(radio, 101), -EINVAL);
    zassert_equal(radio.backlight_percent, 100);
    sdl_close(radio);
    sdl_close(panel);
    printk("Backlight UI heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST(ui_backlight, test_fault_free_inactive_resume_wakes_and_front_power_gate_remains_off) {
    advance(15000);
    zassert_true(model.dimmed());
    radio_power(false);
    radio_service();
    model.sync(radio_snapshot());
    zassert_equal(model.backlight_percent(), 0);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home);
    radio_power(true);
    tick();
    model.advance(k_uptime_get());
    zassert_equal(model.backlight_percent(), 100);
    zassert_false(model.dimmed());
    zassert_equal(radio_snapshot().fault, 0);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Menu);
}

ZTEST_SUITE(ui_backlight, nullptr, setup, before, nullptr, nullptr);
