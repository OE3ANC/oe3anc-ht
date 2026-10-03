// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <ht/companion_codeplug.hpp>
#include <stdio.h>
#include <string.h>
using namespace ht;
using namespace ht::companion;
static Codeplug plug;
static uint8_t bytes[CPS_MAX_BYTES];

static void valid(const uint8_t *expected, size_t length) {
    assert(!decode_codeplug(expected, length, plug));
    size_t written = 0;
    assert(!encode_codeplug(plug, bytes, sizeof(bytes), written));
    assert(written == length && !memcmp(bytes, expected, length));
    assert(encode_codeplug(plug, bytes, length - 1, written));
}

static void invalid(const uint8_t *expected, size_t length) {
    assert(decode_codeplug(expected, length, plug));
}

#include "cps-fixtures.hpp"

int main() {
    fixtures();
    puts("C++ CPS binary conformance passed");
}
