// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#ifdef CONFIG_HT_CODEPLUG_STORAGE
#include <ht/settings.hpp>
#endif

namespace ht {
void UiModel::open_bank(uint32_t id) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    const auto marker = radio_ptt_press_sequence();
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    if (id) {
        error_ = settings_bank(id, bank_draft_, &bank_revision_);
        if (error_) {
            return;
        }
    } else {
        const auto status = settings_status();
        if (status.bank_count == bank_capacity) {
            error_ = -ENOSPC;
            return;
        }
        bank_draft_ = {};
        strcpy(bank_draft_.name, "NEW BANK");
        bank_revision_ = status.revision;
    }
    strcpy(bank_original_name_, bank_draft_.name);
    bank_ptt_sequence_ = marker;
    bank_form_cursor_ = 0;
    bank_cursor_ = bank_add_cursor_ = 0;
    bank_deleting_ = false;
    screen_ = UiScreen::BankEditor;
#else
    (void)id;
    error_ = -ENOTSUP;
#endif
}

void UiModel::cancel_bank(bool interruption) {
    if (!bank_programming(screen_)) {
        return;
    }
    screen_ = interruption ? UiScreen::Home : UiScreen::Banks;
    text_editor_ = {};
    if (!interruption) {
        refresh_list();
    }
}

void UiModel::refresh_bank_list() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (screen_ != UiScreen::BankMembers && screen_ != UiScreen::BankAdd) {
        return;
    }
    list_.ready = false;
    const auto status = settings_status();
    if (status.revision != bank_revision_) {
        error_ = -ESTALE;
        return;
    }
    UiListPage page;
    auto &cursor = screen_ == UiScreen::BankMembers ? bank_cursor_ : bank_add_cursor_;
    page.count = screen_ == UiScreen::BankMembers ? bank_draft_.count : status.channel_count;
    page.cursor = cursor < page.count ? cursor : 0;
    strcpy(page.title, screen_ == UiScreen::BankMembers ? "BANK MEMBERS" : "ADD CHANNELS");
    snprintf(page.detail, sizeof(page.detail), "%.24s / %u", bank_draft_.name,
             unsigned(bank_draft_.count % 257));
    for (unsigned row = 0, first = page.cursor / 4 * 4; row < 4 && first + row < page.count;
         ++row) {
        Channel channel;
        uint32_t revision;
        const int error =
            screen_ == UiScreen::BankMembers
                ? settings_channel(bank_draft_.channel_ids[first + row], channel, &revision)
                : settings_channel_at(0, first + row, channel, &revision);
        if (error || revision != bank_revision_) {
            error_ = -ESTALE;
            return;
        }
        auto &item = page.rows[row];
        item.id = channel.id;
        strcpy(item.name, channel.name);
        snprintf(item.prefix, sizeof(item.prefix), "%03u", unsigned(channel.number % 1000));
        if (screen_ == UiScreen::BankAdd && bank_contains(bank_draft_, channel.id)) {
            strcpy(item.suffix, "IN");
        }
    }
    if (settings_status().revision != bank_revision_) {
        error_ = -ESTALE;
        return;
    }
    page.ready = true;
    list_ = page;
    cursor = page.cursor;
#endif
}

void UiModel::save_bank() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (!next_id_) {
        error_ = -EOVERFLOW;
        return;
    }
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    error_ = bank_deleting_ ? 0 : validate_bank(bank_draft_);
    if (error_) {
        return;
    }
    const auto id = next_id_++;
    error_ = bank_deleting_ ? settings_delete_bank(bank_draft_.id, id, state_, bank_revision_)
                            : settings_put_bank(bank_draft_, id, state_, bank_revision_);
    if (!error_) {
        bank_pending_ = id;
    }
#endif
}

void UiModel::bank_input(const UiInput &input) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (screen_ == UiScreen::BankName) {
        if (input.key == UiKey::Back) {
            screen_ = UiScreen::BankEditor;
            text_editor_ = {};
        } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
            text_editor_.move(input.key == UiKey::Up ? -1 : 1);
        } else if (input.key == UiKey::Left || input.key == UiKey::Erase) {
            text_editor_.erase();
        } else if (input.key == UiKey::Right) {
            error_ = text_editor_.literal(' ');
        } else if (input.key == UiKey::Hash) {
            text_editor_.finish();
        } else if (input.key == UiKey::Character) {
            error_ = text_editor_.literal(input.character);
        } else if (input.key == UiKey::Digit) {
            error_ = text_editor_.digit(
                input.character, input.timestamp_ms < 0 ? k_uptime_get() : input.timestamp_ms);
        } else if (input.key == UiKey::Enter) {
            text_editor_.finish();
            if (!text_editor_.length()) {
                error_ = -EINVAL;
            } else {
                memset(bank_draft_.name, 0, sizeof(bank_draft_.name));
                strcpy(bank_draft_.name, text_editor_.text());
                screen_ = UiScreen::BankEditor;
                text_editor_ = {};
            }
        }
        return;
    }
    if (screen_ == UiScreen::BankSaved) {
        if (input.key == UiKey::Enter || input.key == UiKey::Back) {
            cancel_bank();
        }
        return;
    }
    if (input.key == UiKey::Back) {
        if (screen_ == UiScreen::BankEditor) {
            cancel_bank();
        } else if (screen_ == UiScreen::BankAdd || screen_ == UiScreen::BankActions) {
            screen_ = UiScreen::BankMembers;
            refresh_bank_list();
        } else {
            if (screen_ == UiScreen::BankMembers) {
                bank_form_cursor_ = 1;
            }
            screen_ = UiScreen::BankEditor;
        }
        return;
    }
    if (screen_ == UiScreen::BankDelete) {
        if (input.key == UiKey::Enter) {
            save_bank();
        }
        return;
    }
    if (screen_ == UiScreen::BankMembers || screen_ == UiScreen::BankAdd) {
        if (input.key == UiKey::Right) {
            if (screen_ == UiScreen::BankMembers) {
                screen_ = UiScreen::BankEditor;
                bank_form_cursor_ = 1;
            } else {
                screen_ = UiScreen::BankMembers;
            }
            refresh_bank_list();
            return;
        }
        if (!list_.ready || settings_status().revision != bank_revision_) {
            refresh_bank_list();
            if (!error_) {
                error_ = -EAGAIN;
            }
            return;
        }
        if (input.key == UiKey::Left && screen_ == UiScreen::BankMembers) {
            screen_ = UiScreen::BankAdd;
            bank_add_cursor_ = 0;
            refresh_bank_list();
            return;
        }
        if (input.key == UiKey::Up || input.key == UiKey::Down) {
            if (list_.count) {
                auto &cursor = screen_ == UiScreen::BankMembers ? bank_cursor_ : bank_add_cursor_;
                cursor = (list_.cursor + list_.count + (input.key == UiKey::Down ? 1 : -1)) %
                         list_.count;
                refresh_bank_list();
            }
        } else if (input.key == UiKey::Enter && list_.count) {
            if (screen_ == UiScreen::BankMembers) {
                bank_form_cursor_ = 0;
                screen_ = UiScreen::BankActions;
            } else {
                const auto id = list_.rows[list_.cursor % 4].id;
                if (bank_contains(bank_draft_, id)) {
                    error_ = -EEXIST;
                } else if (bank_draft_.count == channel_capacity) {
                    error_ = -ENOSPC;
                } else {
                    bank_draft_.channel_ids[bank_draft_.count++] = id;
                    bank_cursor_ = bank_draft_.count - 1;
                    refresh_bank_list();
                }
            }
        }
        return;
    }
    if (input.key == UiKey::Up || input.key == UiKey::Down) {
        bank_form_cursor_ = (bank_form_cursor_ + (input.key == UiKey::Down ? 1 : 3)) % 4;
        return;
    }
    if (screen_ == UiScreen::BankActions) {
        if (input.key != UiKey::Enter) {
            return;
        }
        const unsigned position = bank_cursor_;
        if (position >= bank_draft_.count) {
            error_ = -ESTALE;
            return;
        }
        if (bank_form_cursor_ < 2) {
            if ((!bank_form_cursor_ && !position) ||
                (bank_form_cursor_ == 1 && position + 1 == bank_draft_.count)) {
                return;
            }
            const unsigned next = bank_form_cursor_ ? position + 1 : position - 1;
            const auto id = bank_draft_.channel_ids[position];
            bank_draft_.channel_ids[position] = bank_draft_.channel_ids[next];
            bank_draft_.channel_ids[next] = id;
            bank_cursor_ = next;
        } else if (bank_form_cursor_ == 2) {
            memmove(bank_draft_.channel_ids + position, bank_draft_.channel_ids + position + 1,
                    (bank_draft_.count - position - 1) * sizeof(uint32_t));
            bank_draft_.channel_ids[--bank_draft_.count] = 0;
            if (bank_cursor_ && bank_cursor_ == bank_draft_.count) {
                --bank_cursor_;
            }
        }
        screen_ = UiScreen::BankMembers;
        refresh_bank_list();
        return;
    }
    if (input.key == UiKey::Right) {
        bank_deleting_ = false;
        save_bank();
        return;
    }
    if (input.key != UiKey::Enter) {
        return;
    }
    switch (bank_form_cursor_) {
    case 0:
        error_ = text_editor_.begin(TextKind::Name, bank_draft_.name);
        if (!error_) {
            screen_ = UiScreen::BankName;
        }
        break;
    case 1:
        screen_ = UiScreen::BankMembers;
        bank_cursor_ = 0;
        refresh_bank_list();
        break;
    case 2:
        bank_deleting_ = false;
        save_bank();
        break;
    case 3:
        if (bank_draft_.id) {
            bank_deleting_ = true;
            screen_ = UiScreen::BankDelete;
        }
        break;
    }
#else
    (void)input;
#endif
}

void UiModel::bank_actions(const char *(&actions)[4]) const {
    actions[0] = "OK Edit";
    actions[1] = "BACK Cancel";
    actions[2] = "";
    actions[3] = "P2 Save";
    switch (screen_) {
    case UiScreen::BankEditor:
        if (bank_form_cursor_ == 2) {
            actions[0] = "OK Save";
        }
        if (bank_form_cursor_ == 3) {
            actions[0] = bank_draft_.id ? "OK Review" : "";
        }
        break;
    case UiScreen::BankName:
        actions[0] = "OK Use";
        actions[2] = "P1 Erase";
        actions[3] = "P2 Space";
        break;
    case UiScreen::BankMembers:
        actions[0] = list_.ready && list_.count ? "OK Actions" : "";
        actions[1] = "BACK Edit";
        actions[2] = list_.ready ? "P1 Add" : "";
        actions[3] = "P2 Done";
        break;
    case UiScreen::BankAdd:
        actions[0] =
            list_.ready && list_.count && !list_.rows[list_.cursor % 4].suffix[0] ? "OK Add" : "";
        actions[1] = "BACK Done";
        actions[3] = "P2 Done";
        break;
    case UiScreen::BankActions:
        actions[0] = bank_form_cursor_ == 2   ? "OK Remove"
                     : bank_form_cursor_ == 3 ? "OK Done"
                                              : "OK Move";
        if ((!bank_form_cursor_ && !bank_cursor_) ||
            (bank_form_cursor_ == 1 && bank_cursor_ + 1 >= bank_draft_.count)) {
            actions[0] = "";
        }
        actions[3] = "";
        break;
    case UiScreen::BankDelete:
        actions[0] = "OK Delete";
        actions[3] = "";
        break;
    case UiScreen::BankSaved:
        actions[0] = "OK Done";
        actions[1] = "BACK Done";
        actions[3] = "";
        break;
    default:
        break;
    }
    if (bank_pending_) {
        for (auto &action : actions) {
            action = "";
        }
    }
}

void UiModel::bank_lines(char (&text)[8][32]) const {
    if (screen_ == UiScreen::BankEditor) {
        strcpy(text[0], bank_draft_.id ? "EDIT BANK" : "NEW BANK");
        strcpy(text[1], "Local draft / explicit Save");
        snprintf(text[2], 32, "%c Name: %.23s", bank_form_cursor_ == 0 ? '>' : ' ',
                 bank_draft_.name);
        snprintf(text[3], 32, "%c Members: %u", bank_form_cursor_ == 1 ? '>' : ' ',
                 bank_draft_.count);
        snprintf(text[4], 32, "%c Save changes", bank_form_cursor_ == 2 ? '>' : ' ');
        snprintf(text[5], 32, "%c %s", bank_form_cursor_ == 3 ? '>' : ' ',
                 bank_draft_.id ? "Delete bank" : "");
    } else if (screen_ == UiScreen::BankName) {
        strcpy(text[0], "BANK NAME");
        strcpy(text[1], "Keypad multi-tap / # Next");
        strcpy(text[2], text_editor_.text());
        strcpy(text[4], "Up/Down cursor / # Next");
    } else if (screen_ == UiScreen::BankMembers || screen_ == UiScreen::BankAdd) {
        strcpy(text[0], list_.title);
        strcpy(text[1], list_.ready ? list_.detail : "Changed; cancel and reopen");
    } else if (screen_ == UiScreen::BankActions) {
        strcpy(text[0], "MEMBER ACTIONS");
        snprintf(text[1], 32, "Position %u of %u", bank_cursor_ + 1, bank_draft_.count);
        static const char *names[] = {"Move up", "Move down", "Remove from bank", "Done"};
        for (unsigned row = 0; row < 4; ++row) {
            snprintf(text[row + 2], 32, "%c %s", bank_form_cursor_ == row ? '>' : ' ', names[row]);
        }
    } else if (screen_ == UiScreen::BankDelete) {
        strcpy(text[0], "DELETE BANK?");
        snprintf(text[1], 32, "Bank ID %u", bank_draft_.id);
        strcpy(text[2], bank_original_name_);
        strcpy(text[3], "Channels will be kept");
        strcpy(text[5], "Confirm with green OK");
    } else if (screen_ == UiScreen::BankSaved) {
        strcpy(text[0], bank_deleting_                           ? "BANK DELETED"
                        : edit_save_pending_ || edit_save_error_ ? "BANK ACCEPTED"
                                                                 : "BANK SAVED");
        strcpy(text[1], edit_save_error_     ? "Storage error / unsaved"
                        : edit_save_pending_ ? "Saving; keep power on"
                                             : "Durably saved");
        strcpy(text[2], bank_deleting_ ? bank_original_name_ : bank_draft_.name);
        snprintf(text[3], 32, "%u members", bank_draft_.count);
        strcpy(text[5], bank_deleting_                           ? "Channels retained"
                        : edit_save_error_ || edit_save_pending_ ? "Not durable yet"
                                                                 : "Ready");
    }
    if (error_) {
        const char *message = "Check bank / name";
        switch (error_) {
        case -ENOSPC:
            message = screen_ == UiScreen::BankName ? "Name is full" : "Bank limit reached";
            break;
        case -EEXIST:
            message = "Already in this bank";
            break;
        case -ESTALE:
            message = "Changed; cancel and reopen";
            break;
        case -EBUSY:
            message = "Radio busy; try again";
            break;
        case -EROFS:
            message = "Storage is read-only";
            break;
        case -EAGAIN:
            message = "List refreshed; try again";
            break;
        }
        strcpy(text[6], message);
    }
}
} // namespace ht
