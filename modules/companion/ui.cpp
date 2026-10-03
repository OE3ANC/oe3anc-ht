// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion_ui.hpp>
#include <ht/ui_companion.hpp>
#include <string.h>
#include <zephyr/sys/byteorder.h>

namespace ht {
namespace companion {
void UiTransfer::handle(const Frame &request, Frame &response) {
    response.size = 1;
    response.payload[0] = STATUS_INVALID;
    if (request.type == MSG_UI_POLL) {
        if (request.size != 4) {
            return;
        }
        reset();
        size_t length = 0;
        uint32_t revision = 0;
        const int error = ui_copy_presentation(bytes_, sizeof(bytes_), length, revision);
        if (error) {
            response.payload[0] = error == -EBUSY || error == -EAGAIN ? STATUS_BUSY
                                  : error == -ESTALE                  ? STATUS_STALE
                                                                      : STATUS_FAILED;
            return;
        }
        if (!revision || !length || length > sizeof(bytes_)) {
            response.payload[0] = STATUS_FAILED;
            return;
        }
        response.payload[0] = STATUS_OK;
        sys_put_le32(revision, response.payload + 2);
        if (sys_get_le32(request.payload) == revision) {
            response.size = 6;
            response.payload[1] = 0;
            return;
        }
        token_ = request.request;
        length_ = length;
        response.size = 16;
        response.payload[1] = 1;
        sys_put_le32(token_, response.payload + 6);
        sys_put_le16(length_, response.payload + 10);
        sys_put_le32(crc32(bytes_, length_), response.payload + 12);
    } else if (request.type == MSG_UI_CHUNK) {
        if (request.size != 7) {
            return;
        }
        const auto token = sys_get_le32(request.payload);
        const auto offset = sys_get_le16(request.payload + 4);
        const unsigned count = request.payload[6];
        if (!token_ || token != token_ || offset != offset_) {
            response.payload[0] = STATUS_STALE;
            return;
        }
        if (!count || count > UI_CHUNK_BYTES || count > unsigned(length_ - offset_)) {
            return;
        }
        response.payload[0] = STATUS_OK;
        response.size = 7 + count;
        sys_put_le32(token_, response.payload + 1);
        sys_put_le16(offset_, response.payload + 5);
        memcpy(response.payload + 7, bytes_ + offset_, count);
        offset_ += count;
        if (offset_ == length_) {
            token_ = 0;
        }
    } else {
        response.payload[0] = STATUS_UNSUPPORTED;
    }
}
} // namespace companion
} // namespace ht
