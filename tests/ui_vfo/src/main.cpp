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
static char root[] = "/tmp/ht-ui-vfo-XXXXXX", path[160];
static unsigned minimum_free = 32768, minimum_largest = 32768, maximum_fragmentation;
static bool fail_durable;
static bool commit_after_step_read;
extern "C" uint32_t __real__ZN2ht17settings_vfo_stepEPj(uint32_t *revision);

extern "C" uint32_t __wrap__ZN2ht17settings_vfo_stepEPj(uint32_t *revision) {
    const auto step = __real__ZN2ht17settings_vfo_stepEPj(revision);
    if (commit_after_step_read) {
        commit_after_step_read = false;
        // Commit the real owner's authorized Apply between sync's value and status reads.
        settings_service(radio_snapshot(), 1);
    }
    return step;
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
    commit_after_step_read = false;
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
    const char *directory = getenv("HT_UI_VFO_FRAMES");
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
    menu("VFO step");
    zassert_equal(model.screen(), UiScreen::VfoStep);
}

static void reboot() {
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    model = {};
    model.sync(radio_snapshot());
}

ZTEST(ui_vfo, test_step_preview_cancel_apply_tuning_and_persistence) {
    const auto original = radio_snapshot().config.rx_frequency_hz;
    open();
    frame_out("vfo-step");
    key(UiKey::Down); // 10 kHz
    zassert_equal(settings_vfo_step(), 12500);
    zassert_equal(emulator_tuned_frequency(), original);
    key(UiKey::Back);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(settings_vfo_step(), 12500);
    key(UiKey::Enter);
    key(UiKey::Up); // 20 kHz
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[2], "20.000 kHz"), 0);
    frame_out("vfo-step-preview");
    key(UiKey::Enter);
    zassert_true(model.command_pending());
    frame_out("vfo-step-pending");
    tick();
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_equal(settings_vfo_step(), 20000);
    zassert_equal(emulator_tuned_frequency(), original);
    key(UiKey::Back);
    key(UiKey::Up);
    tick();
    zassert_equal(emulator_tuned_frequency(), original + 20000);
    zassert_equal(radio_snapshot().config.tx_frequency_hz, original + 20000);
    key(UiKey::Down);
    tick();
    zassert_equal(emulator_tuned_frequency(), original);
    settings_service(radio_snapshot(), 1100);
    reboot();
    zassert_equal(settings_vfo_step(), 20000);
    key(UiKey::Up);
    tick();
    zassert_equal(emulator_tuned_frequency(), original + 20000);
}

ZTEST(ui_vfo, test_every_step_exact_hertz_and_cycle_boundaries) {
    for (auto expected : vfo_steps_hz) {
        uint32_t revision;
        settings_vfo_step(&revision);
        zassert_ok(settings_put_vfo_step(expected, 700, radio_snapshot(), revision));
        tick();
        const auto original = emulator_tuned_frequency();
        key(UiKey::Up);
        tick();
        zassert_equal(emulator_tuned_frequency(), original + expected);
        key(UiKey::Down);
        tick();
        zassert_equal(emulator_tuned_frequency(), original);
    }
    open();
    key(UiKey::Up);
    char lines[8][32];
    model.lines(lines);
    zassert_equal(strcmp(lines[2], "1.000 kHz"), 0);
    key(UiKey::Down);
    model.lines(lines);
    zassert_equal(strcmp(lines[2], "100.000 kHz"), 0);
    frame_out("vfo-step-largest");
}

ZTEST(ui_vfo, test_tuning_reads_applied_step_after_owner_publication_between_sync_reads) {
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    settings_service(radio_snapshot(), 0);
    radio_service();
    commit_after_step_read = true;
    model.sync(radio_snapshot());
    zassert_false(commit_after_step_read);
    zassert_equal(settings_vfo_step(), 20000);
    zassert_equal(model.screen(), UiScreen::Menu);
    zassert_false(model.command_pending());
    char lines[8][32];
    model.lines(lines);
    bool applied_visible = false;
    for (const auto &line : lines) {
        applied_visible |= line[0] == '>' && strstr(line, "VFO step 20.000");
    }
    zassert_true(applied_visible);
    key(UiKey::Back);
    key(UiKey::Up);
    tick();
    zassert_equal(emulator_tuned_frequency(), 145520001);

    // A later owner publication must also take effect without another UI sync.
    uint32_t revision;
    settings_vfo_step(&revision);
    zassert_ok(settings_put_vfo_step(6250, 702, radio_snapshot(), revision));
    settings_service(radio_snapshot(), 0);
    radio_service();
    settings_service(radio_snapshot(), 1);
    key(UiKey::Down);
    tick();
    zassert_equal(emulator_tuned_frequency(), 145513751);
}

ZTEST(ui_vfo, test_split_offset_preserved_and_band_edges_leave_configuration_unchanged) {
    RadioCommand command;
    command.config = radio_snapshot().config;
    command.config.rx_frequency_hz = 145500001;
    command.config.tx_frequency_hz = 144900001;
    zassert_ok(radio_submit(command));
    tick();
    key(UiKey::Up);
    tick();
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145512501);
    zassert_equal(radio_snapshot().config.tx_frequency_hz, 144912501);
    command.config = radio_snapshot().config;
    command.config.tx_frequency_hz = 136000000;
    zassert_ok(radio_submit(command));
    tick();
    const auto before = radio_snapshot();
    key(UiKey::Down);
    zassert_not_equal(model.error(), 0);
    zassert_false(model.command_pending());
    zassert_true(same_operating(radio_snapshot().config, before.config));
    command.config.rx_frequency_hz = 174000000;
    command.config.tx_frequency_hz = 173400000;
    zassert_ok(radio_submit(command));
    tick();
    key(UiKey::Up);
    zassert_not_equal(model.error(), 0);
    zassert_equal(emulator_tuned_frequency(), 174000000);
}

ZTEST(ui_vfo, test_memory_rocker_remains_channel_selection_and_vfo_preserved) {
    key(UiKey::Left);
    tick();
    zassert_equal(radio_snapshot().selection.operating, Operating::Memory);
    const auto first = radio_snapshot().selection.channel_id;
    key(UiKey::Down);
    tick();
    zassert_not_equal(radio_snapshot().selection.channel_id, first);
    const auto memory = emulator_tuned_frequency();
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    tick();
    zassert_equal(settings_vfo_step(), 20000);
    zassert_equal(emulator_tuned_frequency(), memory);
    key(UiKey::Back);
    key(UiKey::Left);
    tick();
    zassert_equal(emulator_tuned_frequency(), 145500001);
    key(UiKey::Up);
    tick();
    zassert_equal(emulator_tuned_frequency(), 145520001);
}

ZTEST(ui_vfo, test_tx_stale_ptt_focus_power_fault_and_authorized_apply) {
    open();
    key(UiKey::Up);
    radio_ptt(true);
    radio_ptt(false);
    key(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    zassert_equal(settings_vfo_step(), 12500);
    open();
    model.input({UiKey::Release});
    zassert_equal(model.screen(), UiScreen::Home);
    open();
    uint32_t revision;
    auto step = settings_vfo_step(&revision);
    zassert_ok(settings_put_vfo_step(1000, 701, radio_snapshot(), revision));
    tick();
    key(UiKey::Enter);
    tick();
    zassert_equal(model.error(), -ESTALE);
    zassert_equal(settings_vfo_step(), 1000);
    key(UiKey::Back);
    key(UiKey::Back);
    open();
    key(UiKey::Up);
    key(UiKey::Enter);
    settings_service(radio_snapshot(), 0);
    radio_service();
    radio_ptt(true);
    radio_service();
    settings_service(radio_snapshot(), 1);
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    zassert_false(model.command_pending());
    zassert_equal(settings_vfo_step(), 2500);
    radio_ptt(false);
    tick();
    open();
    radio_power(false);
    radio_service();
    model.sync(radio_snapshot());
    zassert_equal(model.screen(), UiScreen::Home);
    radio_power(true);
    tick();
    open();
    radio_report_fault(-EIO);
    tick();
    zassert_equal(model.screen(), UiScreen::Home);
    (void)step;
}

ZTEST(ui_vfo, test_storage_failure_keeps_accepted_step_and_retries) {
    open();
    key(UiKey::Down);
    fail_durable = true;
    key(UiKey::Enter);
    tick();
    zassert_equal(settings_vfo_step(), 10000);
    zassert_true(settings_status().pending);
    zassert_equal(settings_status().save_error, -EIO);
    key(UiKey::Back);
    key(UiKey::Up);
    tick();
    zassert_equal(emulator_tuned_frequency(), 145510001);
    fail_durable = false;
    settings_service(radio_snapshot(), 1100);
    reboot();
    zassert_equal(settings_vfo_step(), 10000);
}

ZTEST_SUITE(ui_vfo, nullptr, setup, before, nullptr, nullptr);
