// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/power_backend.hpp>

namespace ht {
// The developer window remains available to turn the fake radio back on.
int power_backend_prepare() {
    return 0;
}

int power_backend_outputs(bool) {
    return 0;
}
} // namespace ht
