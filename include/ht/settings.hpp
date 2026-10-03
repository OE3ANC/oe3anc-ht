// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/radio.hpp>
#include <stddef.h>

namespace ht {
struct SettingsStatus {
    int load_error = 0;
    int save_error = 0;
    bool pending = false;
    bool read_only = false; // Preserve corrupt/unsupported records until explicit recovery.
    uint32_t generation = 0;
    bool shutdown_pending = false; // bounded inactive-save window, not dirty state
    int shutdown_error = 0;        // last episode result; pending data remains on failure
    Selection selection;
    uint16_t channel_count = 0;
    uint8_t bank_count = 0;
    uint32_t operation_id = 0;
    uint32_t operation_object_id = 0; // Created/edited/deleted stable ID, zero on error/recall.
    int operation_error = 0;
    bool operation_pending = false; // RF acceptance, separate from durable save.
    uint32_t revision = 0; // RAM data revision for drafts, independent of storage generation.
};

// Load before starting radio/worker threads. Failures return safe defaults.
int settings_start(RadioConfig &config);
// Application startup must pass the loaded selection to radio_start as well.
int settings_start(RadioConfig &config, Selection &selection);
// One settings thread owns service and storage I/O. Pass authoritative radio
// snapshots, never UI drafts or raw diagnostic values. The clock is uptime ms.
// Ordinary changes save after 10 seconds without further persistent changes.
// Explicit Save/Apply bypasses that delay.
// Inactive episodes force pending accepted data inside the configured budget;
// errors/expiry stop retries until resume. Cold inactive startup does not flush.
void settings_service(const RadioState &state, int64_t now_ms);
SettingsStatus settings_status();

#ifdef CONFIG_HT_CODEPLUG_STORAGE
struct Channel;
struct Bank;
struct OperatingConfig;
struct UiPreferences;
struct Codeplug;

// Retained result for the last whole replacement, independent of subsequent
// local operation IDs/acks. Durable means a successful complete save after the
// applied revision; later ordinary local edits may be included in that save.
struct SettingsReplacementStatus {
    uint32_t id = 0, revision = 0, generation = 0;
    bool pending = false, durable = false;
    int error = 0, save_error = 0;
};

SettingsReplacementStatus settings_replacement_status();
// Copied bounded readbacks; no borrowed pointers into the owner database.
// Complete applied data and its status/revision under one lock. The caller
// must keep the 36 KiB output in static storage, never on a thread stack.
// A pending operation is not included until controller acceptance/publication.
void settings_copy_codeplug(Codeplug &snapshot, SettingsStatus &snapshot_status);
// One copied whole replacement in the same operation slot as local edits.
// Validates every record and target capability before controller submission;
// rejects a stale RAM revision, preserves max(destination/import) ID counters,
// and publishes the whole RAM database only after the controller accepts it.
// Zero return is queue acceptance, not applied/durable success. Observe the
// matching operation_* result, then pending/save_error/generation for storage.
int settings_replace_codeplug(const Codeplug &replacement, uint32_t request_id,
                              const RadioState &expected, uint32_t revision);
int settings_channel(uint32_t id, Channel &channel, uint32_t *revision = nullptr);
int settings_bank(uint32_t id, Bank &bank, uint32_t *revision = nullptr);
// Coherent display metadata without copying a full bank onto the UI stack.
// Missing identities leave all output arguments unchanged.
int settings_selection_labels(const Selection &selection, char (&name)[25], uint16_t &number,
                              char (&bank)[25]);
void settings_vfo(OperatingConfig &configuration, uint32_t *revision = nullptr);
int settings_channel_number(uint16_t number, Channel &channel, uint32_t *revision = nullptr);
// All channels uses number order; a bank uses its explicit membership order.
int settings_channel_at(uint32_t bank_id, uint16_t index, Channel &channel,
                        uint32_t *revision = nullptr);
// Position in number order (All) or explicit bank order; one bounded scan.
// Missing bank/channel/membership leaves position unchanged.
int settings_channel_position(uint32_t bank_id, uint32_t channel_id, uint16_t &position);
int settings_bank_at(uint8_t index, Bank &bank, uint32_t *revision = nullptr);
int settings_free_channel_number(uint16_t &number);
// Applied preferences only; local UI previews never enter the owner database.
void settings_ui_preferences(UiPreferences &preferences, uint32_t *revision = nullptr);
// Explicit Apply shares the operation slot and receiving-state authorization
// with channel/bank edits, without RF retune. Copies the validated draft;
// operation_* reports RAM acceptance and pending/save_error reports durability.
// Global integer-Hz VFO preference. Same copied revision/metadata Apply contract.
uint32_t settings_vfo_step(uint32_t *revision = nullptr);
int settings_put_vfo_step(uint32_t hz, uint32_t request_id, const RadioState &expected,
                          uint32_t revision);
int settings_put_ui_preferences(const UiPreferences &draft, uint32_t request_id,
                                const RadioState &expected, uint32_t revision);
// One copied recall in flight. id must be nonzero; read operation_* in status
// before submitting another. expected contributes only generation/revision;
// stale drafts, TX/diagnostics/inactive/fault and protected stores are rejected.
// Acceptance is not RF completion or durable save. Call from thread context.
// Optional copied-read RAM revision; zero uses latest owner data. Nonzero
// rejects changes between the copied lookup and owner execution with -ESTALE.
int settings_recall(const Selection &selection, uint32_t id, const RadioState &expected,
                    uint32_t revision = 0);
// Same one-operation slot/ack as recall. Capture the copied draft's RAM revision;
// a stale revision fails without changes. id=0 in a Channel/Bank creates a new
// stable ID (also used for explicit duplicates). Put channel can atomically add
// it to one existing bank; zero skips membership changes. UI owns confirmations.
int settings_put_channel(const Channel &draft, uint32_t request_id, const RadioState &expected,
                         uint32_t revision, uint32_t add_to_bank = 0);
int settings_delete_channel(uint32_t channel_id, uint32_t request_id, const RadioState &expected,
                            uint32_t revision);
int settings_put_bank(const Bank &draft, uint32_t request_id, const RadioState &expected,
                      uint32_t revision);
int settings_delete_bank(uint32_t bank_id, uint32_t request_id, const RadioState &expected,
                         uint32_t revision);
#endif

// Selected storage backend. No retained pointers; load checks exact length.
// Save returns -EAGAIN when flash must wait for an idle radio reservation.
int settings_storage_init();
} // namespace ht
