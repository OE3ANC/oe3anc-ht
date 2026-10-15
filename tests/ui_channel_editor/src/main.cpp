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
static char root[] = "/tmp/ht-ui-editor-XXXXXX", path[160];
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
    const char *directory = getenv("HT_UI_EDITOR_FRAMES");
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

static void select_row(const char *label) {
    for (unsigned i = 0; i < 32; ++i) {
        char lines[8][32];
        model.lines(lines);
        for (unsigned row = 2; row < 6; ++row) {
            if (lines[row][0] == '>' && !strncmp(lines[row] + 2, label, strlen(label)) &&
                (lines[row][2 + strlen(label)] == 0 || lines[row][2 + strlen(label)] == ':')) {
                return;
            }
        }
        key(UiKey::Down);
    }
    zassert_unreachable("Missing field %s", label);
}

static void current() {
    key(UiKey::Monitor);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
}

static void new_channel() {
    current();
    select_row("Save as channel");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
}

static void field(const char *label, const char *value) {
    select_row(label);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelField);
    if (model.text_editor().kind() != TextKind::Frequency) {
        while (model.text_editor().length()) {
            key(UiKey::Star);
        }
    }
    for (; *value; ++value) {
        model.input({UiKey::Character, *value});
    }
    key(UiKey::Enter);
}

static void adjust(const char *label, unsigned steps = 1) {
    select_row(label);
    key(UiKey::Enter);
    for (unsigned i = 0; i < steps; ++i) {
        key(UiKey::Up);
    }
    key(UiKey::Enter);
}

static void save() {
    select_row("Save");
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    tick();
}

static uint32_t channel_id(unsigned number) {
    Channel c;
    zassert_ok(settings_channel_number(number, c));
    return c.id;
}

static void recall(unsigned number = 1) {
    const auto state = radio_snapshot();
    zassert_ok(settings_recall({Operating::Memory, 0, channel_id(number)}, 900, state));
    tick();
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
}

ZTEST(ui_channel_editor, test_save_as_channel_once_with_name_number_bank_and_reboot) {
    const auto config = radio_snapshot().config;
    new_channel();
    frame_out("new-channel");
    field("Name", "VFO A");
    field("Number", "007");
    select_row("Bank");
    key(UiKey::Enter);
    key(UiKey::Down);
    key(UiKey::Enter);
    save();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(settings_status().pending);
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    zassert_equal(strcmp(saved.name, "VFO A"), 0);
    zassert_equal(saved.configuration.rx_frequency_hz, config.rx_frequency_hz);
    zassert_ok(settings_bank(1, bank));
    zassert_true(bank_contains(bank, saved.id));
    RadioConfig reboot;
    Selection selection;
    zassert_ok(settings_start(reboot, selection));
    zassert_ok(settings_channel(saved.id, saved));
    zassert_equal(saved.number, 7);
}

ZTEST(ui_channel_editor, test_split_frequency_validation_and_field_cancel) {
    new_channel();
    field("RX frequency", "145.600001");
    field("TX frequency", "300.000000");
    zassert_equal(model.error(), -EINVAL);
    zassert_equal(model.screen(), UiScreen::ChannelField);
    key(UiKey::Back);
    field("TX frequency", "144.900001");
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001);
    frame_out("channel-frequency");
    save();
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    zassert_equal(saved.configuration.rx_frequency_hz, 145600001);
    zassert_equal(saved.configuration.tx_frequency_hz, 144900001);
}

ZTEST(ui_channel_editor, test_independent_tones_and_explicit_radio_fields) {
    new_channel();
    adjust("TX inhibit");
    adjust("Power");
    adjust("Bandwidth");
    adjust("Squelch");
    select_row("RX tone");
    key(UiKey::Enter);
    adjust("Type");
    field("Value", "088.5");
    select_row("Use tone");
    key(UiKey::Enter);
    select_row("TX tone");
    key(UiKey::Enter);
    adjust("Type", 2);
    field("Value", "008");
    zassert_equal(model.error(), -EINVAL);
    while (model.text_editor().length()) {
        key(UiKey::Star);
    }
    for (char c : "754") {
        if (c) {
            model.input({UiKey::Digit, c});
        }
    }
    key(UiKey::Enter);
    adjust("Polarity");
    select_row("Use tone");
    key(UiKey::Enter);
    frame_out("fm-profile");
    save();
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    zassert_true(saved.configuration.tx_inhibit);
    zassert_equal(saved.configuration.power_mw, 2500);
    zassert_equal(saved.configuration.bandwidth, Bandwidth::Narrow);
    zassert_equal(saved.configuration.squelch, 5);
    zassert_true(same_tone(saved.configuration.rx_tone, {ToneKind::Ctcss, 885, false}));
    zassert_true(same_tone(saved.configuration.tx_tone, {ToneKind::Dcs, 0754, true}));
}

ZTEST(ui_channel_editor, test_m17_destination_can_and_filter) {
    new_channel();
    adjust("Mode");
    adjust("Destination");
    field("Station", "OE3XYZ");
    adjust("CAN");
    adjust("RX CAN check");
    frame_out("m17-profile");
    save();
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    zassert_equal(saved.configuration.mode, Mode::M17);
    zassert_equal(saved.configuration.m17.destination, Destination::Station);
    zassert_equal(strcmp(saved.configuration.m17.callsign, "OE3XYZ"), 0);
    zassert_equal(saved.configuration.m17.can, 1);
    zassert_true(saved.configuration.m17.rx_can_check);
}

ZTEST(ui_channel_editor, test_replacement_is_the_only_save_confirmation) {
    const auto existing = channel_id(2);
    new_channel();
    field("Number", "2");
    select_row("Save");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelConfirm);
    zassert_false(model.command_pending());
    frame_out("channel-replace");
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    key(UiKey::Enter);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(settings_status().channel_count, 6);
    zassert_equal(channel_id(2), existing);
}

ZTEST(ui_channel_editor, test_duplicate_and_delete_preserve_other_channels) {
    recall();
    const auto original = radio_snapshot().selection.channel_id;
    current();
    select_row("Duplicate");
    key(UiKey::Enter);
    save();
    const auto duplicate = channel_id(7);
    zassert_not_equal(duplicate, original);
    current();
    select_row("Delete channel");
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelConfirm);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(settings_status().channel_count, 6);
    Channel saved;
    zassert_equal(settings_channel(original, saved), -ENOENT);
    zassert_ok(settings_channel(duplicate, saved));
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
}

ZTEST(ui_channel_editor, test_active_edit_publishes_after_rf_acceptance_and_preserves_vfo) {
    recall();
    const auto id = radio_snapshot().selection.channel_id;
    current();
    field("Number", "8");
    field("RX frequency", "145.510001");
    save();
    zassert_equal(channel_id(8), id);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145510001);
    OperatingConfig vfo;
    settings_vfo(vfo);
    zassert_equal(vfo.rx_frequency_hz, 145500001);
}

ZTEST(ui_channel_editor, test_failed_rf_apply_does_not_publish_channel) {
    recall();
    const auto id = radio_snapshot().selection.channel_id;
    Channel original;
    zassert_ok(settings_channel(id, original));
    current();
    field("RX frequency", "145.510001");
    select_row("Save");
    key(UiKey::Enter);
    emulator_fail_next(-EIO);
    tick();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    Channel unchanged;
    zassert_ok(settings_channel(id, unchanged));
    zassert_equal(unchanged.configuration.rx_frequency_hz, original.configuration.rx_frequency_hz);
}

ZTEST(ui_channel_editor, test_occupied_renumber_is_rejected_without_losing_draft) {
    recall();
    const auto original = radio_snapshot().selection.channel_id;
    const auto target = channel_id(2);
    current();
    field("Number", "2");
    select_row("Save");
    key(UiKey::Enter);
    zassert_equal(model.error(), -EEXIST);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    zassert_equal(channel_id(2), target);
    zassert_equal(radio_snapshot().selection.channel_id, original);
}

ZTEST(ui_channel_editor, test_durable_error_keeps_draft_and_retry_reuses_identity) {
    new_channel();
    fail_durable = true;
    save();
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    zassert_equal(model.error(), -EIO);
    const auto id = channel_id(7);
    frame_out("channel-save-error");
    fail_durable = false;
    save();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(channel_id(7), id);
    zassert_equal(settings_status().channel_count, 7);
    zassert_false(settings_status().pending);
}

ZTEST(ui_channel_editor, test_stale_draft_and_ptt_do_not_apply_unreviewed_changes) {
    new_channel();
    field("Name", "DRAFT");
    radio_ptt(true);
    tick();
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    radio_ptt(false);
    tick();
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Discard);
    key(UiKey::Up);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(settings_status().channel_count, 6);
    new_channel();
    RadioCommand command;
    command.config = radio_snapshot().config;
    command.config.rx_frequency_hz += 1000;
    zassert_ok(radio_submit(command));
    tick();
    select_row("Save");
    key(UiKey::Enter);
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
}

ZTEST(ui_channel_editor, test_failed_deletion_retries_or_leaves_without_closing_another_editor) {
    for (unsigned leave = 0; leave < 2; ++leave) {
        load();
        recall();
        current();
        select_row("Delete channel");
        key(UiKey::Enter);
        fail_durable = true;
        key(UiKey::Enter);
        tick();
        zassert_equal(model.screen(), UiScreen::ChannelSaved);
        zassert_equal(model.error(), -EIO);
        zassert_true(model.command_pending());
        char lines[8][32];
        model.lines(lines);
        zassert_equal(strcmp(lines[0], "DELETION NOT SAVED"), 0);
        if (leave) {
            key(UiKey::Back);
            zassert_false(model.command_pending());
            current();
            zassert_equal(model.screen(), UiScreen::ChannelEditor);
        }
        fail_durable = false;
        settings_service(radio_snapshot(), 2000);
        model.sync(radio_snapshot());
        zassert_false(settings_status().pending);
        zassert_equal(model.error(), 0);
        zassert_equal(model.screen(), leave ? UiScreen::ChannelEditor : UiScreen::Home);
    }
}

ZTEST_SUITE(ui_channel_editor, nullptr, setup, before, nullptr, nullptr);
