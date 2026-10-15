// SPDX-License-Identifier: GPL-3.0-or-later
// LSF property IDs and pointer handoff follow ListenAI's Apache-2.0 service wrappers.
#include <errno.h>
#include <ht/radio.hpp>
#include <ht/wasm.hpp>
#include <lsf.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "wasm_programs.h"

// Writable, cache-line-isolated AP PSRAM storage retained until physical reboot.
// LsfVariant marshaling translates the PSRAM offset and cleans shared cache lines.
static unsigned char module_bytes[4096]
    __attribute__((section(".psram_section"), aligned(32)));

extern "C" int ht_wasm_backend_run(unsigned program, uint32_t *observed) {
    static const unsigned char *const programs[] = {wasm_test_1, wasm_test_2, wasm_test_3};
    static const size_t sizes[] = {sizeof(wasm_test_1), sizeof(wasm_test_2), sizeof(wasm_test_3)};
    if (program >= 3 || !observed || sizes[program] > sizeof(module_bytes)) {
        return -EINVAL;
    }
    // These groups are unused by normal audio. The vendor requires contiguous IDs.
    static LsfPropertyConfig app[] = {{0, LSF_PROPERTY_FLAG_SYNC | LSF_PROPERTY_FLAG_SET_ONLY,
                                      LSF_PROPERTY_TYPE_PTR}};
    static LsfPropertyConfig thinker[] = {
        {0, LSF_PROPERTY_FLAG_SYNC | LSF_PROPERTY_FLAG_SET_ONLY, LSF_PROPERTY_TYPE_PTR},
        {1, LSF_PROPERTY_FLAG_SYNC | LSF_PROPERTY_FLAG_GET_ONLY, LSF_PROPERTY_TYPE_PTR},
        {2, LSF_PROPERTY_FLAG_SYNC | LSF_PROPERTY_FLAG_SET_ONLY, LSF_PROPERTY_TYPE_PTR},
        {3, LSF_PROPERTY_FLAG_SYNC | LSF_PROPERTY_FLAG_GET_ONLY, LSF_PROPERTY_TYPE_UINT32},
    };
    if (lsf_import_properties(2, app, 1) || lsf_import_properties(1, thinker, 4)) {
        return -EIO;
    }
    memcpy(module_bytes, programs[program], sizes[program]);
    LsfVariant value;
    LsfVariant_init(&value);
    value.variant_type = type_pointer;
    value.u.pointer.pAddr = module_bytes;
    value.u.pointer.size = sizes[program];
    // LSHD is an AP file wrapper only. The property takes the raw module payload.
    int error = lsf_set(2, 0, &value);
    LsfVariant_clear(&value);
    if (error) {
        return -EIO;
    }
    ht::wasm_stage(ht::WasmStage::Checking);
    // main may run asynchronously. A distinctive count verifies main's self-test
    // and the exported getter. Never fetch outputs using these diagnostic counts.
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        if (!ht::radio_power_requested()) {
            return -ECANCELED;
        }
        if (ht::wasm_status().stage == ht::WasmStage::TimedOut) {
            return -ETIMEDOUT;
        }
        LsfVariant *reply = nullptr;
        error = lsf_get(1, 3, &reply);
        if (error || !reply) {
            return -EIO;
        }
        const bool valid = reply->variant_type == type_uint;
        if (valid) {
            *observed = reply->u.uiVal;
        }
        LsfVariant_unref(reply);
        if (!valid) {
            return -EBADMSG;
        }
        if (*observed == (program + 1) * 11) {
            return 0;
        }
        k_sleep(K_MSEC(50));
    }
    return -EBADMSG;
}
