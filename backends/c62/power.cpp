// SPDX-License-Identifier: GPL-3.0-or-later
#include "rf_io.h"
#include "ui_io.h"
#include "bk4819.h"
#include <ht/power_backend.hpp>

namespace ht {
int power_backend_prepare() {
    // Attempt every independent inactive output even if another operation fails.
    const int rf_io = c62_rf_io_init();
    const int bus = bk4819_bus_init();
    const int pa = bus ? bus : bk4819_write(0x33, 0);
    const int rf = bus ? bus : bk4819_write(0x30, 0);
    const int lights = c62_ui_set_power(false);
    return rf_io ? rf_io : pa ? pa : rf ? rf : lights;
}

int power_backend_outputs(bool active) {
    return c62_ui_set_power(active);
}
} // namespace ht
