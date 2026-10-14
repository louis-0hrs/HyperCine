// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_vectors.h"
extern "C" {
#include "temporal.h"
}

namespace {

using HyperCineTest::AlignedVector;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int16_t Sat16(int32_t value) {
    return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, value)));
}

int16_t ArithmeticHalf(int32_t value) {
    return static_cast<int16_t>(value >= 0 ? value / 2 : -((-value + 1) / 2));
}

int16_t Expected(int16_t low, int16_t high, int frame, bool vector_path) {
    const int32_t combined = frame == 0 ? int32_t(low) - high : int32_t(low) + high;
    return ArithmeticHalf(vector_path ? Sat16(combined) : combined);
}

bool Matches(const AlignedVector<int16_t>& frame0,
             const AlignedVector<int16_t>& frame1,
             const AlignedVector<int16_t>& low,
             const AlignedVector<int16_t>& high,
             int width, int height, int pitch_samples, int vector_width) {
    const int vector_columns = width - width % vector_width;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = size_t(y) * pitch_samples + x;
            const bool vector_path = x < vector_columns;
            if (frame0[i] != Expected(low[i], high[i], 0, vector_path) ||
                frame1[i] != Expected(low[i], high[i], 1, vector_path)) return false;
        }
    }
    return true;
}

int RunCase(int width, int height) {
    const int pitch_samples = (width + 7) & ~7;
    const int pitch_bytes = pitch_samples * int(sizeof(int16_t));
    const size_t count = size_t(pitch_samples) * height;
    AlignedVector<int16_t> low(count), high(count), frame0(count), frame1(count), scratch(count);
    static constexpr std::array<std::array<int16_t, 2>, 12> cases{{
        {{0, 0}}, {{-1, 0}}, {{0, -1}}, {{30000, -30000}}, {{-30000, 30000}},
        {{30000, 30000}}, {{-30000, -30000}}, {{32767, 1}}, {{-32768, -1}},
        {{32767, -32768}}, {{-32768, 32767}}, {{16383, 16384}}
    }};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < pitch_samples; ++x) {
            const auto& c = cases[size_t(x + 3 * y) % cases.size()];
            const size_t i = size_t(y) * pitch_samples + x;
            low[i] = c[0];
            high[i] = c[1];
        }
    }

    ROI roi{};
    roi.width = width;
    roi.height = height;
    InvertTemporalQuant16s(reinterpret_cast<PIXEL*>(low.data()), 1, pitch_bytes,
        reinterpret_cast<PIXEL*>(high.data()), 1, pitch_bytes,
        reinterpret_cast<PIXEL*>(frame0.data()), pitch_bytes,
        reinterpret_cast<PIXEL*>(frame1.data()), pitch_bytes, roi,
        reinterpret_cast<PIXEL*>(scratch.data()), scratch.size() * sizeof(int16_t), 10);

    const bool generic = Matches(frame0, frame1, low, high, width, height, pitch_samples, 8);
    const bool pentium4 = Matches(frame0, frame1, low, high, width, height, pitch_samples, 40);
    Require(generic || pentium4, "upstream temporal output matches neither compiled SIMD/tail path");
    if (width < 40) Require(generic != pentium4, "fixture did not distinguish 8- and 40-wide paths");
    return generic ? 8 : 40;
}

} // namespace

int main() {
    try {
        // The function called by TransformInverseTemporalQuant uses the 8-wide
        // generic or 40-wide P4 vector prefix. At p10, its scalar tail uses an
        // arithmetic shift on the unsaturated sum; it does not use / 2.
        const int luma_path = RunCase(32, 2);
        Require(RunCase(16, 2) == luma_path, "luma/chroma dispatch paths disagree");
        RunCase(960, 2);
        RunCase(480, 2);
        std::cout << "upstream FIELDPLUS temporal semantics passed (selected vector width "
                  << luma_path << ", arithmetic-shift scalar tail)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
