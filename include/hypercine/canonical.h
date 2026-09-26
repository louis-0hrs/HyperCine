// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_CANONICAL_H
#define HYPERCINE_CANONICAL_H

#include <stdint.h>
#include <stddef.h>
#include <algorithm>
#include <vector>

// One configuration for every Phase 1C measurement.
// Pixel format, quality, and frame bytes are part of the result, not an accident
// of whichever harness happened to run.

namespace hypercine {

constexpr uint32_t kCanonicalSeed = 0x1337BEEFu;
constexpr int kCanonicalWarmupFrames = 2;
constexpr int kCanonicalTimedFrames = 4;

// CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
// CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3, GOP length 1.
// Those values live in the CineForm headers; this file records the contract.

enum class CanonicalPattern {
    Flat128 = 0,          // Phase 1B matrix full-encoder input
    GradientChecker = 1,  // Phase 1B profiler input
    Canonical = 2         // All future measurements
};

inline const char* PatternName(CanonicalPattern pattern) {
    switch (pattern) {
    case CanonicalPattern::Flat128: return "flat_bgra_128";
    case CanonicalPattern::GradientChecker: return "gradient_checker";
    case CanonicalPattern::Canonical: return "canonical_lcg_0x1337BEEF";
    }
    return "unknown";
}

// Deterministic BGRA8 frame. The generator is not inside the encode timer.
inline std::vector<uint8_t> MakeFrame(int width, int height, CanonicalPattern pattern) {
    std::vector<uint8_t> frame(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    if (pattern == CanonicalPattern::Flat128) {
        std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(128));
        for (size_t i = 3; i < frame.size(); i += 4) frame[i] = 255;
        return frame;
    }
    if (pattern == CanonicalPattern::GradientChecker) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
                uint8_t c1 = static_cast<uint8_t>((x * 255) / width);
                uint8_t c2 = static_cast<uint8_t>((y * 255) / height);
                uint8_t chk = (((x / 32) ^ (y / 32)) & 1) ? 200 : 50;
                frame[idx + 0] = static_cast<uint8_t>((c1 + chk) / 2);
                frame[idx + 1] = static_cast<uint8_t>((c2 + chk) / 2);
                frame[idx + 2] = static_cast<uint8_t>(c1 ^ c2);
                frame[idx + 3] = 255;
            }
        }
        return frame;
    }
    // Spatial gradient mixed with a fixed LCG so entropy is neither empty nor pure noise.
    uint32_t s = kCanonicalSeed;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            s = s * 1664525u + 1013904223u;
            size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            const int safe_w = width > 0 ? width : 1;
            const int safe_h = height > 0 ? height : 1;
            uint32_t gx = static_cast<uint32_t>((x * 255) / safe_w);
            uint32_t gy = static_cast<uint32_t>((y * 255) / safe_h);
            uint32_t n = (s >> 24) & 0x0Fu;
            frame[idx + 0] = static_cast<uint8_t>((gx * 13u + n) / 14u);
            frame[idx + 1] = static_cast<uint8_t>((gy * 9u + (n >> 1)) / 10u);
            frame[idx + 2] = static_cast<uint8_t>(((gx / 2u + gy / 2u) + n) / 2u);
            frame[idx + 3] = 255;
        }
    }
    return frame;
}

struct Resolution {
    const char* name;
    int width;
    int height;
};

inline const Resolution kResolutions[] = {
    {"1080p", 1920, 1080},
    {"4K_UHD", 3840, 2160},
    {"8K_UHD", 7680, 4320},
};

}  // namespace hypercine

#endif
