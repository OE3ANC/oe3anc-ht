// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include <ht/backend.hpp>
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
static char root[] = "/tmp/ht-ui-quick-XXXXXX", path[160];
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
static bool fail_durable;
static bool enable_gain;
extern "C" const RadioCapabilities &__real__ZN2ht20backend_capabilitiesEv();

extern "C" const RadioCapabilities &__wrap__ZN2ht20backend_capabilitiesEv() {
    static RadioCapabilities capabilities;
    capabilities = __real__ZN2ht20backend_capabilitiesEv();
    capabilities.gain = enable_gain;
    return capabilities;
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
    enable_gain = false;
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
    const char *directory = getenv("HT_UI_QUICK_FRAMES");
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
    key(UiKey::Monitor);
    zassert_equal(model.screen(), UiScreen::QuickControls);
}

static void sql() {
    open();
    key(UiKey::Down);
    key(UiKey::Enter);
}

ZTEST(ui_quick, test_explicit_squelch_draft_accept_and_cancel) {
    const auto initial = radio_snapshot().config.squelch;
    sql();
    key(UiKey::Up);
    key(UiKey::Left);
    key(UiKey::Right);
    zassert_equal(radio_snapshot().config.squelch, initial);
    frame_out("squelch-edit");
    key(UiKey::Back);
    key(UiKey::Enter);
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(radio_snapshot().config.squelch, initial + 1);
}

ZTEST(ui_quick, test_memory_squelch_temporary_and_recall_restores) {
    Channel channel;
    zassert_ok(settings_channel_number(1, channel));
    auto state = radio_snapshot();
    zassert_ok(settings_recall({Operating::Memory, 0, channel.id}, 800, state));
    tick();
    sql();
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    zassert_equal(radio_snapshot().config.squelch, channel.configuration.squelch + 1);
    state = radio_snapshot();
    zassert_ok(settings_recall(state.selection, 801, state));
    tick();
    zassert_equal(radio_snapshot().config.squelch, channel.configuration.squelch);
}

ZTEST(ui_quick, test_monitor_listen_are_mutually_exclusive_toggles) {
    open();
    key(UiKey::Down);
    key(UiKey::Down);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().receive_override, ReceiveOverride::Monitor);
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().receive_override, ReceiveOverride::ListenTx);
    open();
    key(UiKey::Up);
    frame_out("listen-toggle");
    key(UiKey::Enter);
    tick();
    zassert_equal(radio_snapshot().receive_override, ReceiveOverride::Normal);
}

ZTEST(ui_quick, test_stale_and_busy_drafts_cannot_apply) {
    sql();
    key(UiKey::Up);
    radio_ptt(true);
    tick();
    key(UiKey::Enter);
    zassert_false(model.command_pending());
    zassert_equal(model.screen(), UiScreen::QuickControls);
    radio_ptt(false);
    tick();
    RadioCommand change;
    change.config = radio_snapshot().config;
    change.config.rx_frequency_hz += 1000;
    zassert_ok(radio_submit(change));
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(radio_snapshot().config.squelch, 4);
}

ZTEST(ui_quick, test_m17_has_editor_and_listen_only) {
    RadioCommand command;
    command.config = radio_snapshot().config;
    command.config.mode = Mode::M17;
    zassert_ok(radio_submit(command));
    tick();
    open();
    key(UiKey::Down);
    zassert_equal(model.form_cursor(), 3);
    key(UiKey::Down);
    zassert_equal(model.form_cursor(), 0);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
    key(UiKey::Monitor);
    zassert_equal(model.screen(), UiScreen::ChannelEditor);
}

ZTEST_SUITE(ui_quick, nullptr, setup, before, nullptr, nullptr);
