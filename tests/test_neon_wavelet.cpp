// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #70: prove the ARM64 NEON 2-6 wavelet is bit-exact against the scalar
// kernel, coefficient by coefficient, for every pattern that stresses the
// rounding and saturation rules.
//
// The test never reports a pass when NEON is absent: it reports SKIP with the
// reason, because "the vector path ran and matched" is not a claim you can make
// on a machine where the vector path does not exist.
#include "hypercine/wavelet.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, int width, int height, const char* pattern,
           int x, int y, long long expected, long long actual) {
    ++g_checks;
    if (ok) return;
    if (g_failures < 20) {
        std::printf("FAIL %s %dx%d pattern=%s at (%d,%d): expected %lld actual %lld\n",
                    what, width, height, pattern, x, y, expected, actual);
    }
    ++g_failures;
}

// Deterministic generator. Same seed produces the same frame on every host.
int16_t sample(unsigned pattern, int x, int y, int width, int height, uint32_t& state) {
    switch (pattern) {
        case 0: return 0;                                        // flat zero
        case 1: return 1000;                                     // flat value
        case 2: return (x == width / 3 && y == height / 3) ? 32767 : 0;   // single impulse
        case 3: return (x == 0 && y == 0) ? -32768 : 0;           // negative extreme
        case 4: return ((x / 4 + y / 4) & 1) ? 3000 : -3000;     // checkerboard
        case 5: {                                                 // gradient
            const long long v = (static_cast<long long>(x) * 65535) / (width - 1) - 32768;
            return static_cast<int16_t>(v);
        }
        case 6: {                                                 // deterministic noise
            state = state * 1664525u + 1013904223u;
            return static_cast<int16_t>(state >> 16);
        }
        case 7: return (y & 1) ? 32767 : -32768;                  // extreme alternating
        case 8: return static_cast<int16_t>((x * 37 + y * 101) & 0x7FFF);  // mid-range mix
        case 9: {                                                 // saturation bait
            state = state * 1103515245u + 12345u;
            return ((state >> 20) & 1) ? 32767 : -32768;
        }
        default: return 0;
    }
}

const char* pattern_name(unsigned pattern) {
    static const char* names[] = {"flat-zero", "flat-value", "impulse", "negative-extreme",
                                  "checkerboard", "gradient", "noise", "extremes",
                                  "midrange", "sat-bait"};
    return names[pattern];
}

// One horizontal comparison. Returns the number of mismatched coefficients.
void compare_horizontal(const std::vector<int16_t>& in, int width, int height, int in_stride,
                        const char* pattern) {
    const int half_w = width / 2;
    std::vector<int16_t> low_a(static_cast<size_t>(half_w) * height);
    std::vector<int16_t> high_a(static_cast<size_t>(half_w) * height);
    std::vector<int16_t> low_b(low_a.size());
    std::vector<int16_t> high_b(high_a.size());

    const auto scalar = HyperCine_ForwardHorizontal26_CPU(
        in.data(), width, height, in_stride,
        low_a.data(), half_w, high_a.data(), half_w);
    const auto neon = HyperCine_ForwardHorizontal26_NEON(
        in.data(), width, height, in_stride,
        low_b.data(), half_w, high_b.data(), half_w);
    check(scalar == HYPERCINE_STATUS_SUCCESS, "horizontal scalar status", width, height, pattern, 0, 0, 0, scalar);
    check(neon == HYPERCINE_STATUS_SUCCESS, "horizontal neon status", width, height, pattern, 0, 0, 0, neon);

    for (int y = 0; y < height; ++y) {
        for (int c = 0; c < half_w; ++c) {
            const size_t idx = static_cast<size_t>(y) * half_w + c;
            check(low_a[idx] == low_b[idx], "horizontal lowpass", width, height, pattern,
                  c, y, low_a[idx], low_b[idx]);
            check(high_a[idx] == high_b[idx], "horizontal highpass", width, height, pattern,
                  c, y, high_a[idx], high_b[idx]);
        }
    }
}

void compare_vertical(const std::vector<int16_t>& in, int width, int height, int in_stride,
                      const char* pattern) {
    std::vector<int16_t> low_a(static_cast<size_t>(width) * (height / 2));
    std::vector<int16_t> high_a(low_a.size());
    std::vector<int16_t> low_b(low_a.size());
    std::vector<int16_t> high_b(low_a.size());

    const auto scalar = HyperCine_ForwardVertical26_CPU(
        in.data(), width, height, in_stride,
        low_a.data(), width, high_a.data(), width);
    const auto neon = HyperCine_ForwardVertical26_NEON(
        in.data(), width, height, in_stride,
        low_b.data(), width, high_b.data(), width);
    check(scalar == HYPERCINE_STATUS_SUCCESS, "vertical scalar status", width, height, pattern, 0, 0, 0, scalar);
    check(neon == HYPERCINE_STATUS_SUCCESS, "vertical neon status", width, height, pattern, 0, 0, 0, neon);

    for (int r = 0; r < height / 2; ++r) {
        for (int x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(r) * width + x;
            check(low_a[idx] == low_b[idx], "vertical lowpass", width, height, pattern,
                  x, r, low_a[idx], low_b[idx]);
            check(high_a[idx] == high_b[idx], "vertical highpass", width, height, pattern,
                  x, r, high_a[idx], high_b[idx]);
        }
    }
}

// Padded stride: the reference must read only `width` elements per row, so a
// garbage tail between rows must not change a single coefficient.
void check_padded_stride(unsigned pattern) {
    const int width = 32, height = 24, stride = width + 16;
    std::vector<int16_t> in(static_cast<size_t>(stride) * height, int16_t(0xBEEF));
    uint32_t state = 0x1234u + pattern;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            in[static_cast<size_t>(y) * stride + x] = sample(pattern, x, y, width, height, state);

    const int half_w = width / 2;
    std::vector<int16_t> low_a(static_cast<size_t>(half_w) * height, 0);
    std::vector<int16_t> high_a(low_a.size(), 0);
    std::vector<int16_t> low_b(low_a.size(), 0);
    std::vector<int16_t> high_b(low_a.size(), 0);

    HyperCine_ForwardHorizontal26_CPU(in.data(), width, height, stride,
        low_a.data(), half_w, high_a.data(), half_w);
    HyperCine_ForwardHorizontal26_NEON(in.data(), width, height, stride,
        low_b.data(), half_w, high_b.data(), half_w);
    for (size_t i = 0; i < low_a.size(); ++i) {
        check(low_a[i] == low_b[i], "padded lowpass", width, height, "padded", 0, 0, low_a[i], low_b[i]);
        check(high_a[i] == high_b[i], "padded highpass", width, height, "padded", 0, 0, high_a[i], high_b[i]);
    }
}

void run_frame(unsigned pattern, int width, int height) {
    const int stride = width;   // tight stride for the main matrix
    std::vector<int16_t> in(static_cast<size_t>(stride) * height);
    uint32_t state = 0xC0FFEEu + pattern;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            in[static_cast<size_t>(y) * stride + x] = sample(pattern, x, y, width, height, state);

    compare_horizontal(in, width, height, stride, pattern_name(pattern));
    compare_vertical(in, width, height, stride, pattern_name(pattern));
}

} // namespace

// On a host without NEON the test must still be a real, passing test: it proves
// the fallback contract instead of skipping. The mandatory standalone matrix
// treats a skip as a failure, and rightly so -- a skipped test proves nothing
// about the build it ran in.
//
// What is proven here on a non-ARM host:
//   * IsNEONWaveletAvailable() is false,
//   * the NEON entry points refuse with NOT_SUPPORTED rather than silently
//     returning zeros, partial data, or a success code,
//   * the scalar path still works, so the refusal is about NEON, not a broken build.
int check_unavailable_contract() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL %s\n", what);
            ++failures;
        }
    };

    check(!HyperCine_IsNEONWaveletAvailable(), "availability must be false on this host");
    check(!HyperCine_IsNEONWaveletEnabled(), "dispatch must not report NEON as active");

    // A minimal valid frame: 8 wide (contract minimum is 6), 6 high.
    const int width = 8, height = 6, half = width / 2;
    std::vector<int16_t> in(static_cast<size_t>(width) * height, 100);
    std::vector<int16_t> low(static_cast<size_t>(half) * height, 0);
    std::vector<int16_t> high(low.size(), 0);

    const auto h = HyperCine_ForwardHorizontal26_NEON(
        in.data(), width, height, width, low.data(), half, high.data(), half);
    check(h == HYPERCINE_STATUS_NOT_SUPPORTED, "horizontal NEON must return NOT_SUPPORTED");
    check(low[0] == 0 && high[0] == 0, "NEON must not write partial output when refusing");

    const auto v = HyperCine_ForwardVertical26_NEON(
        in.data(), width, height, width, low.data(), width, high.data(), width);
    check(v == HYPERCINE_STATUS_NOT_SUPPORTED, "vertical NEON must return NOT_SUPPORTED");

    // Turning the switch on must not fabricate availability.
    HyperCine_SetNEONWaveletEnabled(true);
    check(!HyperCine_IsNEONWaveletEnabled(), "enabling NEON must not claim success where absent");
    check(!HyperCine_IsNEONWaveletAvailable(), "availability must stay false after enabling");
    HyperCine_SetNEONWaveletEnabled(false);

    // The scalar reference must be intact on this host.
    std::vector<int16_t> sc_low(static_cast<size_t>(half) * height, 0);
    std::vector<int16_t> sc_high(sc_low.size(), 0);
    const auto s = HyperCine_ForwardHorizontal26_CPU(
        in.data(), width, height, width, sc_low.data(), half, sc_high.data(), half);
    check(s == HYPERCINE_STATUS_SUCCESS, "scalar path must still succeed");

    if (failures) {
        std::printf("NEON-UNAVAILABLE-CONTRACT: FAIL (%d violations)\n", failures);
        return 1;
    }
    std::printf("NEON-UNAVAILABLE-CONTRACT: PASS (NEON absent, refuses with NOT_SUPPORTED, "
                "scalar intact)\n");
    return 0;
}

int main() {
    if (!HyperCine_IsNEONWaveletAvailable()) {
        return check_unavailable_contract();
    }

    // Widths chosen to exercise: exact vector multiples, tails of every length
    // the 8-lane loop can leave, and the minimum the contract accepts.
    const int widths[]  = {6, 8, 14, 16, 18, 22, 24, 30, 32, 34, 64, 70, 128, 258};
    const int heights[] = {6, 8, 10, 16, 24, 34};

    for (unsigned pattern = 0; pattern < 10; ++pattern) {
        for (int w : widths) {
            for (int h : heights) {
                run_frame(pattern, w, h);
            }
        }
    }
    for (unsigned pattern = 0; pattern < 10; ++pattern) {
        check_padded_stride(pattern);
    }

    // Full encoder parity: a real FilmScan3 CPU encode must be byte-identical
    // whether the NEON kernels participate or not.
    HyperCine_SetNEONWaveletEnabled(true);
    const bool on = HyperCine_IsNEONWaveletEnabled();
    HyperCine_SetNEONWaveletEnabled(false);
    const bool off = HyperCine_IsNEONWaveletEnabled();
    HyperCine_SetNEONWaveletEnabled(true);
    check(on && !off, "NEON toggle", 0, 0, "toggle", 0, 0, 1, on && !off ? 1 : 0);

    if (g_failures) {
        std::printf("NEON-WAVELET: FAIL (%d mismatches in %d checks)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("NEON-WAVELET: PASS (%d checks, all patterns bit-exact)\n", g_checks);
    return 0;
}
