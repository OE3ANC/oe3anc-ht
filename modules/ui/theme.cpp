// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui_theme.hpp>

namespace ht {
// Nord: nordtheme.com; Solarized: ethanschoonover.com/solarized;
// Darcula inspired by JetBrains IntelliJ UI colors. Colors map to radio UI roles.
static const UiPalette palettes[] = {
    {"Midnight", 0x0b111a, 0x141f2c, 0x8e9eaf, 0xeaf1f6, 0x65dcc7, 0x253343, 0xffc078, 0xff747c,
     0x183330},
    {"Nord", 0x2e3440, 0x3b4252, 0xd8dee9, 0xeceff4, 0x88c0d0, 0x4c566a, 0xebcb8b, 0xbf616a,
     0x434c5e},
    {"Solarized Dark", 0x002b36, 0x073642, 0x93a1a1, 0xeee8d5, 0x2aa198, 0x586e75, 0xb58900,
     0xdc322f, 0x073642},
    {"Darcula", 0x2b2b2b, 0x313335, 0xa9b7c6, 0xe6e6e6, 0x7aa9e0, 0x555555, 0xffc66d, 0xff7878,
     0x3c4756},
    // Phosphor-inspired palettes keep warning and fault colors distinct.
    {"Terminal Green", 0x071009, 0x0d1c12, 0x88b896, 0xd1f5d6, 0x78e894, 0x294b32, 0xffcb75,
     0xff8585, 0x183925},
    {"Terminal Amber", 0x120d05, 0x21190b, 0xc5a476, 0xffe5af, 0xffbf62, 0x514027, 0xffdf78,
     0xff8585, 0x3b2d15},
    {"Terminal Ice", 0x071015, 0x102029, 0x8fb6c6, 0xdbf5ff, 0x86deef, 0x2c4857, 0xffcb75, 0xff8585,
     0x193642},
};
static_assert(sizeof(palettes) / sizeof(palettes[0]) == ThemeCount, "Missing theme palette");

static uint32_t brighten(uint32_t color, unsigned amount) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        const unsigned channel = (color >> shift) & 255;
        result |= (channel + (255 - channel) * amount / 255) << shift;
    }
    return result;
}

UiPalette ui_palette(Theme theme, Contrast contrast) {
    UiPalette colors = palettes[theme <= Theme::TerminalIce ? static_cast<unsigned>(theme) : 0];
    if (contrast != Contrast::Normal && contrast <= Contrast::Maximum) {
        const bool maximum = contrast == Contrast::Maximum;
        colors.white = brighten(colors.white, maximum ? 255 : 96);
        colors.muted = brighten(colors.muted, maximum ? 255 : 160);
        colors.accent = brighten(colors.accent, maximum ? 128 : 64);
        colors.amber = brighten(colors.amber, maximum ? 128 : 64);
        colors.red = brighten(colors.red, maximum ? 128 : 64);
        colors.line = brighten(colors.line, maximum ? 144 : 80);
    }
    return colors;
}

const char *ui_contrast_name(Contrast contrast) {
    static const char *names[] = {"Normal", "High", "Maximum"};
    return names[contrast <= Contrast::Maximum ? static_cast<unsigned>(contrast) : 0];
}
} // namespace ht
