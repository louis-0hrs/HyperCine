// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// NEON vs scalar performance for the 2-6 forward wavelet (issue #70).
//
// This file measures. It is not a correctness gate -- that lives in
// tests/test_neon_wavelet.cpp, which compares 1.4M coefficients. Here the two
// paths are compared for time only, and each row asserts band equality for the
// exact input being timed first, so a speedup can never be reported for a path
// that computed something different.
//
// Two measurements, because they answer different questions:
//   * the kernel alone: how much faster is the vector path itself
//   * the whole encode: how much does a caller actually gain
// The second number is always much smaller, and quoting only the first would
// overstate the work.
#include "hypercine/filmscan3.h"
#include "hypercine/standalone.h"
#include "hypercine/wavelet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace hypercine::standalone;
using Clock = std::chrono::steady_clock;

namespace {

// Emitted between rows so the output is valid JSON. Without it the four rows
// were separated only by a newline, which no parser accepts -- found because a
// downstream reader failed on it, not because the benchmark complained.
bool g_first_row = true;
const char* row_separator() {
    if (g_first_row) { g_first_row = false; return ""; }
    return ",\n";
}

double ms(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct Stats { double median = 0, p95 = 0, worst = 0; };

// 95th percentile by nearest rank, ceil(0.95*n)-1. Computed this way on purpose:
// with a short run the naive `v[n*0.95]` lands on the maximum and reports
// p95 == worst, which hides the distribution.
Stats stats_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    Stats s;
    s.median = v[v.size() / 2];
    std::size_t rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(v.size())));
    if (rank > 0) --rank;
    s.p95 = v[std::min(v.size() - 1, rank)];
    s.worst = v.back();
    return s;
}

bool bands_equal(const WaveletLevel& a, const WaveletLevel& b) {
    for (int band = 0; band < 4; ++band) {
        const Plane& pa = a.bands[band];
        const Plane& pb = b.bands[band];
        if (pa.width != pb.width || pa.height != pb.height) return false;
        for (int y = 0; y < pa.height; ++y)
            for (int x = 0; x < pa.width; ++x)
                if (pa.pixels[static_cast<std::size_t>(y) * pa.stride + x] !=
                    pb.pixels[static_cast<std::size_t>(y) * pb.stride + x])
                    return false;
    }
    return true;
}

// Deterministic, non-degenerate content. A flat input would let either path look
// fast for the wrong reason.
Plane make_plane(int w, int h, uint32_t seed) {
    Plane p(w, h);
    uint32_t state = seed;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            state = state * 1664525u + 1013904223u;
            p.pixels[static_cast<std::size_t>(y) * p.stride + x] =
                static_cast<int16_t>(((state >> 16) & 0x3FFF) - 8192);
        }
    }
    return p;
}

std::vector<uint8_t> make_bgra(int w, int h, uint32_t seed) {
    std::vector<uint8_t> bgra(static_cast<std::size_t>(w) * h * 4);
    uint32_t state = seed;
    for (std::size_t i = 0; i < bgra.size(); i += 4) {
        state = state * 1664525u + 1013904223u;
        bgra[i] = static_cast<uint8_t>(state >> 24);
        bgra[i + 1] = static_cast<uint8_t>(state >> 16);
        bgra[i + 2] = static_cast<uint8_t>(state >> 8);
        bgra[i + 3] = 0xFF;
    }
    return bgra;
}

// One kernel measurement. Alternating order keeps a thermal ramp from favouring
// whichever path always ran second.
void measure_kernel(int w, int h) {
    const Plane plane = make_plane(w, h, 0x9E3779B9u);
    const int warmup = 3, iters = 40;
    std::vector<double> on, off;

    for (int i = 0; i < warmup + iters; ++i) {
        HyperCine_SetNEONWaveletEnabled(i % 2 == 0);
        auto t = Clock::now();
        auto a = ForwardLevel(plane, 0);
        const double d1 = ms(t);
        HyperCine_SetNEONWaveletEnabled(i % 2 != 0);
        t = Clock::now();
        auto b = ForwardLevel(plane, 0);
        const double d2 = ms(t);
        if (i == 0) {
            // Restore, then prove equality on the exact input being timed.
            HyperCine_SetNEONWaveletEnabled(true);
            if (!bands_equal(a, b)) {
                std::printf("FAIL: %dx%d bands differ, timing would be meaningless\n", w, h);
                std::exit(1);
            }
        }
        if (i >= warmup) {
            if (i % 2 == 0) { on.push_back(d1); off.push_back(d2); }
            else            { on.push_back(d2); off.push_back(d1); }
        }
    }
    HyperCine_SetNEONWaveletEnabled(true);

    const Stats s_on = stats_of(on), s_off = stats_of(off);
    std::printf("%s{\"scope\":\"kernel\",\"width\":%d,\"height\":%d,\"iters\":%d,"
                "\"neon_median_ms\":%.4f,\"neon_p95_ms\":%.4f,\"neon_worst_ms\":%.4f,"
                "\"scalar_median_ms\":%.4f,\"scalar_p95_ms\":%.4f,\"scalar_worst_ms\":%.4f,"
                "\"speedup\":%.3f,\"bands_bit_equal\":true}\n",
                row_separator(), w, h, iters, s_on.median, s_on.p95, s_on.worst,
                s_off.median, s_off.p95, s_off.worst, s_off.median / s_on.median);
}

// Whole-frame measurement through the production encoder.
void measure_encode(int w, int h) {
    const auto bgra = make_bgra(w, h, 0xABCDEF01u);
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    FilmScan3Geometry geom;
    geom.width = w;
    geom.height = h;

    auto encode_once = [&]() {
        FilmScan3Encoder enc{geom};
        return enc.Encode(bgra.data(), bgra.size(), stride);
    };

    HyperCine_SetNEONWaveletEnabled(true);
    const auto ref_on = encode_once();
    HyperCine_SetNEONWaveletEnabled(false);
    const auto ref_off = encode_once();
    HyperCine_SetNEONWaveletEnabled(true);

    if (ref_on.sample != ref_off.sample) {
        std::printf("FAIL: %dx%d packets differ (%zu vs %zu bytes)\n", w, h,
                    ref_on.sample.size(), ref_off.sample.size());
        std::exit(1);
    }

    const int warmup = 2, iters = 9;
    std::vector<double> on, off;
    for (int i = 0; i < warmup + iters; ++i) {
        HyperCine_SetNEONWaveletEnabled(i % 2 == 0);
        auto t = Clock::now(); encode_once(); const double d1 = ms(t);
        HyperCine_SetNEONWaveletEnabled(i % 2 != 0);
        t = Clock::now(); encode_once(); const double d2 = ms(t);
        if (i >= warmup) {
            if (i % 2 == 0) { on.push_back(d1); off.push_back(d2); }
            else            { on.push_back(d2); off.push_back(d1); }
        }
    }
    HyperCine_SetNEONWaveletEnabled(true);

    const Stats s_on = stats_of(on), s_off = stats_of(off);
    std::printf("%s{\"scope\":\"encode\",\"width\":%d,\"height\":%d,\"iters\":%d,"
                "\"packet_bytes\":%zu,\"packet_equal\":true,"
                "\"neon_median_ms\":%.3f,\"neon_p95_ms\":%.3f,"
                "\"scalar_median_ms\":%.3f,\"scalar_p95_ms\":%.3f,"
                "\"saved_ms\":%.3f,\"saved_percent\":%.2f,\"speedup\":%.4f}\n",
                row_separator(), w, h, iters, ref_on.sample.size(),
                s_on.median, s_on.p95, s_off.median, s_off.p95,
                s_off.median - s_on.median,
                (s_off.median - s_on.median) / s_off.median * 100.0,
                s_off.median / s_on.median);
}

} // namespace

int main() {
    if (!HyperCine_IsNEONWaveletAvailable()) {
        // Not a skip: on a host without NEON there is nothing to compare, and the
        // standalone matrix treats a skip as a failure.
        std::printf("{\"note\":\"NEON not compiled in; nothing to compare\",\"runs\":[]}\n");
        return 0;
    }
    std::printf("{\"device\":\"arm64 NEON\",\"runs\":[\n");
    measure_kernel(1920, 1080);
    measure_encode(1920, 1080);
    measure_kernel(3840, 2160);
    measure_encode(3840, 2160);
    std::printf("]}\n");
    return 0;
}
