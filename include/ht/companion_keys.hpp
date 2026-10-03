// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/companion_protocol.hpp>

namespace ht {
namespace companion {
void handle_keys(const Frame &request, Frame &response);
}
} // namespace ht
