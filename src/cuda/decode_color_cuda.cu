// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_color_cuda.h"
#include <cuda_runtime.h>
#include "decode_error_guard.cuh"
#include <cstdint>

namespace HyperCine {

namespace {

__device__ __forceinline__ int16_t sat_add16_gpu(int16_t a, int16_t b) {
    int32_t sum = int32_t(a) + int32_t(b);
    if (sum > 32767) return 32767;
    if (sum < -32768) return -32768;
    return static_cast<int16_t>(sum);
}

__device__ __forceinline__ int16_t sat_sub16_gpu(int16_t a, int16_t b) {
    int32_t diff = int32_t(a) - int32_t(b);
    if (diff > 32767) return 32767;
    if (diff < -32768) return -32768;
    return static_cast<int16_t>(diff);
}

__device__ __forceinline__ int16_t clamp255_simd_gpu(int16_t v) {
    // Exact emulation of upstream CineForm SSE2 limiter:
    // _mm_adds_epi16(v, 0x7F00)
    int32_t add = int32_t(v) + 0x7F00;
    if (add > 32767) add = 32767;
    if (add < -32768) add = -32768;
    uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(add));
    // _mm_subs_epu16(u, 0x7F00)
    if (u < 0x7F00) return 0;
    return static_cast<int16_t>(u - 0x7F00);
}

__device__ __forceinline__ uint8_t packus8_gpu(int32_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

__global__ void Yuv422ToBgra8Kernel(
    const int16_t* __restrict__ d_y, size_t y_pitch,
    const int16_t* __restrict__ d_v, size_t v_pitch,
    const int16_t* __restrict__ d_u, size_t u_pitch,
    uint8_t* __restrict__ d_bgra, size_t bgra_pitch,
    int width, int height,
    int post_column_pixels,
    ColorCoefficients coeff,
    RowOrientation orientation, uint32_t* frame_error)
{
    if (DecoderFrameFailed(frame_error)) return;
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) return;

    int out_y = (orientation == RowOrientation::BottomUp) ? (height - 1 - y) : y;

    const int16_t* y_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(d_y) + y * y_pitch);
    const int16_t* v_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(d_v) + y * v_pitch);
    const int16_t* u_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(d_u) + y * u_pitch);
    uint8_t* out_row = d_bgra + out_y * bgra_pitch;

    int16_t raw_y = y_row[x];
    int16_t raw_v = v_row[x / 2];
    int16_t raw_u = u_row[x / 2];

    // Inverse reconstruction can undershoot zero. Preserve its sign while
    // reducing precision; unsigned shifts turn dark overshoots into highlights.
    int16_t y8 = static_cast<int16_t>(raw_y >> 2);
    int16_t v8 = static_cast<int16_t>(raw_v >> 2);
    int16_t u8 = static_cast<int16_t>(raw_u >> 2);

    uint8_t b_out = 0, g_out = 0, r_out = 0;

    if (x < post_column_pixels) {
        // Fast SIMD path matching upstream InvertHorizontalStripYUV16sToPackedRGB32 fast loop
        int16_t yy = clamp255_simd_gpu(sat_sub16_gpu(y8, coeff.y_offset));
        int16_t uu = sat_sub16_gpu(clamp255_simd_gpu(u8), coeff.chroma_offset);
        int16_t vv = sat_sub16_gpu(clamp255_simd_gpu(v8), coeff.chroma_offset);

        int16_t yy_slli = static_cast<int16_t>(yy << 7);
        int16_t yy_hi = static_cast<int16_t>((int32_t(yy_slli) * int32_t(coeff.ymult)) >> 16);
        int16_t yy_sse = static_cast<int16_t>(yy_hi << 1);

        int16_t t_r = static_cast<int16_t>((int16_t(vv * coeff.r_vmult)) >> 1);
        int16_t r1 = sat_add16_gpu(yy_sse, t_r);
        r1 = sat_add16_gpu(r1, 32);
        r1 = static_cast<int16_t>(r1 >> 6);

        int16_t t_gv = static_cast<int16_t>((int16_t(vv * coeff.g_vmult)) >> 2);
        int16_t g1 = sat_sub16_gpu(yy_sse, t_gv);
        int16_t t_gu = static_cast<int16_t>((int16_t(uu * coeff.g_umult)) >> 2);
        g1 = sat_sub16_gpu(g1, t_gu);
        g1 = sat_add16_gpu(g1, 32);
        g1 = static_cast<int16_t>(g1 >> 6);

        int16_t t_bu = static_cast<int16_t>(uu * coeff.b_umult);
        int16_t b1 = sat_add16_gpu(yy_sse, t_bu);
        b1 = sat_add16_gpu(b1, 32);
        b1 = static_cast<int16_t>(b1 >> 6);

        b_out = packus8_gpu(b1);
        g_out = packus8_gpu(g1);
        r_out = packus8_gpu(r1);
    } else {
        // Scalar fallback path matching upstream right-border loop
        int32_t yy = y8 - coeff.y_offset;
        int32_t uu = u8 - coeff.chroma_offset;
        int32_t vv = v8 - coeff.chroma_offset;

        int32_t yy_scaled = (yy * coeff.ymult) >> 7;
        int32_t r1 = (yy_scaled                          + coeff.r_vmult * vv + 64) >> 7;
        int32_t g1 = (yy_scaled * 2 - coeff.g_umult * uu - coeff.g_vmult * vv + 128) >> 8;
        int32_t b1 = (yy_scaled     + 2 * coeff.b_umult * uu                  + 64) >> 7;

        b_out = packus8_gpu(b1);
        g_out = packus8_gpu(g1);
        r_out = packus8_gpu(r1);
    }

    out_row[4 * x + 0] = b_out;
    out_row[4 * x + 1] = g_out;
    out_row[4 * x + 2] = r_out;
    out_row[4 * x + 3] = 255;
}

} // anonymous namespace

bool LaunchYuv422ToBgra8(
    const int16_t* d_y, size_t y_pitch,
    const int16_t* d_v, size_t v_pitch,
    const int16_t* d_u, size_t u_pitch,
    uint8_t* d_bgra, size_t bgra_pitch,
    int width, int height,
    ColorMatrix matrix,
    ColorRange range,
    RowOrientation orientation,
    cudaStream_t stream, uint32_t* frame_error)
{
    if (!d_y || !d_v || !d_u || !d_bgra || width <= 0 || height <= 0) {
        return false;
    }

    ColorCoefficients coeff = GetColorCoefficients(matrix, range);

    // Compute exact border column where upstream switches from SIMD to scalar fallback
    int lowpass_width = width / 2;
    int post_column_lowpass = lowpass_width - (lowpass_width % 16);
    while (post_column_lowpass > (lowpass_width - 2 - 2)) {
        post_column_lowpass -= 16;
    }
    int post_column_pixels = post_column_lowpass * 2;

    dim3 block(32, 8);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    Yuv422ToBgra8Kernel<<<grid, block, 0, stream>>>(
        d_y, y_pitch,
        d_v, v_pitch,
        d_u, u_pitch,
        d_bgra, bgra_pitch,
        width, height,
        post_column_pixels,
        coeff,
        orientation, frame_error
    );

    return cudaGetLastError() == cudaSuccess;
}

} // namespace HyperCine
