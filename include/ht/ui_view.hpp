// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/ui_presentation.hpp>
#include <lvgl.h>

namespace ht {
// The display owner starts and updates LVGL. This renderer has no radio/backend
// dependency and is shared by firmware and the forthcoming WASM browser build.
int ui_view_start(lv_obj_t *screen);
void ui_view_update(const UiPresentation &presentation);
} // namespace ht
