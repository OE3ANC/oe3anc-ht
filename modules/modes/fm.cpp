// SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// RSSI threshold/hysteresis and tone gating from reference OpMode_FM.cpp.
#include <ht/fm.hpp>

namespace ht {
bool FmSquelch::update(int16_t rssi_dbm, bool tone_detected) {
    const int threshold = -127 + (level_ * 66) / 15;
    if (!open_ && rssi_dbm > threshold + 1) {
        open_ = true;
    } else if (open_ && rssi_dbm < threshold - 1) {
        open_ = false;
    }
    return tone_ ? tone_detected : open_;
}
} // namespace ht
