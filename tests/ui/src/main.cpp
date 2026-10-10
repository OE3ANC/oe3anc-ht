// SPDX-License-Identifier: GPL-3.0-or-later
#include <SDL.h>
#include <errno.h>
#include <ht/emulator.hpp>
#include <ht/ui.hpp>
#include <ht/ui_presentation_wire.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/ztest.h>

using namespace ht;
static UiModel model;
static lv_color_t buffer[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;

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
    radio_power(true);
    zassert_ok(radio_start(RadioConfig{}));
    model = {};
    model.sync(radio_snapshot());
}

static void key(UiKey key) {
    model.input({key});
}

static void type(const char *text) {
    for (; *text; ++text) {
        model.input({UiKey::Character, *text});
    }
}

static void tick() {
    radio_service();
    model.sync(radio_snapshot());
}

static void select(unsigned index) {
    key(UiKey::Enter);
    for (unsigned i = 0; i < index; ++i) {
        key(UiKey::Down);
    }
}

ZTEST(ui, test_shared_front_fifo_preserves_copies_order_capacity_and_sdl_arrival) {
    UiInput input;
    while (ui_take_input(input)) {
    }
    const UiKey independent[] = {UiKey::Ptt, UiKey::Monitor, UiKey::Release, UiKey::Quit};
    for (auto key : independent) {
        zassert_equal(ui_queue_input({key}), -EINVAL);
    }
    zassert_equal(ui_queue_input({static_cast<UiKey>(255)}), -EINVAL);
    for (unsigned i = 0; i < 16; ++i) {
        input = {UiKey::Digit, static_cast<char>('0' + i % 10), true, int64_t(i)};
        zassert_ok(ui_queue_input(input));
        input.character = 'X'; // Producer storage is never borrowed.
    }
    zassert_equal(ui_queue_input({UiKey::Up}), -EBUSY);
    for (unsigned i = 0; i < 16; ++i) {
        zassert_true(ui_take_input(input));
        zassert_equal(input.key, UiKey::Digit);
        zassert_equal(input.character, '0' + i % 10);
        zassert_equal(input.timestamp_ms, i);
    }
    zassert_false(ui_take_input(input));
    zassert_ok(ui_queue_input({UiKey::Character, 'A', true, 100}));
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.windowID = 1;
    event.key.keysym.sym = SDLK_b;
    zassert_equal(SDL_PushEvent(&event), 1);
    zassert_true(ui_backend_input(input));
    zassert_equal(input.character, 'A');
    zassert_equal(input.timestamp_ms, 100);
    zassert_true(ui_backend_input(input));
    zassert_equal(input.character, 'b');
    zassert_false(ui_backend_input(input));
}

ZTEST(ui, test_connected_cps_guard_retains_local_draft_and_blocks_pending_actions) {
    zassert_true(model.cps_available());
    model.input({UiKey::Enter}, true); // Complete replacement is pending.
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(model.error(), -EBUSY);
    zassert_false(model.command_pending());
    model.input({UiKey::Monitor}, true); // Independent holds do not become ordinary actions.
    zassert_equal(model.screen(), UiScreen::Home);
    type("145.500");
    zassert_equal(model.screen(), UiScreen::Frequency);
    zassert_false(model.cps_available());
    const auto length = model.text_editor().length();
    model.input({UiKey::Character, '0'}, true);
    zassert_equal(model.text_editor().length(), length);
    zassert_equal(strcmp(model.text_editor().text(), "145.500"), 0);
    model.input({UiKey::Ptt}, true); // Safety interruption is handled before the CPS guard.
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_true(model.cps_available());
    type("145.500");
    key(UiKey::Enter);
    zassert_false(model.cps_available()); // An ordinary RF command is still pending.
    tick();
    zassert_true(model.cps_available());
}

ZTEST(ui, test_dcs_display_and_unrelated_frequency_edit_preserve_typed_tones) {
    RadioConfig config;
    config.rx_tone = {ToneKind::Dcs, 0023, true};
    config.tx_tone = {ToneKind::Dcs, 0754, false};
    zassert_ok(radio_start(config));
    model.sync(radio_snapshot());
    select(5); // TX tone; the scroll window includes both tone rows.
    char lines[8][32];
    model.lines(lines);
    bool rx = false, tx = false;
    for (const auto &line : lines) {
        rx |= strstr(line, "D023I") != nullptr;
        tx |= strstr(line, "D754N") != nullptr;
    }
    zassert_true(rx && tx);
    key(UiKey::Back);
    type("145.500");
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
    zassert_true(same_tone(radio_snapshot().config.rx_tone, config.rx_tone));
    zassert_true(same_tone(radio_snapshot().config.tx_tone, config.tx_tone));
}

ZTEST(ui, test_transmit_warning_and_release_prompt) {
    RadioConfig config;
    config.transmit_limit_s = 60;
    zassert_ok(radio_start(config));
    radio_ptt(true);
    tick();
    k_sleep(K_SECONDS(51));
    tick();
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[6], "TX limit in"));
    k_sleep(K_SECONDS(9));
    tick();
    model.lines(lines);
    zassert_equal(strcmp(lines[6], "TX timeout: release PTT"), 0);
    radio_ptt(false);
    tick(); // The UI service dispatches PTT separately from the model.
    model.lines(lines);
    zassert_is_null(strstr(lines[6], "timeout"));
    RadioState fault = radio_snapshot();
    fault.phase = RadioPhase::Fault;
    fault.fault = -EPIPE;
    fault.tx_timed_out = true;
    model.sync(fault);
    model.lines(lines);
    zassert_equal(strcmp(lines[0], "RADIO FAULT"), 0);
}

ZTEST(ui, test_transmit_limit_menu_opens_explicit_apply_editor) {
    select(8); // Timeout follows diagnostics; unsupported gain is skipped.
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::TransmitLimit);
    key(UiKey::Up);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.transmit_limit_s, 0);
    zassert_equal(model.screen(), UiScreen::Menu);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Back);
    tick();
    zassert_equal(radio_snapshot().config.transmit_limit_s, 0);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    radio_ptt(true);
    tick();
    key(UiKey::Enter);
    zassert_equal(model.error(), -EBUSY);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
}

ZTEST(ui, test_frequency_entry_preserves_hertz_and_rejects_bad_values) {
    type("145.500001");
    key(UiKey::Enter);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 430000000);
    tick();
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001);
    zassert_equal(radio_snapshot().config.tx_frequency_hz, 145500001);
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Hash);
    type("300");
    key(UiKey::Enter);
    zassert_equal(model.error(), -EINVAL);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001);
    key(UiKey::Back);
    key(UiKey::Hash);
    type("145.1234567");
    key(UiKey::Enter);
    zassert_equal(model.error(), -EINVAL);
}

ZTEST(ui, test_settings_only_update_after_controller_acceptance) {
    select(0);
    key(UiKey::Enter);
    zassert_equal(radio_snapshot().config.mode, Mode::Fm);
    tick();
    zassert_equal(radio_snapshot().config.mode, Mode::M17);
    radio_ptt(true);
    tick();
    zassert_equal(radio_snapshot().ptt_error, -EADDRNOTAVAIL);
    radio_ptt(false);
    tick();
    key(UiKey::Enter);
    tick(); // Back to FM.
    radio_ptt(true);
    tick();
    key(UiKey::Down); // Bandwidth.
    key(UiKey::Enter);
    tick();
    zassert_equal(model.error(), -EBUSY);
    zassert_equal(radio_snapshot().config.bandwidth, Bandwidth::Wide);
    radio_ptt(false);
    tick();
}

ZTEST(ui, test_mode_settings_navigation_preserves_fm_configuration) {
    RadioConfig config;
    config.bandwidth = Bandwidth::Narrow;
    config.squelch = 9;
    config.rx_tone = {ToneKind::Ctcss, 885, false};
    config.tx_tone = {ToneKind::Ctcss, 1230, false};
    zassert_ok(radio_start(config));
    model.sync(radio_snapshot());
    select(0);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.mode, Mode::M17);
    char lines[8][32];
    const char *items[] = {"Mode: M17",          "Power: 1 W",      "Local callsign",
                           "BK4819 diagnostics", "TX limit: 180 s", "Status"};
    // Every visible control is reachable in each direction; hidden controls
    // neither consume a navigation step nor clip earlier rows from the menu.
    const UiKey directions[] = {UiKey::Down, UiKey::Up};
    for (const auto direction : directions) {
        for (unsigned i = 0; i < 6; ++i) {
            model.lines(lines);
            const unsigned index = direction == UiKey::Down ? i : (6 - i) % 6;
            unsigned selected = 0, visible = 0;
            for (const auto &line : lines) {
                zassert_is_null(strstr(line, "BW:"));
                zassert_is_null(strstr(line, "Squelch:"));
                zassert_is_null(strstr(line, "tone:"));
                if (line[0] == '>') {
                    ++selected;
                    zassert_not_null(strstr(line, items[index]));
                }
                visible += line[0] == '>' || line[0] == ' ';
            }
            zassert_equal(selected, 1);
            UiListPage page;
            model.menu_page(page);
            zassert_equal(page.count, 6);
            zassert_equal(visible, MIN(4, page.count - page.cursor / 4 * 4));
            key(direction);
        }
    }
    key(UiKey::Down); // Power is still editable in M17.
    key(UiKey::Right);
    tick();
    zassert_equal(radio_snapshot().config.power_mw, 2500);
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    const auto restored = radio_snapshot().config;
    zassert_equal(restored.mode, Mode::Fm);
    zassert_equal(restored.bandwidth, config.bandwidth);
    zassert_equal(restored.squelch, config.squelch);
    zassert_equal(restored.rx_tone.value, config.rx_tone.value);
    zassert_equal(restored.tx_tone.value, config.tx_tone.value);
    model.lines(lines);
    zassert_not_null(strstr(lines[3], "BW: 12.5 kHz"));
    zassert_not_null(strstr(lines[4], "Squelch: 9"));
}

ZTEST(ui, test_external_mode_change_reselects_a_supported_menu_item) {
    select(1); // FM bandwidth selected.
    RadioCommand command;
    command.config.mode = Mode::M17;
    zassert_ok(radio_submit(command));
    tick();
    char lines[8][32];
    model.lines(lines);
    UiListPage page;
    model.menu_page(page);
    zassert_equal(strcmp(page.rows[page.cursor % 4].name, "Mode: M17"), 0);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.mode, Mode::Fm);
    zassert_equal(radio_snapshot().config.bandwidth, Bandwidth::Wide);
}

ZTEST(ui, test_callsign_editor_and_capability_filter) {
    select(6); // Gain is unsupported, so the sixth down selects callsign.
    char lines[8][32];
    model.lines(lines);
    for (const auto &line : lines) {
        zassert_is_null(strstr(line, "Gain:"));
    }
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Callsign);
    type("oe3anc");
    key(UiKey::Enter);
    tick();
    zassert_equal(strcmp(radio_snapshot().config.callsign, "OE3ANC"), 0);
    zassert_equal(model.screen(), UiScreen::Home);
}

ZTEST(ui, test_diagnostic_edit_read_write_and_restore) {
    select(7);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Diagnostics);
    key(UiKey::Enter);
    type("40");
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Enter);
    type("beef");
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[2], "BEEF"));
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().register_value, 0xbeef);
    key(UiKey::Back);
    tick();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    key(UiKey::Enter);
    tick();
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().register_value, 0);
}

ZTEST(ui, test_diagnostic_hex_entry_with_physical_keys) {
    select(7);
    key(UiKey::Enter);
    tick();
    key(UiKey::Enter); // Edit address, start at hex 0.
    for (unsigned i = 0; i < 4; ++i) {
        key(UiKey::Up);
    }
    key(UiKey::Star);
    for (unsigned i = 0; i < 4; ++i) {
        key(UiKey::Down);
    }
    key(UiKey::Star);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Enter); // Edit value, wrap backward to F.
    key(UiKey::Down);
    key(UiKey::Star);
    key(UiKey::Star);
    key(UiKey::Hash); // Delete one F, then replace it.
    key(UiKey::Star);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().register_value, 0xff);
    key(UiKey::Back);
    tick();
}

ZTEST(ui, test_sdl_focus_loss_releases_ptt_despite_full_queue) {
    radio_ptt(true);
    tick();
    zassert_true(emulator_transmitting());
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
    bool release = false;
    while (ui_backend_input(input)) {
        if (input.key == UiKey::Release) {
            radio_ptt(false);
            release = true;
        }
    }
    zassert_true(release);
    tick();
    zassert_false(emulator_transmitting());
}

ZTEST(ui, test_sdl_monitor_is_separate_from_text_and_releases_on_focus_loss) {
    UiInput input;
    while (ui_backend_input(input)) {
    }
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.windowID = 1;
    event.key.keysym.sym = SDLK_F3;
    zassert_equal(SDL_PushEvent(&event), 1);
    zassert_true(ui_backend_input(input));
    zassert_equal(input.key, UiKey::Monitor);
    zassert_true(input.pressed);
    radio_monitor(input.pressed);
    tick();
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[2], "MONITOR"));
    event = {};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = 1;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    zassert_equal(SDL_PushEvent(&event), 1);
    // SDL may yield its end-of-poll-cycle sentinel before a newly pushed event.
    zassert_true(ui_backend_input(input) || ui_backend_input(input));
    zassert_equal(input.key, UiKey::Release);
    radio_ptt(false);
    radio_monitor(false);
    tick();
    zassert_false(radio_snapshot().monitor_active);
    event = {};
    event.type = SDL_KEYUP;
    event.key.windowID = 2; // Release in developer window still reaches the owner.
    event.key.keysym.sym = SDLK_F3;
    zassert_equal(SDL_PushEvent(&event), 1);
    zassert_true(ui_backend_input(input) || ui_backend_input(input));
    zassert_equal(input.key, UiKey::Monitor);
    zassert_false(input.pressed);
    event.type = SDL_KEYDOWN;
    event.key.windowID = 1;
    event.key.keysym.sym = SDLK_m;
    zassert_equal(SDL_PushEvent(&event), 1);
    zassert_true(ui_backend_input(input) || ui_backend_input(input));
    zassert_equal(input.key, UiKey::Character);
    zassert_equal(input.character, 'm');
}

ZTEST(ui, test_developer_panel_injects_activity_fault_and_requests_reset) {
    RadioCommand mode;
    mode.config.mode = Mode::M17;
    zassert_ok(radio_submit(mode));
    tick();
    UiInput input;
    while (ui_backend_input(input)) {
    }
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.windowID = 2;
    event.key.keysym.sym = SDLK_RETURN;
    zassert_equal(SDL_PushEvent(&event), 1);
    while (ui_backend_input(input)) {
    }
    tick();
    zassert_true(radio_snapshot().rx_active);
    event.key.keysym.sym = SDLK_DOWN;
    zassert_equal(SDL_PushEvent(&event), 1);
    event.key.keysym.sym = SDLK_RETURN;
    zassert_equal(SDL_PushEvent(&event), 1);
    for (const char *text = "-72"; *text; ++text) {
        event.key.keysym.sym = *text;
        zassert_equal(SDL_PushEvent(&event), 1);
    }
    event.key.keysym.sym = SDLK_RETURN;
    zassert_equal(SDL_PushEvent(&event), 1);
    while (ui_backend_input(input)) {
    }
    tick();
    zassert_equal(radio_snapshot().rssi_dbm, -72);
    event.key.keysym.sym = SDLK_DOWN;
    zassert_equal(SDL_PushEvent(&event), 1);
    event.key.keysym.sym = SDLK_RETURN;
    zassert_equal(SDL_PushEvent(&event), 1);
    for (const char *text = "oe9test"; *text; ++text) {
        event.key.keysym.sym = *text;
        zassert_equal(SDL_PushEvent(&event), 1);
    }
    event.key.keysym.sym = SDLK_RETURN;
    zassert_equal(SDL_PushEvent(&event), 1);
    while (ui_backend_input(input)) {
    }
    tick();
    zassert_equal(strcmp(radio_snapshot().received_callsign, "OE9TEST"), 0);
    for (unsigned i = 0; i < 1; ++i) {
        event.key.keysym.sym = SDLK_DOWN;
        zassert_equal(SDL_PushEvent(&event), 1);
    }
    event.key.keysym.sym = SDLK_RETURN;
    zassert_equal(SDL_PushEvent(&event), 1);
    while (ui_backend_input(input)) {
    }
    tick();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[0], "RADIO FAULT"), 0);
    zassert_equal(strcmp(lines[4], "TX disabled"), 0);
    for (unsigned i = 0; i < 2; ++i) {
        event.key.keysym.sym = SDLK_DOWN;
        zassert_equal(SDL_PushEvent(&event), 1);
    }
    event.key.keysym.sym = SDLK_RETURN;
    zassert_equal(SDL_PushEvent(&event), 1);
    while (ui_backend_input(input)) {
    }
    zassert_true(emulator_take_reset_request());
    zassert_false(emulator_take_reset_request());
    zassert_ok(radio_start(radio_snapshot().config));
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
}

ZTEST(ui, test_lvgl_render_and_bounded_heap) {
    ui_view_update(model);
    ui_backend_service();
    for (auto *it = lv_disp_get_next(nullptr); it; it = lv_disp_get_next(it)) {
        lv_refr_now(it);
    }
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    zassert_equal(memory.total_size, 32768);
    zassert_true(memory.free_size > 4096);
    printk("LVGL heap: %u of %u bytes free\n", static_cast<unsigned>(memory.free_size),
           static_cast<unsigned>(memory.total_size));
    zassert_equal(lv_mem_test(), LV_RES_OK);
    const char *output = getenv("HT_UI_FRAME");
    if (output) {
        FILE *file = fopen(output, "wb");
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
}

ZTEST(ui, test_reset_clears_diagnostics_and_discarded_command) {
    select(7);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Diagnostics);
    zassert_ok(radio_start(radio_snapshot().config));
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    type("145.5");
    key(UiKey::Enter); // Accepted into the queue, then discarded by reset.
    zassert_ok(radio_start(radio_snapshot().config));
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_ok(model.error());
}

ZTEST(ui, test_home_displays_the_keyed_split_frequency) {
    RadioConfig config;
    config.rx_frequency_hz = 439075000;
    config.tx_frequency_hz = 431475000;
    zassert_ok(radio_start(config));
    model.sync(radio_snapshot());
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[1], "439.075000 MHz"), 0);
    radio_ptt(true);
    tick();
    model.lines(lines);
    zassert_equal(strcmp(lines[1], "431.475000 MHz"), 0);
    zassert_not_null(strstr(lines[2], "TX"));
    radio_ptt(false);
    tick();
    model.lines(lines);
    zassert_equal(strcmp(lines[1], "439.075000 MHz"), 0);
}

ZTEST(ui, test_inactive_cancels_editor_and_ignores_front_commands) {
    model.input({UiKey::Character, '1'});
    zassert_equal(model.screen(), UiScreen::Frequency);
    radio_power(false);
    tick();
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[0], "INACTIVE"));
    model.input({UiKey::Character, '2'});
    model.input({UiKey::Enter});
    const auto inactive_command = radio_snapshot().command_id;
    radio_service();
    zassert_equal(radio_snapshot().command_id, inactive_command);
    radio_power(true);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 430000000);
}

ZTEST(ui, test_developer_battery_inputs_and_errors_are_independent) {
    const auto send = [](SDL_Keycode key) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.windowID = 2;
        event.key.keysym.sym = key;
        zassert_equal(SDL_PushEvent(&event), 1);
        UiInput input;
        // Drain both SDL poll cycles and any device input.
        for (unsigned i = 0; i < 2; ++i) {
            while (ui_backend_input(input)) {
            }
        }
    };
    send(SDLK_ESCAPE); // return selection to first developer control
    for (unsigned i = 0; i < 6; ++i) {
        send(SDLK_DOWN);
    }
    send(SDLK_RETURN);
    for (const char *c = "8400"; *c; ++c) {
        send(*c);
    }
    send(SDLK_RETURN);
    int error;
    auto battery = emulator_battery_input(error);
    zassert_ok(error);
    zassert_equal(battery.millivolts, 8400);
    zassert_true(battery.switch_on);
    zassert_false(battery.charger_input);
    send(SDLK_DOWN);
    send(SDLK_RETURN); // charger toggle
    send(SDLK_DOWN);
    send(SDLK_RETURN); // switch toggle independently
    battery = emulator_battery_input(error);
    zassert_true(battery.charger_input);
    zassert_false(battery.switch_on);
    zassert_equal(battery.millivolts, 8400);
    zassert_ok(battery_sample());
    send(SDLK_DOWN);
    send(SDLK_RETURN); // persistent read error
    zassert_equal(battery_sample(), -EIO);
    zassert_equal(battery_snapshot().freshness, BatteryFreshness::Stale);
    zassert_equal(battery_snapshot().reading.millivolts, 8400);
    send(SDLK_RETURN);
    zassert_ok(battery_sample()); // recover same inputs
    send(SDLK_ESCAPE);
    emulator_inject_battery({7200, false, true});
}

static void tap(char digit, unsigned count, int64_t &time) {
    while (count--) {
        model.input({UiKey::Digit, digit, true, time});
        time += 50;
    }
}

ZTEST(ui, test_physical_callsign_multitap_and_explicit_apply) {
    select(6);
    key(UiKey::Enter);
    int64_t time = 100;
    tap('6', 3, time);
    tap('3', 2, time);
    key(UiKey::Hash);
    tap('3', 4, time);
    tap('2', 1, time);
    tap('6', 2, time);
    tap('2', 3, time);
    zassert_equal(strcmp(model.text_editor().text(), "OE3ANC"), 0);
    zassert_equal(radio_snapshot().config.callsign[0], 0);
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    zassert_equal(radio_snapshot().config.callsign[0], 0);
    tick();
    zassert_equal(strcmp(radio_snapshot().config.callsign, "OE3ANC"), 0);
    zassert_equal(model.screen(), UiScreen::Home);
}

ZTEST(ui, test_text_cursor_erase_cancel_and_producer_timing) {
    select(6);
    key(UiKey::Enter);
    model.input({UiKey::Digit, '2', true, 100});
    model.sync(radio_snapshot()); // A late UI sync must not expire queued producer taps.
    model.input({UiKey::Digit, '2', true, 500});
    zassert_equal(strcmp(model.text_editor().text(), "B"), 0);
    model.advance(1249);
    zassert_true(model.text_editor().pending());
    model.advance(1250);
    zassert_false(model.text_editor().pending());
    type("AC");
    key(UiKey::Up);
    key(UiKey::Up);
    type("Z");
    zassert_equal(strcmp(model.text_editor().text(), "BZAC"), 0);
    key(UiKey::Left);
    zassert_equal(strcmp(model.text_editor().text(), "BAC"), 0);
    key(UiKey::Down);
    key(UiKey::Erase);
    zassert_equal(strcmp(model.text_editor().text(), "BC"), 0);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Menu);
    tick();
    zassert_equal(radio_snapshot().config.callsign[0], 0);
}

ZTEST(ui, test_physical_frequency_decimal_and_cancel) {
    int64_t time = 100;
    tap('1', 1, time);
    tap('4', 1, time);
    tap('5', 1, time);
    key(UiKey::Right);
    for (char digit : "500001") {
        if (digit) {
            tap(digit, 1, time);
        }
    }
    zassert_equal(strcmp(model.text_editor().text(), "145.500001"), 0);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001);
    key(UiKey::Hash);
    type("300");
    key(UiKey::Enter);
    zassert_equal(model.error(), -EINVAL);
    zassert_equal(strcmp(model.text_editor().text(), "300"), 0);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001);
}

ZTEST(ui, test_text_draft_cancel_on_hardware_ptt_and_lifecycle) {
    RadioConfig config;
    config.tx_inhibit = true;
    zassert_ok(radio_start(config));
    model.sync(radio_snapshot());
    type("145.5");
    radio_ptt(true);
    radio_ptt(false);  // Entire tap bypasses UI; RX may never change.
    key(UiKey::Enter); // Must drop interrupted Apply, not reinterpret it as menu.
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    tick();
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 430000000);
    select(6);
    key(UiKey::Enter);
    type("OE3ANC");
    radio_ptt(true);
    tick(); // Rejected PTT still cancels the callsign draft.
    zassert_equal(model.screen(), UiScreen::Menu);
    key(UiKey::Enter);
    zassert_equal(model.error(), -EBUSY);
    radio_ptt(false);
    tick();
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Callsign);
    type("OE3ANC");
    model.input({UiKey::Release});
    zassert_equal(model.screen(), UiScreen::Menu);
    key(UiKey::Back);
    type("145.5");
    RadioState fault = radio_snapshot();
    fault.phase = RadioPhase::Fault;
    fault.fault = -EPIPE;
    model.sync(fault);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.callsign[0], 0);
}

ZTEST(ui, test_sdl_literal_and_physical_digit_inputs) {
    UiInput input;
    while (ui_backend_input(input)) {
    }
    const SDL_Keycode keys[] = {SDLK_2, SDLK_KP_2, SDLK_2};
    const UiKey kinds[] = {UiKey::Character, UiKey::Digit, UiKey::Digit};
    for (unsigned i = 0; i < 3; ++i) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.windowID = 1;
        event.key.keysym.sym = keys[i];
        event.key.keysym.mod = i == 2 ? KMOD_ALT : KMOD_NONE;
        zassert_equal(SDL_PushEvent(&event), 1);
        bool received = false;
        for (unsigned j = 0; j < 4 && !received; ++j) {
            received = ui_backend_input(input);
        }
        zassert_true(received);
        zassert_equal(input.key, kinds[i]);
        zassert_equal(input.character, '2');
    }
}

static void text_frame(const char *name) {
    ui_view_update(model);
    lv_refr_now(display);
    auto *root = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
    for (unsigned i = 0; i < lv_obj_get_child_cnt(root); ++i) {
        auto *object = lv_obj_get_child(root, i);
        if (lv_obj_check_type(object, &lv_textarea_class)) {
            zassert_is_null(lv_anim_get(object, nullptr)); // Caret/scroll remain immediate.
            zassert_is_null(lv_anim_get(lv_textarea_get_label(object), nullptr));
        }
    }
    zassert_true(lv_anim_count_running() <= 1); // Only optional page entry may remain.
    lv_tick_inc(150);
    lv_timer_handler();
    lv_refr_now(display); // Export settled geometry.
    zassert_equal(lv_anim_count_running(), 0);
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    zassert_equal(memory.total_size, 32768);
    zassert_true(memory.free_size > 4096);
    const char *directory = getenv("HT_UI_TEXT_FRAMES");
    if (!directory) {
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb");
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

ZTEST(ui, test_presentation_copy_renders_without_live_model_or_backend_reads) {
    static UiPresentation captured;
    static uint16_t expected[160 * 128];
    type("145.500001");
    ui_capture_presentation(model, captured);
    zassert_equal(captured.screen, UiScreen::Frequency);
    zassert_equal(strcmp(captured.text.value, "145.500001"), 0);
    zassert_equal(captured.text.cursor, 10);
    zassert_equal(strcmp(captured.actions[0], "OK Apply"), 0);
    zassert_equal(strcmp(captured.actions[1], "BACK Cancel"), 0);
    captured.motion = false;
    ui_view_update(captured);
    lv_refr_now(display);
    memcpy(expected, frame, sizeof(frame));
    // Destroy the source draft and show another page. The captured copy still
    // reproduces every pixel, including its cursor, field help and footer.
    key(UiKey::Back);
    key(UiKey::Enter);
    key(UiKey::Down);
    ui_view_update(model);
    lv_tick_inc(150);
    lv_timer_handler();
    lv_refr_now(display);
    zassert_equal(model.screen(), UiScreen::Menu);
    ui_view_update(captured);
    lv_refr_now(display);
    zassert_mem_equal(frame, expected, sizeof(frame));
    static uint8_t wire[companion::UI_MAX_BYTES];
    static UiPresentation received;
    size_t length = 0;
    zassert_ok(ui_encode_presentation(captured, wire, sizeof(wire), length));
    zassert_ok(ui_decode_presentation(wire, length, received));
    ui_view_update(received);
    lv_refr_now(display);
    zassert_mem_equal(frame, expected, sizeof(frame));
    zassert_equal(strcmp(captured.text.value, "145.500001"), 0);
    UiPresentation menu;
    ui_capture_presentation(model, menu);
    UiListPage page;
    model.menu_page(page);
    zassert_equal(menu.list.cursor, page.cursor);
    zassert_equal(menu.list.count, page.count);
    zassert_equal(strcmp(menu.list.title, page.title), 0);
    zassert_equal(strcmp(menu.actions[1], "BACK Home"), 0);
}

ZTEST(ui, test_text_lvgl_cursor_viewport_and_render) {
    type("145.500001");
    text_frame("frequency");
    const auto accent =
        lv_color_hex(ui_palette(model.preferences().theme, model.preferences().contrast).accent)
            .full;
    bool visible_caret = false;
    for (unsigned y = 46; y < 75; ++y) {
        for (unsigned x = 8; x < 152; ++x) {
            visible_caret |= frame[y * 160 + x] == accent;
        }
    }
    zassert_true(visible_caret, "The model cursor must actually be rendered inside the field");
    auto *root = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
    lv_obj_t *entry = nullptr;
    for (unsigned i = 0; i < lv_obj_get_child_cnt(root); ++i) {
        auto *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_textarea_class)) {
            entry = child;
            break;
        }
    }
    zassert_not_null(entry);
    zassert_true(lv_obj_check_type(entry, &lv_textarea_class));
    zassert_equal(strcmp(lv_textarea_get_text(entry), "145.500001"), 0);
    zassert_equal(lv_textarea_get_cursor_pos(entry), 10);
    lv_point_t caret;
    lv_label_get_letter_pos(lv_textarea_get_label(entry), 10, &caret);
    zassert_true(caret.x - lv_obj_get_scroll_x(entry) < lv_obj_get_content_width(entry));
    for (unsigned i = 0; i < 10; ++i) {
        key(UiKey::Up);
    }
    text_frame("frequency-start");
    zassert_equal(lv_textarea_get_cursor_pos(entry), 0);
    zassert_equal(lv_obj_get_scroll_x(entry), 0);
    for (unsigned i = 0; i < 10; ++i) {
        key(UiKey::Down);
    }
    for (unsigned i = 0; i < 10; ++i) {
        key(UiKey::Left);
    }
    type("88888888888");
    text_frame("frequency-long");
    lv_label_get_letter_pos(lv_textarea_get_label(entry), 11, &caret);
    zassert_true(lv_obj_get_scroll_x(entry) > 0);
    zassert_true(caret.x - lv_obj_get_scroll_x(entry) < lv_obj_get_content_width(entry));
    for (unsigned i = 0; i < 11; ++i) {
        key(UiKey::Up);
    }
    ui_view_update(model);
    zassert_equal(lv_obj_get_scroll_x(entry), 0);
    key(UiKey::Back);
    select(6);
    key(UiKey::Enter);
    type("OE3ANC/P");
    text_frame("callsign");
    zassert_equal(lv_textarea_get_cursor_pos(entry), 8);
    key(UiKey::Left);
    key(UiKey::Left);
    model.input({UiKey::Digit, '2', true, 100});
    text_frame("callsign-tap");
    zassert_equal(lv_textarea_get_cursor_pos(entry), model.text_editor().cursor() - 1);
    model.advance(850);
    text_frame("callsign-committed");
    zassert_equal(lv_textarea_get_cursor_pos(entry), model.text_editor().cursor());
    zassert_equal(lv_mem_test(), LV_RES_OK);
}

ZTEST_SUITE(ui, nullptr, setup, before, nullptr, nullptr);
