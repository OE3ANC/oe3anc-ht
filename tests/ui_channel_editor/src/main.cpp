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

static void field(const char *value, bool physical = false) {
    zassert_equal(model.screen(), UiScreen::ChannelField);
    while (model.text_editor().length()) {
        key(UiKey::Left);
    }
    for (; *value; ++value) {
        model.input({physical ? UiKey::Digit : UiKey::Character, *value});
    }
    key(UiKey::Enter);
}

static void page(unsigned number) {
    for (unsigned i = 0; i < number; ++i) {
        key(UiKey::Left);
    }
}

static void row(unsigned number) {
    for (unsigned i = 0; i < number; ++i) {
        key(UiKey::Down);
    }
}

static void save() {
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::ChannelReview);
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    tick();
    zassert_ok(model.error());
    zassert_equal(model.screen(), UiScreen::ChannelSaved);
}

static uint32_t channel_id(unsigned number) {
    Channel c;
    zassert_ok(settings_channel_number(number, c));
    return c.id;
}

ZTEST(ui_channel_editor, test_quick_save_physical_name_number_bank_review_and_persistence) {
    const auto config = radio_snapshot().config;
    const auto revision = radio_snapshot().configuration_revision;
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    frame_out("new-channel");
    key(UiKey::Enter);
    while (model.text_editor().length()) {
        key(UiKey::Left);
    }
    const char digits[] = {'8', '3', '6'};
    for (char digit : digits) {
        for (unsigned i = 0; i < 3; ++i) {
            model.input({UiKey::Digit, digit});
        }
        key(UiKey::Hash);
    }
    zassert_equal(strcmp(model.text_editor().text(), "VFO"), 0);
    frame_out("channel-name");
    key(UiKey::Right);
    key(UiKey::Hash); // Space via P2, commit without applying field.
    model.input({UiKey::Digit, '2'});
    key(UiKey::Enter); // A
    row(1);
    key(UiKey::Enter);
    field("007", true);
    page(1);
    row(3);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelBank);
    frame_out("channel-bank");
    key(UiKey::Down);
    key(UiKey::Enter);
    zassert_equal(settings_status().channel_count, 6);
    zassert_equal(radio_snapshot().configuration_revision, revision);
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::ChannelReview);
    frame_out("channel-review");
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    frame_out("channel-pending");
    zassert_equal(settings_status().channel_count, 6);
    tick();
    zassert_equal(model.screen(), UiScreen::ChannelSaved);
    zassert_equal(settings_status().channel_count, 7);
    zassert_false(settings_status().pending);
    frame_out("channel-saved");
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    zassert_equal(strcmp(saved.name, "VFO A"), 0);
    zassert_equal(saved.configuration.rx_frequency_hz, config.rx_frequency_hz);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, config.rx_frequency_hz);
    zassert_ok(settings_bank(1, bank));
    zassert_true(bank_contains(bank, saved.id));
    RadioConfig reboot;
    Selection selection;
    zassert_ok(settings_start(reboot, selection));
    zassert_ok(settings_channel(saved.id, saved));
    zassert_equal(saved.number, 7);
}

ZTEST(ui_channel_editor, test_explicit_frequency_fields_simplex_and_field_cancel_never_retune) {
    key(UiKey::Right);
    row(2);
    key(UiKey::Enter);
    frame_out("channel-frequency");
    field("145.600001");
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    row(1);
    key(UiKey::Enter);
    field("145.1234567");
    zassert_equal(model.screen(), UiScreen::ChannelField);
    zassert_equal(model.error(), -EINVAL);
    key(UiKey::Back);
    key(UiKey::Enter);
    zassert_equal(strcmp(model.text_editor().text(), "145.500001"), 0);
    field("300.000000");
    zassert_equal(model.screen(), UiScreen::ChannelField);
    zassert_equal(model.error(), -EINVAL);
    key(UiKey::Back);
    key(UiKey::Enter);
    zassert_equal(strcmp(model.text_editor().text(), "145.500001"), 0);
    frame_out("channel-tx-frequency");
    field("144.900001");
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001); // Fixture VFO, no RF edit.
    zassert_equal(radio_snapshot().config.tx_frequency_hz, 145500001);
    save();
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    zassert_equal(saved.configuration.rx_frequency_hz, 145600001);
    zassert_equal(saved.configuration.tx_frequency_hz, 144900001);
    key(UiKey::Enter);
    menu("Channels");
    while (selected_number() != 7) {
        key(UiKey::Down);
    }
    key(UiKey::Right);
    page(3);
    key(UiKey::Enter); // Simplex convenience.
    save();
    zassert_ok(settings_channel_number(7, saved));
    zassert_equal(saved.configuration.tx_frequency_hz, saved.configuration.rx_frequency_hz);
}

ZTEST(ui_channel_editor, test_actions_wrap_only_available_rows_and_match_helpers) {
    key(UiKey::Right);
    page(3);
    char lines[8][32];
    const char *actions[4];
    model.lines(lines);
    zassert_not_null(strstr(lines[2], "Use simplex"));
    for (unsigned i = 3; i < 6; ++i) {
        zassert_equal(lines[i][0], 0);
    }
    key(UiKey::Up);
    key(UiKey::Down);
    zassert_equal(model.form_cursor(), 0);
    model.channel_actions(actions);
    zassert_equal(strcmp(actions[0], "OK Simplex"), 0);
    key(UiKey::Back);
    key(UiKey::Left);
    tick();
    key(UiKey::Right);
    page(3);
    model.lines(lines);
    zassert_not_null(strstr(lines[3], "Duplicate"));
    zassert_not_null(strstr(lines[4], "Delete channel"));
    zassert_equal(lines[5][0], 0);
    key(UiKey::Up);
    zassert_equal(model.form_cursor(), 2);
    model.channel_actions(actions);
    zassert_equal(strcmp(actions[0], "OK Review"), 0);
    key(UiKey::Down);
    zassert_equal(model.form_cursor(), 0);
    key(UiKey::Down);
    zassert_equal(model.form_cursor(), 1);
    model.channel_actions(actions);
    zassert_equal(strcmp(actions[0], "OK Copy"), 0);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    page(3);
    key(UiKey::Up);
    zassert_equal(model.form_cursor(), 0); // Copy is a new draft.
}

ZTEST(ui_channel_editor, test_independent_fm_tones_dcs_octal_polarity_bandwidth_sql_power_inhibit) {
    key(UiKey::Right);
    page(1);
    row(1);
    key(UiKey::Enter); // RX only
    row(1);
    key(UiKey::Enter); // 2.5W
    frame_out("channel-radio");
    page(1);
    key(UiKey::Enter);
    row(1);
    key(UiKey::Enter); // narrow, SQL5
    row(1);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelTone);
    key(UiKey::Enter); // CTCSS
    row(1);
    key(UiKey::Enter);
    field("88.5");
    frame_out("rx-tone");
    key(UiKey::Right);
    row(1);
    key(UiKey::Enter);
    key(UiKey::Enter);
    key(UiKey::Enter); // TX DCS
    row(1);
    key(UiKey::Enter);
    field("008");
    zassert_equal(model.error(), -EINVAL);
    frame_out("invalid-dcs");
    field("754", true);
    row(1);
    key(UiKey::Enter);
    frame_out("tx-dcs");
    key(UiKey::Right);
    frame_out("fm-profile");
    save();
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    const auto &op = saved.configuration;
    zassert_true(op.tx_inhibit);
    zassert_equal(op.power_mw, 2500);
    zassert_equal(op.bandwidth, Bandwidth::Narrow);
    zassert_equal(op.squelch, 5);
    zassert_true(same_tone(op.rx_tone, {ToneKind::Ctcss, 885, false}));
    zassert_true(same_tone(op.tx_tone, {ToneKind::Dcs, 0754, true}));
}

ZTEST(ui_channel_editor, test_m17_station_can_filter_and_canonical_mode_changes) {
    key(UiKey::Right);
    page(1);
    key(UiKey::Enter);
    page(1); // M17 settings
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelField);
    field("ALL");
    zassert_equal(model.error(), -EINVAL);
    field("OE3XYZ");
    row(2);
    key(UiKey::Enter);
    row(1);
    key(UiKey::Enter);
    frame_out("m17-profile");
    row(2);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelField);
    frame_out("m17-destination");
    key(UiKey::Back);
    save();
    Channel saved;
    zassert_ok(settings_channel_number(7, saved));
    zassert_equal(saved.configuration.mode, Mode::M17);
    zassert_equal(saved.configuration.m17.destination, Destination::Station);
    zassert_equal(strcmp(saved.configuration.m17.callsign, "OE3XYZ"), 0);
    zassert_equal(saved.configuration.m17.can, 1);
    zassert_true(saved.configuration.m17.rx_can_check);
    zassert_equal(saved.configuration.rx_tone.kind, ToneKind::None);
}

ZTEST(ui_channel_editor,
      test_replacement_names_target_requires_second_confirmation_and_preserves_identity) {
    const auto existing = channel_id(2);
    const auto high = plug.channel_id_high_water;
    key(UiKey::Right);
    row(1);
    key(UiKey::Enter);
    field("2");
    key(UiKey::Right);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelConfirm);
    zassert_false(model.command_pending());
    char lines[8][32];
    model.lines(lines);
    zassert_not_null(strstr(lines[1], "002"));
    zassert_equal(strcmp(lines[2], "CHANNEL 002"), 0);
    frame_out("channel-replace");
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::ChannelReview);
    key(UiKey::Enter);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::ChannelSaved);
    zassert_equal(settings_status().channel_count, 6);
    Channel saved;
    zassert_ok(settings_channel(existing, saved));
    zassert_equal(strcmp(saved.name, "CHANNEL 007"), 0);
    zassert_equal(saved.number, 2);
    zassert_equal(saved.id, existing);
    zassert_equal(plug.channel_id_high_water, high); // Source fixture wasn't mutated.
}

ZTEST(ui_channel_editor, test_duplicate_new_id_and_delete_active_memory_falls_back_to_vfo) {
    key(UiKey::Left);
    tick();
    const auto original = radio_snapshot().selection.channel_id;
    key(UiKey::Right);
    page(3);
    row(1);
    frame_out("channel-actions");
    key(UiKey::Enter);
    save();
    const auto duplicate = channel_id(7);
    zassert_not_equal(duplicate, original);
    Channel source;
    zassert_ok(settings_channel(original, source));
    zassert_equal(settings_status().channel_count, 7);
    key(UiKey::Enter);
    key(UiKey::Right);
    page(3);
    row(2);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelConfirm);
    frame_out("channel-delete");
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    key(UiKey::Enter);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::ChannelSaved);
    zassert_equal(settings_status().channel_count, 6);
    zassert_equal(settings_channel(original, source), -ENOENT);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    zassert_ok(settings_channel(duplicate, source));
}

ZTEST(ui_channel_editor, test_edit_selected_memory_defers_rf_and_failure_preserves_saved_channel) {
    key(UiKey::Left);
    tick();
    const auto id = radio_snapshot().selection.channel_id;
    Channel original;
    zassert_ok(settings_channel(id, original));
    key(UiKey::Right);
    row(2);
    key(UiKey::Enter);
    field("145.510001");
    zassert_equal(radio_snapshot().config.rx_frequency_hz, original.configuration.rx_frequency_hz);
    key(UiKey::Right);
    key(UiKey::Enter);
    emulator_fail_next(-EIO);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    Channel unchanged;
    zassert_ok(settings_channel(id, unchanged));
    zassert_equal(unchanged.configuration.rx_frequency_hz, original.configuration.rx_frequency_hz);
    zassert_false(model.command_pending());
}

ZTEST(ui_channel_editor, test_edit_selected_memory_applies_profile_before_owner_publication) {
    key(UiKey::Left);
    tick();
    const auto id = radio_snapshot().selection.channel_id;
    key(UiKey::Right);
    row(1);
    key(UiKey::Enter);
    field("8");
    row(1);
    key(UiKey::Enter);
    field("145.510001");
    save();
    Channel changed;
    zassert_ok(settings_channel(id, changed));
    zassert_equal(changed.number, 8);
    zassert_equal(channel_id(8), id);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145510001);
    zassert_equal(changed.configuration.rx_frequency_hz, 145510001);
    zassert_equal(radio_snapshot().selection.channel_id, id);
    key(UiKey::Enter);
    key(UiKey::Left);
    tick();
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500001); // Preserved VFO.
}

ZTEST(ui_channel_editor, test_occupied_renumber_and_duplicate_never_overwrite_other_identity) {
    key(UiKey::Left);
    tick();
    const auto id = radio_snapshot().selection.channel_id;
    const auto target = channel_id(2);
    const auto frequency = radio_snapshot().config.rx_frequency_hz;
    key(UiKey::Right);
    row(1);
    key(UiKey::Enter);
    field("2");
    row(1);
    key(UiKey::Enter);
    field("145.600000");
    key(UiKey::Right);
    key(UiKey::Enter);
    zassert_equal(model.error(), -EEXIST);
    zassert_equal(model.screen(), UiScreen::ChannelReview);
    zassert_false(model.command_pending());
    zassert_equal(channel_id(1), id);
    zassert_equal(channel_id(2), target);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, frequency);
    key(UiKey::Back);
    key(UiKey::Back); // Cancel whole edit.
    key(UiKey::Right);
    page(3);
    row(1);
    key(UiKey::Enter); // Explicit Duplicate: free #7/new ID draft.
    row(1);
    key(UiKey::Enter);
    field("2");
    key(UiKey::Right);
    key(UiKey::Enter);
    zassert_equal(model.error(), -EEXIST);
    zassert_false(model.command_pending());
    zassert_equal(settings_status().channel_count, 6);
    zassert_equal(channel_id(2), target);
    key(UiKey::Back);
    key(UiKey::Enter);
    field("8");
    save();
    const auto copy = channel_id(8);
    zassert_not_equal(copy, id);
    zassert_not_equal(copy, target);
    zassert_equal(settings_status().channel_count, 7);
}

ZTEST(ui_channel_editor, test_full_store_replacement_and_duplicate_capacity_preserve_data) {
    load(256);
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    zassert_equal(model.error(), -ENOSPC);
    key(UiKey::Right);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelConfirm);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::ChannelSaved);
    zassert_equal(settings_status().channel_count, 256);
    key(UiKey::Enter);
    key(UiKey::Left);
    tick();
    key(UiKey::Right);
    page(3);
    row(1);
    key(UiKey::Enter);
    zassert_equal(model.error(), -ENOSPC);
    zassert_equal(settings_status().channel_count, 256);
}

ZTEST(ui_channel_editor, test_stale_drafts_busy_and_cancellation_including_coalesced_ptt) {
    key(UiKey::Right);
    RadioCommand c;
    c.config = radio_snapshot().config;
    c.config.gain = 0;
    c.config.squelch = 9;
    c.id = 555;
    zassert_ok(radio_submit(c));
    tick();
    key(UiKey::Right);
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Right);
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Right);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    key(UiKey::Right);
    key(UiKey::Enter);
    model.input({UiKey::Release});
    zassert_equal(model.screen(), UiScreen::Home);
    key(UiKey::Right);
    RadioState state = radio_snapshot();
    state.power_active = false;
    model.sync(state);
    zassert_equal(model.screen(), UiScreen::Home);
    model.sync(radio_snapshot());
    radio_ptt(true);
    tick();
    key(UiKey::Right);
    zassert_equal(model.error(), -EBUSY);
    radio_ptt(false);
    tick();
    key(UiKey::Right);
    radio_report_fault(-EIO);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    zassert_equal(settings_status().channel_count, 6);
}

ZTEST(ui_channel_editor, test_authorized_save_survives_ptt_without_reopening_saved_screen) {
    key(UiKey::Right);
    key(UiKey::Right);
    key(UiKey::Enter);
    settings_service(radio_snapshot(), 0);
    radio_service(); // RF authorization completed before PTT.
    radio_ptt(true);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    zassert_equal(settings_status().channel_count, 7);
    radio_ptt(false);
    tick();
}

ZTEST(ui_channel_editor, test_durable_error_reports_unsaved_then_recovers_without_recreating) {
    key(UiKey::Right);
    fail_durable = true;
    save();
    frame_out("channel-unsaved");
    zassert_true(settings_status().pending);
    zassert_equal(settings_status().save_error, -EIO);
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[1], "Storage error / unsaved"), 0);
    const auto id = channel_id(7);
    fail_durable = false;
    settings_service(radio_snapshot(), 1100);
    model.sync(radio_snapshot());
    zassert_false(settings_status().pending);
    zassert_equal(channel_id(7), id);
    model.lines(lines);
    zassert_equal(strcmp(lines[1], "Durably saved"), 0);
}

ZTEST(ui_channel_editor, test_fixed_widgets_repeated_programming_and_appearance_restore) {
    for (unsigned i = 0; i < 100; ++i) {
        key(UiKey::Right);
        page(i % 4);
        key(UiKey::Down);
        frame_out("editor-stress");
        key(UiKey::Back);
        zassert_equal(model.screen(), UiScreen::Home);
    }
    menu("Appearance");
    frame_out("editor-appearance");
    printf("Editor LVGL: min-free=%u min-largest=%u max-fragmentation=%u%%\n", minimum_free,
           minimum_largest, maximum_fragmentation);
}

ZTEST_SUITE(ui_channel_editor, nullptr, setup, before, nullptr, nullptr);
