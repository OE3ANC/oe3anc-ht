// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/companion.h>
#include <ht/companion_keys.hpp>
#include <ht/ui.hpp>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
using namespace ht;
using namespace ht::companion;

// Keep the actual serial worker idle so tests can own the protocol session.
extern "C" int __wrap_ht_companion_read(uint8_t *, size_t) {
    return 0;
}

static UiModel model;
static Frame request, response;

static void before(void *) {
    radio_power(true);
    zassert_ok(radio_start(RadioConfig{}));
    zassert_ok(ht_companion_set_enabled(true));
    k_sleep(K_MSEC(10));
    ui_remote_session(42);
    ui_remote_clear();
    ui_keypad_star(false, k_uptime_get());
    ui_keypad_cancel_gesture();
    UiInput input;
    while (ui_take_input(input)) {
    }
    model = {};
    auto state = radio_snapshot();
    state.companion_mode = true;
    model.sync(state);
    request = {};
    request.session = 42;
}

static void press(unsigned key, bool down = true) {
    request.type = MSG_UI_KEY;
    request.size = 2;
    request.payload[0] = key;
    request.payload[1] = down;
    handle_keys(request, response);
    zassert_equal(response.size, 1);
    zassert_equal(response.payload[0], STATUS_OK);
}

static void drain() {
    UiInput input;
    while (ui_take_input(input)) {
        model.input(input);
    }
}

static int keep(uint32_t mask) {
    request.type = MSG_UI_KEYS_KEEP;
    request.size = 4;
    sys_put_le32(mask, request.payload);
    handle_keys(request, response);
    return response.payload[0];
}

ZTEST(companion_keys, test_shared_draft_order_and_local_cancel) {
    // A remote numeric tap and a local one share the frequency draft.
    press(UI_KEY_DIGIT_4);
    press(UI_KEY_DIGIT_4, false);
    zassert_ok(ui_queue_input({UiKey::Digit, '3'}));
    press(UI_KEY_DIGIT_0);
    press(UI_KEY_DIGIT_0, false);
    drain();
    zassert_equal(model.screen(), UiScreen::Frequency);
    zassert_equal(strcmp(model.text_editor().text(), "430"), 0);
    zassert_ok(ui_queue_input({UiKey::Back}));
    drain();
    zassert_equal(model.screen(), UiScreen::Home);
    model.input({UiKey::Digit, '4'});
    press(UI_KEY_BACK);
    drain();
    zassert_equal(model.screen(), UiScreen::Home);
}

ZTEST(companion_keys, test_queue_pressure_release_clear_and_session_scope) {
    press(UI_KEY_STAR);
    drain();
    zassert_true(ui_keypad_gesture().pressed);
    for (unsigned i = 0; i < 16; ++i) {
        zassert_ok(ui_queue_input({UiKey::Down}));
    }
    request.type = MSG_UI_KEY;
    request.size = 2;
    request.payload[0] = UI_KEY_UP;
    request.payload[1] = 1;
    handle_keys(request, response);
    zassert_equal(response.payload[0], STATUS_BUSY);
    press(UI_KEY_STAR, false);
    zassert_false(ui_keypad_gesture().pressed);
    ui_remote_clear();
    UiInput input;
    for (unsigned i = 0; i < 16; ++i) {
        zassert_true(ui_take_input(input));
        zassert_equal(input.remote_generation, 0);
    }
    press(UI_KEY_UP);
    zassert_ok(ui_queue_input({UiKey::Back}));
    ui_remote_session(43);
    zassert_true(ui_take_input(input));
    zassert_equal(input.key, UiKey::Back);
    zassert_false(ui_take_input(input));
    press(UI_KEY_UP);
    ui_remote_session(0);
    zassert_false(ui_take_input(input));
    zassert_equal(ui_remote_key({UiKey::Up}, UI_KEY_UP), -ESTALE);
}

ZTEST(companion_keys, test_no_repeat_and_expired_unapplied_keys) {
    press(UI_KEY_DIGIT_2);
    handle_keys(request, response);
    zassert_equal(response.payload[0], STATUS_INVALID);
    zassert_ok(ui_queue_input({UiKey::Back}));
    k_sleep(K_MSEC(UI_KEYS_LEASE_MS));
    zassert_equal(keep(1u << UI_KEY_DIGIT_2), STATUS_STALE);
    UiInput input;
    zassert_true(ui_take_input(input));
    zassert_equal(input.key, UiKey::Back);
    zassert_false(ui_take_input(input));
    press(UI_KEY_DIGIT_2);
    press(UI_KEY_DIGIT_2, false);
    drain();
    zassert_equal(strcmp(model.text_editor().text(), "2"), 0);
}

ZTEST(companion_keys, test_star_lease_hold_and_physical_level_independence) {
    press(UI_KEY_STAR);
    drain();
    model.advance(k_uptime_get());
    for (unsigned i = 0; i < 5; ++i) {
        k_sleep(K_MSEC(200));
        zassert_equal(keep(1u << UI_KEY_STAR), STATUS_OK);
        // C62's physical matrix scan must not overwrite the remote level.
        ui_keypad_star(false, k_uptime_get());
        model.advance(k_uptime_get());
    }
    zassert_true(model.keypad_locked());
    press(UI_KEY_STAR, false);
    press(UI_KEY_DIGIT_4);
    press(UI_KEY_DIGIT_4, false);
    drain();
    zassert_equal(model.screen(), UiScreen::Home); // Same lock blocks virtual navigation.
    // A stalled browser cannot turn another press into a hold.
    press(UI_KEY_STAR);
    drain();
    model.advance(k_uptime_get());
    k_sleep(K_MSEC(1100));
    model.advance(k_uptime_get());
    zassert_true(model.keypad_locked());
    zassert_false(ui_keypad_gesture().pressed);
    // A virtual release cannot release a genuinely held physical Star.
    ui_keypad_star(true, k_uptime_get());
    press(UI_KEY_STAR);
    press(UI_KEY_STAR, false);
    zassert_true(ui_keypad_gesture().pressed);
    k_sleep(K_SECONDS(1));
    model.advance(k_uptime_get());
    zassert_true(model.keypad_locked()); // Mixed origin gesture is cancelled.
    ui_keypad_star(false, k_uptime_get());
}

ZTEST(companion_keys, test_other_held_key_cancels_star_in_both_orders) {
    for (unsigned order = 0; order < 2; ++order) {
        ui_remote_clear();
        if (!order) {
            press(UI_KEY_UP);
            drain();
            model.advance(k_uptime_get());
            press(UI_KEY_STAR);
        } else {
            press(UI_KEY_STAR);
            drain();
            model.advance(k_uptime_get());
            press(UI_KEY_UP);
        }
        drain();
        model.advance(k_uptime_get());
        for (unsigned i = 0; i < 5; ++i) {
            k_sleep(K_MSEC(200));
            zassert_equal(keep((1u << UI_KEY_STAR) | (1u << UI_KEY_UP)), STATUS_OK);
            model.advance(k_uptime_get());
        }
        zassert_false(model.keypad_locked());
    }
}

ZTEST(companion_keys, test_remote_wake_only_and_mode_off_invalidates_queue) {
    model.advance(k_uptime_get() + 60001);
    zassert_true(model.dimmed());
    press(UI_KEY_DIGIT_4);
    press(UI_KEY_DIGIT_4, false);
    drain();
    zassert_false(model.dimmed());
    zassert_equal(model.screen(), UiScreen::Home);
    press(UI_KEY_DIGIT_4);
    press(UI_KEY_DIGIT_4, false);
    drain();
    zassert_equal(model.screen(), UiScreen::Frequency);
    press(UI_KEY_STAR);
    zassert_ok(ui_queue_input({UiKey::Back}));
    zassert_ok(ht_companion_set_enabled(false));
    UiInput input;
    zassert_true(ui_take_input(input));
    zassert_equal(input.remote_generation, 0);
    zassert_false(ui_take_input(input));
    zassert_false(ui_keypad_gesture().pressed);
    zassert_equal(ui_remote_key({UiKey::Up}, UI_KEY_UP), -ESTALE);
}

static void product(void *, const Frame &input, Frame &output) {
    handle_keys(input, output);
}

ZTEST(companion_keys, test_duplicate_key_ack_does_not_repeat_or_renew_hold) {
    Session session("dev-test", "emulator", CAP_UI_KEYS, product);
    Frame hello;
    hello.type = MSG_HELLO;
    hello.request = 1;
    hello.size = 17;
    hello.payload[0] = 8;
    memcpy(hello.payload + 1, "dev-test", 8);
    sys_put_le64(42, hello.payload + 9);
    zassert_true(session.handle(hello, k_uptime_get(), response));
    zassert_equal(response.payload[0], STATUS_OK);
    request.type = MSG_UI_KEY;
    request.request = 2;
    request.size = 2;
    request.payload[0] = UI_KEY_STAR;
    request.payload[1] = 1;
    zassert_true(session.handle(request, k_uptime_get(), response));
    zassert_equal(response.payload[0], STATUS_OK);
    k_sleep(K_MSEC(400));
    zassert_true(session.handle(request, k_uptime_get(), response));
    zassert_equal(response.payload[0], STATUS_OK);
    UiInput input;
    zassert_true(ui_take_input(input));
    zassert_false(ui_take_input(input));
    k_sleep(K_MSEC(100));
    zassert_false(ui_keypad_gesture().pressed);
    zassert_equal(keep(1u << UI_KEY_STAR), STATUS_STALE);
}

ZTEST(companion_keys, test_local_only_disconnect_confirmation) {
    model.input({UiKey::Enter});
    UiListPage page;
    for (unsigned i = 0; i < 30; ++i) {
        model.menu_page(page);
        if (strstr(page.rows[page.cursor % 4].name, "Companion")) {
            break;
        }
        model.input({UiKey::Down});
    }
    model.menu_page(page);
    zassert_not_null(strstr(page.rows[page.cursor % 4].name, "Companion"));
    model.input({UiKey::Enter});
    zassert_equal(model.screen(), UiScreen::CompanionExit);
    press(UI_KEY_OK);
    drain();
    zassert_equal(model.screen(), UiScreen::CompanionExit);
    press(UI_KEY_BACK);
    drain();
    zassert_equal(model.screen(), UiScreen::CompanionExit);
    model.input({UiKey::Back});
    zassert_equal(model.screen(), UiScreen::Menu);
}

ZTEST_SUITE(companion_keys, nullptr, nullptr, before, nullptr, nullptr);
