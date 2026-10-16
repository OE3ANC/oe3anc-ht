// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/modem.hpp>
#include <string.h>

namespace ht {
namespace modem {
void KissParser::reset() {
    size_ = 0;
    started_ = false;
    escaped_ = false;
    invalid_ = false;
}

KissResult KissParser::feed(uint8_t byte, KissMessage &output) {
    if (byte == 0xc0) {
        KissResult result = KissResult::None;
        if (started_ && (size_ || invalid_ || escaped_)) {
            const uint8_t command = bytes_[0];
            const bool data = size_ > 1 && command == 0;
            const bool parameter = size_ == 2 && command >= 1 && command <= 5;
            const bool leave = size_ == 1 && command == 0xff;
            if (!invalid_ && !escaped_ && (data || parameter || leave)) {
                output.command = static_cast<KissCommand>(command);
                output.frame.size = size_ - 1;
                memcpy(output.frame.bytes, bytes_ + 1, output.frame.size);
                result = KissResult::Message;
            } else {
                result = KissResult::Rejected;
            }
        }
        size_ = 0;
        started_ = true;
        escaped_ = false;
        invalid_ = false;
        return result;
    }
    if (!started_ || invalid_) {
        return KissResult::None;
    }
    if (escaped_) {
        escaped_ = false;
        if (byte != 0xdc && byte != 0xdd) {
            invalid_ = true;
            return KissResult::None;
        }
        byte = byte == 0xdc ? 0xc0 : 0xdb;
    } else if (byte == 0xdb) {
        escaped_ = true;
        return KissResult::None;
    }
    if (size_ == sizeof(bytes_)) {
        invalid_ = true;
    } else {
        bytes_[size_++] = byte;
    }
    return KissResult::None;
}

size_t kiss_encode(const uint8_t *data, size_t size, uint8_t *output, size_t capacity) {
    if (!data || !output || !size || size > max_frame_size) {
        return 0;
    }
    size_t required = size + 3;
    for (size_t i = 0; i < size; ++i) {
        required += data[i] == 0xc0 || data[i] == 0xdb;
    }
    if (capacity < required) {
        return 0;
    }
    size_t position = 0;
    output[position++] = 0xc0;
    output[position++] = 0;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == 0xc0 || data[i] == 0xdb) {
            output[position++] = 0xdb;
            output[position++] = data[i] == 0xc0 ? 0xdc : 0xdd;
        } else {
            output[position++] = data[i];
        }
    }
    output[position++] = 0xc0;
    return position;
}
} // namespace modem
} // namespace ht
