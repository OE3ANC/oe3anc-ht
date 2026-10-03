// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/companion_protocol.hpp>

namespace ht {
namespace companion {
// Serial-worker-owned coherent read transfer. UI publication has its own lock.
class UiTransfer {
  public:
    void handle(const Frame &, Frame &);

    void reset() {
        token_ = offset_ = length_ = 0;
    }

  private:
    uint8_t bytes_[UI_MAX_BYTES] = {};
    uint32_t token_ = 0;
    uint16_t offset_ = 0, length_ = 0;
};
} // namespace companion
} // namespace ht
