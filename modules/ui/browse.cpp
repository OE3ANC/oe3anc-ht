// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_HT_CODEPLUG_STORAGE
#include <ht/settings.hpp>
#endif

namespace ht {
void UiModel::refresh_list() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (screen_ != UiScreen::Channels && screen_ != UiScreen::Banks &&
        screen_ != UiScreen::ChannelBank) {
        return;
    }
    list_.ready = false;
    const auto storage = settings_status();
    UiListPage page;
    page.cursor = browse_cursor_;
    Bank bank;
    if (screen_ == UiScreen::Banks || screen_ == UiScreen::ChannelBank) {
        strcpy(page.title, "BANKS");
        strcpy(page.detail, "Select a channel group");
        page.count = storage.bank_count + 1; // All is always available.
        if (page.cursor >= page.count) {
            page.cursor = 0;
        }
        const unsigned first = page.cursor / 4 * 4;
        for (unsigned row = 0; row < 4 && first + row < page.count; ++row) {
            auto &item = page.rows[row];
            if (!first && !row) {
                strcpy(item.name,
                       screen_ == UiScreen::ChannelBank ? "No bank addition" : "All channels");
                if (screen_ == UiScreen::Banks) {
                    snprintf(item.suffix, sizeof(item.suffix), "%u", storage.channel_count);
                }
            } else {
                uint32_t revision;
                if (settings_bank_at(first + row - 1, bank, &revision) ||
                    revision != storage.revision) {
                    return;
                }
                item.id = bank.id;
                memcpy(item.name, bank.name, sizeof(item.name));
                snprintf(item.suffix, sizeof(item.suffix), "%u", bank.count);
            }
        }
    } else {
        strcpy(page.title, "CHANNELS");
        page.count = storage.channel_count;
        const char *name = "All";
        if (browse_bank_) {
            uint32_t revision;
            if (settings_bank(browse_bank_, bank, &revision)) {
                browse_bank_ = 0;
                browse_channel_ = 0;
            } else {
                if (revision != storage.revision) {
                    return;
                }
                name = bank.name;
                page.count = bank.count;
            }
        }
        if (browse_channel_) {
            if (settings_channel_position(browse_bank_, browse_channel_, page.cursor)) {
                browse_channel_ = 0;
                page.cursor = 0;
            }
        }
        if (page.cursor >= page.count) {
            page.cursor = 0;
        }
        snprintf(page.detail, sizeof(page.detail), "%.24s / %u", name, page.count);
        const unsigned first = page.cursor / 4 * 4;
        for (unsigned row = 0; row < 4 && first + row < page.count; ++row) {
            Channel channel;
            uint32_t revision;
            if (settings_channel_at(browse_bank_, first + row, channel, &revision) ||
                revision != storage.revision) {
                return;
            }
            auto &item = page.rows[row];
            item.id = channel.id;
            memcpy(item.name, channel.name, sizeof(item.name));
            snprintf(item.prefix, sizeof(item.prefix), "%03u", channel.number);
            strcpy(item.suffix, channel.configuration.mode == Mode::Fm ? "FM" : "M17");
        }
    }
    // If ownership changed between copied reads, retain the prior page and
    // retry on the next UI sync rather than publish a mixed-revision list.
    if (settings_status().revision != storage.revision) {
        return;
    }
    page.ready = true;
    list_ = page;
    browse_revision_ = storage.revision;
    browse_cursor_ = page.cursor;
    if (screen_ == UiScreen::Channels && page.count) {
        browse_channel_ = page.rows[page.cursor % 4].id;
    }
#endif
}

void UiModel::open_channels(UiScreen return_screen) {
    list_return_ = return_screen;
    browse_bank_ = state_.selection.bank_id;
    browse_channel_ = state_.selection.channel_id;
    list_ = {};
    browse_cursor_ = 0;
    screen_ = UiScreen::Channels;
    refresh_list();
}

void UiModel::recall(const Selection &selection, uint32_t revision) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    if (!next_id_) {
        error_ = -EOVERFLOW;
        return;
    }
    const uint32_t id = next_id_++;
    error_ = settings_recall(selection, id, state_, revision);
    if (!error_) {
        recall_pending_ = id;
    }
#else
    (void)selection;
    (void)revision;
    error_ = -ENOTSUP;
#endif
}

void UiModel::switch_operating() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    auto selection = state_.selection;
    const auto revision = settings_status().revision;
    if (selection.operating == Operating::Memory) {
        selection.operating = Operating::Vfo;
    } else {
        uint16_t index;
        if (settings_channel_position(selection.bank_id, selection.channel_id, index)) {
            Channel channel;
            uint32_t copied_revision;
            if (settings_channel_at(selection.bank_id, 0, channel, &copied_revision)) {
                open_channels(UiScreen::Home);
                return;
            }
            if (copied_revision != revision) {
                error_ = -ESTALE;
                return;
            }
            selection.channel_id = channel.id;
        }
        selection.operating = Operating::Memory;
    }
    recall(selection, revision);
#endif
}

void UiModel::step_memory(int direction) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    auto selection = state_.selection;
    const auto storage = settings_status();
    uint16_t count = storage.channel_count, position = 0;
    if (selection.bank_id) {
        Bank bank;
        uint32_t revision;
        if (settings_bank(selection.bank_id, bank, &revision)) {
            error_ = -ENOENT;
            return;
        }
        if (revision != storage.revision) {
            error_ = -ESTALE;
            return;
        }
        count = bank.count;
    }
    if (!count) {
        open_channels(UiScreen::Home);
        return;
    }
    error_ = settings_channel_position(selection.bank_id, selection.channel_id, position);
    if (error_) {
        return;
    }
    position = (position + count + direction) % count;
    Channel channel;
    uint32_t revision;
    error_ = settings_channel_at(selection.bank_id, position, channel, &revision);
    if (!error_ && revision != storage.revision) {
        error_ = -ESTALE;
    }
    if (!error_) {
        selection.channel_id = channel.id;
        recall(selection, storage.revision);
    }
#else
    (void)direction;
#endif
}

void UiModel::open_number(UiScreen return_screen) {
    text_ptt_sequence_ = radio_ptt_press_sequence();
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    number_return_ = return_screen;
    if (return_screen == UiScreen::Home) {
        browse_bank_ = state_.selection.bank_id;
    }
    text_editor_.begin(TextKind::ChannelNumber);
    screen_ = UiScreen::ChannelNumber;
}

void UiModel::apply_number() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    unsigned number = 0;
    if (!text_editor_.length()) {
        error_ = -EINVAL;
        return;
    }
    for (const char *p = text_editor_.text(); *p; ++p) {
        if (*p < '0' || *p > '9') {
            error_ = -EINVAL;
            return;
        }
        number = number * 10 + (*p - '0');
        if (number > channel_capacity) {
            error_ = -ERANGE;
            return;
        }
    }
    if (!number) {
        error_ = -EINVAL;
        return;
    }
    Channel channel;
    uint32_t revision;
    error_ = settings_channel_number(number, channel, &revision);
    uint16_t position;
    if (!error_) {
        error_ = settings_channel_position(browse_bank_, channel.id, position);
    }
    if (!error_) {
        recall({Operating::Memory, browse_bank_, channel.id}, revision);
    }
#endif
}

void UiModel::browse(const UiInput &input) {
    if (input.key == UiKey::Back) {
        if (screen_ == UiScreen::Banks) {
            screen_ = UiScreen::Channels;
            browse_cursor_ = 0;
            refresh_list();
        } else {
            screen_ = list_return_;
        }
        return;
    }
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (!list_.ready || settings_status().revision != browse_revision_) {
        refresh_list();
        error_ = -EAGAIN;
        return; // Retry the key after the refreshed page is shown.
    }
#endif
    if (screen_ == UiScreen::Banks && input.key == UiKey::Left) {
        open_bank(0);
        return;
    }
    if (screen_ == UiScreen::Banks && input.key == UiKey::Right && list_.count) {
        const auto id = list_.rows[list_.cursor % 4].id;
        if (id) {
            open_bank(id);
        }
        return;
    }
    if (input.key == UiKey::Left && screen_ == UiScreen::Channels) {
        screen_ = UiScreen::Banks;
        browse_cursor_ = 0;
#ifdef CONFIG_HT_CODEPLUG_STORAGE
        Bank bank;
        for (uint8_t i = 0; i < settings_status().bank_count; ++i) {
            if (!settings_bank_at(i, bank) && bank.id == browse_bank_) {
                browse_cursor_ = i + 1;
                break;
            }
        }
#endif
        refresh_list();
        return;
    }
    if (input.key == UiKey::Up || input.key == UiKey::Down) {
        if (list_.count) {
            browse_cursor_ =
                (list_.cursor + list_.count + (input.key == UiKey::Down ? 1 : -1)) % list_.count;
            if (screen_ == UiScreen::Channels) {
                browse_channel_ = 0;
            }
            refresh_list();
        }
    } else if (input.key == UiKey::Right && screen_ == UiScreen::Channels && list_.count) {
        open_channel(list_.rows[list_.cursor % 4].id, UiScreen::Channels);
    } else if (input.key == UiKey::Enter && !list_.count && screen_ == UiScreen::Channels) {
        open_channel(0, UiScreen::Channels);
    } else if (input.key == UiKey::Enter && list_.count) {
        const auto id = list_.rows[list_.cursor % 4].id;
        if (screen_ == UiScreen::Banks) {
            browse_bank_ = id;
            screen_ = UiScreen::Channels;
            browse_cursor_ = 0;
            // Preserve the highlighted channel when it belongs to the new bank.
            refresh_list();
        } else {
            recall({Operating::Memory, browse_bank_, id}, browse_revision_);
        }
    } else if (screen_ == UiScreen::Channels &&
               (input.key == UiKey::Digit || input.key == UiKey::Character) &&
               input.character >= '0' && input.character <= '9') {
        open_number(UiScreen::Channels);
        if (screen_ == UiScreen::ChannelNumber) {
            text_input(input);
        }
    }
}
} // namespace ht
