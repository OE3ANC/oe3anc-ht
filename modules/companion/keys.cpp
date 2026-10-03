// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion_keys.hpp>
#include <ht/ui_input.hpp>
#include <zephyr/sys/byteorder.h>

namespace ht {
namespace companion {
void handle_keys(const Frame &request, Frame &response) {
    response.size = 1;
    response.payload[0] = STATUS_INVALID;
    int error = 0;
    if (request.type == MSG_UI_KEY) {
        if (request.size != 2 || request.payload[0] > UI_KEY_DIGIT_9 || request.payload[1] > 1) {
            return;
        }
        const unsigned key = request.payload[0];
        static constexpr UiKey front[] = {UiKey::Up,    UiKey::Down, UiKey::Left, UiKey::Right,
                                          UiKey::Enter, UiKey::Back, UiKey::Star, UiKey::Hash};
        UiInput input{key < UI_KEY_DIGIT_0 ? front[key] : UiKey::Digit};
        if (key >= UI_KEY_DIGIT_0) {
            input.character = '0' + key - UI_KEY_DIGIT_0;
        }
        input.pressed = request.payload[1];
        error = ui_remote_key(input, key);
    } else if (request.type == MSG_UI_KEYS_CLEAR) {
        if (request.size) {
            return;
        }
        ui_remote_clear();
    } else if (request.type == MSG_UI_KEYS_KEEP) {
        if (request.size != 4) {
            return;
        }
        const auto mask = sys_get_le32(request.payload);
        if (!mask || mask >= (uint32_t(1) << (UI_KEY_DIGIT_9 + 1))) {
            return;
        }
        error = ui_remote_keep(mask);
    } else {
        response.payload[0] = STATUS_UNSUPPORTED;
        return;
    }
    response.payload[0] = !error             ? STATUS_OK
                          : error == -EBUSY  ? STATUS_BUSY
                          : error == -ESTALE ? STATUS_STALE
                          : error == -EINVAL ? STATUS_INVALID
                                             : STATUS_FAILED;
}
} // namespace companion
} // namespace ht
