// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include <ht/settings.hpp>
#include <stdlib.h>
#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

using namespace ht;
static Codeplug loaded;

ZTEST(codeplug_profile, test_host_profile_firmware_interoperability) {
    // This fixture is driven by the host suite, always in a unique temporary
    // directory. Never fall back to a developer's default production profile.
    const char *enabled = getenv("HT_PROFILE_PROBE");
    if (!enabled || strcmp(enabled, "1")) {
        ztest_test_skip();
        return;
    }
    zassert_not_null(getenv("HT_SETTINGS_DIR"));
    zassert_not_null(getenv("HT_PROFILE"));
    RadioConfig configuration;
    zassert_ok(
        settings_start(configuration)); // Validates every stored channel against RF capabilities.
    uint32_t generation = 0;
    zassert_ok(codeplug_load(loaded, generation));
    printk(
        "PROFILE: generation=%u channels=%u banks=%u selected_rx=%u selected_tx=%u mode=%u can=%u\n",
        generation, loaded.channel_count, loaded.bank_count, configuration.rx_frequency_hz,
        configuration.tx_frequency_hz, unsigned(configuration.mode), configuration.m17.can);
    const char *write = getenv("HT_PROFILE_PROBE_WRITE");
    if (write && !strcmp(write, "1")) {
        // Firmware-generated changes let the host decoder prove field mapping
        // independently of its own encoder, while all other settings persist.
        loaded.global.ui.theme = Theme::Nord;
        loaded.global.ui.contrast = Contrast::Maximum;
        loaded.global.vfo_step_hz = 6250;
        loaded.vfo.rx_frequency_hz = 145555000;
        loaded.vfo.tx_frequency_hz = 145555010;
        zassert_ok(codeplug_save(loaded, generation));
        printk("PROFILE: published=%u\n", generation);
    }
}

ZTEST_SUITE(codeplug_profile, nullptr, nullptr, nullptr, nullptr, nullptr);
