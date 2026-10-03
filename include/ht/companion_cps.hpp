// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/codeplug.hpp>
#include <ht/companion_protocol.hpp>
#include <ht/settings.hpp>

namespace ht {
namespace companion {
// One companion thread owns the bounded staging/snapshot and transfer state.
// reset() drops transfers/baselines; a complete accepted owner operation is not
// undone on link loss. Keep this object in static target RAM, never a stack.
class Cps {
  public:
    void reset();
    void handle(const Frame &, Frame &);

  private:
    enum class Phase : uint8_t { Idle, Reading, Ready, Writing, Accepted };
    Phase phase_;
    uint32_t token_, length_, offset_, checksum_, operation_id_;
    SettingsStatus baseline_;
    RadioState radio_;
    Codeplug plug_;
    uint8_t bytes_[CPS_MAX_BYTES];
};
} // namespace companion
} // namespace ht
