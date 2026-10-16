// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/battery.hpp>
#include <ht/codeplug_storage.hpp>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <ht/ui.hpp>
#include <ht/voice.hpp>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zephyr/ztest.h>
using namespace ht;
static UiModel model;
static Codeplug plug;
static char root[] = "/tmp/ht-ui-menu-XXXXXX", path[160];
static lv_color_t pixels[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static uint32_t channel_id, bank_id;
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
static unsigned flush_count;
static bool fail_durable;
extern "C" int __real_fsync(int fd);

extern "C" int __wrap_fsync(int fd) {
    if (fail_durable) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

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
    const auto *directory = getenv("HT_UI_MENU_FRAMES");
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
    zassert_ok(setenv("HT_PROFILE", "menu", 1));
    snprintf(path, sizeof(path), "%s/menu.bin", root);
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
    model.input({UiKey::Enter});
    for (unsigned i = 0; i < 32; ++i) {
        UiListPage page;
        model.menu_page(page);
        if (!strcmp(page.rows[page.cursor % 4].name, "Status")) {
            model.input({UiKey::Enter});
            break;
        }
        model.input({UiKey::Down});
    }
    zassert_equal(model.screen(), UiScreen::Status);
    for (unsigned i = 0; i < 3; ++i) {
        model.input({UiKey::Right});
    }
    UiStatus status;
    model.status(status);
    zassert_not_null(strstr(status.detail, "Unknown"));
    zassert_equal(strcmp(status.rows[0], "Voltage unknown"), 0);
    render("battery-unknown");
    emulator_inject_battery({0, false, false}, -EIO);
    zassert_equal(battery_sample(), -EIO);
    model.status(status);
    zassert_not_null(strstr(status.detail, "Unknown / read error"));
    zassert_equal(status.color, UiStatusColor::Red);
    zassert_equal(strcmp(status.rows[2], "Switch input unknown"), 0);
    render("battery-unknown-error");
    return nullptr;
}

static void load(bool memory = false, Mode mode = Mode::Fm, Theme theme = Theme::Midnight,
                 Contrast contrast = Contrast::Normal) {
    fail_durable = false;
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

static void key(UiKey button) {
    model.input({button});
}

static void find(const char *name) {
    if (model.screen() == UiScreen::Status) {
        key(UiKey::Back);
    }
    if (model.screen() != UiScreen::Menu) {
        key(UiKey::Enter);
    }
    zassert_equal(model.screen(), UiScreen::Menu);
    UiListPage page;
    model.menu_page(page);
    if (strcmp(page.title, "MENU")) {
        key(UiKey::Back);
    }
    const char *group = strstr(name, "FM ") || strstr(name, "LNA") || strstr(name, "RX ") ||
                                strstr(name, "Codec") || strstr(name, "BK4819")
                            ? "Development"
                        : strstr(name, "TX limit") || strstr(name, "callsign") ||
                                strstr(name, "VFO step") || strstr(name, "VFO / Memory")
                            ? "Radio"
                            : nullptr;
    for (unsigned pass = 0; pass < (group ? 2U : 1U); ++pass) {
        const char *target = group && pass == 0 ? group : name;
        bool found = false;
        for (unsigned i = 0; i < 32; ++i) {
            model.menu_page(page);
            if (strstr(page.rows[page.cursor % 4].name, target)) {
                found = true;
                break;
            }
            key(UiKey::Down);
        }
        zassert_true(found, "Missing menu: %s", target);
        if (group && pass == 0) {
            key(UiKey::Enter);
        }
    }
}

static void adjust(int direction) {
    key(UiKey::Enter);
    key(direction > 0 ? UiKey::Up : UiKey::Down);
    key(UiKey::Enter);
}

static void status(unsigned page = 0) {
    if (page >= 5) {
        const char *names[] = {"Codec2", "RX Path", "RX Gain", "RX Squelch"};
        find(names[page - 5]);
    } else {
        find("Status");
    }
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Status);
    if (page < 5) {
        for (unsigned i = 0; i < page; ++i) {
            key(UiKey::Right);
        }
    }
}

ZTEST(ui_menu, test_quick_toggle_draft_ptt_and_discard) {
    key(UiKey::Monitor);
    zassert_equal(model.screen(), UiScreen::QuickControls);
    key(UiKey::Monitor);
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Monitor);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    key(UiKey::Monitor);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    key(UiKey::Right); // Mode / inhibit / power page.
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    key(UiKey::Up);
    key(UiKey::Enter);
    const auto applied = radio_snapshot().config.power_mw;
    radio_ptt(true);
    tick();
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    UiHome home;
    model.home(home);
    zassert_true(home.visible && home.transmitting);
    zassert_equal(radio_snapshot().config.power_mw, applied);
    key(UiKey::Right); // TX cannot move the hidden editor.
    radio_ptt(false);
    tick();
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[4], "2.5"));
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Discard);
    render("discard-draft");
    key(UiKey::Enter); // Default is Continue editing.
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    key(UiKey::Right);
    key(UiKey::Right);
    key(UiKey::Enter); // Apply, once.
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.power_mw, 2500);
}

ZTEST(ui_menu, test_startup_position_skip_ptt_and_saved_switch) {
    model.begin_startup(100);
    zassert_equal(model.screen(), UiScreen::Startup);
    render("startup-top");
    model.advance(1100);
    UiPresentation view;
    ui_capture_presentation(model, view);
    zassert_equal(view.list.cursor, 80);
    render("startup-center");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home); // Skip consumes the ordinary key.
    model.begin_startup(2000);
    radio_ptt(true);
    model.advance(2001);
    zassert_false(model.startup_visible());
    radio_ptt(false);
    tick();
    find("Appearance");
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    key(UiKey::Up);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    UiPreferences preferences;
    settings_ui_preferences(preferences);
    zassert_false(preferences.startup_animation);
    model.begin_startup(3000);
    zassert_false(model.startup_visible());
}

ZTEST(ui_menu, test_workflow_order_wrap_capabilities_and_physical_actions) {
    key(UiKey::Enter);
    UiListPage page;
    model.menu_page(page);
    const char *first[] = {"VFO Settings", "Channels", "Banks", "Radio"};
    for (unsigned i = 0; i < 4; ++i) {
        zassert_equal(strcmp(page.rows[i].name, first[i]), 0);
    }
    render("menu-workflows");
    const auto idle = flush_count;
    ui_view_update(model);
    lv_refr_now(display);
    zassert_equal(flush_count, idle);
    zassert_equal(lv_obj_get_x(label("OK Select")), 8);
    zassert_equal(lv_obj_get_x(label("BACK")), 82);
    const auto revision = radio_snapshot().configuration_revision;
    key(UiKey::Left);
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(radio_snapshot().configuration_revision, revision);
    const auto count = page.count;
    zassert_equal(count, 9); // Includes the opt-in FM modem submenu.
    for (unsigned i = 0; i < count; ++i) {
        key(UiKey::Down);
    }
    model.menu_page(page);
    zassert_equal(page.cursor, 0);
    key(UiKey::Up);
    model.menu_page(page);
    zassert_equal(strcmp(page.rows[page.cursor % 4].name, "Modem / KISS"), 0);
    render("menu-last");
    key(UiKey::Back);
    load(false, Mode::M17);
    key(UiKey::Enter);
    model.menu_page(page);
    zassert_equal(page.count, count - 1); // AFSK modem is only available in FM.
    find("Invert LNA GPIOs");
    model.menu_page(page);
    bool has_lna_polarity = false;
    for (unsigned i = 0; i < page.count; ++i) {
        UiListPage digital;
        model.menu_page(digital);
        const char *name = digital.rows[digital.cursor % 4].name;
        has_lna_polarity |= strstr(name, "Invert LNA GPIOs:") != nullptr;
        zassert_is_null(strstr(name, "BW:"));
        zassert_is_null(strstr(name, "Squelch:"));
        zassert_is_null(strstr(name, "tone:"));
        key(UiKey::Down);
    }
    zassert_true(has_lna_polarity);
}

ZTEST(ui_menu, test_lna_polarity_is_global_saved_and_locked_during_tx) {
    load(true);
    const auto selection = radio_snapshot().selection;
    find("Invert LNA GPIOs");
    adjust(1);
    tick();
    zassert_equal(radio_snapshot().config.lna_inverted, true);
    zassert_true(same_selection(selection, radio_snapshot().selection));
    zassert_true(settings_status().pending);
    render("lna-polarity");
    radio_ptt(true);
    radio_service();
    adjust(1);
    zassert_false(model.command_pending());
    radio_ptt(false);
    tick(10000);
    tick(10001);
    RadioConfig config;
    Selection restored;
    zassert_ok(settings_start(config, restored));
    zassert_equal(config.lna_inverted, true);
    zassert_true(same_selection(selection, restored));
}

ZTEST(ui_menu, test_ctcss_level_is_global_saved_and_locked_during_tx) {
    load(true);
    const auto selection = radio_snapshot().selection;
    find("FM CTCSS level");
    adjust(1);
    tick();
    zassert_equal(radio_snapshot().config.fm_ctcss_level, 75);
    zassert_true(same_selection(selection, radio_snapshot().selection));
    zassert_true(settings_status().pending);
    render("fm-ctcss-level");
    radio_ptt(true);
    radio_service();
    adjust(1);
    zassert_false(model.command_pending());
    radio_ptt(false);
    tick(10000);
    tick(10001);
    RadioConfig config;
    Selection restored;
    zassert_ok(settings_start(config, restored));
    zassert_equal(config.fm_ctcss_level, 75);
    zassert_true(same_selection(selection, restored));
}

ZTEST(ui_menu, test_fm_rx_menu_controls_are_bounded_temporary_and_preserve_memory) {
    load(true);
    const auto initial = radio_snapshot();
    const auto saved = settings_status();
    find("FM weak BW");
    UiListPage page;
    model.menu_page(page);
    zassert_equal(strcmp(page.rows[page.cursor % 4].name, "FM weak BW: 3.40k"), 0);
    zassert_equal(strcmp(page.detail, "FM RX test / until reboot"), 0);
    adjust(1);
    tick();
    zassert_equal(radio_snapshot().fm_rx_controls.weak_filter, 1);
    model.menu_page(page);
    zassert_equal(strcmp(page.rows[page.cursor % 4].name, "FM weak BW: 4.00k"), 0);
    render("fm-rx-controls");
    for (unsigned i = 0; i < 10; ++i) {
        adjust(1);
        tick();
    }
    zassert_equal(radio_snapshot().fm_rx_controls.weak_filter, 7);
    for (unsigned i = 0; i < 10; ++i) {
        adjust(-1);
        tick();
    }
    zassert_equal(radio_snapshot().fm_rx_controls.weak_filter, 0);
    find("FM AF DAC");
    adjust(1);
    tick();
    zassert_equal(radio_snapshot().fm_rx_controls.af_dac_gain, 2);
    radio_ptt(true);
    radio_service();
    adjust(1); // Producer PTT guards even a stale UI snapshot.
    zassert_false(model.command_pending());
    tick();
    zassert_equal(radio_snapshot().fm_rx_controls.af_dac_gain, 2);
    radio_ptt(false);
    tick();
    zassert_true(same_selection(radio_snapshot().selection, initial.selection));
    zassert_true(same_operating(radio_snapshot().config, initial.config));
    zassert_equal(settings_status().revision, saved.revision);
    zassert_false(settings_status().pending);
    zassert_ok(radio_start(initial.config, initial.selection));
    zassert_equal(radio_snapshot().fm_rx_controls.af_dac_gain, 1);
}

ZTEST(ui_menu, test_radio_controls_helpers_pending_and_apply) {
    find("VFO Settings");
    key(UiKey::Enter);
    key(UiKey::Right);
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    key(UiKey::Up);
    key(UiKey::Enter);
    zassert_equal(radio_snapshot().config.power_mw, 1000);
    key(UiKey::Right);
    key(UiKey::Right);
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    tick();
    zassert_equal(radio_snapshot().config.power_mw, 2500);
    zassert_equal(model.screen(), UiScreen::Menu);
    find("Appearance");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Appearance);
    key(UiKey::Back);
    find("TX limit:");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::TransmitLimit);
    key(UiKey::Back);
    find("Local callsign");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Callsign);
    key(UiKey::Back);
    find("Backlight");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Backlight);
    key(UiKey::Back);
}

#ifdef CONFIG_HT_CODEC2
ZTEST(ui_menu, test_codec_statistics_page_reset_and_navigation) {
    load();
    m17::voice_statistics_reset();
    status(5);
    UiStatus page;
    model.status(page);
    zassert_equal(strcmp(page.title, "CODEC2"), 0);
    zassert_equal(strcmp(page.rows[0], "Enc avg/max --/-- ms"), 0);
    zassert_not_null(strstr(page.detail, "heap0"));
    render("codec2-empty");
    {
        m17::VoiceCodec codec;
        zassert_ok(codec.open());
        m17::Speech silence;
        m17::Payload payload;
        zassert_ok(codec.encode(silence, payload));
        zassert_ok(codec.decode(payload, silence));
    }
    model.status(page);
    zassert_equal(strcmp(page.rows[2], "Frames E2 D2"), 0);
    render("codec2-live");
    UiPresentation view;
    ui_capture_presentation(model, view);
    zassert_equal(strcmp(view.actions[0], "OK Reset"), 0);
    const auto revision = settings_status().revision;
    key(UiKey::Enter);
    model.status(page);
    zassert_equal(strcmp(page.rows[2], "Frames E0 D0"), 0);
    zassert_equal(settings_status().revision, revision);
    key(UiKey::Right);
    model.status(page);
    zassert_equal(strcmp(page.title, "RX PATH"), 0);
    zassert_false(model.codec_statistics_page());
    key(UiKey::Left);
    zassert_true(model.codec_statistics_page());
}
#endif

ZTEST(ui_menu, test_status_exact_config_modes_activity_and_no_mutation) {
    load(true);
    const auto state = radio_snapshot();
    const auto stored = settings_status();
    status();
    UiStatus view;
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "RX 145.500001 MHz"), 0);
    zassert_equal(strcmp(view.rows[1], "TX 144.900001 MHz"), 0);
    zassert_not_null(strstr(view.rows[2], "Requested power"));
    render("status-radio");
    key(UiKey::Right);
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "25 kHz / SQL 4"), 0);
    render("status-fm");
    for (unsigned i = 0; i < 50; ++i) {
        key(UiKey::Down);
        key(UiKey::Up);
        key(UiKey::Enter);
        key(UiKey::Right);
    }
    zassert_equal(radio_snapshot().configuration_revision, state.configuration_revision);
    zassert_true(same_selection(radio_snapshot().selection, state.selection));
    zassert_equal(settings_status().revision, stored.revision);
    zassert_false(settings_status().pending);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Menu);
    UiListPage page;
    model.menu_page(page);
    zassert_equal(strcmp(page.rows[page.cursor % 4].name, "Status"), 0);
    load(true, Mode::M17);
    status(1);
    auto digital = radio_snapshot();
    digital.config.m17.can = 15;
    digital.config.m17.rx_can_check = true;
    digital.config.m17.destination = Destination::Station;
    strcpy(digital.config.m17.callsign, "OE1DEST");
    model.sync(digital);
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "Destination OE1DEST"), 0);
    zassert_equal(strcmp(view.rows[1], "CAN 15 / RX filter on"), 0);
    render("status-m17");
    key(UiKey::Right);
    digital.rx_active = true;
    strcpy(digital.received_callsign, "OE3VOICE");
    digital.rssi_dbm = -93;
    model.sync(digital);
    model.status(view);
    zassert_equal(strcmp(view.rows[2], "From OE3VOICE"), 0);
    render("status-activity");
    digital.rx_active = false;
    model.sync(digital);
    model.status(view);
    zassert_equal(strcmp(view.rows[2], "From -"), 0);
}

ZTEST(ui_menu, test_rx_register_pages_show_samples_without_commands_or_stale_tx_values) {
    const bool codec = IS_ENABLED(CONFIG_HT_CODEC2);
    status(codec ? 6 : 5);
    UiStatus view;
    model.status(view);
    zassert_equal(strcmp(view.detail, "RX registers unavailable"), 0);
    auto state = radio_snapshot();
    state.rx_registers.valid = true;
    state.rx_registers.sample_ms = k_uptime_get();
    for (unsigned i = 0; i < sizeof(bk4819_rx_addresses); ++i) {
        state.rx_registers.values[i] = 0xa000 + bk4819_rx_addresses[i];
    }
    model.sync(state);
    model.status(view);
    zassert_equal(strcmp(view.title, "RX PATH"), 0);
    zassert_false(model.codec_statistics_page());
    UiPresentation presentation;
    ui_capture_presentation(model, presentation);
    zassert_not_equal(strcmp(presentation.actions[0], "OK Reset"), 0);
    zassert_equal(strcmp(view.rows[0], "30:A030  33:A033"), 0);
    render("rx-path");
    key(UiKey::Right);
    model.status(view);
    zassert_equal(strcmp(view.title, "RX GAIN"), 0);
    zassert_equal(strcmp(view.rows[3], "7B:A07B  7E:A07E"), 0);
    render("rx-gain");
    key(UiKey::Right);
    model.status(view);
    zassert_equal(strcmp(view.title, "RX SQUELCH"), 0);
    zassert_equal(strcmp(view.rows[3], "4F:A04F  78:A078"), 0);
    render("rx-squelch");
    key(UiKey::Enter);
    zassert_equal(radio_snapshot().command_id, state.command_id);
    state.phase = RadioPhase::Transmitting;
    model.sync(state);
    model.status(view);
    zassert_equal(strcmp(view.detail, "Available while receiving"), 0);
    zassert_equal(view.rows[0][0], 0);
    state.phase = RadioPhase::Receiving;
    state.rx_registers.sample_ms = k_uptime_get() - 1500;
    model.sync(state);
    model.status(view);
    zassert_equal(view.color, UiStatusColor::Amber);
    key(UiKey::Right);
    model.status(view);
    zassert_equal(strcmp(view.title, "CODEC2"), 0);
}

ZTEST(ui_menu, test_battery_page_fresh_charger_stale_error_age_recovery) {
    status(3);
    emulator_inject_battery({7234, true, true}, 0);
    zassert_ok(battery_sample());
    UiStatus view;
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "7234 mV"), 0);
    zassert_equal(strcmp(view.rows[1], "Charger present"), 0);
    zassert_equal(strcmp(view.rows[2], "Switch on"), 0);
    render("battery-fresh");
    const auto tuned = emulator_tuned_frequency();
    emulator_inject_battery({0, false, false}, -EIO);
    zassert_equal(battery_sample(), -EIO);
    model.status(view);
    zassert_not_null(strstr(view.detail, "Stale / read error"));
    zassert_equal(strcmp(view.rows[0], "Last 7234 mV"), 0);
    zassert_equal(strcmp(view.rows[2], "Last Switch on"), 0);
    zassert_equal(view.color, UiStatusColor::Red);
    render("battery-error");
    zassert_equal(emulator_tuned_frequency(), tuned);
    zassert_true(radio_snapshot().power_active);
    emulator_inject_battery({7351, false, true}, 0);
    zassert_ok(battery_sample());
    k_sleep(K_MSEC(301));
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "Last 7351 mV"), 0);
    zassert_equal(view.color, UiStatusColor::Amber);
    render("battery-stale");
    zassert_ok(battery_sample());
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "7351 mV"), 0);
}

ZTEST(ui_menu, test_storage_dirty_error_clean_and_read_only_reporting) {
    RadioCommand changed;
    changed.config = radio_snapshot().config;
    changed.config.power_mw = 2500;
    zassert_ok(radio_submit(changed));
    tick();
    status(4);
    UiStatus view;
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "Storage pending / dirty"), 0);
    render("storage-pending");
    fail_durable = true;
    settings_service(radio_snapshot(), 10000);
    model.status(view);
    zassert_not_null(strstr(view.detail, "Save error"));
    zassert_equal(view.color, UiStatusColor::Red);
    zassert_equal(strcmp(view.rows[0], "Storage pending / dirty"), 0);
    render("storage-error");
    fail_durable = false;
    settings_service(radio_snapshot(), 11000);
    model.status(view);
    zassert_equal(strcmp(view.rows[0], "Storage clean"), 0);
    render("storage-clean");
    // A damaged existing profile is retained read-only; Status is still useful.
    FILE *bad = fopen(path, "wb");
    zassert_not_null(bad);
    zassert_equal(fwrite("bad", 1, 3, bad), 3);
    zassert_ok(fclose(bad));
    RadioConfig config;
    Selection selection;
    zassert_not_equal(settings_start(config, selection), 0);
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
    status(4);
    model.status(view);
    zassert_true(settings_status().read_only);
    zassert_not_null(strstr(view.detail, "error"));
    zassert_equal(view.color, UiStatusColor::Red);
    zassert_equal(strcmp(view.rows[0], "Read-only / retained data"), 0);
    render("storage-read-only");
    FILE *retained = fopen(path, "rb");
    zassert_not_null(retained);
    zassert_equal(fgetc(retained), 'b');
    zassert_ok(fclose(retained));
}

ZTEST(ui_menu, test_menu_status_ptt_focus_warning_timeout_fault_priority) {
    status();
    radio_ptt(true);
    tick();
    render("status-tx");
    UiPresentation presentation;
    ui_capture_presentation(model, presentation);
    zassert_true(presentation.home.visible && presentation.home.transmitting);
    key(UiKey::Right);
    radio_ptt(false);
    tick();
    zassert_equal(model.screen(), UiScreen::Status);
    UiStatus view;
    auto state = radio_snapshot();
    state.phase = RadioPhase::Transmitting;
    state.tx_warning = true;
    state.tx_remaining_s = 3;
    model.sync(state);
    render("status-warning");
    key(UiKey::Left);
    model.status(view);
    zassert_not_null(strstr(view.title, "RADIO"));
    state.phase = RadioPhase::Receiving;
    state.tx_warning = false;
    state.tx_timed_out = true;
    model.sync(state);
    render("status-timeout");
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Status);
    state.phase = RadioPhase::Fault;
    state.fault = -EPIPE;
    model.sync(state);
    render("status-fault");
    auto *screen = lv_disp_get_scr_act(display);
    zassert_false(lv_obj_has_flag(lv_obj_get_child(screen, -1), LV_OBJ_FLAG_HIDDEN));
    state.power_active = false;
    model.sync(state);
    render("status-inactive");
    zassert_false(lv_obj_has_flag(lv_obj_get_child(screen, -1), LV_OBJ_FLAG_HIDDEN));
}

ZTEST(ui_menu, test_hidden_tx_actions_cannot_apply_after_release) {
    find("VFO Settings");
    const auto original = radio_snapshot();
    const auto stored = settings_status();
    // Check both a physical press before controller service and settled TX.
    for (unsigned settled = 0; settled < 2; ++settled) {
        radio_ptt(true);
        if (settled) {
            tick();
        }
        const char *actions[4];
        model.menu_actions(actions);
        zassert_equal(strcmp(actions[0], ""), 0);
        key(UiKey::Left);
        key(UiKey::Right);
        key(UiKey::Enter);
        zassert_false(model.command_pending());
        zassert_equal(model.screen(), UiScreen::Menu);
        radio_ptt(false);
        tick();
        zassert_equal(radio_snapshot().config.power_mw, original.config.power_mw);
        zassert_equal(radio_snapshot().configuration_revision, original.configuration_revision);
        zassert_equal(settings_status().revision, stored.revision);
        zassert_false(settings_status().pending);
    }
}

ZTEST(ui_menu, test_selection_animation_not_restarted_motion_off_and_style_restore) {
    key(UiKey::Enter);
    ui_view_update(model);
    key(UiKey::Down);
    ui_view_update(model);
    zassert_true(lv_anim_count_running() > 0);
    lv_tick_inc(70);
    lv_timer_handler();
    ui_view_update(model);
    lv_tick_inc(75);
    lv_timer_handler();
    zassert_equal(lv_anim_count_running(), 0);
    render("menu-selected");
    key(UiKey::Back);
    ui_view_update(model);
    label("430.000001");
    auto state = radio_snapshot();
    state.tx_warning = true;
    state.phase = RadioPhase::Transmitting;
    state.tx_remaining_s = 7;
    model.sync(state);
    ui_view_update(model);
    zassert_equal(lv_anim_count_running(), 0);
    load();
    plug.global.ui.animations = false;
    uint32_t generation = settings_status().generation;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
    key(UiKey::Enter);
    ui_view_update(model);
    key(UiKey::Down);
    ui_view_update(model);
    zassert_equal(lv_anim_count_running(), 0);
    find("Appearance");
    key(UiKey::Enter);
    ui_view_update(model);
    label("Theme: Midnight");
    key(UiKey::Back);
    status();
    render("status-after-appearance");
    zassert_equal(lv_obj_get_style_text_font(label("RX 430.000001 MHz"), 0),
                  &lv_font_montserrat_10);
}

ZTEST(ui_menu, test_all_theme_contrast_status_screens_bounded) {
    for (unsigned theme = 0; theme < ThemeCount; ++theme) {
        for (unsigned contrast = 0; contrast < 3; ++contrast) {
            load(false, Mode::Fm, static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            key(UiKey::Enter);
            char name[40];
            snprintf(name, sizeof(name), "menu-theme-%u-%u", theme, contrast);
            render(name);
            status();
            snprintf(name, sizeof(name), "status-theme-%u-%u", theme, contrast);
            render(name);
            const auto colors =
                ui_palette(static_cast<Theme>(theme), static_cast<Contrast>(contrast));
            zassert_equal(lv_obj_get_style_bg_color(lv_disp_get_scr_act(display), 0).full,
                          lv_color_hex(colors.background).full);
            zassert_equal(lv_obj_get_style_text_color(label("RX 430.000001 MHz"), 0).full,
                          lv_color_hex(colors.white).full);
        }
    }
}

ZTEST(ui_menu, test_modem_menu_settings_back_and_ptt_exit) {
    find("Modem / KISS");
    key(UiKey::Enter);
    UiListPage page;
    model.menu_page(page);
    zassert_equal(strcmp(page.title, "MODEM / AFSK1200"), 0);
    zassert_not_null(strstr(page.rows[0].name, "off"));
    key(UiKey::Enter);
    tick();
    zassert_true(modem_enabled());
    key(UiKey::Down);
    key(UiKey::Down); // Skip the read-only modem state row.
    const auto delay = modem_status().settings.txdelay;
    adjust(1);
    tick();
    zassert_equal(modem_status().settings.txdelay, delay + 1);
    zassert_true(modem_enabled());
    key(UiKey::Back);
    model.menu_page(page);
    zassert_equal(strcmp(page.title, "MENU"), 0);
    zassert_equal(strcmp(page.rows[page.cursor % 4].name, "Modem / KISS"), 0);
    key(UiKey::Enter);
    radio_ptt(true);
    tick();
    zassert_false(modem_enabled());
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    tick();
}

ZTEST(ui_menu, test_modem_status_updates_without_navigation) {
    find("Modem / KISS");
    key(UiKey::Enter);
    auto status = [&](const char *expected) {
        model.sync(radio_snapshot());
        UiListPage page;
        model.menu_page(page);
        zassert_equal(strcmp(page.rows[1].name, expected), 0, "%s", page.rows[1].name);
        zassert_true(strlen(page.rows[1].name) < 28);
        ui_view_update(model);
        label(expected);
    };
    status("Off");
    key(UiKey::Enter);
    tick();
    status("Ready");
    auto settings = modem_status().settings;
    settings.slot_time = 255;
    settings.persistence = 255;
    modem_settings(settings);
    modem::KissMessage packet;
    packet.frame.size = 1;
    packet.frame.bytes[0] = 42;
    modem_host(packet, modem_epoch());
    BackendStatus busy;
    busy.rx_active = true;
    emulator_inject(busy);
    tick();
    status("Waiting: channel busy");
    busy.rx_active = false;
    emulator_inject(busy);
    tick();
    status("Queued: waiting slot");
    k_sleep(K_MSEC(2560));
    tick();
    status("Transmitting");
    modem_disable();
    tick();
    status("Off");
    auto config = radio_snapshot().config;
    config.tx_inhibit = true;
    zassert_ok(radio_start(config));
    model.sync(radio_snapshot());
    find("Modem / KISS");
    key(UiKey::Enter);
    key(UiKey::Enter);
    tick();
    status("TX inhibited");
}

static void finish(void *) {
    unlink(path);
    rmdir(root);
    printk("Menu/status heap: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST(ui_menu, test_menu_has_no_companion_mode_and_ptt_remains_available) {
    key(UiKey::Enter);
    UiListPage page;
    for (unsigned i = 0; i < 30; ++i) {
        model.menu_page(page);
        zassert_is_null(strstr(page.rows[page.cursor % 4].name, "Companion"));
        key(UiKey::Down);
    }
    key(UiKey::Back);
    radio_ptt(true);
    tick();
    zassert_equal(radio_snapshot().phase, RadioPhase::Transmitting);
    radio_ptt(false);
    tick();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
}

ZTEST_SUITE(ui_menu, nullptr, setup, before, nullptr, finish);
