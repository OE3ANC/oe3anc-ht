// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

// KISS FFT is configured in fixed caller buffers. A future allocation path
// must fail rather than silently consume the application heap.
#define KISS_FFT_MALLOC(bytes) NULL
#define KISS_FFT_FREE(pointer) ((void)0)
