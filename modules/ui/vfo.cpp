// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <errno.h>
#ifdef CONFIG_HT_CODEPLUG_STORAGE
#include <ht/settings.hpp>
#endif
namespace ht {
void UiModel::tune_vfo(int direction) {
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    RadioCommand command;
    command.config = state_.config;
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    const auto step = settings_vfo_step();
#else
    const auto step = vfo_step_hz_;
#endif
    const int64_t shift = int64_t(step) * direction;
    const int64_t rx = int64_t(command.config.rx_frequency_hz) + shift;
    const int64_t tx = int64_t(command.config.tx_frequency_hz) + shift;
    if (rx < 0 || tx < 0 || rx > UINT32_MAX || tx > UINT32_MAX) {
        error_ = -ERANGE;
        return;
    }
    command.config.rx_frequency_hz = rx;
    command.config.tx_frequency_hz = tx;
    error_ = validate_config(command.config);
    if (!error_) {
        submit(command);
    }
}

void UiModel::open_step() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    step_ptt_sequence_ = radio_ptt_press_sequence();
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    step_draft_ = settings_vfo_step(&step_revision_);
    screen_ = UiScreen::VfoStep;
#else
    error_ = -ENOTSUP;
#endif
}

void UiModel::cancel_step(bool interruption) {
    if (screen_ == UiScreen::VfoStep) {
        screen_ = interruption ? UiScreen::Home : UiScreen::Menu;
    }
}

void UiModel::step_input(const UiInput &input) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (input.key == UiKey::Back) {
        cancel_step();
    } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
        size_t index = 0;
        while (index + 1 < vfo_step_count && vfo_steps_hz[index] != step_draft_) {
            ++index;
        }
        index = (index + vfo_step_count + (input.key == UiKey::Up ? 1 : -1)) % vfo_step_count;
        step_draft_ = vfo_steps_hz[index];
    } else if (input.key == UiKey::Enter) {
        if (!next_id_) {
            error_ = -EOVERFLOW;
            return;
        }
        error_ = settings_put_vfo_step(step_draft_, next_id_++, state_, step_revision_);
        if (!error_) {
            step_pending_ = next_id_ - 1;
        }
    }
#else
    (void)input;
#endif
}
} // namespace ht
