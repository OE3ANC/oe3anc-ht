// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ht {
enum class WasmStage : uint8_t { Ready, WaitingAudio, Loading, Checking, Passed, Failed, TimedOut };
struct WasmStatus {
    WasmStage stage = WasmStage::Ready;
    uint8_t program = 0;
    uint32_t observed = 0;
    int error = 0;
};

// Controller-only submission after entering exclusive diagnostics. One attempt per boot.
int wasm_start(unsigned program);
bool wasm_radio_locked();
WasmStatus wasm_status();
// Backend progress only; does not extend the overall deadline.
void wasm_stage(WasmStage stage);
} // namespace ht

// Blocking vendor call, confined to the diagnostic worker.
extern "C" int ht_wasm_backend_run(unsigned program, uint32_t *observed);
