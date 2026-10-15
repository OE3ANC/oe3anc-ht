// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/ui.hpp>
#include <string.h>
#include <zephyr/ztest.h>

// Shared navigation for tests which exercise a settings page rather than menu ordering.
inline void ui_test_menu(ht::UiModel &model, const char *name) {
    using namespace ht;
    for (unsigned i = 0; i < 2 && model.screen() == UiScreen::Menu; ++i) {
        model.input({UiKey::Back});
    }
    zassert_equal(model.screen(), UiScreen::Home);
    model.input({UiKey::Enter});
    const char *group = !strncmp(name, "BK4819", 6) || !strncmp(name, "FM ", 3) ||
                                !strncmp(name, "Invert", 6) ? "Development"
                        : !strncmp(name, "VFO step", 8) || !strncmp(name, "TX limit", 8) ||
                                  !strncmp(name, "Local callsign", 14) ||
                                  !strncmp(name, "VFO /", 5) ? "Radio" : nullptr;
    for (unsigned pass = 0; pass < (group ? 2u : 1u); ++pass) {
        const char *target = group && !pass ? group : name;
        bool found = false;
        for (unsigned i = 0; i < 32; ++i) {
            UiListPage page;
            model.menu_page(page);
            if (strstr(page.rows[page.cursor % 4].name, target)) { found = true; break; }
            model.input({UiKey::Down});
        }
        zassert_true(found, "Missing menu item %s", target);
        model.input({UiKey::Enter});
    }
}
