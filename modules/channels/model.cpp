// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug.hpp>
#include <errno.h>
#include <string.h>

namespace ht {
namespace {
bool frequency(uint32_t hz) {
    return (hz >= 136000000 && hz <= 174000000) || (hz >= 400000000 && hz <= 480000000);
}

bool name_valid(const char (&name)[radio_name_size]) {
    if (!name[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(name); ++i) {
        if (!name[i]) {
            return true;
        }
        if (name[i] < ' ' || name[i] > '~') {
            return false;
        }
    }
    return false;
}

bool counts_valid(const Codeplug &p) {
    return p.channel_count <= channel_capacity && p.bank_count <= bank_capacity;
}

bool contains(const Bank &bank, uint32_t id) {
    if (bank.count > channel_capacity) {
        return false;
    }
    for (size_t i = 0; i < bank.count; ++i) {
        if (bank.channel_ids[i] == id) {
            return true;
        }
    }
    return false;
}

int channel_valid(const Channel &channel) {
    if (channel.number < 1 || channel.number > channel_capacity || !name_valid(channel.name)) {
        return -EINVAL;
    }
    return validate_operating(channel.configuration);
}

int bank_valid(const Codeplug &p, const Bank &bank) {
    const int validation = validate_bank(bank);
    if (validation) {
        return validation;
    }
    for (size_t i = 0; i < bank.count; ++i) {
        if (!find_channel(p, bank.channel_ids[i])) {
            return -ENOENT;
        }
    }
    return 0;
}

int selection_valid(const Codeplug &p, const Selection &s) {
    if (s.operating != Operating::Vfo && s.operating != Operating::Memory) {
        return -EINVAL;
    }
    const Bank *bank = s.bank_id ? find_bank(p, s.bank_id) : nullptr;
    if ((s.bank_id && !bank) || (s.channel_id && !find_channel(p, s.channel_id))) {
        return -ENOENT;
    }
    if (s.operating == Operating::Memory && !s.channel_id) {
        return -EINVAL;
    }
    return bank && s.channel_id && !contains(*bank, s.channel_id) ? -EINVAL : 0;
}
} // namespace

int validate_operating(const OperatingConfig &c) {
    if (!frequency(c.rx_frequency_hz) || !frequency(c.tx_frequency_hz) || !c.power_mw ||
        c.power_mw > 5000 || c.squelch > 15 || (c.mode != Mode::Fm && c.mode != Mode::M17) ||
        (c.bandwidth != Bandwidth::Narrow && c.bandwidth != Bandwidth::Wide) ||
        !valid_tone(c.rx_tone) || !valid_tone(c.tx_tone) || !valid_m17_settings(c.m17)) {
        return -EINVAL;
    }
    if (c.mode == Mode::Fm) {
        return c.m17.destination != Destination::Broadcast || c.m17.can || c.m17.rx_can_check
                   ? -EINVAL
                   : 0;
    }
    return c.rx_tone.kind != ToneKind::None || c.tx_tone.kind != ToneKind::None ? -EINVAL : 0;
}

bool bank_contains(const Bank &bank, uint32_t channel_id) {
    return channel_id && contains(bank, channel_id);
}

int validate_ui_preferences(const UiPreferences &u) {
    if (u.theme > Theme::TerminalIce || u.contrast > Contrast::Maximum ||
        (u.brightness_percent != 25 && u.brightness_percent != 50 && u.brightness_percent != 75 &&
         u.brightness_percent != 100) ||
        (u.idle_s != 0 && u.idle_s != 15 && u.idle_s != 30 && u.idle_s != 60) ||
        (u.dim_percent != 10 && u.dim_percent != 20 && u.dim_percent != 30)) {
        return -EINVAL;
    }
    return 0;
}

bool valid_vfo_step(uint32_t hz) {
    for (auto step : vfo_steps_hz) {
        if (step == hz) {
            return true;
        }
    }
    return false;
}

int validate_global(const GlobalSettings &g) {
    return (g.local_callsign[0] && !valid_callsign(g.local_callsign)) || g.gain > 15 ||
                   (!valid_transmit_limit(g.transmit_limit_s) || !valid_vfo_step(g.vfo_step_hz))
               ? -EINVAL
               : validate_ui_preferences(g.ui);
}

int validate_channel(const Channel &channel) {
    return channel.id ? channel_valid(channel) : -EINVAL;
}

int validate_bank(const Bank &bank) {
    // A zero ID is permitted for creation drafts; record codec rejects it.
    if (!name_valid(bank.name) || bank.count > channel_capacity) {
        return -EINVAL;
    }
    for (size_t i = 0; i < bank.count; ++i) {
        if (!bank.channel_ids[i]) {
            return -EINVAL;
        }
        for (size_t j = 0; j < i; ++j) {
            if (bank.channel_ids[i] == bank.channel_ids[j]) {
                return -EEXIST;
            }
        }
    }
    return 0;
}

const Channel *find_channel(const Codeplug &p, uint32_t id) {
    if (!id || !counts_valid(p)) {
        return nullptr;
    }
    for (size_t i = 0; i < p.channel_count; ++i) {
        if (p.channels[i].id == id) {
            return &p.channels[i];
        }
    }
    return nullptr;
}

const Channel *find_channel_number(const Codeplug &p, uint16_t number) {
    if (!counts_valid(p)) {
        return nullptr;
    }
    for (size_t i = 0; i < p.channel_count; ++i) {
        if (p.channels[i].number == number) {
            return &p.channels[i];
        }
    }
    return nullptr;
}

const Bank *find_bank(const Codeplug &p, uint32_t id) {
    if (!id || !counts_valid(p)) {
        return nullptr;
    }
    for (size_t i = 0; i < p.bank_count; ++i) {
        if (p.banks[i].id == id) {
            return &p.banks[i];
        }
    }
    return nullptr;
}

int validate_codeplug(const Codeplug &p) {
    if (!counts_valid(p) || validate_global(p.global) || validate_operating(p.vfo)) {
        return -EINVAL;
    }
    for (size_t i = 0; i < p.channel_count; ++i) {
        const Channel &c = p.channels[i];
        if (!c.id || c.id > p.channel_id_high_water || channel_valid(c)) {
            return -EINVAL;
        }
        for (size_t j = 0; j < i; ++j) {
            if (c.id == p.channels[j].id || c.number == p.channels[j].number) {
                return -EEXIST;
            }
        }
    }
    for (size_t i = 0; i < p.bank_count; ++i) {
        const Bank &b = p.banks[i];
        if (!b.id || b.id > p.bank_id_high_water) {
            return -EINVAL;
        }
        const int error = bank_valid(p, b);
        if (error) {
            return error;
        }
        for (size_t j = 0; j < i; ++j) {
            if (b.id == p.banks[j].id) {
                return -EEXIST;
            }
        }
    }
    return selection_valid(p, p.selection);
}

int check_selection(const Codeplug &p, const Selection &selection) {
    return counts_valid(p) ? selection_valid(p, selection) : -EINVAL;
}

int check_channel_update(const Codeplug &p, const Channel &draft) {
    if (!counts_valid(p) || channel_valid(draft)) {
        return -EINVAL;
    }
    const Channel *existing = find_channel(p, draft.id);
    if (draft.id && !existing) {
        return -ENOENT;
    }
    const Channel *number = find_channel_number(p, draft.number);
    if (number && number != existing) {
        return -EEXIST;
    }
    if (!existing && p.channel_count == channel_capacity) {
        return -ENOSPC;
    }
    if (!existing && p.channel_id_high_water == UINT32_MAX) {
        return -EOVERFLOW;
    }
    return 0;
}

int put_channel(Codeplug &p, const Channel &draft, uint32_t &id) {
    const int error = check_channel_update(p, draft);
    if (error) {
        return error;
    }
    const Channel *existing = find_channel(p, draft.id);
    Channel saved = draft; // Also permits a draft borrowed from this store.
    if (existing) {
        p.channels[existing - p.channels] = saved;
    } else {
        saved.id = ++p.channel_id_high_water;
        p.channels[p.channel_count++] = saved;
    }
    id = saved.id;
    return 0;
}

int delete_channel(Codeplug &p, uint32_t id) {
    const Channel *c = find_channel(p, id);
    if (!c) {
        return -ENOENT;
    }
    const size_t index = c - p.channels;
    memmove(p.channels + index, p.channels + index + 1,
            (p.channel_count - index - 1) * sizeof(Channel));
    p.channels[--p.channel_count] = {};
    for (size_t i = 0; i < p.bank_count; ++i) {
        Bank &b = p.banks[i];
        size_t kept = 0;
        for (size_t j = 0; j < b.count; ++j) {
            if (b.channel_ids[j] != id) {
                b.channel_ids[kept++] = b.channel_ids[j];
            }
        }
        memset(b.channel_ids + kept, 0, (b.count - kept) * sizeof(uint32_t));
        b.count = kept;
    }
    if (p.selection.channel_id == id) {
        p.selection.channel_id = 0;
        p.selection.operating = Operating::Vfo;
    }
    return 0;
}

int check_bank_update(const Codeplug &p, const Bank &draft) {
    if (!counts_valid(p)) {
        return -EINVAL;
    }
    const int error = bank_valid(p, draft);
    if (error) {
        return error;
    }
    const Bank *existing = find_bank(p, draft.id);
    if (draft.id && !existing) {
        return -ENOENT;
    }
    if (!existing && p.bank_count == bank_capacity) {
        return -ENOSPC;
    }
    if (!existing && p.bank_id_high_water == UINT32_MAX) {
        return -EOVERFLOW;
    }
    return 0;
}

int put_bank(Codeplug &p, const Bank &draft, uint32_t &id) {
    const int error = check_bank_update(p, draft);
    if (error) {
        return error;
    }
    const Bank *existing = find_bank(p, draft.id);
    Bank saved = draft;
    if (existing) {
        p.banks[existing - p.banks] = saved;
    } else {
        saved.id = ++p.bank_id_high_water;
        p.banks[p.bank_count++] = saved;
    }
    if (p.selection.bank_id == saved.id && p.selection.channel_id &&
        !contains(saved, p.selection.channel_id)) {
        p.selection.bank_id = 0;
    }
    id = saved.id;
    return 0;
}

int delete_bank(Codeplug &p, uint32_t id) {
    const Bank *b = find_bank(p, id);
    if (!b) {
        return -ENOENT;
    }
    const size_t index = b - p.banks;
    memmove(p.banks + index, p.banks + index + 1, (p.bank_count - index - 1) * sizeof(Bank));
    p.banks[--p.bank_count] = {};
    if (p.selection.bank_id == id) {
        p.selection.bank_id = 0;
    }
    return 0;
}

int select_operating(Codeplug &p, const Selection &s) {
    const int error = check_selection(p, s);
    if (!error) {
        p.selection = s;
    }
    return error;
}
} // namespace ht
