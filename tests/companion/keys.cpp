// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <ht/companion_keys.hpp>
#include <ht/ui_input.hpp>
#include <string.h>
using namespace ht;
using namespace ht::companion;
static UiInput input;
static unsigned wire_key, calls;
static uint32_t mask;

namespace ht {
int ui_remote_key(UiInput value, unsigned key) {
    input = value;
    wire_key = key;
    ++calls;
    return 0;
}

int ui_remote_keep(uint32_t value) {
    mask = value;
    ++calls;
    return 0;
}

void ui_remote_clear() {
    ++calls;
}
} // namespace ht

static void fixture(unsigned type, const uint8_t *bytes, unsigned length, unsigned status) {
    Frame request, response;
    request.type = type;
    request.size = length;
    memcpy(request.payload, bytes, length);
    calls = 0;
    handle_keys(request, response);
    assert(response.size == 1 && response.payload[0] == status);
    assert(calls == (status == STATUS_OK ? 1u : 0u));
    if (status != STATUS_OK)
        return;
    if (type == MSG_UI_KEYS_KEEP) {
        assert(mask == 64);
    }
    if (type != MSG_UI_KEY)
        return;
    assert(wire_key == bytes[0] && input.pressed == bool(bytes[1]));
    if (wire_key >= UI_KEY_DIGIT_0) {
        assert(input.key == UiKey::Digit &&
               input.character == static_cast<char>('0' + wire_key - UI_KEY_DIGIT_0));
    } else {
        const UiKey keys[] = {UiKey::Up,    UiKey::Down, UiKey::Left, UiKey::Right,
                              UiKey::Enter, UiKey::Back, UiKey::Star, UiKey::Hash};
        assert(input.key == keys[wire_key]);
    }
}

#include "key-fixtures.hpp"

int main() {
    fixtures();
}
