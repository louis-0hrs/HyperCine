// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <cuda_runtime.h>
#include "decode_error_guard.cuh"
#include <cstdint>

namespace HyperCine {

__device__ __forceinline__ int16_t sat16_gpu(int32_t val) {
    if (val < -32768) return -32768;
    if (val > 32767) return 32767;
    return static_cast<int16_t>(val);
}

__device__ __forceinline__ int16_t sat16_add_gpu(int16_t a, int16_t b) {
    return sat16_gpu(int32_t(a) + int32_t(b));
}

__device__ __forceinline__ int16_t sat16_sub_gpu(int16_t a, int16_t b) {
    return sat16_gpu(int32_t(a) - int32_t(b));
}

// Vertical Inverse Wavelet Kernel
// Reads lowlow (LL), lowhigh (LH), highlow (HL), highhigh (HH)
// Computes even_low, odd_low, even_high, odd_high and writes them to vert_low and vert_high
__global__ inline void VerticalInverseKernel(
    const int16_t* __restrict__ lowlow, int ll_pitch,
    const int16_t* __restrict__ lowhigh, int lh_pitch,
    const int16_t* __restrict__ highlow, int hl_pitch,
    const int16_t* __restrict__ highhigh, int hh_pitch,
    int16_t* __restrict__ vert_low, int vl_pitch,
    int16_t* __restrict__ vert_high, int vh_pitch,
    int width, int height, int descale, uint32_t* frame_error = nullptr)
{
    if (DecoderFrameFailed(frame_error)) return;
    int c = blockIdx.x * blockDim.x + threadIdx.x;
    int r = blockIdx.y * blockDim.y + threadIdx.y;

    if (c >= width || r >= height) return;

    int ll_stride = ll_pitch / sizeof(int16_t);
    int lh_stride = lh_pitch / sizeof(int16_t);
    int hl_stride = hl_pitch / sizeof(int16_t);
    int hh_stride = hh_pitch / sizeof(int16_t);
    int vl_stride = vl_pitch / sizeof(int16_t);
    int vh_stride = vh_pitch / sizeof(int16_t);

    int16_t el, ol, eh, oh;

    if (r == 0) {
        // Top border
        int32_t even_l = (((11 * lowlow[0 * ll_stride + c] - 4 * lowlow[1 * ll_stride + c] + lowlow[2 * ll_stride + c] + 4) >> 3) + highlow[0 * hl_stride + c]) >> 1;
        int32_t odd_l  = (((5  * lowlow[0 * ll_stride + c] + 4 * lowlow[1 * ll_stride + c] - lowlow[2 * ll_stride + c] + 4) >> 3) - highlow[0 * hl_stride + c]) >> 1;
        el = sat16_gpu(even_l);
        ol = sat16_gpu(odd_l);

        int32_t even_h = (((11 * lowhigh[0 * lh_stride + c] - 4 * lowhigh[1 * lh_stride + c] + lowhigh[2 * lh_stride + c] + 4) >> 3) + highhigh[0 * hh_stride + c]) >> 1;
        int32_t odd_h  = (((5  * lowhigh[0 * lh_stride + c] + 4 * lowhigh[1 * lh_stride + c] - lowhigh[2 * lh_stride + c] + 4) >> 3) - highhigh[0 * hh_stride + c]) >> 1;
        eh = sat16_gpu(even_h);
        oh = sat16_gpu(odd_h);
    } else if (r == height - 1) {
        // Bottom border
        int32_t even_l = (((5  * lowlow[r * ll_stride + c] + 4 * lowlow[(r - 1) * ll_stride + c] - lowlow[(r - 2) * ll_stride + c] + 4) >> 3) + highlow[r * hl_stride + c]) >> 1;
        int32_t odd_l  = (((11 * lowlow[r * ll_stride + c] - 4 * lowlow[(r - 1) * ll_stride + c] + lowlow[(r - 2) * ll_stride + c] + 4) >> 3) - highlow[r * hl_stride + c]) >> 1;
        el = sat16_gpu(even_l);
        ol = sat16_gpu(odd_l);

        int32_t even_h = (((5  * lowhigh[r * lh_stride + c] + 4 * lowhigh[(r - 1) * lh_stride + c] - lowhigh[(r - 2) * lh_stride + c] + 4) >> 3) + highhigh[r * hh_stride + c]) >> 1;
        int32_t odd_h  = (((11 * lowhigh[r * lh_stride + c] - 4 * lowhigh[(r - 1) * lh_stride + c] + lowhigh[(r - 2) * lh_stride + c] + 4) >> 3) - highhigh[r * hh_stride + c]) >> 1;
        eh = sat16_gpu(even_h);
        oh = sat16_gpu(odd_h);
    } else {
        // Interior rows
        if (descale == 0) {
            // 16-bit saturating math (Levels 2 & 0)
            int16_t l0 = lowlow[(r - 1) * ll_stride + c];
            int16_t l1 = lowlow[r * ll_stride + c];
            int16_t l2 = lowlow[(r + 1) * ll_stride + c];
            int16_t h_val = highlow[r * hl_stride + c];

            int16_t diff_even = sat16_add_gpu(sat16_sub_gpu(l0, l2), 4) >> 3;
            el = (sat16_add_gpu(sat16_add_gpu(l1, diff_even), h_val)) >> 1;

            int16_t diff_odd = sat16_add_gpu(sat16_add_gpu(sat16_sub_gpu(0, l0), l2), 4) >> 3;
            ol = (sat16_sub_gpu(sat16_add_gpu(l1, diff_odd), h_val)) >> 1;

            int16_t lh0 = lowhigh[(r - 1) * lh_stride + c];
            int16_t lh1 = lowhigh[r * lh_stride + c];
            int16_t lh2 = lowhigh[(r + 1) * lh_stride + c];
            int16_t hh_val = highhigh[r * hh_stride + c];

            int16_t diff_h_even = sat16_add_gpu(sat16_sub_gpu(lh0, lh2), 4) >> 3;
            eh = (sat16_add_gpu(sat16_add_gpu(lh1, diff_h_even), hh_val)) >> 1;

            int16_t diff_h_odd = sat16_add_gpu(sat16_add_gpu(sat16_sub_gpu(0, lh0), lh2), 4) >> 3;
            oh = (sat16_sub_gpu(sat16_add_gpu(lh1, diff_h_odd), hh_val)) >> 1;
        } else {
            // 32-bit unpacked math (Level 1)
            int32_t l0 = lowlow[(r - 1) * ll_stride + c];
            int32_t l1 = lowlow[r * ll_stride + c];
            int32_t l2 = lowlow[(r + 1) * ll_stride + c];
            int32_t h_val = highlow[r * hl_stride + c];

            int32_t even_l = (((l0 - l2 + 4) >> 3) + l1 + h_val) >> 1;
            int32_t odd_l  = (((-l0 + l2 + 4) >> 3) + l1 - h_val) >> 1;
            el = sat16_gpu(even_l);
            ol = sat16_gpu(odd_l);

            int32_t lh0 = lowhigh[(r - 1) * lh_stride + c];
            int32_t lh1 = lowhigh[r * lh_stride + c];
            int32_t lh2 = lowhigh[(r + 1) * lh_stride + c];
            int32_t hh_val = highhigh[r * hh_stride + c];

            int32_t even_h = (((lh0 - lh2 + 4) >> 3) + lh1 + hh_val) >> 1;
            int32_t odd_h  = (((-lh0 + lh2 + 4) >> 3) + lh1 - hh_val) >> 1;
            eh = sat16_gpu(even_h);
            oh = sat16_gpu(odd_h);
        }
    }

    vert_low[(2 * r) * vl_stride + c]     = el;
    vert_low[(2 * r + 1) * vl_stride + c] = ol;
    vert_high[(2 * r) * vh_stride + c]     = eh;
    vert_high[(2 * r + 1) * vh_stride + c] = oh;
}

// Horizontal Inverse Wavelet Kernel
// Reads vert_low (L) and vert_high (H)
// Reconstructs row y of output (width 2*width)
__global__ inline void HorizontalInverseKernel(
    const int16_t* __restrict__ vert_low, int vl_pitch,
    const int16_t* __restrict__ vert_high, int vh_pitch,
    int16_t* __restrict__ out, int out_pitch,
    int width, int height2, int descale, uint32_t* frame_error = nullptr)
{
    if (DecoderFrameFailed(frame_error)) return;
    int c = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (c >= width || y >= height2) return;

    int vl_stride = vl_pitch / sizeof(int16_t);
    int vh_stride = vh_pitch / sizeof(int16_t);
    int out_stride = out_pitch / sizeof(int16_t);

    const int16_t* L = &vert_low[y * vl_stride];
    const int16_t* H = &vert_high[y * vh_stride];
    int16_t* row_out = &out[y * out_stride];

    int16_t out_even, out_odd;

    if (c == 0) {
        // Left border
        int32_t left_even = ((11 * L[0] - 4 * L[1] + L[2] + 4) >> 3) + H[0];
        int32_t left_odd  = ((5  * L[0] + 4 * L[1] - L[2] + 4) >> 3) - H[0];
        if (descale == 0) {
            out_even = sat16_gpu(left_even >> 1);
            out_odd  = sat16_gpu(left_odd >> 1);
        } else {
            out_even = sat16_gpu(left_even * 2);
            out_odd  = sat16_gpu(left_odd * 2);
        }
    } else if (c == width - 1) {
        // Right border
        int32_t right_even = ((5  * L[c] + 4 * L[c - 1] - L[c - 2] + 4) >> 3) + H[c];
        int32_t right_odd  = ((11 * L[c] - 4 * L[c - 1] + L[c - 2] + 4) >> 3) - H[c];
        if (descale == 0) {
            out_even = sat16_gpu(right_even >> 1);
            out_odd  = sat16_gpu(right_odd >> 1);
        } else {
            out_even = sat16_gpu(right_even * 2);
            out_odd  = sat16_gpu(right_odd * 2);
        }
    } else {
        // Interior columns
        if (descale == 0) {
            int16_t diff_e = sat16_add_gpu(sat16_sub_gpu(L[c - 1], L[c + 1]), 4) >> 3;
            int16_t ev = sat16_add_gpu(sat16_add_gpu(L[c], diff_e), H[c]) >> 1;

            int16_t diff_o = sat16_add_gpu(sat16_add_gpu(sat16_sub_gpu(0, L[c - 1]), L[c + 1]), 4) >> 3;
            int16_t od = sat16_sub_gpu(sat16_add_gpu(L[c], diff_o), H[c]) >> 1;

            out_even = sat16_gpu(ev);
            out_odd  = sat16_gpu(od);
        } else {
            int16_t diff_e = sat16_add_gpu(sat16_sub_gpu(L[c - 1], L[c + 1]), 4) >> 3;
            int16_t ev = sat16_add_gpu(sat16_add_gpu(L[c], diff_e), H[c]);

            int16_t diff_o = sat16_add_gpu(sat16_add_gpu(sat16_sub_gpu(0, L[c - 1]), L[c + 1]), 4) >> 3;
            int16_t od = sat16_sub_gpu(sat16_add_gpu(L[c], diff_o), H[c]);

            out_even = sat16_gpu(int32_t(ev) * 2);
            out_odd  = sat16_gpu(int32_t(od) * 2);
        }
    }

    row_out[2 * c]     = out_even;
    row_out[2 * c + 1] = out_odd;
}

// GOP-1 field combine. Upstream InvertInterlacedRow16sToRow16u SSE2, without the
// final << 6. The 16-bit color converter applies that scale itself.
//   biased = sat_add(sat_sub/add(low, high), 30720)
//   limited = unsigned_sat_sub(biased, 30720)   // clamps the sum/diff to [0, 2047]
//   code = arithmetic_shift(limited, 1)
// even is frame row 2*r. odd is row 2*r+1.
__device__ __forceinline__ int16_t FieldCodeSse2(int16_t sum_or_diff) {
    constexpr int kBias = 32767 - 2047;
    int32_t biased = int32_t(sum_or_diff) + kBias;
    if (biased > 32767) biased = 32767;
    if (biased < -32768) biased = -32768;
    const uint16_t bits = static_cast<uint16_t>(static_cast<int16_t>(biased));
    const uint16_t limited = bits < static_cast<uint16_t>(kBias)
        ? 0 : static_cast<uint16_t>(bits - static_cast<uint16_t>(kBias));
    const int16_t signed_bits = static_cast<int16_t>(limited);
    return static_cast<int16_t>(int32_t(signed_bits) >> 1);
}

__global__ inline void FieldCombineKernel(
    const int16_t* __restrict__ low, int low_pitch,
    const int16_t* __restrict__ high, int high_pitch,
    int16_t* __restrict__ output, int output_pitch,
    int width, int field_rows, uint32_t* frame_error = nullptr)
{
    if (DecoderFrameFailed(frame_error)) return;
    int c = blockIdx.x * blockDim.x + threadIdx.x;
    int r = blockIdx.y * blockDim.y + threadIdx.y;
    if (c >= width || r >= field_rows) return;

    const int low_stride = low_pitch / int(sizeof(int16_t));
    const int high_stride = high_pitch / int(sizeof(int16_t));
    const int out_stride = output_pitch / int(sizeof(int16_t));
    const int16_t lo = low[r * low_stride + c];
    const int16_t hi = high[r * high_stride + c];
    output[(2 * r) * out_stride + c] = FieldCodeSse2(sat16_sub_gpu(lo, hi));
    output[(2 * r + 1) * out_stride + c] = FieldCodeSse2(sat16_add_gpu(lo, hi));
}

} // namespace HyperCine
