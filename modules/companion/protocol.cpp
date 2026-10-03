// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/companion_protocol.hpp>
#include <string.h>

namespace ht {
namespace companion {
static void put(uint8_t *data, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        data[i] = value >> (8 * i);
    }
}

static uint64_t get(const uint8_t *data, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
        value |= uint64_t(data[i]) << (8 * i);
    }
    return value;
}

uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

size_t encode(const Frame &frame, uint8_t *output, size_t capacity) {
    if (frame.size > MAX_PAYLOAD || frame.flags > FLAG_RESPONSE || !output ||
        capacity < MAX_ENCODED) {
        return 0;
    }
    uint8_t raw[HEADER_SIZE + MAX_PAYLOAD + 4] = {'H', 'T'};
    raw[2] = frame.major;
    raw[3] = frame.minor;
    raw[4] = frame.type;
    raw[5] = frame.flags;
    put(raw + 6, frame.size, 2);
    put(raw + 8, frame.request, 4);
    put(raw + 12, frame.session, 8);
    memcpy(raw + HEADER_SIZE, frame.payload, frame.size);
    const size_t size = HEADER_SIZE + frame.size;
    put(raw + size, crc32(raw, size), 4);
    // COBS, with leading/trailing zero for recovery after a partial write.
    output[0] = 0;
    size_t cursor = 2, code_position = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < size + 4; ++i) {
        if (raw[i] == 0) {
            output[code_position] = code;
            code_position = cursor++;
            code = 1;
        } else {
            output[cursor++] = raw[i];
            ++code;
        }
    }
    output[code_position] = code;
    output[cursor++] = 0;
    return cursor;
}

bool decode(const uint8_t *encoded, size_t size, Frame &frame) {
    if (!encoded || !size || size > HEADER_SIZE + MAX_PAYLOAD + 5) {
        return false;
    }
    uint8_t raw[HEADER_SIZE + MAX_PAYLOAD + 4];
    size_t input = 0, output = 0;
    while (input < size) {
        const unsigned code = encoded[input++];
        if (!code || input + code - 1 > size) {
            return false;
        }
        for (unsigned i = 1; i < code; ++i) {
            if (!encoded[input] || output >= sizeof(raw)) {
                return false;
            }
            raw[output++] = encoded[input++];
        }
        if (code < 255 && input < size) {
            if (output >= sizeof(raw)) {
                return false;
            }
            raw[output++] = 0;
        }
    }
    if (output < HEADER_SIZE + 4 || raw[0] != 'H' || raw[1] != 'T') {
        return false;
    }
    const size_t payload = get(raw + 6, 2);
    if (payload > MAX_PAYLOAD || output != HEADER_SIZE + payload + 4 ||
        get(raw + output - 4, 4) != crc32(raw, output - 4) || raw[5] > FLAG_RESPONSE) {
        return false;
    }
    frame.major = raw[2];
    frame.minor = raw[3];
    frame.type = raw[4];
    frame.flags = raw[5];
    frame.size = payload;
    frame.request = get(raw + 8, 4);
    frame.session = get(raw + 12, 8);
    memcpy(frame.payload, raw + HEADER_SIZE, payload);
    return true;
}

void Decoder::reset() {
    size_ = 0;
    started_ = 0;
    discard_ = false;
}

bool Decoder::feed(uint8_t byte, int64_t now_ms, Frame &frame) {
    if (size_ && now_ms - started_ >= FRAME_TIMEOUT_MS) {
        discard_ = true;
        size_ = 0;
    }
    if (byte == 0) {
        const bool valid = !discard_ && size_ && decode(data_, size_, frame);
        reset();
        return valid;
    }
    if (discard_) {
        return false;
    }
    if (!size_) {
        started_ = now_ms;
    }
    if (size_ >= sizeof(data_)) {
        discard_ = true;
        size_ = 0;
        return false;
    }
    data_[size_++] = byte;
    return false;
}

Session::Session(const char *release, const char *target, uint32_t capabilities, Handler handler,
                 void *context)
    : release_(release), target_(target), capabilities_(capabilities), handler_(handler),
      context_(context) {
}

void Session::reset() {
    if (session_) {
        previous_session_ = session_;
    }
    session_ = 0;
    last_request_ = 0;
    deadline_ = 0;
    cached_ = false;
}

bool Session::expire(int64_t now_ms) {
    if (session_ && now_ms >= deadline_) {
        reset();
        return true;
    }
    return false;
}

bool Session::handle(const Frame &request, int64_t now_ms, Frame &response) {
    expire(now_ms);
    if (request.flags != FLAG_REQUEST || !request.request) {
        return false;
    }
    response = {};
    response.type = request.type;
    response.flags = FLAG_RESPONSE;
    response.request = request.request;
    response.session = request.session;
    response.size = 1;
    response.payload[0] = STATUS_OK;
    if (request.size > MAX_PAYLOAD) {
        response.payload[0] = STATUS_INVALID;
        return true;
    }
    if (request.major != MAJOR || request.minor != MINOR) {
        response.payload[0] = STATUS_MISMATCH;
        return true;
    }
    if (request.type == MSG_HELLO) {
        if (request.session || request.size < 10) {
            response.payload[0] = STATUS_INVALID;
            return true;
        }
        const unsigned length = request.payload[0];
        if (!length || length > MAX_RELEASE || request.size != length + 9) {
            response.payload[0] = STATUS_INVALID;
            return true;
        }
        for (unsigned i = 0; i < length; ++i) {
            if (request.payload[1 + i] < 0x21 || request.payload[1 + i] > 0x7e) {
                response.payload[0] = STATUS_INVALID;
                return true;
            }
        }
        const uint64_t nonce = get(request.payload + 1 + length, 8);
        if (!nonce) {
            response.payload[0] = STATUS_INVALID;
            return true;
        }
        // Identical Hello retries do not reset request ordering or the lease.
        if (cached_ && request.type == last_input_.type && request.request == last_input_.request &&
            request.size == last_input_.size &&
            !memcmp(request.payload, last_input_.payload, request.size)) {
            response = last_output_;
            return true;
        }
        reset();
        const size_t own_length = strlen(release_), target_length = strlen(target_);
        if (!own_length || own_length > MAX_RELEASE || !target_length || target_length > 8) {
            response.payload[0] = STATUS_INVALID;
            return true;
        }
        if (own_length != length || memcmp(request.payload + 1, release_, length)) {
            response.payload[0] = STATUS_MISMATCH;
        } else if (nonce == previous_session_) {
            response.payload[0] = STATUS_SESSION;
        } else {
            session_ = nonce;
            response.session = nonce;
            last_request_ = request.request;
            deadline_ = now_ms + LEASE_MS;
        }
        unsigned cursor = 1;
        response.payload[cursor++] = own_length;
        memcpy(response.payload + cursor, release_, own_length);
        cursor += own_length;
        response.payload[cursor++] = target_length;
        memcpy(response.payload + cursor, target_, target_length);
        cursor += target_length;
        put(response.payload + cursor, capabilities_, 4);
        cursor += 4;
        put(response.payload + cursor, MAX_PAYLOAD, 2);
        cursor += 2;
        put(response.payload + cursor, LEASE_MS, 2);
        cursor += 2;
        response.size = cursor;
    } else {
        if (!session_ || request.session != session_) {
            response.payload[0] = STATUS_SESSION;
            return true;
        }
        if (cached_ && request.request == last_request_) {
            if (request.type == last_input_.type && request.size == last_input_.size &&
                !memcmp(request.payload, last_input_.payload, request.size)) {
                response = last_output_;
                return true;
            }
            response.payload[0] = STATUS_STALE;
            return true;
        }
        if (request.request <= last_request_) {
            response.payload[0] = STATUS_STALE;
            return true;
        }
        last_request_ = request.request;
        if (request.type == MSG_PING || request.type == MSG_CLOSE) {
            if (request.size) {
                response.payload[0] = STATUS_INVALID;
            } else if (request.type == MSG_CLOSE) {
                reset();
                return true;
            } else {
                deadline_ = now_ms + LEASE_MS;
            }
        } else if (handler_) {
            handler_(context_, request, response);
            // New valid product requests keep paced transfers alive. A cached
            // retry never re-enters the handler or renews the lease.
            if (response.payload[0] == STATUS_OK) {
                deadline_ = now_ms + LEASE_MS;
            }
        } else {
            response.payload[0] = STATUS_UNSUPPORTED;
        }
    }
    if (session_) {
        last_input_ = request;
        last_output_ = response;
        cached_ = true;
    }
    return true;
}
} // namespace companion
} // namespace ht
