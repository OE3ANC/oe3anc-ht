// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/companion_contract.hpp>
#include <stddef.h>
#include <stdint.h>

namespace ht {
namespace companion {
struct Frame {
    uint8_t major = MAJOR, minor = MINOR, type = 0, flags = FLAG_REQUEST;
    uint16_t size = 0;
    uint32_t request = 0;
    uint64_t session = 0;
    uint8_t payload[MAX_PAYLOAD] = {};
};

uint32_t crc32(const uint8_t *data, size_t size);
size_t encode(const Frame &frame, uint8_t *output, size_t capacity);
bool decode(const uint8_t *encoded, size_t size, Frame &frame);

// Feed the entire stream, including zero delimiters. Oversize/expired frames
// discard through the next delimiter. An error cannot expose partial payloads.
class Decoder {
  public:
    bool feed(uint8_t byte, int64_t now_ms, Frame &frame);
    void reset();

  private:
    uint8_t data_[MAX_ENCODED] = {};
    size_t size_ = 0;
    int64_t started_ = 0;
    bool discard_ = false;
};

// Thread-owned session core. It has no dependency on Zephyr, UI or RF state.
// Status replies carry one status byte. Hello also returns release/limits.
class Session {
  public:
    using Handler = void (*)(void *, const Frame &, Frame &);
    explicit Session(const char *release, const char *target, uint32_t capabilities = 0,
                     Handler handler = nullptr, void *context = nullptr);
    bool handle(const Frame &request, int64_t now_ms, Frame &response);
    bool expire(int64_t now_ms);
    void reset();

    uint64_t identity() const {
        return session_;
    }

  private:
    const char *release_, *target_;
    uint32_t capabilities_;
    Handler handler_;
    void *context_;
    uint64_t session_ = 0, previous_session_ = 0;
    uint32_t last_request_ = 0;
    int64_t deadline_ = 0;
    Frame last_input_, last_output_;
    bool cached_ = false;
};
} // namespace companion
} // namespace ht
