/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from OpenRTX Modulator.cpp, M17/DSP.hpp and core/fir.hpp,
 * reference commit 9d800e69f5f3c7275857c232c32786e6526ead80.
 * FIR implementation originally adapted from Rob Riggs, Mobilinkd LLC.
 */
#include <ht/m17_modem.hpp>
#include <string.h>

namespace ht {
namespace m17 {
namespace {
constexpr float taps[81] = {
    -0.003195702904062073, -0.002930279157647190, -0.001940667871554463, -0.000356087678023658,
    0.001547011339077758,  0.003389554791179751,  0.004761898604225673,  0.005310860846138910,
    0.004824746306020221,  0.003297923526848786,  0.000958710871218619,  -0.001749908029791816,
    -0.004238694106631223, -0.005881783042101693, -0.006150256456781309, -0.004745376707651645,
    -0.001704189656473565, 0.002547854551539951,  0.007215575568844704,  0.011231038205363532,
    0.013421952197060707,  0.012730475385624438,  0.008449554307303753,  0.000436744366018287,
    -0.010735380379191660, -0.023726883538258272, -0.036498030780605324, -0.046500883189991064,
    -0.050979050575999614, -0.047340680079891187, -0.033554880492651755, -0.008513823955725943,
    0.027696543159614194,  0.073664520037517042,  0.126689053778116234,  0.182990955139333916,
    0.238080025892859704,  0.287235637987091563,  0.326040247765297220,  0.350895727088112619,
    0.359452932027607974,  0.350895727088112619,  0.326040247765297220,  0.287235637987091563,
    0.238080025892859704,  0.182990955139333916,  0.126689053778116234,  0.073664520037517042,
    0.027696543159614194,  -0.008513823955725943, -0.033554880492651755, -0.047340680079891187,
    -0.050979050575999614, -0.046500883189991064, -0.036498030780605324, -0.023726883538258272,
    -0.010735380379191660, 0.000436744366018287,  0.008449554307303753,  0.012730475385624438,
    0.013421952197060707,  0.011231038205363532,  0.007215575568844704,  0.002547854551539951,
    -0.001704189656473565, -0.004745376707651645, -0.006150256456781309, -0.005881783042101693,
    -0.004238694106631223, -0.001749908029791816, 0.000958710871218619,  0.003297923526848786,
    0.004824746306020221,  0.005310860846138910,  0.004761898604225673,  0.003389554791179751,
    0.001547011339077758,  -0.000356087678023658, -0.001940667871554463, -0.002930279157647190,
    -0.003195702904062073,
};
constexpr int8_t symbols[] = {1, 3, -1, -3};
constexpr float gain = 23000.0f;
} // namespace

void Modulator::reset() {
    memset(history_, 0, sizeof(history_));
    position_ = 0;
}

float Modulator::filter(float sample) {
    position_ = position_ == 0 ? 80 : position_ - 1;
    history_[position_] = sample;
    history_[position_ + 81] = sample;
    float value = 0.0f;
    // Preserve reference accumulation order and its contiguous double history.
    for (unsigned i = 0; i < 80; i += 4) {
        value += history_[position_ + i] * taps[i];
        value += history_[position_ + i + 1] * taps[i + 1];
        value += history_[position_ + i + 2] * taps[i + 2];
        value += history_[position_ + i + 3] * taps[i + 3];
    }
    return value + history_[position_ + 80] * taps[80];
}

void Modulator::render(const Frame &frame, TxSamples &output) {
    for (unsigned i = 0; i < 1920; ++i) {
        float input = 0.0f;
        if (i % 10 == 0) {
            const unsigned symbol = i / 10;
            const unsigned dibit = (frame.bytes[symbol / 4] >> (6 - 2 * (symbol % 4))) & 3;
            input = symbols[dibit] * gain;
        }
        // With symbols bounded to +/-3, the largest absolute per-phase tap
        // sum gives <31,859, so truncation stays inside the int16_t range.
        output.samples[i] = static_cast<int16_t>(filter(input));
    }
}
} // namespace m17
} // namespace ht
