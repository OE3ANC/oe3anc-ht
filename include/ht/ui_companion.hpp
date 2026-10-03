// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/radio.hpp>
#include <stddef.h>

namespace ht {
struct Codeplug;
// Copied canonical bytes from the UI owner, never a live model or LVGL object.
int ui_copy_presentation(uint8_t *, size_t capacity, size_t &length, uint32_t &revision);
// Thread-context copied submission. Serialized with the UI owner's model
// processing; rejects open local editors/dialogs or pending local operations.
// The UI guards ordinary keys while accepted application work settles. PTT,
// release, monitor, power and fault paths remain independent.
int ui_replace_codeplug(const Codeplug &, uint32_t id, const RadioState &, uint32_t revision);
} // namespace ht
