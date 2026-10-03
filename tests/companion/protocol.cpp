// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <ht/companion_protocol.hpp>
#include <stdio.h>
#include <string.h>
using namespace ht::companion;

static void same(const Frame &a, const Frame &b) {
    assert(a.major == b.major && a.minor == b.minor && a.type == b.type && a.flags == b.flags);
    assert(a.size == b.size && a.request == b.request && a.session == b.session);
    assert(!memcmp(a.payload, b.payload, a.size));
}

static void check_wire(const Frame &frame, const uint8_t *expected, size_t size) {
    uint8_t wire[MAX_ENCODED];
    assert(encode(frame, wire, sizeof(wire)) == size);
    assert(!memcmp(wire, expected, size));
    Frame decoded;
    assert(decode(wire + 1, size - 2, decoded));
    same(frame, decoded);
    Decoder decoder;
    for (size_t i = 0; i < size; ++i) {
        assert(decoder.feed(wire[i], i, decoded) == (i == size - 1));
    }
    same(frame, decoded);
    wire[size - 2] ^= 1;
    assert(!decode(wire + 1, size - 2, decoded));
    for (size_t i = 0; i < size; ++i) {
        assert(!decoder.feed(wire[i], 300, decoded));
    }
    wire[size - 2] ^= 1;
    // Oversize, expired, malformed and partial prefixes recover at leading zero.
    for (unsigned i = 0; i < MAX_ENCODED + 20; ++i) {
        assert(!decoder.feed(1, 400, decoded));
    }
    for (size_t i = 0; i < size; ++i) {
        assert(decoder.feed(wire[i], 410, decoded) == (i == size - 1));
    }
    decoder.feed(1, 0, decoded);
    decoder.feed(2, FRAME_TIMEOUT_MS, decoded);
    for (size_t i = 0; i < size; ++i) {
        assert(decoder.feed(wire[i], 600, decoded) == (i == size - 1));
    }
    assert(!encode(frame, wire, MAX_ENCODED - 1));
}

#include "fixtures.hpp"

static Frame hello(uint64_t nonce, const char *release = "dev-test") {
    Frame f;
    f.type = MSG_HELLO;
    f.request = 1;
    const size_t size = strlen(release);
    f.payload[0] = size;
    memcpy(f.payload + 1, release, size);
    for (unsigned i = 0; i < 8; ++i) {
        f.payload[size + 1 + i] = nonce >> (i * 8);
    }
    f.size = size + 9;
    return f;
}

static void sessions() {
    Session session("dev-test", "c62");
    Frame response;
    auto h = hello(42);
    h.major++;
    assert(session.handle(h, 0, response) && response.size == 1 &&
           response.payload[0] == STATUS_MISMATCH && response.major == MAJOR);
    assert(!session.identity());
    h.major = MAJOR;
    assert(session.handle(h, 0, response) && response.payload[0] == STATUS_OK);
    assert(response.session == 42 && session.identity() == 42);
    Frame original = response;
    assert(session.handle(h, 300, response));
    same(response, original);
    // Retrying a response must not extend the original lease.
    assert(!session.expire(999));
    assert(session.expire(1000));
    assert(session.handle(h, 1001, response) && response.payload[0] == STATUS_SESSION);
    assert(session.handle(hello(43, "wrong"), 1002, response));
    assert(response.payload[0] == STATUS_MISMATCH && !session.identity());
    assert(session.handle(hello(43), 1003, response) && session.identity() == 43);
    Frame ping;
    ping.type = MSG_PING;
    ping.request = 2;
    ping.session = 99;
    assert(session.handle(ping, 1010, response) && response.payload[0] == STATUS_SESSION);
    ping.session = 43;
    assert(session.handle(ping, 1011, response) && response.payload[0] == STATUS_OK);
    ping.type = 99;
    assert(session.handle(ping, 1012, response) && response.payload[0] == STATUS_STALE);
    ping.request = 3;
    assert(session.handle(ping, 1013, response) && response.payload[0] == STATUS_UNSUPPORTED);
    ping.type = MSG_PING;
    ping.request = 2;
    assert(session.handle(ping, 1014, response) && response.payload[0] == STATUS_STALE);
    ping.request = 4;
    ping.size = 1;
    assert(session.handle(ping, 1015, response) && response.payload[0] == STATUS_INVALID);
    ping.size = MAX_PAYLOAD + 1;
    assert(session.handle(ping, 1016, response) && response.payload[0] == STATUS_INVALID);
    ping.size = 0;
    ping.request = 5;
    ping.major++;
    assert(session.handle(ping, 1017, response) && response.payload[0] == STATUS_MISMATCH);
    ping.major = MAJOR;
    ping.flags = FLAG_RESPONSE;
    assert(!session.handle(ping, 1018, response));
    ping.flags = FLAG_REQUEST;
    ping.request = 0;
    assert(!session.handle(ping, 1019, response));
    ping.request = 5;
    ping.type = MSG_CLOSE;
    assert(session.handle(ping, 1020, response) && response.payload[0] == STATUS_OK);
    assert(!session.identity());
    auto invalid = hello(1, "invalid space");
    assert(session.handle(invalid, 1021, response) && response.payload[0] == STATUS_INVALID);
    invalid = hello(0);
    assert(session.handle(invalid, 1021, response) && response.payload[0] == STATUS_INVALID);
}

int main() {
    assert(crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926);
    fixtures();
    sessions();
    puts("C++ companion conformance and sessions passed");
}
