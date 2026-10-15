// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/audio.hpp>
#include <ht/radio.hpp>
#include <ht/wasm.hpp>
#include <zephyr/kernel.h>

namespace ht {
static struct k_spinlock lock;
static WasmStatus status;
static int64_t deadline;
static struct k_thread worker;
K_THREAD_STACK_DEFINE(wasm_worker_stack, 4096);

static bool pending() {
    return status.stage == WasmStage::WaitingAudio || status.stage == WasmStage::Loading ||
           status.stage == WasmStage::Checking;
}

WasmStatus wasm_status() {
    const auto key = k_spin_lock(&lock);
    if (pending() && k_uptime_get() >= deadline) {
        status.stage = WasmStage::TimedOut;
        status.error = -ETIMEDOUT;
    }
    const auto result = status;
    k_spin_unlock(&lock, key);
    return result;
}

bool wasm_radio_locked() {
    return wasm_status().stage != WasmStage::Ready;
}

void wasm_stage(WasmStage stage) {
    const auto key = k_spin_lock(&lock);
    if (pending()) {
        status.stage = stage;
    }
    k_spin_unlock(&lock, key);
}

static void run(void *, void *, void *) {
    const unsigned program = wasm_status().program;
    const int64_t audio_deadline = k_uptime_get() + 3000;
    int error = 0;
    while (!audio_quiescent()) {
        error = audio_status().error;
        if (error || !radio_power_requested() || k_uptime_get() >= audio_deadline) {
            error = error ? error : !radio_power_requested() ? -ECANCELED : -ETIMEDOUT;
            break;
        }
        k_sleep(K_MSEC(10));
    }
    uint32_t observed = 0;
    if (!error && !radio_power_requested()) {
        error = -ECANCELED;
    }
    if (!error) {
        wasm_stage(WasmStage::Loading);
        error = ht_wasm_backend_run(program, &observed);
    }
    // A late response cannot turn a timed-out attempt into success. Never abort
    // a blocked vendor thread: its stack and module storage may still be borrowed.
    const auto key = k_spin_lock(&lock);
    if (pending()) {
        status.observed = observed;
        status.error = error ? error : observed != (program + 1) * 11 ? -EBADMSG : 0;
        status.stage = k_uptime_get() >= deadline ? WasmStage::TimedOut
                       : status.error            ? WasmStage::Failed
                                                 : WasmStage::Passed;
        if (status.stage == WasmStage::TimedOut) {
            status.error = -ETIMEDOUT;
        }
    }
    k_spin_unlock(&lock, key);
}

int wasm_start(unsigned program) {
    if (program >= 3) {
        return -EINVAL;
    }
    const auto key = k_spin_lock(&lock);
    if (status.stage != WasmStage::Ready) {
        k_spin_unlock(&lock, key);
        return -EBUSY;
    }
    status.program = program;
    status.stage = WasmStage::WaitingAudio;
    deadline = k_uptime_get() + 10000;
    k_spin_unlock(&lock, key);
    k_thread_create(&worker, wasm_worker_stack, K_THREAD_STACK_SIZEOF(wasm_worker_stack), run, nullptr,
                    nullptr, nullptr, 7, 0, K_NO_WAIT);
    return 0;
}
} // namespace ht
