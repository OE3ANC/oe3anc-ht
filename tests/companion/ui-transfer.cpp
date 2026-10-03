// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <errno.h>
#include <ht/companion_ui.hpp>
#include <string.h>
#include <zephyr/sys/byteorder.h>
using namespace ht::companion;
static uint8_t published[UI_MAX_BYTES];
static uint32_t revision = 1;
static int copy_error;

namespace ht {
int ui_copy_presentation(uint8_t *bytes, size_t capacity, size_t &length, uint32_t &rev) {
    if (copy_error) {
        return copy_error;
    }
    assert(capacity >= sizeof(published));
    memcpy(bytes, published, sizeof(published));
    length = sizeof(published);
    rev = revision;
    return 0;
}
} // namespace ht

int main() {
    UiTransfer transfer;
    Frame request, response;
    request.type = MSG_UI_POLL;
    request.request = 12;
    request.size = 4;
    transfer.handle(request, response);
    assert(response.size == 16 && response.payload[1] == 1);
    assert(sys_get_le32(response.payload + 6) == 12);
    assert(sys_get_le16(response.payload + 10) == UI_MAX_BYTES);
    const auto checksum = sys_get_le32(response.payload + 12);
    memset(published, 42, sizeof(published));
    ++revision;
    request.type = MSG_UI_CHUNK;
    request.size = 7;
    sys_put_le32(12, request.payload);
    sys_put_le16(1, request.payload + 4);
    request.payload[6] = 180;
    transfer.handle(request, response);
    assert(response.payload[0] == STATUS_STALE);
    sys_put_le16(0, request.payload + 4);
    request.payload[6] = 181;
    transfer.handle(request, response);
    assert(response.payload[0] == STATUS_INVALID);
    uint8_t received[UI_MAX_BYTES];
    for (unsigned offset = 0; offset < sizeof(received);) {
        const unsigned count = sizeof(received) - offset > 180 ? 180 : sizeof(received) - offset;
        sys_put_le16(offset, request.payload + 4);
        request.payload[6] = count;
        transfer.handle(request, response);
        assert(response.payload[0] == STATUS_OK && response.size == 7 + count);
        assert(sys_get_le16(response.payload + 5) == offset);
        memcpy(received + offset, response.payload + 7, count);
        offset += count;
    }
    assert(crc32(received, sizeof(received)) == checksum && received[0] == 0);
    transfer.handle(request, response);
    assert(response.payload[0] == STATUS_STALE);
    request.type = MSG_UI_POLL;
    request.size = 4;
    sys_put_le32(revision, request.payload);
    transfer.handle(request, response);
    assert(response.size == 6 && response.payload[1] == 0);
    const int errors[] = {-EBUSY, -EAGAIN, -ESTALE, -EINVAL};
    const unsigned statuses[] = {STATUS_BUSY, STATUS_BUSY, STATUS_STALE, STATUS_FAILED};
    for (unsigned i = 0; i < 4; ++i) {
        copy_error = errors[i];
        transfer.handle(request, response);
        assert(response.size == 1 && response.payload[0] == statuses[i]);
    }
    copy_error = 0;
    sys_put_le32(0, request.payload);
    transfer.handle(request, response);
    transfer.reset();
    request.type = MSG_UI_CHUNK;
    request.size = 7;
    transfer.handle(request, response);
    assert(response.payload[0] == STATUS_STALE);
}
