// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/radio.hpp>

namespace ht {
class FmSquelch {
  public:
    void configure(const RadioConfig &config) {
        level_ = config.squelch;
        tone_ = config.rx_tone.kind != ToneKind::None;
        open_ = false;
    }

    bool update(int16_t rssi_dbm, bool tone_detected);

  private:
    uint8_t level_ = 4;
    bool tone_ = false;
    bool open_ = false;
};
} // namespace ht
