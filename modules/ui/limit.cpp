// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace ht {
void UiModel::open_limit() {
    limit_ptt_sequence_ = radio_ptt_press_sequence();
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    limit_draft_ = state_.config.transmit_limit_s;
    limit_generation_ = state_.generation;
    limit_revision_ = state_.configuration_revision;
    limit_selection_ = state_.selection;
    screen_ = UiScreen::TransmitLimit;
}

void UiModel::cancel_limit(bool interruption) {
    if (screen_ == UiScreen::TransmitLimit) {
        screen_ = interruption ? UiScreen::Home : UiScreen::Menu;
    }
}

void UiModel::limit_input(const UiInput &input) {
    if (input.key == UiKey::Back) {
        cancel_limit();
    } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
        static const uint16_t limits[] = {0, 60, 120, 180};
        unsigned index = 0;
        while (index < 3 && limits[index] != limit_draft_) {
            ++index;
        }
        limit_draft_ = limits[(index + 4 + (input.key == UiKey::Up ? 1 : -1)) % 4];
    } else if (input.key == UiKey::Enter) {
        RadioCommand command;
        command.kind = CommandKind::TransmitLimit;
        command.config.transmit_limit_s = limit_draft_;
        command.expected_generation = limit_generation_;
        command.expected_revision = limit_revision_;
        command.selection = limit_selection_;
        submit(command);
    }
}

void UiModel::limit_lines(char (&text)[8][32]) const {
    strcpy(text[0], "TRANSMIT LIMIT");
    strcpy(text[1], "Up/Down selects duration");
    if (limit_draft_) {
        snprintf(text[2], 32, "%u seconds", limit_draft_);
    } else {
        strcpy(text[2], "Off");
    }
    strcpy(text[3], limit_draft_ ? "Warning: final 10 seconds" : "No automatic TX limit");
    strcpy(text[4], limit_draft_ ? "Release PTT after timeout" : "PTT release still stops TX");
    strcpy(text[5], "Global / persistent setting");
}
} // namespace ht
