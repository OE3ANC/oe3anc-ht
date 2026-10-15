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
static char root[] = "/tmp/ht-ui-banks-XXXXXX", path[160];
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
static bool fail_durable;
enum class ReadHook { None, Member, Add };
static ReadHook read_hook;

static void publish(ReadHook hook) {
    if (read_hook != hook) {
        return;
    }
    read_hook = ReadHook::None;
    settings_service(radio_snapshot(), 1);
    zassert_false(settings_status().operation_pending);
}

extern "C" int __real__ZN2ht16settings_channelEjRNS_7ChannelEPj(uint32_t, Channel &, uint32_t *);

extern "C" int __wrap__ZN2ht16settings_channelEjRNS_7ChannelEPj(uint32_t id, Channel &channel,
                                                                uint32_t *revision) {
    const int result = __real__ZN2ht16settings_channelEjRNS_7ChannelEPj(id, channel, revision);
    publish(ReadHook::Member);
    return result;
}

extern "C" int __real__ZN2ht19settings_channel_atEjtRNS_7ChannelEPj(uint32_t, uint16_t, Channel &,
                                                                    uint32_t *);

extern "C" int __wrap__ZN2ht19settings_channel_atEjtRNS_7ChannelEPj(uint32_t bank, uint16_t index,
                                                                    Channel &channel,
                                                                    uint32_t *revision) {
    const int result =
        __real__ZN2ht19settings_channel_atEjtRNS_7ChannelEPj(bank, index, channel, revision);
    publish(ReadHook::Add);
    return result;
}

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
    read_hook = ReadHook::None;
    radio_ptt(false);
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
    const char *directory = getenv("HT_UI_BANK_FRAMES");
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

static void row(unsigned count) {
    while (count--) {
        key(UiKey::Down);
    }
}

static void banks() {
    menu("Banks");
    zassert_equal(model.screen(), UiScreen::Banks);
}

static void edit() {
    banks();
    row(2); // All, New, Local.
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BrowseActions);
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankEditor);
}

static void name(const char *value) {
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankName);
    while (model.text_editor().length()) {
        key(UiKey::Star);
    }
    while (*value) {
        model.input({UiKey::Character, *value++});
    }
    key(UiKey::Enter);
}

static void save() {
    while (model.form_cursor() != 2) {
        key(UiKey::Down);
    }
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    tick();
}

ZTEST(ui_banks, test_new_name_save_once_and_reboot) {
    banks();
    key(UiKey::Down);
    key(UiKey::Enter);
    name("My bank");
    frame_out("bank-new");
    save();
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_equal(settings_status().bank_count, 3);
    RadioConfig config;
    zassert_ok(settings_start(config));
    zassert_ok(settings_bank(3, bank));
    zassert_equal(strcmp(bank.name, "My bank"), 0);
}

ZTEST(ui_banks, test_unchanged_name_does_not_open_unsaved_dialog) {
    edit();
    key(UiKey::Enter);
    key(UiKey::Enter);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Banks);
}

ZTEST(ui_banks, test_cancelled_delete_then_save_changes_never_deletes_bank) {
    edit();
    name("Renamed");
    row(3);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankDelete);
    key(UiKey::Back);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Discard);
    key(UiKey::Down); // Continue -> Save.
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(strcmp(bank.name, "Renamed"), 0);
    zassert_equal(settings_status().bank_count, 2);
}

ZTEST(ui_banks, test_membership_remove_and_add_commit_once) {
    edit();
    row(1);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankMembers);
    const auto removed = model.list_page().rows[0].id;
    key(UiKey::Enter);
    row(2);
    key(UiKey::Enter);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.count, 3);
    key(UiKey::Up); // Wrap to Add channels.
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankAdd);
    frame_out("bank-add");
    for (unsigned i = 0;
         i < 6 && model.list_page().rows[model.list_page().cursor % 4].id != removed; ++i) {
        key(UiKey::Down);
    }
    key(UiKey::Enter);
    key(UiKey::Back);
    key(UiKey::Back);
    save();
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.count, 3);
    zassert_equal(bank.channel_ids[2], removed);
}

ZTEST(ui_banks, test_delete_confirmation_retains_channels) {
    edit();
    row(3);
    key(UiKey::Enter);
    frame_out("bank-delete");
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_equal(settings_bank(1, bank), -ENOENT);
    zassert_equal(settings_status().channel_count, 6);
}

ZTEST(ui_banks, test_durable_error_retains_draft_and_retry_reuses_bank) {
    edit();
    name("Retry");
    fail_durable = true;
    save();
    zassert_equal(model.screen(), UiScreen::BankEditor);
    zassert_equal(model.error(), -EIO);
    fail_durable = false;
    save();
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(strcmp(bank.name, "Retry"), 0);
    zassert_equal(settings_status().bank_count, 2);
}

ZTEST(ui_banks, test_ptt_preserves_name_and_discard_leaves_saved_bank) {
    edit();
    name("Draft");
    radio_ptt(true);
    tick();
    zassert_equal(model.screen(), UiScreen::BankEditor);
    radio_ptt(false);
    tick();
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Discard);
    key(UiKey::Up);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(strcmp(bank.name, "Local"), 0);
}

ZTEST(ui_banks, test_full_bank_capacity_and_page_buttons) {
    load(256);
    banks();
    key(UiKey::Right);
    zassert_equal(model.list_page().cursor, 4);
    key(UiKey::Left);
    zassert_equal(model.list_page().cursor, 0);
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_equal(model.error(), -ENOSPC);
    zassert_equal(model.screen(), UiScreen::Banks);
}

ZTEST(ui_banks, test_failed_deletion_retries_or_leaves_without_closing_another_editor) {
    for (unsigned leave = 0; leave < 2; ++leave) {
        load();
        edit();
        row(3);
        key(UiKey::Enter);
        fail_durable = true;
        key(UiKey::Enter);
        tick();
        zassert_equal(model.screen(), UiScreen::BankSaved);
        zassert_equal(model.error(), -EIO);
        zassert_true(model.command_pending());
        char lines[8][32];
        model.lines(lines);
        zassert_equal(strcmp(lines[0], "DELETION NOT SAVED"), 0);
        if (leave) {
            key(UiKey::Back);
            zassert_false(model.command_pending());
            key(UiKey::Back);
            key(UiKey::Back);
            edit();
            zassert_equal(model.screen(), UiScreen::BankEditor);
        }
        fail_durable = false;
        settings_service(radio_snapshot(), 2000);
        model.sync(radio_snapshot());
        zassert_false(settings_status().pending);
        zassert_equal(model.error(), 0);
        zassert_equal(model.screen(), leave ? UiScreen::BankEditor : UiScreen::Banks);
    }
}

ZTEST_SUITE(ui_banks, nullptr, setup, before, nullptr, nullptr);
