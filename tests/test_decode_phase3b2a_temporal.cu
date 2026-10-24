// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "decode_inverse_kernels.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(cudaError_t status, const char* what) {
    if (status != cudaSuccess) throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
}

int16_t Sat16(int32_t v) {
    return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, v)));
}

int16_t Reference(int16_t low, int16_t high, int frame, bool scalar_tail) {
    int32_t combined = frame == 0 ? int32_t(low) - high : int32_t(low) + high;
    if (!scalar_tail) combined = Sat16(combined);
    return static_cast<int16_t>(combined >= 0 ? combined / 2 : -((-combined + 1) / 2));
}

void RunCase(int width, int vector_width) {
    static constexpr std::array<std::array<int16_t, 2>, 16> cases{{
        {{0, 0}}, {{8, 2}}, {{-8, -2}}, {{-1, 0}}, {{0, -1}},
        {{30000, -30000}}, {{30000, 30000}}, {{-30000, -30000}},
        {{-30000, 30000}}, {{32767, -1}}, {{-32768, 1}},
        {{32767, 32767}}, {{-32768, -32768}}, {{16383, -30000}},
        {{-16384, 30000}}, {{12345, -23456}}
    }};
    std::vector<int16_t> low(static_cast<size_t>(width));
    std::vector<int16_t> high(static_cast<size_t>(width));
    for (int i = 0; i < width; ++i) {
        const auto& c = cases[size_t(i) % cases.size()];
        low[size_t(i)] = c[0];
        high[size_t(i)] = c[1];
    }
    // Force overflow cases into any scalar tail. Upstream preserves the full
    // signed sum there, unlike its saturating SIMD prefix.
    const int vector_columns = width - width % vector_width;
    if (vector_columns < width) {
        low[size_t(vector_columns)] = 30000;
        high[size_t(vector_columns)] = -30000;
        if (vector_columns + 1 < width) {
            low[size_t(vector_columns + 1)] = -30000;
            high[size_t(vector_columns + 1)] = 30000;
        }
    }

    int16_t *d_low = nullptr, *d_high = nullptr, *d_output = nullptr;
    const size_t bytes = size_t(width) * sizeof(int16_t);
    Check(cudaMalloc(&d_low, bytes), "cudaMalloc low");
    Check(cudaMalloc(&d_high, bytes), "cudaMalloc high");
    Check(cudaMalloc(&d_output, bytes), "cudaMalloc output");
    try {
        Check(cudaMemcpy(d_low, low.data(), bytes, cudaMemcpyHostToDevice), "copy low");
        Check(cudaMemcpy(d_high, high.data(), bytes, cudaMemcpyHostToDevice), "copy high");
        const dim3 block(128, 1);
        const dim3 grid((width + int(block.x) - 1) / int(block.x), 1);
        for (int frame = 0; frame < 2; ++frame) {
            HyperCine::FieldPlusTemporalInverseKernel<<<grid, block>>>(
                d_low, int(bytes), d_high, int(bytes), d_output, int(bytes),
                width, 1, frame, vector_width);
            Check(cudaGetLastError(), "launch temporal inverse");
            std::vector<int16_t> actual(static_cast<size_t>(width));
            Check(cudaMemcpy(actual.data(), d_output, bytes, cudaMemcpyDeviceToHost), "copy output");
            for (int x = 0; x < width; ++x) {
                const bool tail = x >= vector_columns;
                const int16_t expected = Reference(low[size_t(x)], high[size_t(x)], frame, tail);
                if (actual[size_t(x)] != expected)
                    throw std::runtime_error("temporal coefficient mismatch at width=" + std::to_string(width) +
                        ", x=" + std::to_string(x) + ", frame=" + std::to_string(frame));
            }
        }
    } catch (...) {
        cudaFree(d_output); cudaFree(d_high); cudaFree(d_low);
        throw;
    }
    cudaFree(d_output); cudaFree(d_high); cudaFree(d_low);
}

void RunFrameGeometry(int frame_width) {
    if (frame_width < 64 || frame_width > 7680 || frame_width % 16 != 0)
        throw std::runtime_error("illegal progressive FIELDPLUS frame width in geometry audit");
    const int luma_temporal_width = frame_width / 2;
    const int chroma_temporal_width = frame_width / 4;
    RunCase(luma_temporal_width, 8);
    RunCase(chroma_temporal_width, 8);
    RunCase(luma_temporal_width, 40);
    RunCase(chroma_temporal_width, 40);
}

} // namespace

int main() {
    try {
        int devices = 0;
        Check(cudaGetDeviceCount(&devices), "cudaGetDeviceCount");
        if (devices == 0) throw std::runtime_error("no CUDA device available");
        // The temporal wavelet operates at half frame resolution: at 64x64
        // its luma/chroma widths are 32/16, and at 1920x1080 they are 960/480.
        // Audit legal widths from every modulo-80 class: with 16-pixel frame
        // width alignment these classes cover both the 8-wide generic path
        // and the 40-wide Pentium 4 path's possible scalar tails. The GPU
        // output is compared with the saturation and tail rules confirmed by
        // the direct upstream InvertTemporalQuant16s oracle.
        for (const int frame_width : {64, 80, 96, 112, 128}) RunFrameGeometry(frame_width);
        RunFrameGeometry(1920);
        std::cout << "progressive FIELDPLUS temporal inverse exact vectors passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
