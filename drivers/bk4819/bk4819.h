/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum bk4819_tone_kind { BK4819_TONE_NONE, BK4819_TONE_CTCSS, BK4819_TONE_DCS };

struct bk4819_tone {
    enum bk4819_tone_kind kind;
    uint16_t value;
    bool inverted;
};

struct bk4819_config {
    uint32_t rx_frequency_hz;
    uint32_t tx_frequency_hz;
    bool tx_inhibit;
    struct bk4819_tone rx_tone;
    struct bk4819_tone tx_tone;
    bool wide;
    bool m17;
    uint8_t fm_weak_filter; /* REG_43[11:9], 0..7. Ignored in M17. */
    uint8_t fm_ctcss_level; /* REG_51[6:0], 0..127. Use 74 for the baseline. */
    uint8_t fm_af_dac_gain; /* REG_48[3:0], 0..15. Use 1 for the baseline. */
};

/* One controller-owned transceiver. No borrowed configuration is retained.
 * Frequencies are requested Hz; the synthesizer rounds down to 10 Hz.
 * Every operation reports GPIO/transport errors to its owner. */
int bk4819_initialize(void);
int bk4819_configure(const struct bk4819_config *config);
int bk4819_receive(void);
int bk4819_transmit(void); /* Caller sets APC and prepares audio before keying. */
int bk4819_disable(void);  /* PA/LNA/LED GPIOs go inactive before other writes. */
int bk4819_af(bool enabled);
int bk4819_rssi(int16_t *dbm);
/* Reads the configured RX tone's detector/polarity, without changing outputs. */
int bk4819_tone_detected(bool *detected);
/* Normal 23-bit DCS word; caller validates the nine-bit input first. */
uint32_t bk4819_dcs_word(uint16_t code);

/* Compile-time bus implementation: C62 GPIO transport or test register model.
 * Call only from the transceiver owner. Writes to REG_33 can enable the PA;
 * the target diagnostic adapter must prevent that during diagnostics. */
int bk4819_bus_init(void);
int bk4819_read(uint8_t address, uint16_t *value);
int bk4819_write(uint8_t address, uint16_t value);

#ifdef __cplusplus
}
#endif
