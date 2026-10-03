// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion_ptt.hpp>
#include <ht/radio.hpp>
#include <zephyr/sys/byteorder.h>

namespace ht {
namespace companion {
void handle_ptt(const Frame &request, Frame &response) {
    response.size = 1;
    response.payload[0] = STATUS_INVALID;
    int error;
    if (request.type == MSG_PTT_PRESS) {
        if (request.size) {
            return;
        }
        error = radio_remote_ptt_press(request.session, request.request);
    } else if (request.type == MSG_PTT_KEEP) {
        if (request.size != 4 || !sys_get_le32(request.payload)) {
            return;
        }
        error = radio_remote_ptt_keep(request.session, sys_get_le32(request.payload));
    } else if (request.type == MSG_PTT_RELEASE) {
        if (request.size) {
            return;
        }
        error = radio_remote_ptt_release(request.session);
    } else {
        response.payload[0] = STATUS_UNSUPPORTED;
        return;
    }
    response.payload[0] = !error             ? STATUS_OK
                          : error == -EBUSY  ? STATUS_BUSY
                          : error == -ESTALE ? STATUS_STALE
                          : error == -EINVAL ? STATUS_INVALID
                                             : STATUS_FAILED;
    if (!error && request.type == MSG_PTT_PRESS) {
        response.size = 5;
        sys_put_le32(request.request, response.payload + 1);
    }
}
} // namespace companion
} // namespace ht
