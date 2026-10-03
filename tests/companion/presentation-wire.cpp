// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <ht/ui_presentation_wire.hpp>
#include <stdio.h>
#include <string.h>
using namespace ht;
static UiPresentation view;
static uint8_t encoded[ht::companion::UI_MAX_BYTES];

static void valid(const uint8_t *bytes, size_t size) {
    assert(!ui_decode_presentation(bytes, size, view));
    size_t length = 0;
    assert(!ui_encode_presentation(view, encoded, sizeof(encoded), length));
    assert(length == size && !memcmp(bytes, encoded, size));
}

static void invalid(const uint8_t *bytes, size_t size) {
    assert(ui_decode_presentation(bytes, size, view));
}

#include "ui-fixtures.hpp"

int main() {
    fixtures();
    UiSnapshotStore store;
    size_t length = 0;
    uint32_t revision = 0;
    assert(store.copy(0, encoded, sizeof(encoded), length, revision) == -EAGAIN);
    view = {};
    view.home.visible = true;
    assert(!store.publish(view, 0));
    assert(!store.copy(0, encoded, sizeof(encoded), length, revision));
    assert(revision == 1);
    strcpy(view.text.value, "hidden draft");
    assert(!store.publish(view, 10));
    assert(!store.copy(499, encoded, sizeof(encoded), length, revision));
    assert(revision == 1);
    strcpy(view.home.name, "Changed");
    assert(!store.publish(view, 20));
    assert(!store.copy(20, encoded, sizeof(encoded), length, revision));
    assert(revision == 2);
    assert(store.copy(520, encoded, sizeof(encoded), length, revision) == -ESTALE);
    assert(store.copy(19, encoded, sizeof(encoded), length, revision) == -ESTALE);
    assert(store.copy(20, encoded, 1, length, revision) == -ENOSPC);
    view.home.bars = 6;
    assert(store.publish(view, 21));
    assert(store.copy(21, encoded, sizeof(encoded), length, revision));
    puts("C++ UI snapshot fixtures, visible revisions and freshness passed");
}
