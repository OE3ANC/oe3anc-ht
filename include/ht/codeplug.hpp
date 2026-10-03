// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/radio.hpp>
#include <ht/ui_preferences.hpp>
#include <stddef.h>

namespace ht {
constexpr size_t channel_capacity = 256;
constexpr size_t bank_capacity = 16;
constexpr size_t radio_name_size = 25; // 24 printable ASCII characters plus NUL.

// Persistent operating fields only; local callsign and gain are global.
// The inactive mode's fields must be at their defaults (the JSON has one mode).
struct OperatingConfig {
    uint32_t rx_frequency_hz = 433500000;
    uint32_t tx_frequency_hz = 433500000;
    uint32_t power_mw = 1000;
    Mode mode = Mode::Fm;
    Bandwidth bandwidth = Bandwidth::Wide;
    uint8_t squelch = 4;
    bool tx_inhibit = false;
    Tone rx_tone;
    Tone tx_tone;
    M17Settings m17;
};

constexpr uint32_t vfo_steps_hz[] = {1000,  2500,  5000,  6250,  10000,
                                     12500, 20000, 25000, 50000, 100000};
constexpr size_t vfo_step_count = sizeof(vfo_steps_hz) / sizeof(vfo_steps_hz[0]);
bool valid_vfo_step(uint32_t hz);

struct GlobalSettings {
    char local_callsign[10] = {};
    uint8_t gain = 0;
    uint16_t transmit_limit_s = 180; // Zero means off.
    UiPreferences ui;
    uint32_t vfo_step_hz = 12500;
};

struct Channel {
    uint32_t id = 0; // Creation drafts use zero; committed IDs are positive.
    uint16_t number = 1;
    char name[radio_name_size] = {};
    OperatingConfig configuration;
};

struct Bank {
    uint32_t id = 0;
    char name[radio_name_size] = {};
    uint16_t count = 0;
    uint32_t channel_ids[channel_capacity] = {};
};

// Single settings owner. Keep in static target RAM, never on a thread stack or
// in a UI snapshot. UI edits use one Channel/Bank draft and copied selections.
struct Codeplug {
    uint32_t channel_id_high_water = 0;
    uint32_t bank_id_high_water = 0;
    GlobalSettings global;
    OperatingConfig vfo;
    Selection selection;
    uint16_t channel_count = 0;
    uint8_t bank_count = 0;
    Channel channels[channel_capacity];
    Bank banks[bank_capacity];
};

static_assert(sizeof(Codeplug) <= 36 * 1024, "Bound the full in-memory codeplug on both targets");

// Contract validation, independent of currently implemented RF features.
// The controller/importer must additionally validate target capabilities.
int validate_operating(const OperatingConfig &configuration);
int validate_ui_preferences(const UiPreferences &preferences);
int validate_global(const GlobalSettings &settings);
int validate_channel(const Channel &channel); // Includes positive committed ID.
int validate_bank(const Bank &bank); // Allows draft ID zero; full validation checks IDs/references.
int validate_codeplug(const Codeplug &codeplug);
const Channel *find_channel(const Codeplug &codeplug, uint32_t id);
const Channel *find_channel_number(const Codeplug &codeplug, uint16_t number);
const Bank *find_bank(const Codeplug &codeplug, uint32_t id);
bool bank_contains(const Bank &bank, uint32_t channel_id);

// Mutations require a valid, exclusively owned Codeplug. They validate drafts
// before writing and leave the store/counters/output ID unchanged on failure.
// Use id=0 to create, an existing id to edit; explicit duplication uses id=0.
int check_channel_update(const Codeplug &codeplug, const Channel &draft);
int check_bank_update(const Codeplug &codeplug, const Bank &draft);
int check_selection(const Codeplug &codeplug, const Selection &selection);
int put_channel(Codeplug &codeplug, const Channel &draft, uint32_t &id);
int delete_channel(Codeplug &codeplug, uint32_t id);
int put_bank(Codeplug &codeplug, const Bank &draft, uint32_t &id);
int delete_bank(Codeplug &codeplug, uint32_t id);
int select_operating(Codeplug &codeplug, const Selection &selection);
} // namespace ht
