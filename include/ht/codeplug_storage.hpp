// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/codeplug.hpp>

namespace ht {
// Single settings owner; call settings_storage_init before use. No retained
// caller pointers or heap allocation. Load writes an unpublished staging store:
// discard it on failure, and publish only after a successful complete load.
int codeplug_load(Codeplug &staging, uint32_t &generation);
enum class SaveRadioState : uint8_t { Any, Active, Inactive };

struct CodeplugSavePolicy {
    int64_t deadline_ms = 0; // absolute Zephyr uptime; zero disables deadline
    SaveRadioState radio_state = SaveRadioState::Any;
};

// Cooperative checks between synchronous I/O calls; never abort a primitive or
// claim a hard wall-clock bound on vendor flash/host fsync. Active owner saves
// cancel on off edges; inactive saves cancel on raw on intent. Any is for
// explicit offline storage work. Owner saves also cancel on pending faults.
int codeplug_save_check(const CodeplugSavePolicy &policy);
// generation is the expected committed version (zero for a new profile).
// Changed only when a new generation becomes visible, including an uncertain
// durability error after publication. Such an error must remain pending/retry.
int codeplug_save(const Codeplug &committed, uint32_t &generation,
                  const CodeplugSavePolicy &policy = {});

// Internal backend session, owned by the same settings context. These calls
// implement only persistent-record transport, not an OS/RF abstraction.
enum class StoreRecord : uint8_t { Manifest, Channel, Bank };
int codeplug_store_open(bool writing, uint32_t &generation, const CodeplugSavePolicy &policy = {});
int codeplug_store_read(StoreRecord kind, size_t index, uint8_t *bytes, size_t capacity,
                        size_t &length);
int codeplug_store_write(StoreRecord kind, size_t index, const uint8_t *bytes, size_t length);
// published reports visible replacement even if its final durability check
// fails. Read close verifies EOF; abort closes/releases the session only.
int codeplug_store_close(bool commit, bool &published);
} // namespace ht
