// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/ui_preferences.hpp>

namespace ht {
// Semantic RGB colors; convert to the display's RGB565 only in the LVGL owner.
struct UiPalette {
    const char *name;
    uint32_t background, panel, muted, white, accent, line, amber, red, selection;
};

UiPalette ui_palette(Theme theme, Contrast contrast);
const char *ui_contrast_name(Contrast contrast);
} // namespace ht
