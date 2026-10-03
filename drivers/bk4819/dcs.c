/* SPDX-FileCopyrightText: 2023 Dual Tachyon
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from DCS_CalculateGolay in DualTachyon/uv-k5-firmware,
 * revision 6f8afac8864e0349ecf8b10f91ce8f69273013db (dcs.c).
 * Accept the actual code rather than a table index; inversion belongs to the
 * transceiver. License text: LICENSES/Apache-2.0.txt.
 */
#include "bk4819.h"

uint32_t bk4819_dcs_word(uint16_t code) {
    /* Systematic Golay(23,12), information bits first on the wire. DCS fixes
     * information bits 11:9 to 100; the other nine bits are the octal code.
     * Polynomial feedback 0x475 is the lower 11 bits of the reciprocal
     * generator 0xc75. TX polarity is a separate BK4819 register field. */
    const uint16_t information = code | 0x800U;
    uint16_t remainder = information;
    for (unsigned bit = 0; bit < 12; ++bit) {
        const bool feedback = (remainder & 0x800U) != 0;
        remainder = (remainder << 1) & 0xfffU;
        if (feedback) {
            remainder ^= 0x8eaU;
        }
    }
    return information | ((uint32_t)(remainder >> 1) << 12);
}
