// SPDX-License-Identifier: GPL-3.0-or-later
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
static Bank bank;
static lv_color_t buffer[160 * 16];
static uint16_t frame[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static char root[] = "/tmp/ht-ui-channels-XXXXXX", path[160];
enum class AckHook { None, Page, Position, Number };
static AckHook ack_hook;
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;

static void publish_ack(AckHook hook) {
    if (ack_hook != hook) {
        return;
    }
    ack_hook = AckHook::None;
    settings_service(radio_snapshot(), 1); // Complete an already-authorized real owner edit.
    zassert_false(settings_status().operation_pending);
}

extern "C" int __real__ZN2ht19settings_channel_atEjtRNS_7ChannelEPj(uint32_t, uint16_t, Channel &,
                                                                    uint32_t *);

extern "C" int __wrap__ZN2ht19settings_channel_atEjtRNS_7ChannelEPj(uint32_t bank, uint16_t index,
                                                                    Channel &channel,
                                                                    uint32_t *revision) {
    const int result =
        __real__ZN2ht19settings_channel_atEjtRNS_7ChannelEPj(bank, index, channel, revision);
    publish_ack(AckHook::Page);
    return result;
}

extern "C" int __real__ZN2ht25settings_channel_positionEjjRt(uint32_t, uint32_t, uint16_t &);

extern "C" int __wrap__ZN2ht25settings_channel_positionEjjRt(uint32_t bank, uint32_t id,
                                                             uint16_t &position) {
    const int result = __real__ZN2ht25settings_channel_positionEjjRt(bank, id, position);
    publish_ack(AckHook::Position);
    return result;
}

extern "C" int __real__ZN2ht23settings_channel_numberEtRNS_7ChannelEPj(uint16_t, Channel &,
                                                                       uint32_t *);

extern "C" int __wrap__ZN2ht23settings_channel_numberEtRNS_7ChannelEPj(uint16_t number,
                                                                       Channel &channel,
                                                                       uint32_t *revision) {
    const int result =
        __real__ZN2ht23settings_channel_numberEtRNS_7ChannelEPj(number, channel, revision);
    publish_ack(AckHook::Number);
    return result;
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
    ack_hook = AckHook::None;
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

static void type(const char *text) {
    for (; *text; ++text) {
        model.input({UiKey::Character, *text});
    }
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
    for (unsigned i = 0; i < 20 && !found; ++i) {
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

static uint16_t selected_number() {
    return strtoul(model.list_page().rows[model.list_page().cursor % 4].prefix, nullptr, 10);
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
    const char *directory = getenv("HT_UI_CHANNEL_FRAMES");
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

ZTEST(ui_channels, test_sorted_page_wrap_and_explicit_recall) {
    const auto vfo = radio_snapshot().config.rx_frequency_hz;
    menu("Channels");
    zassert_equal(model.screen(), UiScreen::Channels);
    zassert_equal(model.list_page().count, 6);
    zassert_equal(selected_number(), 1);
    frame_out("channels");
    for (unsigned i = 0; i < 4; ++i) {
        key(UiKey::Down);
    }
    zassert_equal(model.list_page().cursor, 4);
    zassert_equal(selected_number(), 5);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, vfo);
    frame_out("channels-page-two");
    key(UiKey::Down);
    key(UiKey::Down);
    zassert_equal(selected_number(), 1);
    key(UiKey::Up);
    zassert_equal(selected_number(), 6);
    key(UiKey::Enter);
    zassert_true(model.recall_pending());
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    frame_out("recall-pending");
    tick();
    zassert_false(model.recall_pending());
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    zassert_equal(radio_snapshot().selection.channel_id, find_channel_number(plug, 6)->id);
}

ZTEST(ui_channels, test_bank_preview_cancel_order_empty_and_recall) {
    menu("Channels");
    key(UiKey::Left);
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_equal(model.list_page().count, 3);
    frame_out("banks");
    key(UiKey::Down);
    key(UiKey::Back);
    zassert_equal(model.list_page().count, 6);
    key(UiKey::Left);
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_equal(model.list_page().count, 3);
    zassert_equal(selected_number(), 1);
    key(UiKey::Up);
    zassert_equal(selected_number(), 4);
    frame_out("channels-local");
    zassert_equal(radio_snapshot().selection.bank_id, 0); // Preview is local.
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().selection.bank_id, 1);
    key(UiKey::Down);
    tick();
    zassert_equal(radio_snapshot().selection.channel_id, find_channel_number(plug, 1)->id);
    key(UiKey::Up);
    tick();
    zassert_equal(radio_snapshot().selection.channel_id, find_channel_number(plug, 4)->id);
    menu("Channels");
    key(UiKey::Left);
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_equal(model.list_page().count, 0);
    frame_out("channels-empty-bank");
    key(UiKey::Enter);
    zassert_false(model.recall_pending());
    zassert_equal(radio_snapshot().selection.bank_id, 1);
}

ZTEST(ui_channels, test_vfo_memory_round_trip_numeric_selection_and_direct_tuning) {
    key(UiKey::Left);
    zassert_true(model.recall_pending());
    tick();
    const auto memory = radio_snapshot().selection.channel_id;
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    key(UiKey::Left);
    tick();
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001);
    key(UiKey::Left);
    tick();
    zassert_equal(radio_snapshot().selection.channel_id, memory);
    model.input({UiKey::Digit, '3'});
    zassert_equal(model.screen(), UiScreen::ChannelNumber);
    frame_out("channel-number");
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().config.mode, Mode::M17);
    key(UiKey::Hash);
    type("145.5");
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    Channel stored;
    zassert_ok(settings_channel(find_channel_number(plug, 3)->id, stored));
    zassert_equal(stored.configuration.rx_frequency_hz, 433037500);
    key(UiKey::Left);
    tick();
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 433037500);
}

ZTEST(ui_channels, test_numeric_validation_membership_cancel_and_ptt_interrupt) {
    menu("Channels");
    key(UiKey::Left);
    key(UiKey::Down);
    key(UiKey::Enter);
    type("2");
    key(UiKey::Enter);
    zassert_equal(model.error(), -ENOENT);
    zassert_false(model.recall_pending());
    frame_out("channel-not-in-bank");
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Channels);
    type("999");
    key(UiKey::Enter);
    zassert_equal(model.error(), -ERANGE);
    key(UiKey::Back);
    type("0");
    key(UiKey::Enter);
    zassert_equal(model.error(), -EINVAL);
    key(UiKey::Back);
    type("4");
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Channels);
    zassert_false(model.recall_pending());
    tick();
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
}

ZTEST(ui_channels, test_tx_stale_recall_and_fault_preserve_active_selection) {
    menu("Channels");
    radio_ptt(true);
    tick();
    zassert_true(emulator_transmitting());
    key(UiKey::Enter);
    zassert_equal(model.error(), -EBUSY);
    zassert_false(model.recall_pending());
    radio_ptt(false);
    tick();
    key(UiKey::Enter);
    zassert_true(model.recall_pending());
    RadioCommand configure;
    configure.config = radio_snapshot().config;
    configure.config.power_mw = 2500;
    zassert_ok(radio_submit(configure));
    tick();
    tick();
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(model.screen(), UiScreen::Channels);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    radio_report_fault(-EPIPE);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(emulator_transmitting());
}

ZTEST(ui_channels, test_maximum_capacity_pages_and_position_reads) {
    load(256);
    menu("Channels");
    key(UiKey::Up);
    zassert_equal(model.list_page().cursor, 255);
    zassert_equal(selected_number(), 256);
    zassert_equal(strcmp(model.list_page().rows[0].prefix, "253"), 0);
    frame_out("channels-last-page");
    uint16_t position = 999;
    zassert_ok(settings_channel_position(0, find_channel_number(plug, 256)->id, position));
    zassert_equal(position, 255);
    zassert_ok(settings_channel_position(1, find_channel_number(plug, 4)->id, position));
    zassert_equal(position, 0);
    zassert_equal(settings_channel_position(1, find_channel_number(plug, 2)->id, position),
                  -ENOENT);
    zassert_equal(position, 0);
    zassert_equal(settings_channel_position(999, 1, position), -ENOENT);
    zassert_equal(position, 0);
    key(UiKey::Left);
    key(UiKey::Up);
    zassert_equal(model.list_page().count, 17);
    zassert_equal(model.list_page().cursor, 16);
    zassert_equal(model.list_page().rows[0].id, 16);
    zassert_equal(strcmp(model.list_page().rows[0].suffix, "256"), 0);
    frame_out("banks-last-page");
}

ZTEST(ui_channels, test_empty_memory_and_interrupted_pending_recall) {
    load(0);
    key(UiKey::Left);
    zassert_equal(model.screen(), UiScreen::Channels);
    zassert_equal(model.list_page().count, 0);
    frame_out("channels-empty");
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Home);
    load();
    menu("Channels");
    key(UiKey::Enter);
    settings_service(radio_snapshot(), 0);
    radio_power(false);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
    zassert_false(model.recall_pending());
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(emulator_transmitting());
}

ZTEST(ui_channels, test_list_motion_finishes_and_scene_switch_restores_appearance) {
    menu("Channels");
    ui_view_update(model);
    lv_tick_inc(150);
    lv_timer_handler(); // Settle optional page entry before checking selection alone.
    key(UiKey::Down);
    ui_view_update(model);
    zassert_equal(lv_anim_count_running(), 1);
    lv_tick_inc(70);
    lv_timer_handler();
    ui_view_update(model); // Ordinary refresh must not restart it.
    lv_tick_inc(100);
    lv_timer_handler();
    zassert_equal(lv_anim_count_running(), 0);
    key(UiKey::Back);
    key(UiKey::Back);
    menu("Appearance");
    frame_out("appearance-after-list");
    auto *root = lv_obj_get_child(lv_disp_get_scr_act(display), -1);
    unsigned palette_names = 0;
    for (unsigned i = 0; i < lv_obj_get_child_cnt(root); ++i) {
        auto *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            for (unsigned t = 0; t < 4; ++t) {
                palette_names += !strcmp(lv_label_get_text(child),
                                         ui_palette(static_cast<Theme>(t), Contrast::Normal).name);
            }
        }
    }
    zassert_equal(palette_names, 4);
    key(UiKey::Back);
    key(UiKey::Back);
    menu("Channels");
    for (unsigned i = 0; i < 100; ++i) {
        key(UiKey::Down);
        frame_out("channels-stress");
    }
    printk("Channel LVGL: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

static void authorize_preferences(AckHook hook) {
    UiPreferences preferences;
    uint32_t revision;
    settings_ui_preferences(preferences, &revision);
    preferences.contrast = Contrast::High;
    zassert_ok(settings_put_ui_preferences(preferences, 801, radio_snapshot(), revision));
    settings_service(radio_snapshot(), 0);
    radio_service();
    zassert_true(settings_status().operation_pending);
    model.sync(radio_snapshot()); // RF revision is current; only RAM publication remains.
    ack_hook = hook;
}

ZTEST(ui_channels, test_page_publication_fence_preserves_cursor_and_blocks_mixed_recall) {
    menu("Channels");
    for (unsigned i = 0; i < 3; ++i) {
        key(UiKey::Down);
    }
    zassert_equal(selected_number(), 4);
    authorize_preferences(AckHook::Page);
    key(UiKey::Down);
    zassert_false(model.list_page().ready);
    zassert_equal(model.list_page().cursor, 3);
    zassert_equal(selected_number(), 4); // Old rows and cursor stay together.
    key(UiKey::Enter);
    zassert_false(model.recall_pending());
    zassert_equal(model.error(), -EAGAIN);
    zassert_true(model.list_page().ready);
    zassert_equal(selected_number(), 5);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().selection.channel_id, find_channel_number(plug, 5)->id);
}

ZTEST(ui_channels, test_reordered_bank_step_and_numeric_epoch_fences) {
    menu("Channels");
    key(UiKey::Left);
    key(UiKey::Down);
    key(UiKey::Enter);
    key(UiKey::Up);
    key(UiKey::Enter);
    tick(); // Active Local channel4, old order4/1/3.
    zassert_ok(settings_bank(1, bank));
    const auto swap = bank.channel_ids[0];
    bank.channel_ids[0] = bank.channel_ids[1];
    bank.channel_ids[1] = swap;
    zassert_ok(settings_put_bank(bank, 802, radio_snapshot(), settings_status().revision));
    settings_service(radio_snapshot(), 0);
    radio_service();
    model.sync(radio_snapshot());
    ack_hook = AckHook::Position;
    key(UiKey::Down);
    zassert_equal(model.error(), -ESTALE);
    zassert_false(model.recall_pending());
    zassert_equal(radio_snapshot().selection.channel_id, find_channel_number(plug, 4)->id);
    key(UiKey::Down);
    tick();
    zassert_equal(radio_snapshot().selection.channel_id, find_channel_number(plug, 3)->id);
    type("1");
    authorize_preferences(AckHook::Number);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(radio_snapshot().selection.channel_id, find_channel_number(plug, 3)->id);
    zassert_equal(model.screen(), UiScreen::ChannelNumber);
}

ZTEST_SUITE(ui_channels, nullptr, setup, before, nullptr, nullptr);
