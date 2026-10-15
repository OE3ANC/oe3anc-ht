// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/audio.hpp>
#include <ht/battery.hpp>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <ht/ui.hpp>
#include <ht/ui_presentation_wire.hpp>
#include <ht/wasm.hpp>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

using namespace ht;
static UiModel model;
static atomic_t calls;
static atomic_t quiet;
static atomic_t release_rpc;
static atomic_t returned;
static const char *scenario;

namespace ht {
bool audio_quiescent() {
    return atomic_get(&quiet);
}
AudioStatus audio_status() {
    return {};
}
} // namespace ht

extern "C" int ht_wasm_backend_run(unsigned program, uint32_t *observed) {
    atomic_inc(&calls);
    wasm_stage(WasmStage::Checking);
    while (!atomic_get(&release_rpc)) {
        k_sleep(K_MSEC(10));
    }
    *observed = !strcmp(scenario, "mismatch") ? 1 : (program + 1) * 11;
    atomic_set(&returned, 1);
    return !strcmp(scenario, "failure") ? -EIO : 0;
}

static void command(CommandKind kind, unsigned program = 0) {
    RadioCommand request;
    request.kind = kind;
    request.wasm_program = program;
    zassert_ok(radio_submit(request));
    radio_service();
}

static void press(UiKey key) {
    model.input({key});
    radio_service();
    model.sync(radio_snapshot());
}

static void select(const char *label) {
    for (unsigned i = 0; i < 32; ++i) {
        UiListPage page;
        model.menu_page(page);
        if (strstr(page.rows[page.cursor % 4].name, label)) {
            press(UiKey::Enter);
            return;
        }
        press(UiKey::Down);
    }
    zassert_unreachable("Missing %s", label);
}

ZTEST(wasm_loader, test_exclusive_loader) {
    scenario = getenv("HT_WASM_TEST_CASE");
    if (!scenario) {
        scenario = "pass";
    }
    char root[] = "/tmp/ht-wasm-loader-XXXXXX";
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "wasm", 1));
    zassert_ok(settings_storage_init());
    radio_power(true);
    zassert_ok(radio_start(RadioConfig{}));
    emulator_inject_battery({7200, false, true}, 0);
    zassert_ok(battery_sample());
    model.sync(radio_snapshot());
    command(CommandKind::WasmTest);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_false(wasm_radio_locked());
    press(UiKey::Enter);
    select("Development");
    select("WASM loader");
    zassert_equal(model.screen(), UiScreen::Diagnostics);
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    UiListPage page;
    model.diagnostic_page(page);
    zassert_equal(strcmp(page.title, "WASM LOADER"), 0);
    zassert_equal(page.count, 4);
    // Exiting without loading restores normal receive, then re-enter via the menu.
    press(UiKey::Back);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_equal(model.screen(), UiScreen::Menu);
    press(UiKey::Enter);
    zassert_equal(model.screen(), UiScreen::Diagnostics);
    command(CommandKind::WasmTest, 255);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    zassert_false(wasm_radio_locked());
    radio_ptt(true);
    command(CommandKind::WasmTest);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_false(emulator_transmitting());
    zassert_false(wasm_radio_locked());
    radio_ptt(false);
    radio_service();
    model.sync(radio_snapshot());
    // Select PCM gain through the actual menu and confirm the pending result.
    press(UiKey::Down);
    press(UiKey::Enter);
    zassert_true(wasm_radio_locked());
    k_sleep(K_MSEC(20));
    zassert_equal(wasm_status().stage, WasmStage::WaitingAudio);
    zassert_equal(atomic_get(&calls), 0);
    atomic_set(&quiet, 1);
    for (unsigned i = 0; i < 100 && !atomic_get(&calls); ++i) {
        k_sleep(K_MSEC(10));
    }
    zassert_equal(atomic_get(&calls), 1);
    zassert_equal(wasm_status().stage, WasmStage::Checking);
    command(CommandKind::ExitDiagnostics);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    command(CommandKind::WasmTest, 2);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    if (!strcmp(scenario, "timeout")) {
        k_sleep(K_MSEC(10100));
        zassert_equal(wasm_status().stage, WasmStage::TimedOut);
    }
    atomic_set(&release_rpc, 1);
    for (unsigned i = 0; i < 100 && !atomic_get(&returned); ++i) {
        k_sleep(K_MSEC(10));
    }
    k_sleep(K_MSEC(20));
    const auto status = wasm_status();
    zassert_equal(status.stage, !strcmp(scenario, "pass") ? WasmStage::Passed
                               : !strcmp(scenario, "timeout") ? WasmStage::TimedOut
                                                               : WasmStage::Failed);
    model.sync(radio_snapshot());
    model.diagnostic_page(page);
    zassert_not_null(strstr(page.detail, !strcmp(scenario, "pass") ? "PASS test 2: 22"
                                        : !strcmp(scenario, "timeout") ? "timeout" : "error"));
    static UiPresentation presentation, decoded;
    static uint8_t encoded[companion::UI_MAX_BYTES];
    size_t length = 0;
    ui_capture_presentation(model, presentation);
    zassert_ok(ui_encode_presentation(presentation, encoded, sizeof(encoded), length));
    zassert_ok(ui_decode_presentation(encoded, length, decoded));
    zassert_equal(strcmp(decoded.list.title, "WASM LOADER"), 0);
    zassert_equal(strcmp(decoded.list.detail, page.detail), 0);
    zassert_true(wasm_radio_locked());
    command(CommandKind::ExitDiagnostics);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    radio_power(false);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    radio_power(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_false(emulator_transmitting());
    zassert_equal(radio_start(RadioConfig{}), -EBUSY);
}

ZTEST_SUITE(wasm_loader, nullptr, nullptr, nullptr, nullptr, nullptr);
