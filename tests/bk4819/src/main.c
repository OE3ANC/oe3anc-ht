/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <bk4819.h>
#include "../vectors/dcs.h"
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>

static uint16_t registers[128];
static int fail_address = -1;
static bool fail_read;
static bool failed;
static int fail_value = -1;
static bool keyed_at_failure;
static uint8_t first_write;
static unsigned writes;
static uint16_t dcs_high, dcs_low;

int bk4819_bus_init(void) {
    memset(registers, 0, sizeof(registers));
    writes = 0;
    return 0;
}

int bk4819_read(uint8_t address, uint16_t *value) {
    if (address > 0x7f || !value) {
        return -EINVAL;
    }
    if (fail_read && address == fail_address && !failed) {
        failed = true;
        return -EIO;
    }
    *value = registers[address];
    return 0;
}

int bk4819_write(uint8_t address, uint16_t value) {
    if (address > 0x7f) {
        return -EINVAL;
    }
    if (!writes++) {
        first_write = address;
    }
    if (!fail_read && address == fail_address && !failed &&
        (fail_value < 0 || value == fail_value)) {
        failed = true;
        keyed_at_failure = (registers[0x33] & 0x1c) != 0;
        return -EIO;
    }
    registers[address] = value;
    if (address == 0x08) {
        if (value & 0x8000) {
            dcs_high = value & 0x0fff;
        } else {
            dcs_low = value & 0x0fff;
        }
    }
    return 0;
}

static void before(void *unused) {
    ARG_UNUSED(unused);
    fail_address = -1;
    fail_value = -1;
    keyed_at_failure = false;
    fail_read = failed = false;
    dcs_high = dcs_low = 0;
    zassert_ok(bk4819_initialize());
}

static struct bk4819_config fm(uint32_t frequency) {
    return (struct bk4819_config){.rx_frequency_hz = frequency,
                                  .tx_frequency_hz = frequency,
                                  .wide = true,
                                  .fm_af_dac_gain = 1};
}

ZTEST(bk4819, test_fm_voice_protection_and_m17_round_trip) {
    struct bk4819_config config = fm(145500000);
    config.tx_tone = (struct bk4819_tone){BK4819_TONE_CTCSS, 885, false};
    registers[0x19] = 0x9234; // Reset disables microphone AGC.
    registers[0x2b] = 0x8707; // Seed bypasses; retain RX and unrelated fields.
    registers[0x47] = 0x6043;
    registers[0x4b] = 0x7122;
    registers[0x51] = 0x0155; // Preserve the sub-audio gain and bandwidth fields.
    const uint16_t microphone_gain = registers[0x7d];
    const uint16_t deviation = registers[0x40];

    for (unsigned i = 0; i < 2; ++i) {
        zassert_ok(bk4819_configure(&config));
        zassert_ok(bk4819_transmit());
        zassert_equal(registers[0x19], 0x1234);
        zassert_equal(registers[0x2b], 0x8700);
        zassert_equal(registers[0x47], 0x6042);
        zassert_equal(registers[0x4b], 0x7102);
        zassert_equal(registers[0x7d], microphone_gain);
        zassert_equal(registers[0x40], deviation);
        zassert_equal(registers[0x51], 0x9155);
        zassert_equal(registers[0x07], 1827);

        config.m17 = true;
        zassert_ok(bk4819_configure(&config));
        zassert_ok(bk4819_transmit());
        zassert_equal(registers[0x19], 0x9234);
        zassert_equal(registers[0x2b], 0x8707);
        zassert_equal(registers[0x47] & 1, 1);
        zassert_equal(registers[0x4b], 0x7122);
        zassert_equal(registers[0x51] & 0x8000, 0);
        config.m17 = false;
        config.wide = false;
        config.rx_frequency_hz = config.tx_frequency_hz = 430000000;
    }
}

ZTEST(bk4819, test_fm_voice_configuration_failure_prevents_tx) {
    const uint8_t addresses[] = {0x19, 0x2b, 0x47, 0x4b};
    for (unsigned read = 0; read < 2; ++read) {
        for (unsigned i = 0; i < ARRAY_SIZE(addresses); ++i) {
            before(NULL);
            struct bk4819_config config = fm(145500000);
            zassert_ok(bk4819_configure(&config));
            zassert_ok(bk4819_transmit());
            fail_address = addresses[i];
            fail_read = read != 0;
            zassert_equal(bk4819_configure(&config), -EIO);
            zassert_true(failed);
            zassert_equal(registers[0x33], 0);
            zassert_equal(registers[0x30] & 0x010e, 0);
            zassert_equal(bk4819_transmit(), -ENODEV);
        }
    }
}

ZTEST(bk4819, test_fm_rx_controls_preserve_unrelated_fields_and_m17_profile) {
    struct bk4819_config config = fm(145500000);
    registers[0x43] = 0xc1cf;
    registers[0x48] = 0xb3c1;
    config.fm_weak_filter = 7;
    config.fm_af_dac_gain = 15;
    zassert_ok(bk4819_configure(&config));
    zassert_equal(registers[0x43], (0xc1cf & ~0x0e30) | 0x0e20);
    zassert_equal(registers[0x48], 0xb3cf);
    zassert_ok(bk4819_receive());
    zassert_ok(bk4819_transmit());
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x43] & 0x0e00, 0x0e00);
    zassert_equal(registers[0x48], 0xb3cf);
    const uint16_t filter = registers[0x43];
    config.m17 = true;
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x43] & 0x7ffc, 0x7808);
    zassert_equal(registers[0x48], 0xb7f1); // FM DAC override cannot change M17 level.
    config.m17 = false;
    zassert_ok(bk4819_configure(&config));
    zassert_equal(registers[0x43], filter);
    zassert_equal(registers[0x48], 0xb3cf);
    config.fm_weak_filter = 0;
    config.fm_af_dac_gain = 0;
    config.wide = false;
    zassert_ok(bk4819_configure(&config));
    zassert_equal(registers[0x43], 0xc1cf & ~0x0e30);
    zassert_equal(registers[0x48], 0xb3c0);
    zassert_ok(bk4819_receive());
    writes = 0;
    config.fm_weak_filter = 8;
    zassert_equal(bk4819_configure(&config), -EINVAL);
    config.fm_weak_filter = 0;
    config.fm_af_dac_gain = 16;
    zassert_equal(bk4819_configure(&config), -EINVAL);
    zassert_equal(writes, 0);
    config.fm_af_dac_gain = 1;
    fail_address = 0x48;
    zassert_equal(bk4819_configure(&config), -EIO);
    zassert_equal(registers[0x33], 0);
    zassert_equal(bk4819_receive(), -ENODEV);
}

ZTEST(bk4819, test_bias_startup_and_configuration_required_before_tx) {
    zassert_equal(registers[0x33], 0);
    zassert_equal(registers[0x30] & 0x010e, 0);
    zassert_equal(registers[0x47] & 0x0f00, 0);
    zassert_equal(bk4819_transmit(), -ENODEV);
    zassert_equal(bk4819_receive(), -ENODEV);
}

ZTEST(bk4819, test_simplex_band_selection_and_ten_hertz_tuning) {
    struct bk4819_config config = fm(174000000);
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x33], 0x0040); // Includes VHF band top.
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x33], 0x0014); // VHF PA and TX LED only.
    zassert_equal(registers[0x30], 0xc1fe);
    writes = 0;
    zassert_ok(bk4819_disable());
    zassert_equal(first_write, 0x33); // Unkey before tone/PLL work.
    zassert_equal(registers[0x33], 0);
    config = fm(430123459);
    zassert_ok(bk4819_configure(&config));
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000; // Caller memory is not retained.
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x33], 0x0020);
    zassert_equal(((uint32_t)registers[0x39] << 16) | registers[0x38], 43012345);
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x33], 0x000c);
}

ZTEST(bk4819, test_high_ctcss_tone_does_not_overflow_and_disable_clears_tx_tone) {
    struct bk4819_config config = fm(145500000);
    config.tx_tone = (struct bk4819_tone){BK4819_TONE_CTCSS, 2541, false};
    config.rx_tone = (struct bk4819_tone){BK4819_TONE_CTCSS, 885, false};
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x07], 5246);
    zassert_equal(registers[0x51] & 0x9000, 0x9000);
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x07], 1827);
    zassert_equal(registers[0x51] & 0x8000, 0);
    registers[0x0c] = 1U << 10;
    bool detected;
    zassert_ok(bk4819_tone_detected(&detected));
    zassert_true(detected);
}

ZTEST(bk4819, test_digital_profile_preserves_unrelated_bits_and_restores_fm) {
    struct bk4819_config config = fm(430000000);
    zassert_ok(bk4819_configure(&config));
    registers[0x19] = 0x1234;
    registers[0x2b] = 0x8000;
    registers[0x47] |= 2;
    const uint16_t fm_filter = registers[0x43];
    const uint16_t fm_gain = registers[0x48];
    config.m17 = true;
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    zassert_ok(bk4819_af(true));
    zassert_equal(registers[0x47] & 0x0f03, 0x0903);
    zassert_equal(registers[0x19], 0x9234);
    zassert_equal(registers[0x2b], 0x8707);
    zassert_equal(registers[0x48] & 0x0ff0, 0x07f0);
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x48] & 0x03f0, fm_gain & 0x03f0);
    zassert_equal(registers[0x7e] & 0x8000, 0x8000);
    zassert_ok(bk4819_af(false));
    zassert_equal(registers[0x47] & 3, 3);
    config.m17 = false;
    zassert_ok(bk4819_configure(&config));
    zassert_equal(registers[0x19], 0x1234);
    zassert_equal(registers[0x2b], 0x8000);
    zassert_equal(registers[0x43], fm_filter);
    zassert_equal(registers[0x48], fm_gain);
}

ZTEST(bk4819, test_failed_tx_unkeys_and_returns_bus_error) {
    struct bk4819_config config = fm(430000000);
    zassert_ok(bk4819_configure(&config));
    fail_read = true; // Fail tone/status reads independently of shutdown.
    fail_address = 0x51;
    zassert_equal(bk4819_transmit(), -EIO);
    zassert_equal(registers[0x33], 0);
    fail_read = false;
    failed = false;
    fail_address = 0x30;
    fail_value = 0xc1fe; // Fail after the PA was enabled, not the initial unkey.
    zassert_equal(bk4819_transmit(), -EIO);
    zassert_true(keyed_at_failure);
    zassert_equal(registers[0x33], 0);
    fail_address = -1;
    registers[0x67] = 150;
    int16_t rssi;
    zassert_ok(bk4819_rssi(&rssi));
    zassert_equal(rssi, -85);
}

ZTEST(bk4819, test_invalid_configuration_and_failed_reinitialization) {
    struct bk4819_config config = fm(300000000);
    zassert_equal(bk4819_configure(&config), -EINVAL);
    config = fm(430000000);
    config.tx_tone = (struct bk4819_tone){BK4819_TONE_CTCSS, 100, false};
    zassert_equal(bk4819_configure(&config), -EINVAL);
    fail_address = 0x49;
    zassert_equal(bk4819_initialize(), -EIO);
    zassert_equal(registers[0x33], 0);
    zassert_equal(bk4819_transmit(), -ENODEV);
}

ZTEST(bk4819, test_split_tuning_unkeys_before_pll_and_inhibit_leaves_rx) {
    struct bk4819_config config = fm(439075009);
    config.tx_frequency_hz = 145125009; // TX band is independent of RX.
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    writes = 0;
    zassert_ok(bk4819_transmit());
    zassert_equal(first_write, 0x33);
    zassert_equal(registers[0x33], 0x0014);
    zassert_equal(((uint32_t)registers[0x39] << 16) | registers[0x38], 14512500);
    writes = 0;
    zassert_ok(bk4819_receive());
    zassert_equal(first_write, 0x33);
    zassert_equal(registers[0x33], 0x0020);
    zassert_equal(((uint32_t)registers[0x39] << 16) | registers[0x38], 43907500);
    config.tx_inhibit = true;
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    writes = 0;
    zassert_equal(bk4819_transmit(), -EPERM);
    zassert_equal(writes, 0);
    zassert_equal(registers[0x33], 0x0020);
    config.tx_frequency_hz = 300000000;
    zassert_equal(bk4819_configure(&config), -EINVAL);
    zassert_equal(registers[0x33], 0x0020);
    config.tx_frequency_hz = 145125009;
    config.tx_inhibit = false;
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    fail_address = 0x38;
    zassert_equal(bk4819_transmit(), -EIO);
    zassert_false(keyed_at_failure);
    zassert_equal(registers[0x33], 0);
}

ZTEST(bk4819, test_all_dcs_words_against_independent_upstream_encoder) {
    for (unsigned code = 0; code < 512; ++code) {
        zassert_equal(bk4819_dcs_word(code), dcs_words[code], "octal code %03o", code);
    }
}

ZTEST(bk4819, test_dcs_rx_tx_values_and_polarities_are_independent) {
    struct bk4819_config config = fm(145500000);
    config.rx_tone = (struct bk4819_tone){BK4819_TONE_DCS, 0023, true};
    config.tx_tone = (struct bk4819_tone){BK4819_TONE_DCS, 0754, false};
    zassert_ok(bk4819_configure(&config));
    registers[0x51] = 0xffff; // Clear stale mode/width/external-input/1050Hz fields.
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x51] & 0xfc00, 0);
    zassert_equal(registers[0x51] & 0x03ff, 0x03ff); // Preserve gain/bandwidth fields.
    zassert_equal(registers[0x07], 0x4ad7);
    zassert_equal(((uint32_t)dcs_high << 12) | dcs_low, dcs_words[0023]);
    bool detected = false;
    registers[0x0c] = (1U << 14) | (1U << 10);
    zassert_ok(bk4819_tone_detected(&detected));
    zassert_false(detected);
    registers[0x0c] = 1U << 15;
    zassert_ok(bk4819_tone_detected(&detected));
    zassert_true(detected);
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x51] & 0xfc00, 0x8000);
    zassert_equal(((uint32_t)dcs_high << 12) | dcs_low, dcs_words[0754]);
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x51] & 0x8000, 0);
    zassert_equal(((uint32_t)dcs_high << 12) | dcs_low, dcs_words[0023]);
    config.rx_tone.inverted = false;
    config.tx_tone.inverted = true;
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    registers[0x0c] = 1U << 15;
    zassert_ok(bk4819_tone_detected(&detected));
    zassert_false(detected);
    registers[0x0c] = 1U << 14;
    zassert_ok(bk4819_tone_detected(&detected));
    zassert_true(detected);
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x51] & 0xfc00, 0xa000);
    // Hardware polarity bit inverts once; the loaded word remains normal.
    zassert_equal(((uint32_t)dcs_high << 12) | dcs_low, dcs_words[0754]);
}

ZTEST(bk4819, test_mixed_tones_clear_stale_mode_polarity_and_enable_bits) {
    struct bk4819_config config = fm(145500000);
    config.rx_tone = (struct bk4819_tone){BK4819_TONE_CTCSS, 885, false};
    config.tx_tone = (struct bk4819_tone){BK4819_TONE_DCS, 0023, true};
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x51] & 0xfc00, 0xa000);
    zassert_ok(bk4819_receive());
    zassert_equal(registers[0x51] & 0xfc00, 0x1000);
    zassert_equal(registers[0x07], 1827);
    config.rx_tone = config.tx_tone = (struct bk4819_tone){0};
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    bool detected = true;
    registers[0x0c] = 0xffff;
    zassert_ok(bk4819_tone_detected(&detected));
    zassert_false(detected);
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x51] & 0xfc00, 0);
    config.tx_tone = (struct bk4819_tone){BK4819_TONE_CTCSS, 2541, false};
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x51] & 0xfc00, 0x9000);
    config.m17 = true;
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x51] & 0x8000, 0); // M17 cannot transmit a retained FM tone.
    config.m17 = false;
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_transmit());
    zassert_equal(registers[0x51] & 0xfc00, 0x9000);
}

ZTEST(bk4819, test_invalid_tones_leave_current_rx_undisturbed) {
    struct bk4819_config config = fm(430000000);
    zassert_ok(bk4819_configure(&config));
    zassert_ok(bk4819_receive());
    const struct bk4819_tone invalid[] = {{BK4819_TONE_NONE, 23, false},
                                          {BK4819_TONE_NONE, 0, true},
                                          {BK4819_TONE_CTCSS, 669, false},
                                          {BK4819_TONE_CTCSS, 2542, false},
                                          {BK4819_TONE_CTCSS, 885, true},
                                          {BK4819_TONE_DCS, 01000, false},
                                          {99, 0, false}};
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); ++i) {
        config.rx_tone = invalid[i];
        writes = 0;
        zassert_equal(bk4819_configure(&config), -EINVAL);
        zassert_equal(writes, 0);
        zassert_equal(registers[0x33], 0x0020);
    }
}

ZTEST(bk4819, test_dcs_write_failure_unkeys_and_failed_detector_retains_output) {
    const int values[] = {-1, 0x813}; // Fail either high word or D023's low word.
    for (unsigned i = 0; i < ARRAY_SIZE(values); ++i) {
        if (i) {
            before(NULL);
        }
        struct bk4819_config config = fm(430000000);
        config.tx_tone = config.rx_tone = (struct bk4819_tone){BK4819_TONE_DCS, 0023, false};
        zassert_ok(bk4819_configure(&config));
        fail_address = 0x08;
        fail_value = values[i];
        zassert_equal(bk4819_transmit(), -EIO);
        zassert_false(keyed_at_failure);
        zassert_equal(registers[0x33], 0);
        zassert_equal(registers[0x51] & 0x8000, 0);
        fail_address = 0x0c;
        fail_read = true;
        failed = false;
        bool detected = true;
        zassert_equal(bk4819_tone_detected(&detected), -EIO);
        zassert_true(detected);
    }
}

ZTEST_SUITE(bk4819, NULL, NULL, before, NULL, NULL);
