// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <ht/settings.hpp>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
void UiModel::wake_light(int64_t now) {
    dimmed_ = false;
    light_activity_ms_ = now;
}

uint8_t UiModel::backlight_percent() const {
    if (!state_.power_active) {
        return 0;
    }
    return dimmed_ && applied_ui_.dim_percent < applied_ui_.brightness_percent
               ? applied_ui_.dim_percent
               : applied_ui_.brightness_percent;
}

void UiModel::advance_light(int64_t now) {
    const auto ptt = radio_ptt_press_sequence(), monitor = radio_monitor_press_sequence();
    if (ptt != wake_ptt_sequence_ || monitor != wake_monitor_sequence_ || radio_ptt_requested() ||
        radio_monitor_requested() || radio_latched_fault() || state_.fault ||
        state_.phase == RadioPhase::Transmitting || state_.monitor_active) {
        wake_light(now);
    }
    wake_ptt_sequence_ = ptt;
    wake_monitor_sequence_ = monitor;
    if (now < light_activity_ms_ || !applied_ui_.idle_s) {
        wake_light(now);
    } else {
        dimmed_ = applied_ui_.dim_percent < applied_ui_.brightness_percent &&
                  now - light_activity_ms_ >= int64_t(applied_ui_.idle_s) * 1000;
    }
}

bool UiModel::light_input(const UiInput &input) {
    const auto now = k_uptime_get();
    if (input.key == UiKey::Ptt || input.key == UiKey::Monitor) {
        if (input.pressed) {
            wake_light(now);
        }
        return false;
    }
    if (input.key == UiKey::Release || input.key == UiKey::Quit || !input.pressed) {
        return false;
    }
    const bool consume = dimmed_;
    wake_light(now);
    return consume;
}

void UiModel::open_light() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    light_ptt_sequence_ = radio_ptt_press_sequence();
    if (!ui_backend_has_backlight()) {
        error_ = -ENOTSUP;
        return;
    }
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    settings_ui_preferences(light_draft_, &light_revision_);
    light_cursor_ = 0;
    screen_ = UiScreen::Backlight;
#endif
}

void UiModel::cancel_light(bool interruption) {
    if (screen_ == UiScreen::Backlight) {
        screen_ = interruption ? UiScreen::Home : UiScreen::Menu;
    }
}

void UiModel::backlight_input(const UiInput &input) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (input.key == UiKey::Back) {
        cancel_light();
    } else if (input.key == UiKey::Left) {
        light_cursor_ = (light_cursor_ + 1) % 3;
    } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
        static const uint8_t active[] = {25, 50, 75, 100}, idle[] = {0, 15, 30, 60},
                             dim[] = {10, 20, 30};
        auto &value = light_cursor_ == 0   ? light_draft_.brightness_percent
                      : light_cursor_ == 1 ? light_draft_.idle_s
                                           : light_draft_.dim_percent;
        const auto *choices = light_cursor_ == 0 ? active : light_cursor_ == 1 ? idle : dim;
        const unsigned count = light_cursor_ == 2 ? 3 : 4;
        unsigned index = 0;
        while (index + 1 < count && choices[index] != value) {
            ++index;
        }
        value = choices[(index + count + (input.key == UiKey::Up ? 1 : -1)) % count];
    } else if (input.key == UiKey::Enter) {
        if (!next_id_) {
            error_ = -EOVERFLOW;
            return;
        }
        error_ = settings_put_ui_preferences(light_draft_, next_id_++, state_, light_revision_);
        if (!error_) {
            light_pending_ = next_id_ - 1;
        }
    }
#else
    (void)input;
#endif
}

void UiModel::backlight_lines(char (&text)[8][32]) const {
    strcpy(text[0], "BACKLIGHT");
    strcpy(text[1], "Explicit Apply / no preview");
    snprintf(text[2], 32, "%c Active: %u%%", light_cursor_ == 0 ? '>' : ' ',
             light_draft_.brightness_percent);
    if (light_draft_.idle_s) {
        snprintf(text[3], 32, "%c Idle dim: %u s", light_cursor_ == 1 ? '>' : ' ',
                 light_draft_.idle_s);
    } else {
        snprintf(text[3], 32, "%c Idle dim: Never", light_cursor_ == 1 ? '>' : ' ');
    }
    snprintf(text[4], 32, "%c Dim: %u%%", light_cursor_ == 2 ? '>' : ' ', light_draft_.dim_percent);
    strcpy(text[5], "  Dim capped by active");
}
} // namespace ht
