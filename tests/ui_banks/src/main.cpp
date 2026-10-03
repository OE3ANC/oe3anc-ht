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

static uint32_t channel_id(unsigned number) {
    Channel c;
    zassert_ok(settings_channel_number(number, c));
    return c.id;
}

static void banks() {
    menu("Banks");
    zassert_equal(model.screen(), UiScreen::Banks);
    while (model.list_page().rows[model.list_page().cursor % 4].id) {
        key(UiKey::Up);
    }
}

static void edit(unsigned index = 1) {
    banks();
    row(index);
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::BankEditor);
}

static void members() {
    row(1);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankMembers);
}

static void name(const char *value) {
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankName);
    while (model.text_editor().length()) {
        key(UiKey::Left);
    }
    while (*value) {
        model.input({UiKey::Character, *value++});
    }
    key(UiKey::Enter);
}

static void save() {
    key(UiKey::Right);
    zassert_true(model.command_pending());
    tick();
    zassert_ok(model.error());
    zassert_equal(model.screen(), UiScreen::BankSaved);
}

static void restart() {
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
}

ZTEST(ui_banks, test_new_physical_name_draft_cancel_save_and_reboot) {
    banks();
    frame_out("banks");
    key(UiKey::Left);
    frame_out("bank-new");
    key(UiKey::Enter);
    while (model.text_editor().length()) {
        key(UiKey::Left);
    }
    const char digits[] = {'8', '3', '7', '8'};
    const unsigned taps[] = {1, 2, 4, 1};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned t = 0; t < taps[i]; ++t) {
            model.input({UiKey::Digit, digits[i]});
        }
        key(UiKey::Hash);
    }
    zassert_equal(strcmp(model.text_editor().text(), "TEST"), 0);
    frame_out("bank-name");
    key(UiKey::Enter);
    zassert_equal(settings_status().bank_count, 2);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_equal(settings_status().bank_count, 2);
    key(UiKey::Left);
    name("TEST");
    key(UiKey::Right);
    frame_out("bank-pending");
    tick();
    zassert_equal(model.screen(), UiScreen::BankSaved);
    frame_out("bank-saved");
    zassert_ok(settings_bank(3, bank));
    zassert_equal(strcmp(bank.name, "TEST"), 0);
    zassert_equal(bank.count, 0);
    restart();
    zassert_ok(settings_bank(3, bank));
    zassert_equal(strcmp(bank.name, "TEST"), 0);
}

ZTEST(ui_banks, test_name_field_cancel_and_empty_name_preserve_draft) {
    edit();
    key(UiKey::Enter);
    while (model.text_editor().length()) {
        key(UiKey::Left);
    }
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankName);
    zassert_equal(model.error(), -EINVAL);
    key(UiKey::Back);
    save();
    zassert_ok(settings_bank(1, bank));
    zassert_equal(strcmp(bank.name, "Local"), 0);
    zassert_equal(bank.count, 3);
}

ZTEST(ui_banks, test_membership_add_order_remove_shared_ids_only_commit_on_save) {
    const auto frequency = radio_snapshot().config.rx_frequency_hz;
    const auto configuration_revision = radio_snapshot().configuration_revision;
    edit();
    members();
    frame_out("bank-members");
    zassert_equal(selected_number(), 4);
    key(UiKey::Enter);
    frame_out("bank-actions-top");
    key(UiKey::Enter); // unavailable move up
    zassert_equal(model.screen(), UiScreen::BankActions);
    row(1);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankMembers);
    zassert_equal(model.list_page().cursor, 1);
    zassert_equal(selected_number(), 4);
    key(UiKey::Enter);
    row(2);
    frame_out("bank-actions-remove");
    key(UiKey::Enter);
    zassert_equal(selected_number(), 3); // [1,3], removed 4
    key(UiKey::Left);
    frame_out("bank-add");
    zassert_equal(selected_number(), 1);
    key(UiKey::Enter);
    zassert_equal(model.error(), -EEXIST); // already IN, no duplicate
    key(UiKey::Down);
    key(UiKey::Enter); // add 2
    row(2);
    key(UiKey::Enter); // add 4, stays in picker
    frame_out("bank-add-selected");
    key(UiKey::Right);
    zassert_equal(selected_number(), 4);
    key(UiKey::Enter);
    key(UiKey::Enter); // move 4 up from tail
    zassert_equal(model.list_page().cursor, 2);
    zassert_equal(selected_number(), 4);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.count, 3);
    zassert_equal(bank.channel_ids[0], channel_id(4));
    zassert_equal(radio_snapshot().configuration_revision, configuration_revision);
    key(UiKey::Right);
    save();
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.count, 4);
    const unsigned expected[] = {1, 3, 4, 2};
    for (unsigned i = 0; i < 4; ++i) {
        zassert_equal(bank.channel_ids[i], channel_id(expected[i]));
    }
    zassert_equal(radio_snapshot().config.rx_frequency_hz, frequency);
    zassert_equal(emulator_tuned_frequency(), frequency);
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Right); // edit empty bank 2
    members();
    frame_out("bank-empty");
    key(UiKey::Left);
    key(UiKey::Enter);
    key(UiKey::Right);
    key(UiKey::Right);
    save();
    zassert_ok(settings_bank(2, bank));
    zassert_equal(bank.count, 1);
    zassert_equal(bank.channel_ids[0], channel_id(1));
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.channel_ids[0], channel_id(1)); // shared reference
}

ZTEST(ui_banks, test_delete_original_saved_name_confirm_cancel_and_channels_retained) {
    const auto selected = channel_id(4);
    zassert_ok(settings_recall({Operating::Memory, 1, selected}, 800, radio_snapshot()));
    tick();
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config = radio_snapshot().config;
    quick.config.squelch = 7;
    quick.id = 801;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    tick();
    radio_monitor(true);
    tick();
    zassert_true(radio_snapshot().monitor_active);
    const auto frequency = emulator_tuned_frequency();
    edit();
    name("Renamed draft");
    row(3);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankDelete);
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[2], "Local"), 0);
    frame_out("bank-delete");
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::BankEditor);
    zassert_equal(settings_status().bank_count, 2);
    key(UiKey::Enter);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::BankSaved);
    frame_out("bank-deleted");
    zassert_equal(settings_bank(1, bank), -ENOENT);
    zassert_equal(settings_status().bank_count, 1);
    zassert_equal(settings_status().channel_count, 6);
    zassert_equal(channel_id(4), selected);
    zassert_equal(radio_snapshot().selection.bank_id, 0);
    zassert_equal(radio_snapshot().selection.channel_id, selected);
    zassert_equal(emulator_tuned_frequency(), frequency);
    zassert_equal(radio_snapshot().config.squelch, 7);
    zassert_true(radio_snapshot().monitor_active);
    radio_monitor(false);
    tick();
    restart();
    zassert_equal(settings_bank(1, bank), -ENOENT);
    zassert_equal(settings_status().channel_count, 6);
}

ZTEST(ui_banks, test_active_bank_member_remove_preserves_radio_and_falls_back_to_all) {
    zassert_ok(settings_recall({Operating::Memory, 1, channel_id(4)}, 900, radio_snapshot()));
    tick();
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config = radio_snapshot().config;
    quick.config.squelch = 7;
    quick.id = 901;
    quick.expected_generation = radio_snapshot().generation;
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_submit(quick));
    tick();
    const auto frequency = radio_snapshot().config.rx_frequency_hz;
    edit();
    members();
    key(UiKey::Enter);
    row(2);
    key(UiKey::Enter);
    key(UiKey::Right);
    save();
    zassert_equal(radio_snapshot().selection.bank_id, 0);
    zassert_equal(radio_snapshot().selection.channel_id, channel_id(4));
    zassert_equal(radio_snapshot().config.rx_frequency_hz, frequency);
    zassert_equal(radio_snapshot().config.squelch, 7);
    Channel channel;
    zassert_ok(settings_channel(channel_id(4), channel));
    zassert_equal(channel.configuration.squelch, 4);
}

ZTEST(ui_banks, test_capacity_full_bank_page_order_delete_and_stable_id_recreate) {
    load(256);
    banks();
    key(UiKey::Left);
    zassert_equal(model.screen(), UiScreen::Banks);
    zassert_equal(model.error(), -ENOSPC);
    frame_out("banks-full");
    row(16);
    key(UiKey::Right);
    members();
    key(UiKey::Up);
    zassert_equal(model.list_page().count, 256);
    zassert_equal(model.list_page().cursor, 255);
    frame_out("bank-full-last");
    const auto last_id = model.list_page().rows[3].id;
    key(UiKey::Enter);
    row(1);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankActions);
    key(UiKey::Back);
    key(UiKey::Left);
    key(UiKey::Enter);
    zassert_equal(model.error(), -EEXIST);
    key(UiKey::Right);
    key(UiKey::Enter);
    key(UiKey::Enter); // move last up one
    zassert_equal(model.list_page().cursor, 254);
    zassert_equal(model.list_page().rows[2].id, last_id);
    key(UiKey::Right);
    save();
    zassert_ok(settings_bank(16, bank));
    zassert_equal(bank.count, 256);
    zassert_equal(bank.channel_ids[254], last_id);
    key(UiKey::Enter);
    key(UiKey::Right);
    row(3);
    key(UiKey::Enter);
    key(UiKey::Enter);
    tick();
    zassert_equal(settings_status().bank_count, 15);
    key(UiKey::Enter);
    key(UiKey::Left);
    name("Replacement");
    save();
    zassert_ok(settings_bank(17, bank));
    zassert_equal(settings_bank(16, bank), -ENOENT);
    zassert_equal(settings_status().channel_count, 256);
}

ZTEST(ui_banks, test_empty_store_add_and_all_view_not_editable) {
    load(0);
    banks();
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::Banks);
    row(1);
    key(UiKey::Right);
    members();
    key(UiKey::Left);
    frame_out("bank-add-empty");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::BankAdd);
    zassert_equal(model.list_page().count, 0);
    key(UiKey::Right);
    key(UiKey::Right);
    save();
    zassert_ok(settings_bank(1, bank));
    zassert_equal(bank.count, 0);
}

ZTEST(ui_banks, test_stale_and_ptt_power_fault_cancellation) {
    edit();
    RadioCommand command;
    command.config = radio_snapshot().config;
    command.config.squelch = 9;
    command.id = 700;
    zassert_ok(radio_submit(command));
    tick();
    key(UiKey::Right);
    tick();
    zassert_equal(model.error(), -ESTALE);
    zassert_false(model.command_pending());
    key(UiKey::Back);
    key(UiKey::Right);
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    edit();
    model.input({UiKey::Release});
    zassert_equal(model.screen(), UiScreen::Home);
    edit();
    RadioState off = radio_snapshot();
    off.power_active = false;
    model.sync(off);
    zassert_equal(model.screen(), UiScreen::Home);
    model.sync(radio_snapshot());
    edit();
    radio_report_fault(-EIO);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(settings_status().bank_count, 2);
}

ZTEST(ui_banks, test_authorized_save_survives_ptt_without_reopening_saved_screen) {
    banks();
    key(UiKey::Left);
    name("ACCEPTED");
    key(UiKey::Right);
    settings_service(radio_snapshot(), 0);
    radio_service();
    radio_ptt(true);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    zassert_ok(settings_bank(3, bank));
    zassert_equal(strcmp(bank.name, "ACCEPTED"), 0);
    radio_ptt(false);
    tick();
}

ZTEST(ui_banks, test_durable_error_retained_and_retry_not_new_identity) {
    edit();
    name("UNSAVED");
    fail_durable = true;
    save();
    frame_out("bank-unsaved");
    zassert_true(settings_status().pending);
    zassert_equal(settings_status().save_error, -EIO);
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[1], "Storage error / unsaved"), 0);
    zassert_ok(settings_bank(1, bank));
    zassert_equal(strcmp(bank.name, "UNSAVED"), 0);
    fail_durable = false;
    settings_service(radio_snapshot(), 1100);
    model.sync(radio_snapshot());
    zassert_false(settings_status().pending);
    model.lines(lines);
    zassert_equal(strcmp(lines[1], "Durably saved"), 0);
    restart();
    zassert_ok(settings_bank(1, bank));
    zassert_equal(strcmp(bank.name, "UNSAVED"), 0);
}

ZTEST(ui_banks, test_fixed_widgets_repeated_navigation_and_other_scenes_restore) {
    for (unsigned i = 0; i < 100; ++i) {
        edit();
        members();
        key(UiKey::Left);
        key(UiKey::Down);
        frame_out("bank-stress");
        key(UiKey::Back);
        key(UiKey::Back);
        key(UiKey::Back);
        key(UiKey::Back);
        key(UiKey::Back);
        key(UiKey::Back);
        zassert_equal(model.screen(), UiScreen::Home);
    }
    menu("Appearance");
    frame_out("bank-appearance");
    key(UiKey::Back);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    frame_out("bank-channel-restore");
    key(UiKey::Back);
    printf("Bank LVGL: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST(ui_banks, test_owner_publication_mid_page_keeps_cursor_and_rows_paired) {
    const ReadHook hooks[] = {ReadHook::Member, ReadHook::Add};
    for (ReadHook hook : hooks) {
        load();
        edit();
        members();
        if (hook == ReadHook::Add) {
            key(UiKey::Left);
        }
        const auto page = model.list_page();
        Channel edited;
        uint32_t revision;
        zassert_ok(settings_channel(channel_id(1), edited, &revision));
        strcpy(edited.name, "EXTERNAL EDIT");
        zassert_ok(settings_put_channel(edited, 999, radio_snapshot(), revision));
        settings_service(radio_snapshot(), 0);
        radio_service();
        zassert_true(settings_status().operation_pending);
        read_hook = hook;
        key(UiKey::Down);
        zassert_equal(read_hook, ReadHook::None);
        zassert_false(model.list_page().ready);
        zassert_equal(model.error(), -ESTALE);
        zassert_equal(model.list_page().cursor, page.cursor);
        for (unsigned i = 0; i < 4; ++i) {
            zassert_equal(model.list_page().rows[i].id, page.rows[i].id);
        }
        key(UiKey::Enter);
        zassert_equal(model.error(), -ESTALE);
        if (hook == ReadHook::Add) {
            key(UiKey::Back);
        }
        key(UiKey::Right);
        key(UiKey::Right);
        tick(); // Owner rejects stale bank revision.
        zassert_equal(model.error(), -ESTALE);
        zassert_false(model.command_pending());
        zassert_ok(settings_bank(1, bank));
        zassert_equal(bank.count, 3);
    }
}

ZTEST_SUITE(ui_banks, nullptr, setup, before, nullptr, nullptr);
