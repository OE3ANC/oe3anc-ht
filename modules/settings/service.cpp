// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/codeplug_storage.hpp>
#include <ht/settings.hpp>
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
#ifdef CONFIG_BOARD_C62
#define SETTINGS_RAM __attribute__((section(".psram_section")))
#else
#define SETTINGS_RAM
#endif
// One store, constructed/loaded before worker startup; never copied to a stack.
// The settings worker is its sole owner after startup.
static Codeplug database SETTINGS_RAM;
// One bounded complete replacement; readers supply their own static snapshot.
// status_mutex protects this copied input until the owner settles the operation.
static Codeplug replacement SETTINGS_RAM;
static RadioConfig observed;
static RadioConfig saved;
static Selection saved_selection;
static RadioCommand recall;
static uint32_t recall_token;
static bool recall_waiting;
enum class Operation : uint8_t {
    Recall,
    PutChannel,
    DeleteChannel,
    PutBank,
    DeleteBank,
    PutUi,
    PutStep,
    Replace
};

// Local edits keep one small copied draft; complete replacement uses the single
// fixed staging database above. Neither payload lives on a worker stack/heap.
static struct {
    Operation kind;
    uint32_t revision, entity_id, add_to_bank, step_hz;
    Channel channel;
    Bank bank;
    UiPreferences ui;
} request SETTINGS_RAM;

static bool content_dirty;
static uint32_t generation;
static constexpr int64_t autosave_delay_ms = 10000;
static int64_t save_at;
static bool read_only;
static bool shutdown_episode, was_inactive, shutdown_done;
static uint32_t shutdown_sequence;
static int64_t shutdown_until;
static int protection_error;
static SettingsStatus status;
static SettingsReplacementStatus replacement_status;
static K_MUTEX_DEFINE(status_mutex);

static OperatingConfig operating(const RadioConfig &config) {
    OperatingConfig result;
    result.rx_frequency_hz = config.rx_frequency_hz;
    result.tx_frequency_hz = config.tx_frequency_hz;
    result.tx_inhibit = config.tx_inhibit;
    result.power_mw = config.power_mw;
    result.mode = config.mode;
    result.bandwidth = config.bandwidth;
    result.squelch = config.squelch;
    if (config.mode == Mode::Fm) {
        result.rx_tone = config.rx_tone;
        result.tx_tone = config.tx_tone;
    } else {
        result.m17 = config.m17;
    }
    return result;
}

// Typed operating fields survive loading and ordinary tuning unchanged.
static int radio_config(const OperatingConfig &op, const GlobalSettings &global,
                        RadioConfig &config) {
    RadioConfig result;
    result.rx_frequency_hz = op.rx_frequency_hz;
    result.tx_frequency_hz = op.tx_frequency_hz;
    result.tx_inhibit = op.tx_inhibit;
    result.power_mw = op.power_mw;
    result.mode = op.mode;
    result.bandwidth = op.bandwidth;
    result.squelch = op.squelch;
    result.rx_tone = op.rx_tone;
    result.tx_tone = op.tx_tone;
    result.m17 = op.m17;
    result.gain = global.gain;
    result.transmit_limit_s = global.transmit_limit_s;
    result.fm_ctcss_level = global.fm_ctcss_level;
    memcpy(result.callsign, global.local_callsign, sizeof(result.callsign));
    const int error = validate_config(result);
    if (!error) {
        config = result;
    }
    return error;
}

static void update_global(const RadioConfig &config) {
    database.global.gain = config.gain;
    database.global.transmit_limit_s = config.transmit_limit_s;
    database.global.fm_ctcss_level = config.fm_ctcss_level;
    memset(database.global.local_callsign, 0, sizeof(database.global.local_callsign));
    memcpy(database.global.local_callsign, config.callsign, strlen(config.callsign));
}

static bool same_config(const RadioConfig &a, const RadioConfig &b) {
    return a.fm_ctcss_level == b.fm_ctcss_level && same_operating(a, b) && a.gain == b.gain &&
           a.transmit_limit_s == b.transmit_limit_s && !strcmp(a.callsign, b.callsign);
}

static void reset_database() {
    database.channel_id_high_water = database.bank_id_high_water = 0;
    database.global = {};
    database.vfo = {};
    database.selection = {};
    database.channel_count = database.bank_count = 0;
    // Unused array slots are inaccessible and excluded from encoding.
}

int settings_start(RadioConfig &config) {
    Selection selection;
    return settings_start(config, selection);
}

int settings_start(RadioConfig &config, Selection &selection) {
    config = {};
    reset_database();
    generation = 0;
    read_only = false;
    shutdown_episode = was_inactive = shutdown_done = false;
    shutdown_sequence = radio_snapshot().shutdown_sequence;
    shutdown_until = 0;
    int error = settings_storage_init();
    if (!error) {
        error = codeplug_load(database, generation);
        if (!error) {
            RadioConfig checked;
            error = radio_config(database.vfo, database.global, checked);
            for (size_t i = 0; !error && i < database.channel_count; ++i) {
                error = radio_config(database.channels[i].configuration, database.global, checked);
            }
            if (!error) {
                const auto *channel = database.selection.operating == Operating::Memory
                                          ? find_channel(database, database.selection.channel_id)
                                          : nullptr;
                error = radio_config(channel ? channel->configuration : database.vfo,
                                     database.global, config);
            }
        }
    }
    if (error) {
        config = {};
        reset_database();
        database.vfo = operating(config);
        generation = 0;
        read_only = error != -ENOENT;
    }
    protection_error = read_only ? error : 0;
    observed = config;
    radio_config(database.vfo, database.global, saved);
    selection = saved_selection = database.selection;
    recall_waiting = false;
    content_dirty = false;
    save_at = 0;
    k_mutex_lock(&status_mutex, K_FOREVER);
    status = {};
    replacement_status = {};
    status.load_error = error;
    status.pending = false;
    status.read_only = read_only;
    status.generation = generation;
    status.selection = selection;
    status.channel_count = database.channel_count;
    status.bank_count = database.bank_count;
    status.revision = 1;
    k_mutex_unlock(&status_mutex);
    return error;
}

// Called with status_mutex held, only by the settings owner.
static void finish_shutdown(int error) {
    shutdown_done = true;
    status.shutdown_pending = false;
    status.shutdown_error = error;
    if (error) {
        status.save_error = error;
    }
}

// Called with status_mutex held. Compare persistent VFO/global/identity, never
// the active memory's transient squelch or a whole padded C++ object.
static void modified(int64_t now_ms) {
    RadioConfig current;
    radio_config(database.vfo, database.global, current);
    status.pending = content_dirty || !same_config(current, saved) ||
                     !same_selection(database.selection, saved_selection);
    status.selection = database.selection;
    status.channel_count = database.channel_count;
    status.bank_count = database.bank_count;
    ++status.revision;
    save_at = now_ms + autosave_delay_ms;
    status.save_error = read_only ? protection_error : 0;
}

static void finish_recall(int error) {
    status.operation_pending = false;
    status.operation_error = error;
    recall_waiting = false;
    if (request.kind == Operation::Replace) {
        replacement_status.pending = false;
        replacement_status.error = error;
        if (!error) {
            replacement_status.revision = status.revision;
        }
        replacement_status.generation = status.generation;
    }
}

static bool same_ui(const UiPreferences &a, const UiPreferences &b) {
    return a.theme == b.theme && a.contrast == b.contrast && a.animations == b.animations &&
           a.brightness_percent == b.brightness_percent && a.idle_s == b.idle_s &&
           a.dim_percent == b.dim_percent;
}

static int commit_operation(int64_t now_ms) {
    if (request.kind == Operation::Replace) {
        database = replacement;
        observed = recall.config;
        content_dirty = true;
        modified(now_ms);
        save_at = now_ms;
        return 0;
    }
    if (request.kind == Operation::PutStep) {
        if (database.global.vfo_step_hz != request.step_hz) {
            database.global.vfo_step_hz = request.step_hz;
            content_dirty = true;
            modified(now_ms);
            save_at = now_ms;
        }
        return 0;
    }
    if (request.kind == Operation::PutUi) {
        if (!same_ui(database.global.ui, request.ui)) {
            database.global.ui = request.ui;
            content_dirty = true;
            modified(now_ms);
            save_at = now_ms;
        }
        return 0; // Does not change selection, RF configuration or object identity.
    }
    int error = 0;
    uint32_t object_id = request.entity_id;
    switch (request.kind) {
    case Operation::PutChannel:
        error = put_channel(database, request.channel, object_id);
        if (!error && request.add_to_bank) {
            // Prevalidated bank/capacity; single owner, no structural edits in flight.
            Bank &bank = database.banks[find_bank(database, request.add_to_bank) - database.banks];
            if (!bank_contains(bank, object_id)) {
                bank.channel_ids[bank.count++] = object_id;
            }
        }
        break;
    case Operation::DeleteChannel:
        error = delete_channel(database, object_id);
        break;
    case Operation::PutBank:
        error = put_bank(database, request.bank, object_id);
        break;
    case Operation::DeleteBank:
        error = delete_bank(database, object_id);
        break;
    case Operation::Recall:
    case Operation::Replace:
    case Operation::PutUi:
    case Operation::PutStep:
        break;
    }
    if (error) {
        return error;
    }
    const bool selection_changed = !same_selection(database.selection, recall.selection);
    error = select_operating(database, recall.selection);
    if (error) {
        return error;
    }
    if (request.kind != Operation::Recall) {
        content_dirty = true;
        status.operation_object_id = object_id;
    }
    if (selection_changed || request.kind != Operation::Recall) {
        modified(now_ms);
    }
    // An explicit channel/bank Save or preferences Apply starts publication immediately;
    // only ordinary operating/global/selection changes use the debounce.
    if (request.kind != Operation::Recall) {
        save_at = now_ms;
    }
    return 0;
}

static bool acknowledge_recall(const RadioState &state, int64_t now_ms) {
    if (!status.operation_pending || !recall_waiting) {
        return false;
    }
    if (state.recall_id == recall.id && state.recall_generation == recall.expected_generation) {
        const int error = state.recall_error ? state.recall_error : commit_operation(now_ms);
        finish_recall(error);
        return !error && request.kind != Operation::Recall;
    } else if (state.generation != recall.expected_generation) {
        // Restart/off purges queued commands. Never wait forever for that ACK.
        finish_recall(-ESTALE);
    }
    return false;
}

static int prepare_operation(const RadioState &state) {
    if (request.kind == Operation::Recall) {
        const int error = check_selection(database, recall.selection);
        if (error) {
            return error;
        }
    } else {
        recall.kind = CommandKind::Edit;
        recall.selection = state.selection;
        int error = 0;
        switch (request.kind) {
        case Operation::Replace: {
            error = validate_codeplug(replacement);
            RadioConfig checked;
            if (!error) {
                error = radio_config(replacement.vfo, replacement.global, checked);
            }
            for (size_t i = 0; !error && i < replacement.channel_count; ++i) {
                error = radio_config(replacement.channels[i].configuration, replacement.global,
                                     checked);
            }
            if (error) {
                return error;
            }
            if (replacement.channel_id_high_water < database.channel_id_high_water) {
                replacement.channel_id_high_water = database.channel_id_high_water;
            }
            if (replacement.bank_id_high_water < database.bank_id_high_water) {
                replacement.bank_id_high_water = database.bank_id_high_water;
            }
            const auto *channel = replacement.selection.operating == Operating::Memory
                                      ? find_channel(replacement, replacement.selection.channel_id)
                                      : nullptr;
            error = radio_config(channel ? channel->configuration : replacement.vfo,
                                 replacement.global, recall.config);
            if (error) {
                return error;
            }
            recall.kind = CommandKind::Recall;
            recall.selection = replacement.selection;
            return 0;
        }
        case Operation::PutChannel: {
            error = check_channel_update(database, request.channel);
            if (!error) {
                error = radio_config(request.channel.configuration, database.global, recall.config);
            }
            if (!error && request.add_to_bank) {
                const auto *bank = find_bank(database, request.add_to_bank);
                if (!bank) {
                    error = -ENOENT;
                } else if (!bank_contains(*bank, request.channel.id) &&
                           bank->count == channel_capacity) {
                    error = -ENOSPC;
                }
            }
            if (!error && state.selection.operating == Operating::Memory &&
                request.channel.id == state.selection.channel_id) {
                recall.kind = CommandKind::Recall;
                return 0; // Complete new channel config already validated above.
            }
            break;
        }
        case Operation::DeleteChannel:
            if (!find_channel(database, request.entity_id)) {
                error = -ENOENT;
            } else if (recall.selection.channel_id == request.entity_id) {
                recall.selection.operating = Operating::Vfo;
                recall.selection.channel_id = 0;
                if (state.selection.operating == Operating::Memory) {
                    recall.kind = CommandKind::Recall;
                }
            }
            break;
        case Operation::PutBank:
            error = check_bank_update(database, request.bank);
            if (!error && request.bank.id && request.bank.id == recall.selection.bank_id &&
                recall.selection.channel_id &&
                !bank_contains(request.bank, recall.selection.channel_id)) {
                recall.selection.bank_id = 0;
            }
            break;
        case Operation::DeleteBank:
            if (!find_bank(database, request.entity_id)) {
                error = -ENOENT;
            } else if (recall.selection.bank_id == request.entity_id) {
                recall.selection.bank_id = 0;
            }
            break;
        case Operation::PutUi:
            error = validate_ui_preferences(request.ui);
            break;
        case Operation::PutStep:
            error = valid_vfo_step(request.step_hz) ? 0 : -EINVAL;
            break;
        case Operation::Recall:
            break;
        }
        if (error) {
            return error;
        }
    }
    if (recall.kind == CommandKind::Edit) {
        return 0;
    }
    const Channel *channel = recall.selection.operating == Operating::Memory
                                 ? find_channel(database, recall.selection.channel_id)
                                 : nullptr;
    return radio_config(channel ? channel->configuration : database.vfo, database.global,
                        recall.config);
}

static void submit_recall(const RadioState &state) {
    if (!status.operation_pending || recall_waiting) {
        return;
    }
    int error = 0;
    if (read_only) {
        error = -EROFS;
    } else if (state.generation != recall.expected_generation ||
               state.configuration_revision != recall.expected_revision) {
        error = -ESTALE;
    } else if (request.revision && request.revision != status.revision) {
        error = -ESTALE;
    } else if (!state.power_active) {
        error = -EHOSTDOWN;
    } else if (state.fault) {
        error = state.fault;
    } else if (state.phase != RadioPhase::Receiving) {
        error = -EBUSY;
    }
    if (!error) {
        error = prepare_operation(state);
    }
    if (!error) {
        error = radio_submit(recall);
    }
    if (error) {
        finish_recall(error);
    } else {
        recall_waiting = true;
    }
}

void settings_service(const RadioState &state, int64_t now_ms) {
    const bool inactive = state.phase == RadioPhase::Inactive && !state.power_active;
    k_mutex_lock(&status_mutex, K_FOREVER);
    const bool explicit_save = acknowledge_recall(state, now_ms);
    if (state.shutdown_sequence != shutdown_sequence) {
        // A sequence survives a busy settings worker and coalesced off/on.
        // It does not mistake a cold inactive start for a runtime switch-off.
        shutdown_sequence = state.shutdown_sequence;
        shutdown_until = state.shutdown_ms + CONFIG_HT_SHUTDOWN_SAVE_BUDGET_MS;
        shutdown_episode = true;
        shutdown_done = false;
        status.shutdown_error = 0;
    }
    if (!inactive) {
        // An inactive transaction never continues through normal radio resume.
        // Pending data returns to the ordinary debounce/retry policy afterward.
        if (shutdown_episode && !shutdown_done) {
            finish_shutdown(-ECANCELED);
        }
        if (was_inactive && status.pending) {
            save_at = now_ms + autosave_delay_ms;
        }
        shutdown_episode = false;
        status.shutdown_pending = false;
    }
    was_inactive = inactive;
    if (state.phase == RadioPhase::Starting || state.fault || state.phase == RadioPhase::Fault ||
        validate_config(state.config)) {
        if (status.operation_pending && !recall_waiting) {
            submit_recall(state);
        }
        if (inactive && shutdown_episode && !shutdown_done) {
            finish_shutdown(-ECANCELED);
        }
        k_mutex_unlock(&status_mutex);
        return;
    }
    if (!same_config(state.config, observed) ||
        !same_selection(state.selection, database.selection)) {
        RadioConfig vfo;
        radio_config(database.vfo, database.global, vfo);
        const bool changed =
            !same_selection(state.selection, database.selection) ||
            (state.selection.operating == Operating::Vfo && !same_operating(state.config, vfo)) ||
            state.config.fm_ctcss_level != observed.fm_ctcss_level ||
            state.config.gain != observed.gain ||
            state.config.transmit_limit_s != observed.transmit_limit_s ||
            strcmp(state.config.callsign, observed.callsign);
        observed = state.config;
        if (state.selection.operating == Operating::Vfo) {
            // Ordinary tuning is a VFO edit. Committed memories are explicit
            // saves, so temporary tuning never overwrites a stored channel.
            database.vfo = operating(observed);
        }
        select_operating(database, state.selection);
        update_global(observed);
        if (changed) {
            modified(now_ms);
        }
    }
    submit_recall(state);
    // A later ordinary Configure may be part of this snapshot. Reconciliation
    // must not postpone the explicit Save just accepted on the same tick.
    if (explicit_save) {
        save_at = now_ms;
    }
    CodeplugSavePolicy policy{0, SaveRadioState::Active};
    bool should_save = status.pending && !read_only && now_ms >= save_at;
    if (inactive) {
        should_save = false;
        // Cold inactive startup is not a switch-off episode. A controller off
        // transition forces pending accepted settings after RF has stopped.
        if (shutdown_episode && !shutdown_done) {
            if (!status.pending) {
                finish_shutdown(0);
            } else if (read_only) {
                finish_shutdown(protection_error);
            } else if (k_uptime_get() >= shutdown_until) {
                finish_shutdown(-ETIMEDOUT);
            } else {
                status.shutdown_pending = true;
                policy = {shutdown_until, SaveRadioState::Inactive};
                should_save = true;
            }
        }
    }
    k_mutex_unlock(&status_mutex);
    if (!should_save) {
        return;
    }
    // No snapshot mutex spans I/O. C62 records reserve an idle radio state;
    // Linux holds its profile lock and atomically replaces the durable file.
    const int error = codeplug_save(database, generation, policy);
    if (error == -EAGAIN) {
        // A primitive can report deferral after publication. Reconcile visible
        // generation even while dirty data waits for a later bounded retry.
        k_mutex_lock(&status_mutex, K_FOREVER);
        status.generation = generation;
        if (replacement_status.revision && !replacement_status.durable) {
            replacement_status.generation = generation;
        }
        k_mutex_unlock(&status_mutex);
        return;
    } // only deferrals retry inside inactive budget
    k_mutex_lock(&status_mutex, K_FOREVER);
    status.generation = generation;
    status.save_error = error;
    if (replacement_status.revision && !replacement_status.durable) {
        replacement_status.generation = generation;
        replacement_status.save_error = error;
        replacement_status.durable = !error;
    }
    if (error) {
        save_at = now_ms + 1000;
        // A conflicting generation needs an explicit reload/recovery decision.
        // Retrying it cannot make progress and must not overwrite external data.
        if (error == -ESTALE) {
            read_only = status.read_only = true;
            protection_error = error;
        }
    } else {
        radio_config(database.vfo, database.global, saved);
        saved_selection = database.selection;
        content_dirty = false;
        status.pending = false;
    }
    if (inactive) {
        finish_shutdown(error);
    }
    k_mutex_unlock(&status_mutex);
}

SettingsStatus settings_status() {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const auto result = status;
    k_mutex_unlock(&status_mutex);
    return result;
}

void settings_copy_codeplug(Codeplug &snapshot, SettingsStatus &snapshot_status) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    snapshot = database;
    snapshot_status = status;
    k_mutex_unlock(&status_mutex);
}

SettingsReplacementStatus settings_replacement_status() {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const auto result = replacement_status;
    k_mutex_unlock(&status_mutex);
    return result;
}

// status_mutex held. Payloads are assigned directly; no 1 KiB temporary struct.
static int begin_operation(uint32_t id, const RadioState &expected, uint32_t revision) {
    if (!id || !revision) {
        return -EINVAL;
    }
    int error = 0;
    if (read_only) {
        error = -EROFS;
    } else if (status.operation_pending) {
        error = -EBUSY;
    } else if (recall_token == UINT32_MAX) {
        error = -EOVERFLOW;
    } else {
        recall = {};
        recall.kind = CommandKind::Recall;
        recall.id = ++recall_token;
        recall.expected_generation = expected.generation;
        recall.expected_revision = expected.configuration_revision;
        recall_waiting = false;
        request.kind = Operation::Recall;
        request.revision = revision;
        request.entity_id = request.add_to_bank = 0;
        status.operation_id = id;
        status.operation_object_id = 0;
        status.operation_error = 0;
        status.operation_pending = true;
    }
    return error;
}

int settings_replace_codeplug(const Codeplug &draft, uint32_t id, const RadioState &expected,
                              uint32_t revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    // Unlike local recall, a complete replacement always binds the exact read.
    int error = !id || !revision ? -EINVAL : revision != status.revision ? -ESTALE : 0;
    if (!error) {
        error = begin_operation(id, expected, revision);
    }
    if (!error) {
        replacement = draft;
        request.kind = Operation::Replace;
        replacement_status = {};
        replacement_status.id = id;
        replacement_status.pending = true;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

int settings_recall(const Selection &selection, uint32_t id, const RadioState &expected,
                    uint32_t revision) {
    if (!valid_selection(selection)) {
        return -EINVAL;
    }
    k_mutex_lock(&status_mutex, K_FOREVER);
    const int error = begin_operation(id, expected, revision ? revision : status.revision);
    if (!error) {
        recall.selection = selection;
        request.revision = revision; // Zero deliberately recalls the latest owner data.
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

int settings_put_channel(const Channel &draft, uint32_t id, const RadioState &expected,
                         uint32_t revision, uint32_t add_to_bank) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const int error = begin_operation(id, expected, revision);
    if (!error) {
        request.kind = Operation::PutChannel;
        request.channel = draft;
        request.add_to_bank = add_to_bank;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

int settings_delete_channel(uint32_t channel_id, uint32_t id, const RadioState &expected,
                            uint32_t revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const int error = begin_operation(id, expected, revision);
    if (!error) {
        request.kind = Operation::DeleteChannel;
        request.entity_id = channel_id;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

int settings_put_bank(const Bank &draft, uint32_t id, const RadioState &expected,
                      uint32_t revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const int error = begin_operation(id, expected, revision);
    if (!error) {
        request.kind = Operation::PutBank;
        request.bank = draft;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

int settings_delete_bank(uint32_t bank_id, uint32_t id, const RadioState &expected,
                         uint32_t revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const int error = begin_operation(id, expected, revision);
    if (!error) {
        request.kind = Operation::DeleteBank;
        request.entity_id = bank_id;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

uint32_t settings_vfo_step(uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const auto hz = database.global.vfo_step_hz;
    if (revision) {
        *revision = status.revision;
    }
    k_mutex_unlock(&status_mutex);
    return hz;
}

int settings_put_vfo_step(uint32_t hz, uint32_t id, const RadioState &expected, uint32_t revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const int error = begin_operation(id, expected, revision);
    if (!error) {
        request.kind = Operation::PutStep;
        request.step_hz = hz;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

int settings_put_ui_preferences(const UiPreferences &draft, uint32_t id, const RadioState &expected,
                                uint32_t revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const int error = begin_operation(id, expected, revision);
    if (!error) {
        request.kind = Operation::PutUi;
        request.ui = draft;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

void settings_ui_preferences(UiPreferences &preferences, uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    preferences = database.global.ui;
    if (revision) {
        *revision = status.revision;
    }
    k_mutex_unlock(&status_mutex);
}

int settings_channel(uint32_t id, Channel &channel, uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const auto *stored = find_channel(database, id);
    if (stored) {
        channel = *stored;
        if (revision) {
            *revision = status.revision;
        }
    }
    k_mutex_unlock(&status_mutex);
    return stored ? 0 : -ENOENT;
}

int settings_bank(uint32_t id, Bank &bank, uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const auto *stored = find_bank(database, id);
    if (stored) {
        bank = *stored;
        if (revision) {
            *revision = status.revision;
        }
    }
    k_mutex_unlock(&status_mutex);
    return stored ? 0 : -ENOENT;
}

int settings_selection_labels(const Selection &selection, char (&name)[25], uint16_t &number,
                              char (&bank)[25]) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const auto *channel = find_channel(database, selection.channel_id);
    const auto *group = selection.bank_id ? find_bank(database, selection.bank_id) : nullptr;
    const bool found =
        channel && (!selection.bank_id || (group && bank_contains(*group, channel->id)));
    if (found) {
        memcpy(name, channel->name, sizeof(name));
        number = channel->number;
        if (group) {
            memcpy(bank, group->name, sizeof(bank));
        } else {
            strcpy(bank, "All channels");
        }
    }
    k_mutex_unlock(&status_mutex);
    return found ? 0 : -ENOENT;
}

void settings_vfo(OperatingConfig &configuration, uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    configuration = database.vfo;
    if (revision) {
        *revision = status.revision;
    }
    k_mutex_unlock(&status_mutex);
}

int settings_channel_number(uint16_t number, Channel &channel, uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const auto *stored = find_channel_number(database, number);
    if (stored) {
        channel = *stored;
        if (revision) {
            *revision = status.revision;
        }
    }
    k_mutex_unlock(&status_mutex);
    return stored ? 0 : -ENOENT;
}

int settings_channel_at(uint32_t bank_id, uint16_t index, Channel &channel, uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const Channel *stored = nullptr;
    if (bank_id) {
        const auto *bank = find_bank(database, bank_id);
        if (bank && index < bank->count) {
            stored = find_channel(database, bank->channel_ids[index]);
        }
    } else if (index < database.channel_count) {
        // ponytail: bounded 256-record scan, no sorting buffer/cache to keep coherent.
        // UI reads visible rows only; add an index only if measured browsing needs it.
        uint16_t previous = 0;
        for (size_t row = 0; row <= index; ++row) {
            stored = nullptr;
            for (size_t i = 0; i < database.channel_count; ++i) {
                const auto &candidate = database.channels[i];
                if (candidate.number > previous && (!stored || candidate.number < stored->number)) {
                    stored = &candidate;
                }
            }
            previous = stored->number; // Valid store: numbers are unique and index is in range.
        }
    }
    if (stored) {
        channel = *stored;
        if (revision) {
            *revision = status.revision;
        }
    }
    k_mutex_unlock(&status_mutex);
    return stored ? 0 : -ENOENT;
}

int settings_channel_position(uint32_t bank_id, uint32_t channel_id, uint16_t &position) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    int error = -ENOENT;
    uint16_t found = 0;
    if (bank_id) {
        const auto *bank = find_bank(database, bank_id);
        if (bank) {
            for (uint16_t i = 0; i < bank->count; ++i) {
                if (bank->channel_ids[i] == channel_id) {
                    found = i;
                    error = 0;
                    break;
                }
            }
        }
    } else {
        const auto *channel = find_channel(database, channel_id);
        if (channel) {
            for (uint16_t i = 0; i < database.channel_count; ++i) {
                found += database.channels[i].number < channel->number;
            }
            error = 0;
        }
    }
    if (!error) {
        position = found;
    }
    k_mutex_unlock(&status_mutex);
    return error;
}

int settings_bank_at(uint8_t index, Bank &bank, uint32_t *revision) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    const bool found = index < database.bank_count;
    if (found) {
        bank = database.banks[index];
        if (revision) {
            *revision = status.revision;
        }
    }
    k_mutex_unlock(&status_mutex);
    return found ? 0 : -ENOENT;
}

int settings_free_channel_number(uint16_t &number) {
    k_mutex_lock(&status_mutex, K_FOREVER);
    bool used[channel_capacity + 1] = {};
    for (size_t i = 0; i < database.channel_count; ++i) {
        used[database.channels[i].number] = true;
    }
    int error = -ENOSPC;
    for (uint16_t candidate = 1; candidate <= channel_capacity; ++candidate) {
        if (!used[candidate]) {
            number = candidate;
            error = 0;
            break;
        }
    }
    k_mutex_unlock(&status_mutex);
    return error;
}
} // namespace ht
