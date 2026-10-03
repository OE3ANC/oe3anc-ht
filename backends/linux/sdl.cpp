// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl.hpp"
#include <errno.h>
#include <stdio.h>

namespace ht {
void sdl_close(SdlDisplay &display) {
    if (display.texture) {
        SDL_DestroyTexture(display.texture);
    }
    if (display.renderer) {
        SDL_DestroyRenderer(display.renderer);
    }
    if (display.window) {
        SDL_DestroyWindow(display.window);
    }
    display = {};
}

int sdl_open(SdlDisplay &display, const char *title, int width, int height, int scale) {
    if (width <= 0 || height <= 0 || scale < 1 || scale > 8) {
        return -EINVAL;
    }
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return -EIO;
    }
    display.width = width;
    display.height = height;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    display.window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                      width * scale, height * scale, 0);
    if (display.window) {
        display.renderer = SDL_CreateRenderer(display.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (display.renderer) {
        display.texture = SDL_CreateTexture(display.renderer, SDL_PIXELFORMAT_RGB565,
                                            SDL_TEXTUREACCESS_STREAMING, width, height);
    }
    if (!display.texture || SDL_RenderSetLogicalSize(display.renderer, width, height)) {
        fprintf(stderr, "SDL display: %s\n", SDL_GetError());
        sdl_close(display);
        return -EIO;
    }
    SDL_RenderSetIntegerScale(display.renderer, SDL_TRUE);
    SDL_SetRenderDrawColor(display.renderer, 0, 0, 0, 255);
    return 0;
}

static int present(SdlDisplay &display) {
    if (SDL_RenderClear(display.renderer) ||
        SDL_RenderCopy(display.renderer, display.texture, nullptr, nullptr)) {
        return -EIO;
    }
    SDL_RenderPresent(display.renderer);
    return 0;
}

int sdl_flush(SdlDisplay &display, const lv_area_t &area, const lv_color_t *pixels) {
    const SDL_Rect rectangle{area.x1, area.y1, area.x2 - area.x1 + 1, area.y2 - area.y1 + 1};
    if (SDL_UpdateTexture(display.texture, &rectangle, pixels, rectangle.w * 2)) {
        return -EIO;
    }
    return present(display);
}

int sdl_backlight(SdlDisplay &display, uint8_t percent) {
    if (percent > 100 || !display.texture) {
        return -EINVAL;
    }
    if (percent == display.backlight_percent) {
        return 0;
    }
    const uint8_t color = unsigned(percent) * 255 / 100;
    if (SDL_SetTextureColorMod(display.texture, color, color, color)) {
        return -EIO;
    }
    const int error = present(display);
    if (!error) {
        display.backlight_percent = percent;
    }
    return error;
}

bool sdl_input(UiInput &input, uint32_t &window_id) {
    static SDL_Scancode star_scancode = SDL_SCANCODE_UNKNOWN;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        input = {UiKey::Release};
        if (event.type == SDL_QUIT) {
            input.key = UiKey::Quit;
            window_id = 0;
            return true;
        }
        if (event.type == SDL_WINDOWEVENT) {
            window_id = event.window.windowID;
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                star_scancode = SDL_SCANCODE_UNKNOWN;
                return true;
            }
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                input.key = UiKey::Quit;
                return true;
            }
            continue;
        }
        if ((event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) || event.key.repeat) {
            continue;
        }
        window_id = event.key.windowID;
        input.pressed = event.type == SDL_KEYDOWN;
        const auto key = event.key.keysym.sym;
        // Shift may be released before 8. Preserve the physical key's release
        // identity rather than translating key-up using its new modifiers.
        if (!input.pressed && star_scancode != SDL_SCANCODE_UNKNOWN &&
            event.key.keysym.scancode == star_scancode) {
            star_scancode = SDL_SCANCODE_UNKNOWN;
            input.key = UiKey::Star;
            return true;
        }
        if ((event.key.keysym.mod & KMOD_SHIFT) && (key == SDLK_3 || key == SDLK_8)) {
            input.key = key == SDLK_3 ? UiKey::Hash : UiKey::Star;
            if (input.pressed && input.key == UiKey::Star) {
                star_scancode = event.key.keysym.scancode;
            }
            return true;
        }
        switch (key) {
        case SDLK_UP:
            input.key = UiKey::Up;
            break;
        case SDLK_DOWN:
            input.key = UiKey::Down;
            break;
        case SDLK_LEFT:
            input.key = UiKey::Left;
            break;
        case SDLK_RIGHT:
            input.key = UiKey::Right;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            input.key = UiKey::Enter;
            break;
        case SDLK_ESCAPE:
            input.key = UiKey::Back;
            break;
        case SDLK_BACKSPACE:
        case SDLK_DELETE:
            input.key = UiKey::Erase;
            break;
        case SDLK_SPACE:
            input.key = UiKey::Ptt;
            break;
        case SDLK_F3:
            input.key = UiKey::Monitor;
            break;
        case SDLK_ASTERISK:
        case SDLK_KP_MULTIPLY:
            input.key = UiKey::Star;
            if (input.pressed) {
                star_scancode = event.key.keysym.scancode;
            }
            break;
        case SDLK_HASH:
            input.key = UiKey::Hash;
            break;
        default:
            if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
                input.character = '1' + key - SDLK_KP_1;
            } else if (key == SDLK_KP_0) {
                input.character = '0';
            } else if (key == SDLK_KP_PERIOD) {
                input.character = '.';
            } else if ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9') || key == '.' ||
                       key == '-' || key == '/') {
                input.character = static_cast<char>(key);
            } else {
                continue;
            }
            const bool keypad = (key >= SDLK_KP_1 && key <= SDLK_KP_9) || key == SDLK_KP_0 ||
                                ((event.key.keysym.mod & KMOD_ALT) && key >= '0' && key <= '9');
            input.key = keypad ? UiKey::Digit : UiKey::Character;
        }
        return true;
    }
    return false;
}
} // namespace ht
