// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/battery.hpp>

namespace ht {
class PowerSwitchFilter {
  public:
    bool update(const BatterySnapshot &sample, int64_t now) {
        if (sample.freshness != BatteryFreshness::Fresh || sample.error || sample.sample_ms < 0 ||
            sample.sample_ms > now || now - sample.sample_ms > battery_freshness_ms) {
            candidate_valid_ = false;
            return active_;
        }
        if (sample.sample_ms == last_sample_) {
            return active_;
        }
        if (sample.sample_ms < last_sample_ ||
            (last_sample_ >= 0 && sample.sample_ms - last_sample_ > battery_freshness_ms)) {
            candidate_valid_ = false;
        }
        last_sample_ = sample.sample_ms;
        if (!candidate_valid_ || candidate_ != sample.reading.switch_on) {
            candidate_valid_ = true;
            candidate_ = sample.reading.switch_on;
            candidate_at_ = sample.sample_ms;
        } else if (sample.sample_ms - candidate_at_ >= 200) {
            active_ = candidate_;
        }
        return active_;
    }

  private:
    bool active_ = false;
    bool candidate_ = false;
    bool candidate_valid_ = false;
    int64_t candidate_at_ = 0;
    int64_t last_sample_ = -1;
};
} // namespace ht
