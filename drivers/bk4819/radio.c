/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from reference BK4819.c and radio_C62.cpp. Preserve tested register
 * sequences and flat-baseband masks; use copied config and checked bus I/O.
 */
#include "bk4819.h"
#include <errno.h>
#include <stddef.h>
#include <zephyr/kernel.h>

static struct bk4819_config configuration;
static bool digital;
static bool initialized;
static bool configured;

struct register_value {
    uint8_t address;
    uint16_t value;
};
static const struct register_value initialization[] = {
    {0x00, 0x8000}, {0x00, 0x0000}, {0x33, 0x0000}, {0x37, 0x1d0f}, {0x13, 0x03be}, {0x12, 0x037b},
    {0x11, 0x027b}, {0x10, 0x007a}, {0x14, 0x0019}, {0x49, 0x2a38}, {0x7b, 0x8420}, {0x7d, 0xe959},
    {0x48, 0xb3c1}, {0x1e, 0x4c58}, {0x1f, 0xa656}, {0x3e, 0xa037}, {0x3f, 0x07fe}, {0x2a, 0x7fff},
    {0x28, 0x6b00}, {0x53, 59000},  {0x2c, 0x5705}, {0x4b, 0x7102},
};
static const struct register_value initialization_tail[] = {
    {0x77, 0x88ef}, {0x26, 0x13a0}, {0x4e, 0x6f15}, {0x4f, 0x3f3e}, {0x09, 0x006f}, {0x09, 0x106b},
    {0x09, 0x2067}, {0x09, 0x3062}, {0x09, 0x4050}, {0x09, 0x5047}, {0x09, 0x603a}, {0x09, 0x702c},
    {0x09, 0x8041}, {0x09, 0x9037}, {0x09, 0xa025}, {0x09, 0xb017}, {0x09, 0xc0e4}, {0x09, 0xd0cb},
    {0x09, 0xe0b5}, {0x09, 0xf09f}, {0x74, 0xfa02}, {0x44, 0x8f88}, {0x45, 0x3201},
};

static const struct {
    uint8_t address;
    uint16_t mask;
    uint16_t value;
} profile[] = {
    {0x19, 0x8000, 0x8000}, /* Microphone AGC off. */
    {0x2b, 0x0707, 0x0707}, /* Speech filters/emphasis bypassed. */
    {0x31, 0x000c, 0x0000}, /* Compander and VOX off. */
    {0x40, 0x1fff, 0x14d2}, /* Raw deviation field; not hertz. */
    {0x43, 0x7ffc, 0x7808}, /* Flat RX filter, 12.5 kHz channel. */
    {0x47, 0x0003, 0x0001}, /* TX filter bypass; RX polarity below. */
    {0x48, 0x0ff0, 0x0400}, /* RX gain1 -6 dB; gain2 below. */
    {0x4b, 0x0020, 0x0020}, /* Audio limiter off. */
    {0x50, 0x8000, 0x0000}, /* TX unmuted after beeps. */
    {0x70, 0x8080, 0x0000}, /* Tone generators off. */
    {0x7d, 0x001f, 0x0000}, /* Fixed minimum microphone gain. */
    {0x7e, 0x803f, 0x0000}, /* DC filters off, RX RF AGC on. */
};

static uint16_t fm_profile[ARRAY_SIZE(profile)];

static int update(uint8_t address, uint16_t mask, uint16_t value) {
    uint16_t previous;
    int error = bk4819_read(address, &previous);
    return error ? error : bk4819_write(address, (previous & ~mask) | (value & mask));
}

static int write_table(const struct register_value *table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const int error = bk4819_write(table[i].address, table[i].value);
        if (error) {
            return error;
        }
    }
    return 0;
}

int bk4819_disable(void) {
    /* Direct write: unkeying cannot depend on a successful read first. The
     * C62 uses GPIOs 0..4 for LNA, PA and TX LED; remaining outputs are unused. */
    const int pa_error = bk4819_write(0x33, 0);
    const int rf_error = bk4819_write(0x30, 0);
    const int dac_error = bk4819_write(0x30, 1U << 9);
    const int tone_error = update(0x51, 0x8000, 0);
    return pa_error ? pa_error : rf_error ? rf_error : dac_error ? dac_error : tone_error;
}

static int finish(int error) {
    if (error) {
        bk4819_disable();
    }
    return error;
}

static int apply_digital(bool transmit) {
    for (size_t i = 0; i < ARRAY_SIZE(profile); ++i) {
        uint16_t value = profile[i].value;
        if (profile[i].address == 0x47) {
            value |= transmit ? (fm_profile[i] & 2) : 2;
        }
        if (profile[i].address == 0x48) {
            value |= transmit ? (fm_profile[i] & 0x03f0) : 0x03f0;
        }
        if (profile[i].address == 0x7e && transmit) {
            value |= 0x8000;
        }
        const int error = update(profile[i].address, profile[i].mask, value);
        if (error) {
            return error;
        }
    }
    return update(0x51, 0x8000, 0);
}

static int apply_fm_voice(void) {
    /* BK4819 V3 register list (2020-12-18): MIC AGC resets disabled.
     * Enable it before the speech filters to reduce overload from loud audio.
     * Keep the 300 Hz high-pass, low-pass, pre-emphasis and ALC enabled;
     * CTCSS/CDCSS is generated separately, not in the microphone PCM stream. */
    int error = update(0x19, 0x8000, 0);
    if (!error) {
        error = update(0x2b, 0x0007, 0);
    }
    if (!error) {
        error = update(0x47, 0x0001, 0);
    }
    if (!error) {
        error = update(0x4b, 0x0020, 0);
    }
    return error;
}

int bk4819_initialize(void) {
    initialized = false;
    configured = false;
    digital = false;
    int error = bk4819_bus_init();
    if (error) {
        return error;
    }
    error = write_table(initialization, ARRAY_SIZE(initialization));
    if (!error) {
        error = update(0x40, 0x0fff, 0x04d2);
    }
    if (!error) {
        error = write_table(initialization_tail, ARRAY_SIZE(initialization_tail));
    }
    if (!error) {
        error = update(0x31, 0x0008, 0);
    }
    if (!error) {
        error = bk4819_write(0x28, 0x6b38);
    }
    if (!error) {
        error = bk4819_write(0x29, 0xb4cb);
    }
    if (!error) {
        error = bk4819_write(0x36, 0xdfbf);
    }
    if (!error) {
        error = bk4819_write(0x47, 0x6040);
    }
    /* Software squelch owns audio gating; preserve hardware timing fields. */
    if (!error) {
        error = bk4819_write(0x78, 0);
    }
    if (!error) {
        error = update(0x4f, 0x7f7f, 0x7f7f);
    }
    if (!error) {
        error = update(0x4d, 0x00ff, 0x00ff);
    }
    if (!error) {
        error = update(0x4e, 0x00ff, 0x00ff);
    }
    if (!error) {
        error = bk4819_disable();
    }
    /* Tested bias settling: ADC on, RX DSP off, external PAs remain off. */
    uint16_t power;
    if (!error) {
        error = bk4819_read(0x30, &power);
    }
    if (!error) {
        error = bk4819_write(0x30, 1U << 2);
    }
    if (!error) {
        k_sleep(K_MSEC(250));
        error = bk4819_write(0x30, power);
    }
    if (!error) {
        initialized = true;
    }
    return finish(error);
}

static bool supported_frequency(uint32_t hz) {
    return (hz >= 136000000 && hz <= 174000000) || (hz >= 400000000 && hz <= 480000000);
}

static bool valid_tone(const struct bk4819_tone *tone) {
    switch (tone->kind) {
    case BK4819_TONE_NONE:
        return !tone->value && !tone->inverted;
    case BK4819_TONE_CTCSS:
        return tone->value >= 670 && tone->value <= 2541 && !tone->inverted;
    case BK4819_TONE_DCS:
        return tone->value <= 0777;
    default:
        return false;
    }
}

int bk4819_configure(const struct bk4819_config *config) {
    if (!initialized) {
        return -ENODEV;
    }
    if (!config || !supported_frequency(config->rx_frequency_hz) ||
        !supported_frequency(config->tx_frequency_hz)) {
        return -EINVAL;
    }
    if (!valid_tone(&config->rx_tone) || !valid_tone(&config->tx_tone) ||
        config->fm_weak_filter > 7 || config->fm_af_dac_gain > 15) {
        return -EINVAL;
    }
    configured = false;
    int error = bk4819_disable();
    if (!error) {
        error = bk4819_af(false);
    }
    if (digital && !config->m17) {
        for (size_t i = 0; i < ARRAY_SIZE(profile) && !error; ++i) {
            error = update(profile[i].address, profile[i].mask, fm_profile[i]);
        }
    }
    if (!digital && config->m17) {
        for (size_t i = 0; i < ARRAY_SIZE(profile) && !error; ++i) {
            uint16_t value;
            error = bk4819_read(profile[i].address, &value);
            if (!error) {
                fm_profile[i] = value & profile[i].mask;
            }
        }
    }
    if (!error && !config->m17) {
        error = apply_fm_voice();
    }
    if (!error) {
        error = config->m17 ? apply_digital(false)
                            : update(0x43, 0x0e30,
                                     (config->fm_weak_filter << 9) | (config->wide ? 0x20 : 0));
    }
    if (!error) {
        // DAC gain is RX audio only. Keep the tested M17 baseband level even
        // when switching from FM with an experimental audio gain selected.
        error = update(0x48, 0x000f, config->m17 ? 1 : config->fm_af_dac_gain);
    }
    if (!error) {
        configuration = *config;
        digital = config->m17;
        configured = true;
    }
    return finish(error);
}

static int rx_on(void) {
    int error = bk4819_write(0x37, 0x1f0f);
    k_busy_wait(1);
    if (!error) {
        error = bk4819_write(0x30, 0x0200);
    }
    if (!error) {
        error = bk4819_write(0x30, 0xbff1);
    }
    return error;
}

static int tune(uint32_t hz) {
    const uint32_t frequency = hz / 10;
    int error = bk4819_write(0x39, frequency >> 16);
    if (!error) {
        error = bk4819_write(0x38, frequency & 0xffff);
    }
    return error ? error : rx_on();
}

static int tone(const struct bk4819_tone *selection, bool transmit) {
    /* Own mode/width/source/polarity fields while preserving automatic bandwidth
     * and unrelated low fields. Disable TX until word writes finish. */
    const uint16_t mode = selection->kind == BK4819_TONE_CTCSS ? 0x1000 : 0;
    int error = update(0x51, 0xfc00, mode);
    if (error || selection->kind == BK4819_TONE_NONE) {
        return error;
    }
    if (selection->kind == BK4819_TONE_CTCSS) {
        /* The reference's uint32_t product overflows above about 208 Hz. */
        error = bk4819_write(0x07, (uint64_t)selection->value * 2064888 / 1000000);
    } else {
        const uint32_t word = bk4819_dcs_word(selection->value);
        error = bk4819_write(0x07, 0x4000 | 0x0ad7); /* 134.4 bit/s, tested crystal family. */
        if (!error) {
            error = bk4819_write(0x08, 0x8000 | (word >> 12));
        }
        if (!error) {
            error = bk4819_write(0x08, word & 0x0fff);
        }
    }
    if (!error && transmit) {
        const uint16_t polarity = selection->inverted ? 0x2000 : 0;
        /* REG_51[6:0] resets to minimum gain; enabling the tone alone leaves
         * its level unset. Provisional values from egzumer's BK4819 driver:
         * CTCSS 74, DCS 51. C62 sub-audio deviation still needs bench calibration.
         * https://github.com/egzumer/uv-k5-firmware-custom/blob/main/driver/bk4819.c */
        const uint16_t gain = selection->kind == BK4819_TONE_CTCSS ? 74 : 51;
        error = update(0x51, 0xa07f, 0x8000 | polarity | gain);
    }
    return error;
}

int bk4819_receive(void) {
    if (!initialized || !configured) {
        return -ENODEV;
    }
    int error = bk4819_disable();
    if (!error) {
        error = bk4819_write(0x33, configuration.rx_frequency_hz <= 174000000 ? 0x0040 : 0x0020);
    }
    if (!error) {
        error = tune(configuration.rx_frequency_hz);
    }
    if (!error && digital) {
        error = apply_digital(false);
    }
    if (!error && !digital) {
        error = tone(&configuration.rx_tone, false);
    }
    if (!error) {
        error = rx_on();
    }
    return finish(error);
}

int bk4819_transmit(void) {
    if (!initialized || !configured) {
        return -ENODEV;
    }
    if (configuration.tx_inhibit) {
        return -EPERM;
    }
    int error = bk4819_disable();
    if (!error) {
        error = tune(configuration.tx_frequency_hz);
    }
    if (!error && digital) {
        error = apply_digital(true);
    }
    if (!error && !digital) {
        error = tone(&configuration.tx_tone, true);
    }
    if (!error) {
        error = bk4819_write(0x33, configuration.tx_frequency_hz <= 174000000 ? 0x0014 : 0x000c);
    }
    if (!error) {
        error = bk4819_write(0x30, 0);
    }
    if (!error) {
        error = bk4819_write(0x30, 0xc1fe);
    }
    return finish(error);
}

int bk4819_af(bool enabled) {
    return update(0x47, 0x0f00, enabled ? (digital ? 0x0900 : 0x0100) : 0);
}

int bk4819_rssi(int16_t *dbm) {
    if (!dbm) {
        return -EINVAL;
    }
    uint16_t value;
    const int error = bk4819_read(0x67, &value);
    if (!error) {
        *dbm = (value & 0x01ff) / 2 - 160;
    }
    return error;
}

int bk4819_tone_detected(bool *detected) {
    if (!detected) {
        return -EINVAL;
    }
    if (!initialized || !configured) {
        return -ENODEV;
    }
    if (digital || configuration.rx_tone.kind == BK4819_TONE_NONE) {
        *detected = false;
        return 0;
    }
    uint16_t value;
    const int error = bk4819_read(0x0c, &value);
    if (!error) {
        const unsigned bit = configuration.rx_tone.kind == BK4819_TONE_CTCSS ? 10
                             : configuration.rx_tone.inverted                ? 15
                                                                             : 14;
        *detected = (value & (1U << bit)) != 0;
    }
    return error;
}
