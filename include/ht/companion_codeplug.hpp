// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/codeplug.hpp>
#include <ht/companion_contract.hpp>

namespace ht {
namespace companion {
// CPS binary v2: explicit HTDB manifest v2 and channel/bank v1 records,
// all with wire generation 1. RAM/storage revisions travel in transfer metadata.
// Outputs are staging only: decode failure may partially fill an unpublished
// Codeplug; discard it. Never pass the settings owner's applied database here.
int encode_codeplug(const Codeplug &, uint8_t *, size_t capacity, size_t &written);
int decode_codeplug(const uint8_t *, size_t length, Codeplug &staging);
} // namespace companion
} // namespace ht
