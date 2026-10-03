// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/radio.hpp>
#include <ht/voice.hpp>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#ifdef CONFIG_HT_BATTERY
#include <ht/battery.hpp>
#endif
#ifdef CONFIG_HT_AUDIO
#include <ht/audio.hpp>
#endif
#ifdef CONFIG_THREAD_ANALYZER
#include <zephyr/debug/thread_analyzer.h>
#endif

LOG_MODULE_REGISTER(ht_resources);

static void report(void *, void *, void *) {
    while (true) {
        k_sleep(K_SECONDS(15));
#ifdef CONFIG_THREAD_ANALYZER
        thread_analyzer_print();
#endif
        STRUCT_SECTION_FOREACH(k_heap, heap) {
            sys_memory_stats stats{};
            const auto key = k_spin_lock(&heap->lock);
            const int error = sys_heap_runtime_stats_get(&heap->heap, &stats);
            k_spin_unlock(&heap->lock, key);
            if (!error)
                LOG_INF("heap %p capacity=%zu used=%zu peak=%zu free=%zu", heap,
                        heap->heap.init_bytes, stats.allocated_bytes, stats.max_allocated_bytes,
                        stats.free_bytes);
        }
        ht::m17::VoiceHeapUsage codec;
        const int codec_error = ht::m17::voice_heap_usage(codec);
        if (!codec_error)
            LOG_INF("Codec2 heap used=%zu peak=%zu free=%zu", codec.used_bytes, codec.peak_bytes,
                    codec.free_bytes);
        else
            LOG_INF("Codec2 heap sampling unavailable: %d", codec_error);
#ifdef CONFIG_HT_AUDIO
        const auto audio = ht::audio_status();
        LOG_INF("audio error=%d FM discard=%u silence speaker=%u radio=%u", audio.error,
                audio.fm_samples_discarded, audio.silence_samples[0], audio.silence_samples[1]);
#endif
        const auto radio = ht::radio_snapshot();
#ifdef CONFIG_HT_BATTERY
        const auto battery = ht::battery_snapshot();
        if (battery.freshness == ht::BatteryFreshness::Fresh) {
            LOG_INF("battery %u mV charger-input=%u switch-on=%u", battery.reading.millivolts,
                    unsigned(battery.reading.charger_input), unsigned(battery.reading.switch_on));
        } else {
            LOG_INF("battery freshness=%u error=%d last-sample=%lld", unsigned(battery.freshness),
                    battery.error, (long long)battery.sample_ms);
        }
#endif
        LOG_INF("radio phase=%u mode=%u fault=%d", unsigned(radio.phase),
                unsigned(radio.config.mode), radio.fault);
    }
}

K_THREAD_DEFINE(resource_thread, 2048, report, nullptr, nullptr, nullptr, 11, 0, 0);
