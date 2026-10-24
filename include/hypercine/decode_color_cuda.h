// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <cstdint>
#if defined(HYPERCINE_CUDA_AVAILABLE) || defined(__CUDACC__)
#include <cuda_runtime.h>
#else
typedef struct CUstream_st* cudaStream_t;
typedef struct CUevent_st* cudaEvent_t;
typedef int cudaError_t;
#endif

namespace HyperCine {

enum class ColorMatrix : int {
    BT709 = 0,
    BT601 = 1
};

enum class ColorRange : int {
    CG = 0, // Computer Graphics (Full range: luma offset 16, range 0..255)
    VS = 1  // Video Systems (Studio range: luma offset 0, range 16..235)
};

enum class RowOrientation : int {
    BottomUp = 0, // Windows DIB standard (row 0 at buffer bottom, matching upstream BGRA)
    TopDown = 1   // Standard top-to-bottom
};

struct ColorCoefficients {
    int16_t ymult;
    int16_t r_vmult;
    int16_t g_vmult;
    int16_t g_umult;
    int16_t b_umult;
    int16_t y_offset;
    int16_t chroma_offset;
};

// Returns exact integer coefficients matching upstream ComputeColorCoefficientsYUVToRGB
inline ColorCoefficients GetColorCoefficients(ColorMatrix matrix, ColorRange range) {
    ColorCoefficients c{};
    c.chroma_offset = 128;
    if (range == ColorRange::CG) {
        c.ymult = 19072; // 128 * 149
        c.y_offset = 16;
        if (matrix == ColorMatrix::BT709) {
            c.r_vmult = 230;
            c.g_vmult = 137;
            c.g_umult = 55;
            c.b_umult = 135;
        } else { // BT601
            c.r_vmult = 204;
            c.g_vmult = 208;
            c.g_umult = 100;
            c.b_umult = 129;
        }
    } else { // ColorRange::VS
        c.ymult = 16384; // 128 * 128
        c.y_offset = 0;
        if (matrix == ColorMatrix::BT709) {
            c.r_vmult = 197;
            c.g_vmult = 118;
            c.g_umult = 47;
            c.b_umult = 116;
        } else { // BT601
            c.r_vmult = 175;
            c.g_vmult = 179;
            c.g_umult = 86;
            c.b_umult = 111;
        }
    }
    return c;
}

// Launch exact CUDA kernel converting planar signed-16 YUV422 to packed BGRA8
bool LaunchYuv422ToBgra8(
    const int16_t* d_y, size_t y_pitch,
    const int16_t* d_v, size_t v_pitch,
    const int16_t* d_u, size_t u_pitch,
    uint8_t* d_bgra, size_t bgra_pitch,
    int width, int height,
    ColorMatrix matrix = ColorMatrix::BT709,
    ColorRange range = ColorRange::CG,
    RowOrientation orientation = RowOrientation::BottomUp,
    cudaStream_t stream = nullptr,
    uint32_t* frame_error = nullptr,
    bool interlaced_row16 = false,
    // Optional: receives the launch result instead of collapsing it to bool,
    // so fail-closed callers can preserve the original CUDA error.
    cudaError_t* launch_error = nullptr
);

} // namespace HyperCine
