// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <SDL.h>
#include <ht/ui.hpp>

namespace ht {
// Reusable Linux facilities. No fake-radio or developer-control dependencies.
struct SdlDisplay {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    int width = 0;
    int height = 0;
    uint8_t backlight_percent = 100;
};

int sdl_open(SdlDisplay &display, const char *title, int width, int height, int scale);
void sdl_close(SdlDisplay &display);
int sdl_flush(SdlDisplay &display, const lv_area_t &area, const lv_color_t *pixels);
int sdl_backlight(SdlDisplay &display, uint8_t percent);
bool sdl_input(UiInput &input, uint32_t &window_id);
} // namespace ht
