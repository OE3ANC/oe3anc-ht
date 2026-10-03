// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/battery_backend.hpp>
#include <zephyr/kernel.h>

namespace ht {
static K_MUTEX_DEFINE(cache_lock);
static BatterySnapshot cache;
static bool have_reading;
static bool initialized;

const BatteryCapabilities &battery_capabilities() {
    return battery_backend_capabilities();
}

int battery_sample() {
    // The sole sampling owner performs bounded I/O outside the snapshot lock.
    int error = initialized ? 0 : battery_backend_init();
    initialized = !error;
    BatteryReading reading;
    if (!error) {
        error = battery_backend_read(reading);
    }
    if (error > 0) {
        error = -EIO;
    }
    const int64_t now = k_uptime_get();
    k_mutex_lock(&cache_lock, K_FOREVER);
    cache.attempt_ms = now;
    cache.error = error;
    if (!error) {
        cache.reading = reading;
        cache.sample_ms = now;
        have_reading = true;
    }
    cache.freshness = !have_reading ? BatteryFreshness::Unknown
                      : error       ? BatteryFreshness::Stale
                                    : BatteryFreshness::Fresh;
    k_mutex_unlock(&cache_lock);
    return error;
}

BatterySnapshot battery_snapshot() {
    k_mutex_lock(&cache_lock, K_FOREVER);
    BatterySnapshot result = cache;
    if (have_reading && k_uptime_get() - result.sample_ms > battery_freshness_ms) {
        result.freshness = BatteryFreshness::Stale;
    }
    k_mutex_unlock(&cache_lock);
    return result;
}
} // namespace ht
