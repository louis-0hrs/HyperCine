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
#include "hypercine/filmscan3.h"    // QuantizeFilmScan3 reference rule
#include "hypercine/standalone.h"   // EncodeBgra for the end-to-end packet check
#include "hypercine/wavelet.h"

using hypercine::standalone::QuantizeFilmScan3;
using hypercine::standalone::QuantizeFilmScan3Block;

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


// End-to-end parity: a complete FilmScan3 CPU encode must be byte-identical
// whether the NEON kernels participate or not. Coefficient equality per level
// is necessary but not sufficient -- quantization, hashing and entropy packing
// all run on top, and a difference introduced by any of them would only show up
// here.
void check_full_encode_parity(int width, int height, const char* pattern, unsigned seed) {
    // BGRA source with the same deterministic generator as the coefficient tests.
    std::vector<uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
    uint32_t state = seed;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int16_t v = sample(9, x, y, width, height, state);
            uint8_t* px = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            px[0] = static_cast<uint8_t>(v & 0xFF);          // B
            px[1] = static_cast<uint8_t>((v >> 3) & 0xFF);   // G
            px[2] = static_cast<uint8_t>((v >> 7) & 0xFF);   // R
            px[3] = 0xFF;                                     // A
        }
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4;

    HyperCine_SetNEONWaveletEnabled(true);
    const auto with_neon = hypercine::standalone::EncodeBgra(
        bgra.data(), bgra.size(), width, height, stride);
    HyperCine_SetNEONWaveletEnabled(false);
    const auto without = hypercine::standalone::EncodeBgra(
        bgra.data(), bgra.size(), width, height, stride);
    HyperCine_SetNEONWaveletEnabled(true);

    // A vacuous pass would be two empty packets. Require a real packet before
    // comparing bytes, otherwise "equal" proves nothing.
    check(with_neon.size() > 1024, "encode produced a real packet", width, height, pattern,
          0, 0, -1, static_cast<long long>(with_neon.size()));
    check(with_neon.size() == without.size(), "encode packet size", width, height, pattern,
          0, 0, static_cast<long long>(without.size()), static_cast<long long>(with_neon.size()));
    if (with_neon.size() != without.size()) return;

    std::size_t first_diff = with_neon.size();
    for (std::size_t i = 0; i < with_neon.size(); ++i) {
        if (with_neon[i] != without[i]) { first_diff = i; break; }
    }
    check(first_diff == with_neon.size(), "encode packet bytes", width, height, pattern,
          static_cast<int>(first_diff), 0,
          first_diff == with_neon.size() ? 0 : 1, 0);
    if (first_diff != with_neon.size()) {
        std::printf("  first differing byte %zu: neon=%u scalar=%u\n", first_diff,
                    static_cast<unsigned>(with_neon[first_diff]),
                    static_cast<unsigned>(without[first_diff]));
    }
}


// Quantisation block parity. The encoder quantises whole planes through
// QuantizeFilmScan3Block, so that function has to match the single-value
// reference exactly -- including the `divisor == 1` identity and the unsigned
// midpoint subtleties. Tested directly rather than only through a packet,
// because a packet comparison cannot tell which stage differed.
void check_quantize_block_parity() {
    // Divisor coverage: every small value, then a spread across the legal range.
    std::vector<unsigned> divisors;
    for (unsigned q = 1; q <= 300; ++q) divisors.push_back(q);
    for (unsigned q = 301; q <= 32768; q += 137) divisors.push_back(q);
    divisors.push_back(32768);

    // Values that stress the rule: extremes, the unsigned midpoint edge, and a
    // deterministic sweep.
    std::vector<int16_t> values;
    for (int v : {-32768, -32767, -16384, -3, -2, -1, 0, 1, 2, 3, 16384, 32766, 32767})
        values.push_back(static_cast<int16_t>(v));
    uint32_t st = 0x5EEDu;
    for (int i = 0; i < 37; ++i) {
        st = st * 1664525u + 1013904223u;
        values.push_back(static_cast<int16_t>(st >> 16));
    }

    for (unsigned q : divisors) {
        std::vector<int16_t> block = values;
        // Reference: element by element through the single-value function.
        std::vector<int16_t> expected = values;
        for (auto& v : expected) v = QuantizeFilmScan3(v, q);

        QuantizeFilmScan3Block(block.data(), block.size(), q);

        for (std::size_t i = 0; i < block.size(); ++i) {
            check(block[i] == expected[i], "quantize block", 0, 0, "quantize",
                  static_cast<int>(i), static_cast<int>(q),
                  expected[i], block[i]);
        }
    }

    // Tail lengths: the vector loop handles 8 at a time, every remainder must
    // still go through the scalar rule.
    for (std::size_t n = 1; n <= 24; ++n) {
        std::vector<int16_t> block(values.begin(), values.begin() + n);
        std::vector<int16_t> expected = block;
        for (auto& v : expected) v = QuantizeFilmScan3(v, 7);
        QuantizeFilmScan3Block(block.data(), block.size(), 7);
        for (std::size_t i = 0; i < n; ++i)
            check(block[i] == expected[i], "quantize tail", 0, 0, "quantize-tail",
                  static_cast<int>(i), static_cast<int>(n), expected[i], block[i]);
    }

    // Empty and null must be no-ops, not crashes.
    QuantizeFilmScan3Block(nullptr, 0, 5);
    std::vector<int16_t> empty;
    QuantizeFilmScan3Block(empty.data(), empty.size(), 5);
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

    // Quantisation block parity, directly against the reference rule.
    check_quantize_block_parity();

    // End-to-end: whole packets, both switch states.
    // Widths must be multiples of 128 for the encoder contract; heights 8.
    for (int w : {128, 256, 384, 512}) {
        for (int h : {48, 96}) {
            check_full_encode_parity(w, h, "e2e-packet", 0xA5A5u + static_cast<unsigned>(w));
        }
    }


    // ---------------------------------------------------------------------
    // FilmScan3 full-packet parity across real geometries (issue #70, M1-C).
    //
    // The EncodeBgra loop above only reaches small tiles because the legacy
    // encoder carries kMaxPixels = 2,097,152 -- below 4K UHD. FilmScan3Encoder
    // has its own, larger budget, so the geometries that actually matter for a
    // production claim have to go through it. Anything less leaves the question
    // "does NEON agree at more than 512 pixels wide" unanswered.
    // ---------------------------------------------------------------------
    {
        struct Geometry { int width, height; const char* label; };
        const Geometry geometries[] = {
            {128,   24,   "minimum"},
            {1280,  720,  "hd-720"},
            {1920,  1080, "fhd-1080"},
            {2048,  1080, "dci-2048"},
            {3840,  2160, "uhd-4k"},
        };
        for (const auto& g : geometries) {
            const std::size_t pixels =
                static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height);
            std::vector<uint8_t> bgra(pixels * 4);
            // Deterministic, and different per geometry so one geometry cannot
            // accidentally reuse another's passing result.
            uint32_t state = 0x5EED0000u + static_cast<uint32_t>(pixels & 0xFFFFu);
            for (std::size_t i = 0; i < pixels; ++i) {
                state = state * 1664525u + 1013904223u;
                const uint8_t texture = static_cast<uint8_t>(state >> 24);
                uint8_t* px = bgra.data() + i * 4;
                px[0] = static_cast<uint8_t>((i * 7u + texture) & 0xFF);
                px[1] = static_cast<uint8_t>((i * 13u + texture) & 0xFF);
                px[2] = static_cast<uint8_t>((i * 29u + texture) & 0xFF);
                px[3] = 0xFF;
            }
            const std::size_t stride = static_cast<std::size_t>(g.width) * 4;

            hypercine::standalone::FilmScan3Geometry geometry{g.width, g.height, 0};

            HyperCine_SetNEONWaveletEnabled(true);
            hypercine::standalone::FilmScan3Encoder with_neon(geometry);
            const auto neon_frame = with_neon.Encode(bgra.data(), bgra.size(), stride);

            HyperCine_SetNEONWaveletEnabled(false);
            hypercine::standalone::FilmScan3Encoder without(geometry);
            const auto scalar_frame = without.Encode(bgra.data(), bgra.size(), stride);
            HyperCine_SetNEONWaveletEnabled(true);

            // A vacuous pass would be two empty packets.
            check(!neon_frame.sample.empty() && neon_frame.sample.size() > 1024,
                  "filmscan3 packet is real", g.width, g.height, g.label, 0, 0, 1,
                  static_cast<long long>(neon_frame.sample.size()));
            check(neon_frame.sample.size() == scalar_frame.sample.size(),
                  "filmscan3 packet size", g.width, g.height, g.label, 0, 0,
                  static_cast<long long>(scalar_frame.sample.size()),
                  static_cast<long long>(neon_frame.sample.size()));

            if (neon_frame.sample.size() == scalar_frame.sample.size()) {
                std::size_t first_diff = neon_frame.sample.size();
                for (std::size_t i = 0; i < neon_frame.sample.size(); ++i) {
                    if (neon_frame.sample[i] != scalar_frame.sample[i]) { first_diff = i; break; }
                }
                check(first_diff == neon_frame.sample.size(),
                      "filmscan3 packet bytes", g.width, g.height, g.label,
                      static_cast<int>(first_diff), 0, 0, 1);
                if (first_diff != neon_frame.sample.size()) {
                    std::printf("  first differing byte %zu: neon=%u scalar=%u (%s)\n",
                                first_diff, neon_frame.sample[first_diff],
                                scalar_frame.sample[first_diff], g.label);
                }
            }

            // All 30 coefficient hashes, not just the packet. A packet can match
            // while an individual band differs and quantization absorbs it.
            bool hashes_equal = neon_frame.coefficient_fnv64.size() ==
                                scalar_frame.coefficient_fnv64.size();
            std::size_t first_hash = 0;
            if (hashes_equal) {
                for (std::size_t i = 0; i < neon_frame.coefficient_fnv64.size(); ++i) {
                    if (neon_frame.coefficient_fnv64[i] != scalar_frame.coefficient_fnv64[i]) {
                        hashes_equal = false; first_hash = i; break;
                    }
                }
            }
            check(hashes_equal, "filmscan3 coefficient hashes", g.width, g.height, g.label,
                  static_cast<int>(first_hash), 0, 0, 1);
            check(neon_frame.coefficient_fnv64.size() == 30,
                  "filmscan3 has 30 hashes", g.width, g.height, g.label, 0, 0, 30,
                  static_cast<long long>(neon_frame.coefficient_fnv64.size()));
        }

        // Sequential frames must also agree: the frontend workspace is reused and
        // the rate state evolves, so a first-frame-only comparison would miss it.
        {
            const int w = 1280, h = 720;
            const std::size_t pixels = static_cast<std::size_t>(w) * h;
            hypercine::standalone::FilmScan3Geometry geometry{w, h, 0};
            HyperCine_SetNEONWaveletEnabled(true);
            hypercine::standalone::FilmScan3Encoder neon_enc(geometry);
            HyperCine_SetNEONWaveletEnabled(false);
            hypercine::standalone::FilmScan3Encoder scalar_enc(geometry);
            HyperCine_SetNEONWaveletEnabled(true);

            for (int frame = 0; frame < 8; ++frame) {
                std::vector<uint8_t> bgra(pixels * 4);
                uint32_t state = 0xC0FFEE00u + static_cast<uint32_t>(frame) * 0x9E3779B9u;
                for (std::size_t i = 0; i < pixels; ++i) {
                    state = state * 1664525u + 1013904223u;
                    const uint8_t t = static_cast<uint8_t>(state >> 24);
                    uint8_t* px = bgra.data() + i * 4;
                    px[0] = static_cast<uint8_t>((i * (7 + frame) + t) & 0xFF);
                    px[1] = static_cast<uint8_t>((i * (13 + frame) + t) & 0xFF);
                    px[2] = static_cast<uint8_t>((i * (29 + frame) + t) & 0xFF);
                    px[3] = 0xFF;
                }
                HyperCine_SetNEONWaveletEnabled(true);
                const auto a = neon_enc.Encode(bgra.data(), bgra.size(), w * 4);
                HyperCine_SetNEONWaveletEnabled(false);
                const auto b = scalar_enc.Encode(bgra.data(), bgra.size(), w * 4);
                HyperCine_SetNEONWaveletEnabled(true);

                const bool same = a.sample == b.sample &&
                                  a.state.frame_index == b.state.frame_index &&
                                  a.state.fs_rate_limiter == b.state.fs_rate_limiter;
                check(same, "filmscan3 8-frame sequence", w, h, "sequence", frame, 0, 0, 1);
                if (!same) {
                    std::printf("  frame %d differs: bytes %zu vs %zu, limiter %d vs %d\n",
                                frame, a.sample.size(), b.sample.size(),
                                a.state.fs_rate_limiter, b.state.fs_rate_limiter);
                }
            }
        }
    }

    // Switch sanity: the flag must actually flip.
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
