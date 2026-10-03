// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <errno.h>
#include <ht/companion_contract.hpp>
#include <ht/ui_presentation.hpp>
#include <stddef.h>
#include <stdint.h>

namespace ht {
// Explicit bounded UI snapshot schema 1; no native struct bytes or pointers.
int ui_encode_presentation(const UiPresentation &, uint8_t *, size_t capacity, size_t &length);
// Output is staging data and may be partial on failure; publish only on success.
int ui_decode_presentation(const uint8_t *, size_t length, UiPresentation &);

// Caller serializes publish/copy (the production UI model mutex). Comparing
// canonical bytes avoids revisions caused by struct padding or invisible drafts.
class UiSnapshotStore {
  public:
    int publish(const UiPresentation &, int64_t now_ms);
    int copy(int64_t now_ms, uint8_t *, size_t capacity, size_t &length, uint32_t &revision) const;

  private:
    uint8_t bytes_[companion::UI_MAX_BYTES] = {}, staging_[companion::UI_MAX_BYTES] = {};
    size_t size_ = 0;
    uint32_t revision_ = 0;
    int64_t updated_ = 0;
    int error_ = -EAGAIN; // No UI publication yet.
};
} // namespace ht
