// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ht {
enum class Theme : uint8_t { Midnight = 0, Nord = 1, SolarizedDark = 2, Darcula = 3 };
enum class Contrast : uint8_t { Normal = 0, High = 1, Maximum = 2 };

struct UiPreferences {
    Theme theme = Theme::Midnight;
    Contrast contrast = Contrast::Normal;
    bool animations = true;
    uint8_t brightness_percent = 100;
    uint8_t idle_s = 15; // Zero means never dim.
    uint8_t dim_percent = 10;
};
} // namespace ht
