// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ht {
// Minimal inactive RF/audio/light outputs; no DSP or normal RF initialization.
int power_backend_prepare();
int power_backend_outputs(bool active);
} // namespace ht
