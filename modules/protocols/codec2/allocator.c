// SPDX-License-Identifier: GPL-3.0-or-later
#include "allocator.h"
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>

#ifdef CONFIG_BOARD_C62
#define CODEC_RAM __attribute__((section(".psram_section")))
#else
#define CODEC_RAM
#endif
static uint8_t heap_buffer[32768] CODEC_RAM __aligned(8);
static struct k_heap heap;
static bool initialized;
#ifdef CONFIG_ZTEST
static int fail_after = -1;
static unsigned attempts;
#endif

void ht_codec2_heap_init(void) {
    if (!initialized) {
        k_heap_init(&heap, heap_buffer, sizeof(heap_buffer));
        initialized = true;
    }
}

void *ht_codec2_malloc(size_t size) {
#ifdef CONFIG_ZTEST
    ++attempts;
    if (fail_after == 0)
        return NULL;
    if (fail_after > 0)
        --fail_after;
#endif
    return k_heap_alloc(&heap, size, K_NO_WAIT);
}

void *ht_codec2_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size)
        return NULL;
    void *memory = ht_codec2_malloc(count * size);
    if (memory)
        memset(memory, 0, count * size);
    return memory;
}

void ht_codec2_free(void *pointer) {
    if (pointer)
        k_heap_free(&heap, pointer);
}
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
int ht_codec2_heap_stats(struct sys_memory_stats *stats) {
    // Caller holds codec_lock, also serializing heap initialization.
    ht_codec2_heap_init();
    return sys_heap_runtime_stats_get(&heap.heap, stats);
}
#endif
#ifdef CONFIG_ZTEST
void ht_codec2_test_fail_after(int successful_allocations) {
    fail_after = successful_allocations;
    attempts = 0;
}

unsigned ht_codec2_test_attempts(void) {
    return attempts;
}

size_t ht_codec2_test_heap_used(void) {
    if (!initialized)
        return 0;
    struct sys_memory_stats stats;
    sys_heap_runtime_stats_get(&heap.heap, &stats);
    return stats.allocated_bytes;
}
#endif
