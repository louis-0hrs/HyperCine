// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
// Integer metadata features for the frame-strategy classifier.
// Coefficients are the parser's padded counts. Shares are truncating permille.
// Percentile indexes for 27 values are (n-1)/4, n/2, 3*(n-1)/4 and 9*(n-1)/10.
// Channel 0 is Y, 1 is V and 2 is U, matching GpuEntropyBandDescriptor.
#include "hypercine/decode_entropy_cuda.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace HyperCine {
struct Phase3adFeatures {
    int32_t l0_share = 0, l1_share = 0, l2_share = 0;
    int32_t l0_over_l1 = 0, l0_over_l2 = 0, l1_over_l2 = 0;
    int32_t y_share = 0, u_share = 0, v_share = 0, chroma_share = 0, uv_asym = 0;
    int32_t top1_share = 0, top3_share = 0, top6_share = 0;
    int32_t dense_count = 0, ratio_dense_count = 0;
    int32_t dense_l0 = 0, dense_l1 = 0, dense_l2 = 0, dense_y = 0, dense_chroma = 0;
    int32_t bpco_mean = 0, bpco_max = 0, bpco_median = 0, bpco_top3 = 0;
    int32_t bpco_l0 = 0, bpco_l1 = 0, bpco_l2 = 0, bpco_y = 0, bpco_chroma = 0;
    int32_t n_bpco_ge_50 = 0, n_bpco_ge_150 = 0, n_bpco_ge_400 = 0, n_bpco_ge_800 = 0;
    int32_t n_bytes_ge_4096 = 0, n_bytes_ge_16384 = 0, n_bytes_ge_32768 = 0;
    int32_t n_bytes_ge_65536 = 0, n_bytes_ge_131072 = 0, n_bytes_ge_262144 = 0;
    int32_t gini_milli = 0, hhi_permille = 0, mad_permille = 0, cv_permille = 0;
    int32_t p25_share = 0, p75_share = 0, p90_share = 0;
    int32_t l0_max_share = 0, l1_max_share = 0, l2_max_share = 0;
    int32_t y_max_share = 0, chroma_max_share = 0, y_top_within = 0, chroma_top_within = 0;
    int32_t quant_min = 0, quant_max = 0, quant_mean = 0;
    int32_t cb_cs9 = 0, cb_cs17 = 0, cb_cs18 = 0, diff_count = 0;
    int32_t active_bpco_mean = 0;
    int32_t width = 0, height = 0, total_payload = 0;
    int band_count = 0;
};
inline uint64_t Phase3adIsqrt(uint64_t n) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}
inline Phase3adFeatures ComputePhase3adFeatures(const std::vector<GpuEntropyBandDescriptor>& bands) {
    struct Row { uint64_t bytes, coeff, active, bpco, active_bpco; int channel, level, quant, codebook, diff; };
    std::array<Row, 27> rows{};
    Phase3adFeatures out;
    for (const auto& d : bands) {
        if (!d.subband || out.band_count >= 27) continue;
        auto& row = rows[size_t(out.band_count++)];
        row.bytes = d.payload_bytes;
        row.coeff = d.coefficient_count;
        row.active = uint64_t(std::max(0, d.width)) * uint64_t(std::max(0, d.height));
        row.bpco = row.coeff ? row.bytes * 1000 / row.coeff : 0;
        row.active_bpco = row.active ? row.bytes * 1000 / row.active : 0;
        row.channel = d.channel; row.level = d.level; row.quant = d.quantization;
        row.codebook = d.codebook_id; row.diff = d.difference_coding ? 1 : 0;
    }
    const int n = out.band_count;
    if (!n) return out;
    out.width = 0; out.height = 0;
    for (const auto& d : bands) if (d.subband && d.level == 0 && d.band == 1) {
        out.width = d.width * 2; out.height = d.height * 2; break;
    }
    std::array<int, 27> order{};
    for (int i = 0; i < n; ++i) order[size_t(i)] = i;
    std::sort(order.begin(), order.begin() + n, [&](int a, int b) {
        return rows[size_t(a)].bytes < rows[size_t(b)].bytes ||
            (rows[size_t(a)].bytes == rows[size_t(b)].bytes && a < b);
    });
    uint64_t total = 0, sum_bpco = 0, sum_active = 0, sum_sq = 0, quant_sum = 0;
    uint64_t level_bytes[3] = {}, level_coeff[3] = {}, level_max[3] = {};
    uint64_t ch_bytes[3] = {}, ch_coeff[3] = {}, ch_max[3] = {};
    out.quant_min = rows[0].quant; out.bpco_max = 0;
    auto share = [](uint64_t num, uint64_t den) -> int32_t { return den ? int32_t(num * 1000 / den) : 0; };
    auto ratio = [](uint64_t num, uint64_t den) -> int32_t {
        if (!den) return num ? 20000 : 0;
        const uint64_t value = num * 1000 / den;
        return int32_t(value > 20000 ? 20000 : value);
    };
    for (int i = 0; i < n; ++i) {
        const auto& row = rows[size_t(i)];
        total += row.bytes; sum_bpco += row.bpco; sum_active += row.active_bpco; sum_sq += row.bytes * row.bytes;
        quant_sum += uint64_t(row.quant);
        out.quant_min = (std::min)(out.quant_min, row.quant);
        out.quant_max = (std::max)(out.quant_max, row.quant);
        out.bpco_max = (std::max)(out.bpco_max, int32_t(row.bpco > 0x7fffffff ? 0x7fffffff : row.bpco));
        const bool ratio_dense = row.coeff && row.bytes * 100 >= row.coeff * 105;
        const bool dense = row.bytes >= 32768 && ratio_dense;
        if (dense) ++out.dense_count;
        if (ratio_dense) ++out.ratio_dense_count;
        if (row.level >= 0 && row.level < 3) {
            level_bytes[row.level] += row.bytes; level_coeff[row.level] += row.coeff;
            level_max[row.level] = (std::max)(level_max[row.level], row.bytes);
            if (dense && row.level == 0) ++out.dense_l0;
            else if (dense && row.level == 1) ++out.dense_l1;
            else if (dense && row.level == 2) ++out.dense_l2;
        }
        if (row.channel >= 0 && row.channel < 3) {
            ch_bytes[row.channel] += row.bytes; ch_coeff[row.channel] += row.coeff;
            ch_max[row.channel] = (std::max)(ch_max[row.channel], row.bytes);
            if (dense) { if (row.channel == 0) ++out.dense_y; else ++out.dense_chroma; }
        }
        if (row.bpco >= 50) ++out.n_bpco_ge_50;
        if (row.bpco >= 150) ++out.n_bpco_ge_150;
        if (row.bpco >= 400) ++out.n_bpco_ge_400;
        if (row.bpco >= 800) ++out.n_bpco_ge_800;
        if (row.bytes >= 4096) ++out.n_bytes_ge_4096;
        if (row.bytes >= 16384) ++out.n_bytes_ge_16384;
        if (row.bytes >= 32768) ++out.n_bytes_ge_32768;
        if (row.bytes >= 65536) ++out.n_bytes_ge_65536;
        if (row.bytes >= 131072) ++out.n_bytes_ge_131072;
        if (row.bytes >= 262144) ++out.n_bytes_ge_262144;
        if (row.codebook == 0) ++out.cb_cs9; else if (row.codebook == 1) ++out.cb_cs17; else if (row.codebook == 2) ++out.cb_cs18;
        out.diff_count += row.diff;
    }
    out.total_payload = total > 0x7fffffff ? 0x7fffffff : int32_t(total);
    out.l0_share = share(level_bytes[0], total); out.l1_share = share(level_bytes[1], total); out.l2_share = share(level_bytes[2], total);
    out.l0_over_l1 = ratio(level_bytes[0], level_bytes[1]); out.l0_over_l2 = ratio(level_bytes[0], level_bytes[2]);
    out.l1_over_l2 = ratio(level_bytes[1], level_bytes[2]);
    out.y_share = share(ch_bytes[0], total); out.v_share = share(ch_bytes[1], total); out.u_share = share(ch_bytes[2], total);
    const uint64_t chroma = ch_bytes[1] + ch_bytes[2];
    out.chroma_share = share(chroma, total);
    const uint64_t u = ch_bytes[2], v = ch_bytes[1];
    out.uv_asym = share(u > v ? u - v : v - u, u + v);
    auto ranked = [&](int k) { return rows[size_t(order[size_t(k)])].bytes; };
    uint64_t top3 = 0, top6 = 0;
    for (int i = 0; i < n && i < 3; ++i) top3 += ranked(n - 1 - i);
    for (int i = 0; i < n && i < 6; ++i) top6 += ranked(n - 1 - i);
    out.top1_share = share(ranked(n - 1), total); out.top3_share = share(top3, total); out.top6_share = share(top6, total);
    out.p25_share = share(ranked((n - 1) / 4), total);
    out.p75_share = share(ranked(3 * (n - 1) / 4), total);
    out.p90_share = share(ranked(9 * (n - 1) / 10), total);
    out.bpco_mean = int32_t(sum_bpco / uint64_t(n));
    out.active_bpco_mean = int32_t(sum_active / uint64_t(n));
    std::array<int, 27> by_bpco{};
    for (int i = 0; i < n; ++i) by_bpco[size_t(i)] = i;
    std::sort(by_bpco.begin(), by_bpco.begin() + n, [&](int a, int b) {
        return rows[size_t(a)].bpco < rows[size_t(b)].bpco || (rows[size_t(a)].bpco == rows[size_t(b)].bpco && a < b);
    });
    out.bpco_median = int32_t(rows[size_t(by_bpco[size_t(n / 2)])].bpco);
    uint64_t top_bpco = 0;
    for (int i = 0; i < n && i < 3; ++i) top_bpco += rows[size_t(by_bpco[size_t(n - 1 - i)])].bpco;
    out.bpco_top3 = int32_t(top_bpco / uint64_t((std::min)(n, 3)));
    auto weighted = [](uint64_t bytes, uint64_t coeff) -> int32_t {
        return coeff ? int32_t(bytes * 1000 / coeff) : 0;
    };
    out.bpco_l0 = weighted(level_bytes[0], level_coeff[0]);
    out.bpco_l1 = weighted(level_bytes[1], level_coeff[1]);
    out.bpco_l2 = weighted(level_bytes[2], level_coeff[2]);
    out.bpco_y = weighted(ch_bytes[0], ch_coeff[0]);
    out.bpco_chroma = weighted(chroma, ch_coeff[1] + ch_coeff[2]);
    const uint64_t mean = total / uint64_t(n);
    uint64_t var_sum = 0, mad_sum = 0, sum_ix = 0;
    const uint64_t median = ranked(n / 2);
    for (int i = 0; i < n; ++i) {
        const uint64_t value = ranked(i);
        const uint64_t delta = value > mean ? value - mean : mean - value;
        var_sum += delta * delta;
        const uint64_t mad = value > median ? value - median : median - value;
        mad_sum += mad;
        sum_ix += uint64_t(i + 1) * value;
    }
    const uint64_t sd = Phase3adIsqrt(var_sum / uint64_t(n));
    out.cv_permille = mean ? int32_t(sd * 1000 / mean) : 0;
    out.mad_permille = mean ? int32_t((mad_sum / uint64_t(n)) * 1000 / mean) : 0;
    out.gini_milli = total ? int32_t((2000 * sum_ix - 1000 * uint64_t(n + 1) * total) / (uint64_t(n) * total)) : 0;
    out.hhi_permille = total ? int32_t(sum_sq * 1000 / (total * total)) : 0;
    out.l0_max_share = share(level_max[0], level_bytes[0]);
    out.l1_max_share = share(level_max[1], level_bytes[1]);
    out.l2_max_share = share(level_max[2], level_bytes[2]);
    out.y_max_share = share(ch_max[0], total);
    const uint64_t chroma_max = (std::max)(ch_max[1], ch_max[2]);
    out.chroma_max_share = share(chroma_max, total);
    out.y_top_within = share(ch_max[0], ch_bytes[0]);
    out.chroma_top_within = share(chroma_max, chroma);
    out.quant_mean = int32_t(quant_sum / uint64_t(n));
    return out;
}
inline void Phase3adSelfCheck() {
    if (Phase3adIsqrt(0) != 0 || Phase3adIsqrt(1) != 1 || Phase3adIsqrt(10) != 3 || Phase3adIsqrt(100) != 10)
        throw std::runtime_error("phase3ad integer square root");
    std::vector<GpuEntropyBandDescriptor> bands(30);
    for (int i = 0; i < 30; ++i) {
        bands[size_t(i)].subband = i < 3 ? 0 : 1;
        bands[size_t(i)].payload_bytes = 1000;
        bands[size_t(i)].coefficient_count = 1000;
        bands[size_t(i)].width = 100; bands[size_t(i)].height = 10;
        bands[size_t(i)].channel = (i - 3) / 9; bands[size_t(i)].level = 0; bands[size_t(i)].quantization = 4;
    }
    const auto features = ComputePhase3adFeatures(bands);
    if (features.band_count != 27 || features.gini_milli != 0 || features.top1_share != 37 || features.hhi_permille != 37 ||
        features.bpco_mean != 1000 || features.total_payload != 27000)
        throw std::runtime_error("phase3ad feature self-check");
}
} // namespace HyperCine
