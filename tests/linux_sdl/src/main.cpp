// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../backends/linux/sdl.hpp"
#include <errno.h>
#include <stdlib.h>
#include <zephyr/ztest.h>
using namespace ht;
static SdlDisplay radio_display, second_display;
static lv_color_t pixels[160 * 128];

static void *setup() {
    zassert_ok(setenv("SDL_VIDEODRIVER", "dummy", 1));
    return nullptr;
}

static void after(void *) {
    sdl_close(radio_display);
    sdl_close(second_display);
}

static void drain() {
    UiInput input;
    uint32_t window;
    for (unsigned i = 0; i < 2; ++i) {
        while (sdl_input(input, window)) {
        }
    }
}

static bool poll(UiInput &input, uint32_t &window) {
    // SDL may end its current poll cycle before delivering a newly pushed event.
    return sdl_input(input, window) || sdl_input(input, window);
}

static uint32_t pixel(SdlDisplay &display, int x, int y) {
    uint32_t result = 0;
    const SDL_Rect rectangle{x, y, 1, 1};
    zassert_ok(SDL_RenderReadPixels(display.renderer, &rectangle, SDL_PIXELFORMAT_ARGB8888, &result,
                                    sizeof(result)));
    return result & 0xffffff;
}

static void paint(SdlDisplay &display) {
    for (unsigned i = 0; i < 160 * 128; ++i) {
        pixels[i].full = i % 160 < 80 ? 0xf800 : 0x001f;
    }
    zassert_ok(sdl_flush(display, {0, 0, 159, 127}, pixels));
}

static SDL_Event key_event(SDL_Keycode key, bool pressed = true, uint16_t modifiers = 0,
                           bool repeat = false) {
    SDL_Event event{};
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = SDL_GetWindowID(radio_display.window);
    event.key.keysym.sym = key;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    event.key.keysym.mod = modifiers;
    event.key.repeat = repeat;
    return event;
}

static void push(SDL_Event event) {
    zassert_equal(SDL_PushEvent(&event), 1);
}

ZTEST(linux_sdl, test_native_rgb565_geometry_nearest_integer_scales_and_close) {
    for (unsigned scale = 1; scale <= 8; ++scale) {
        zassert_ok(sdl_open(radio_display, "Linux display", 160, 128, scale));
        int width, height;
        SDL_GetWindowSize(radio_display.window, &width, &height);
        zassert_equal(width, 160 * scale);
        zassert_equal(height, 128 * scale);
        SDL_RenderGetLogicalSize(radio_display.renderer, &width, &height);
        zassert_equal(width, 160);
        zassert_equal(height, 128);
        zassert_true(SDL_RenderGetIntegerScale(radio_display.renderer));
        uint32_t format;
        int access;
        zassert_ok(SDL_QueryTexture(radio_display.texture, &format, &access, &width, &height));
        zassert_equal(format, SDL_PIXELFORMAT_RGB565);
        zassert_equal(access, SDL_TEXTUREACCESS_STREAMING);
        zassert_equal(width, 160);
        zassert_equal(height, 128);
        paint(radio_display);
        zassert_equal(pixel(radio_display, 80 * scale - 1, 64 * scale), 0xff0000);
        zassert_equal(pixel(radio_display, 80 * scale, 64 * scale), 0x0000ff);
        sdl_close(radio_display);
        sdl_close(radio_display);
        zassert_is_null(radio_display.window);
        zassert_is_null(radio_display.texture);
        zassert_is_null(radio_display.renderer);
    }
}

ZTEST(linux_sdl, test_partial_flush_and_brightness_are_local_to_one_window) {
    zassert_ok(sdl_open(radio_display, "Radio", 160, 128, 3));
    zassert_ok(sdl_open(second_display, "Other Linux window", 160, 128, 1));
    paint(radio_display);
    paint(second_display);
    lv_color_t patch[4];
    for (auto &color : patch) {
        color.full = 0x07e0;
    }
    zassert_ok(sdl_flush(radio_display, {1, 2, 2, 3}, patch));
    zassert_equal(pixel(radio_display, 3, 6), 0x00ff00);
    zassert_equal(pixel(radio_display, 9, 6), 0xff0000);
    zassert_equal(pixel(second_display, 1, 2), 0xff0000);
    zassert_ok(sdl_backlight(radio_display, 25));
    const auto dim = pixel(radio_display, 9, 6);
    zassert_true(dim > 0 && dim < 0x640000);
    zassert_equal(dim & 0xffff, 0);
    zassert_equal(pixel(second_display, 3, 2), 0xff0000);
    zassert_ok(sdl_backlight(radio_display, 0));
    zassert_equal(pixel(radio_display, 9, 6), 0);
    zassert_ok(sdl_backlight(radio_display, 100));
    zassert_equal(pixel(radio_display, 9, 6), 0xff0000);
}

ZTEST(linux_sdl, test_invalid_dimensions_scale_and_backlight_reject_without_window) {
    zassert_equal(sdl_open(radio_display, "Invalid", 0, 128, 1), -EINVAL);
    zassert_equal(sdl_open(radio_display, "Invalid", 160, -1, 1), -EINVAL);
    zassert_equal(sdl_open(radio_display, "Invalid", 160, 128, 0), -EINVAL);
    zassert_equal(sdl_open(radio_display, "Invalid", 160, 128, 9), -EINVAL);
    zassert_is_null(radio_display.window);
    zassert_equal(sdl_backlight(radio_display, 10), -EINVAL);
    zassert_ok(sdl_open(radio_display, "Valid", 160, 128, 1));
    zassert_equal(sdl_backlight(radio_display, 101), -EINVAL);
    zassert_equal(radio_display.backlight_percent, 100);
}

ZTEST(linux_sdl, test_typed_physical_and_literal_key_routing_preserves_window_and_release) {
    zassert_ok(sdl_open(radio_display, "Keys", 160, 128, 1));
    drain();

    struct Mapping {
        SDL_Keycode host;
        UiKey key;
        char character = 0;
        uint16_t modifiers = 0;
    };

    const Mapping keys[] = {
        {SDLK_UP, UiKey::Up},
        {SDLK_DOWN, UiKey::Down},
        {SDLK_LEFT, UiKey::Left},
        {SDLK_RIGHT, UiKey::Right},
        {SDLK_RETURN, UiKey::Enter},
        {SDLK_KP_ENTER, UiKey::Enter},
        {SDLK_ESCAPE, UiKey::Back},
        {SDLK_BACKSPACE, UiKey::Erase},
        {SDLK_DELETE, UiKey::Erase},
        {SDLK_SPACE, UiKey::Ptt},
        {SDLK_F3, UiKey::Monitor},
        {SDLK_HASH, UiKey::Hash},
        {SDLK_3, UiKey::Hash, 0, KMOD_SHIFT},
        {SDLK_2, UiKey::Character, '2'},
        {SDLK_a, UiKey::Character, 'a'},
        {SDLK_PERIOD, UiKey::Character, '.'},
        {SDLK_MINUS, UiKey::Character, '-'},
        {SDLK_SLASH, UiKey::Character, '/'},
        {SDLK_KP_2, UiKey::Digit, '2'},
        {SDLK_KP_0, UiKey::Digit, '0'},
        {SDLK_2, UiKey::Digit, '2', KMOD_ALT},
    };
    const bool levels[] = {true, false};
    for (const auto &mapping : keys) {
        for (const bool pressed : levels) {
            push(key_event(mapping.host, pressed, mapping.modifiers));
            UiInput input;
            uint32_t window;
            zassert_true(poll(input, window));
            zassert_equal(input.key, mapping.key);
            zassert_equal(input.character, mapping.character);
            zassert_equal(input.pressed, pressed);
            zassert_equal(window, SDL_GetWindowID(radio_display.window));
        }
    }
}

ZTEST(linux_sdl, test_focus_close_repeat_and_shift_star_release_contract) {
    zassert_ok(sdl_open(radio_display, "Events", 160, 128, 1));
    drain();
    UiInput input;
    uint32_t window;
    push(key_event(SDLK_UP, true, 0, true));
    push(key_event(SDLK_UNKNOWN));
    zassert_false(poll(input, window));
    push(key_event(SDLK_8, true, KMOD_SHIFT));
    zassert_true(poll(input, window));
    zassert_equal(input.key, UiKey::Star);
    push(key_event(SDLK_8, false));
    zassert_true(poll(input, window));
    zassert_equal(input.key, UiKey::Star);
    zassert_false(input.pressed);
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = SDL_GetWindowID(radio_display.window);
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    push(event);
    zassert_true(poll(input, window));
    zassert_equal(input.key, UiKey::Release);
    zassert_equal(window, SDL_GetWindowID(radio_display.window));
    event.window.event = SDL_WINDOWEVENT_CLOSE;
    push(event);
    zassert_true(poll(input, window));
    zassert_equal(input.key, UiKey::Quit);
}

ZTEST_SUITE(linux_sdl, nullptr, setup, nullptr, after, nullptr);
