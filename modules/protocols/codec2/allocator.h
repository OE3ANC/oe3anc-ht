// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
// Private codec heap. VoiceCodec holds the module lock during every call.
void ht_codec2_heap_init(void);
void *ht_codec2_malloc(size_t size);
void *ht_codec2_calloc(size_t count, size_t size);
void ht_codec2_free(void *pointer);
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
struct sys_memory_stats;
int ht_codec2_heap_stats(struct sys_memory_stats *stats);
#endif
#ifdef CONFIG_ZTEST
void ht_codec2_test_fail_after(int successful_allocations);
unsigned ht_codec2_test_attempts(void);
size_t ht_codec2_test_heap_used(void);
#endif
#ifdef __cplusplus
}
#else
// Forced include for this library's C files only. Include libc declarations
// first, then route all pinned Codec2/KISS allocation sites to the same heap.
#define malloc ht_codec2_malloc
#define calloc ht_codec2_calloc
#define free ht_codec2_free
#endif
