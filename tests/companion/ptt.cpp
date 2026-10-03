// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <ht/companion_ptt.hpp>
#include <ht/radio.hpp>
#include <string.h>
using namespace ht;
using namespace ht::companion;
static unsigned calls;

namespace ht {
int radio_remote_ptt_press(uint64_t session, uint32_t token) {
    assert(session == 42 && token == 2);
    ++calls;
    return 0;
}

int radio_remote_ptt_keep(uint64_t session, uint32_t token) {
    assert(session == 42 && token == 2);
    ++calls;
    return 0;
}

int radio_remote_ptt_release(uint64_t session) {
    assert(session == 42);
    ++calls;
    return 0;
}
} // namespace ht

static void fixture(unsigned type, const uint8_t *bytes, unsigned length, unsigned status) {
    Frame request, response;
    request.type = type;
    request.session = 42;
    request.request = 2;
    request.size = length;
    memcpy(request.payload, bytes, length);
    calls = 0;
    handle_ptt(request, response);
    assert(response.payload[0] == status);
    assert(response.size == (status == STATUS_OK && type == MSG_PTT_PRESS ? 5 : 1));
    assert(calls == (status == STATUS_OK ? 1u : 0u));
    if (status == STATUS_OK && type == MSG_PTT_PRESS) {
        const uint8_t token[] = {2, 0, 0, 0};
        assert(!memcmp(response.payload + 1, token, 4));
    }
}

#include "ptt-fixtures.hpp"

int main() {
    fixtures();
}
