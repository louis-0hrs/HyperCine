// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "wavelet_cuda_common.cuh"

// Quantize a single 16-bit coefficient: sign-preserving (abs + midpoint) * (2^16/divisor) >> 16
__device__ __forceinline__ int16_t Quantize16sTo16s(int32_t val, int divisor, int midpoint) {
    if (divisor <= 1) return CUDASaturate16S(val);
    uint32_t multiplier = (1U << 16) / static_cast<uint32_t>(divisor);
    int32_t sign = (val < 0) ? -1 : 1;
    int32_t abs_val = (val < 0) ? -val : val;
    uint32_t q = ((static_cast<uint32_t>(abs_val) + static_cast<uint32_t>(midpoint)) * multiplier) >> 16;
    return CUDASaturate16S(sign * static_cast<int32_t>(q));
}

// Fused vertical filter + quantization kernel.
// Inputs:  lowpass  (horizontal lowpass,  width × height)
//          highpass (horizontal highpass, width × height)
// Outputs: d_ll (vert-low  of horiz-low),  d_hl (vert-high of horiz-low)
//          d_lh (vert-low  of horiz-high), d_hh (vert-high of horiz-high)
// Each output is width × (height/2).
__global__ void ForwardVerticalQuant26_RefKernel(
    const int16_t* __restrict__ lowpass,
    const int16_t* __restrict__ highpass,
    int width, int height,
    int low_stride, int high_stride,
    int16_t* __restrict__ d_ll, int ll_stride,
    int16_t* __restrict__ d_lh, int lh_stride,
    int16_t* __restrict__ d_hl, int hl_stride,
    int16_t* __restrict__ d_hh, int hh_stride,
    int quant_ll, int quant_lh, int quant_hl, int quant_hh,
    int midpoint
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int r = blockIdx.y * blockDim.y + threadIdx.y;
    const int half_h = height / 2;

    if (x >= width || r >= half_h) return;

    const int y = r * 2;

    // --- Process horizontal LOWPASS → LL (vert lowpass) and HL (vert highpass) ---
    // Vertical lowpass: sum of adjacent pair
    int32_t even_low = static_cast<int32_t>(lowpass[y * low_stride + x])
                     + static_cast<int32_t>(lowpass[(y + 1) * low_stride + x]);

    // Vertical highpass: 2-6 biorthogonal filter
    int32_t odd_low;
    if (r == 0) {
        // Top border: [5, -11, 4, 4, -1, -1] / 8
        odd_low = (5 * static_cast<int32_t>(lowpass[0 * low_stride + x])
                 - 11 * static_cast<int32_t>(lowpass[1 * low_stride + x])
                 + 4 * static_cast<int32_t>(lowpass[2 * low_stride + x])
                 + 4 * static_cast<int32_t>(lowpass[3 * low_stride + x])
                 - 1 * static_cast<int32_t>(lowpass[4 * low_stride + x])
                 - 1 * static_cast<int32_t>(lowpass[5 * low_stride + x]) + 4) >> 3;
    } else if (r < half_h - 1) {
        // Interior: [-1, -1, 8, -8, 1, 1] / 8
        int32_t hp = - static_cast<int32_t>(lowpass[(y - 2) * low_stride + x])
                     - static_cast<int32_t>(lowpass[(y - 1) * low_stride + x])
                     + static_cast<int32_t>(lowpass[(y + 2) * low_stride + x])
                     + static_cast<int32_t>(lowpass[(y + 3) * low_stride + x]);
        hp = (hp + 4) >> 3;
        odd_low = hp + static_cast<int32_t>(lowpass[y * low_stride + x])
                     - static_cast<int32_t>(lowpass[(y + 1) * low_stride + x]);
    } else {
        // Bottom border: [1, 1, -4, -4, 11, -5] / 8
        odd_low = (11 * static_cast<int32_t>(lowpass[y * low_stride + x])
                 - 5 * static_cast<int32_t>(lowpass[(y + 1) * low_stride + x])
                 - 4 * static_cast<int32_t>(lowpass[(y - 1) * low_stride + x])
                 - 4 * static_cast<int32_t>(lowpass[(y - 2) * low_stride + x])
                 + 1 * static_cast<int32_t>(lowpass[(y - 3) * low_stride + x])
                 + 1 * static_cast<int32_t>(lowpass[(y - 4) * low_stride + x]) + 4) >> 3;
    }

    // --- Process horizontal HIGHPASS → LH (vert lowpass) and HH (vert highpass) ---
    int32_t even_high = static_cast<int32_t>(highpass[y * high_stride + x])
                      + static_cast<int32_t>(highpass[(y + 1) * high_stride + x]);

    int32_t odd_high;
    if (r == 0) {
        odd_high = (5 * static_cast<int32_t>(highpass[0 * high_stride + x])
                  - 11 * static_cast<int32_t>(highpass[1 * high_stride + x])
                  + 4 * static_cast<int32_t>(highpass[2 * high_stride + x])
                  + 4 * static_cast<int32_t>(highpass[3 * high_stride + x])
                  - 1 * static_cast<int32_t>(highpass[4 * high_stride + x])
                  - 1 * static_cast<int32_t>(highpass[5 * high_stride + x]) + 4) >> 3;
    } else if (r < half_h - 1) {
        int32_t hp = - static_cast<int32_t>(highpass[(y - 2) * high_stride + x])
                     - static_cast<int32_t>(highpass[(y - 1) * high_stride + x])
                     + static_cast<int32_t>(highpass[(y + 2) * high_stride + x])
                     + static_cast<int32_t>(highpass[(y + 3) * high_stride + x]);
        hp = (hp + 4) >> 3;
        odd_high = hp + static_cast<int32_t>(highpass[y * high_stride + x])
                      - static_cast<int32_t>(highpass[(y + 1) * high_stride + x]);
    } else {
        odd_high = (11 * static_cast<int32_t>(highpass[y * high_stride + x])
                  - 5 * static_cast<int32_t>(highpass[(y + 1) * high_stride + x])
                  - 4 * static_cast<int32_t>(highpass[(y - 1) * high_stride + x])
                  - 4 * static_cast<int32_t>(highpass[(y - 2) * high_stride + x])
                  + 1 * static_cast<int32_t>(highpass[(y - 3) * high_stride + x])
                  + 1 * static_cast<int32_t>(highpass[(y - 4) * high_stride + x]) + 4) >> 3;
    }

    // Quantize and write
    d_ll[r * ll_stride + x] = Quantize16sTo16s(even_low,  quant_ll, midpoint);
    d_hl[r * hl_stride + x] = Quantize16sTo16s(odd_low,   quant_hl, midpoint);
    d_lh[r * lh_stride + x] = Quantize16sTo16s(even_high, quant_lh, midpoint);
    d_hh[r * hh_stride + x] = Quantize16sTo16s(odd_high,  quant_hh, midpoint);
}

__global__ void ForwardHorizontal26_RefKernel(
    const int16_t* __restrict__ src, int width, int height, int src_stride,
    int16_t* __restrict__ lowpass, int low_stride,
    int16_t* __restrict__ highpass, int high_stride
) {
    const int c = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int half_w = width / 2;

    if (c >= half_w || y >= height) return;

    const int16_t* in_row = src + y * src_stride;
    const int x = c * 2;

    // Lowpass: sum of adjacent pair
    int32_t low_sum = static_cast<int32_t>(in_row[x]) + static_cast<int32_t>(in_row[x + 1]);
    lowpass[y * low_stride + c] = CUDASaturate16S(low_sum);

    // Highpass: 2-6 biorthogonal wavelet filter
    if (c == 0) {
        // Left border filter: [5, -11, 4, 4, -1, -1] / 8
        int32_t sum = 5 * static_cast<int32_t>(in_row[0])
                    - 11 * static_cast<int32_t>(in_row[1])
                    + 4 * static_cast<int32_t>(in_row[2])
                    + 4 * static_cast<int32_t>(in_row[3])
                    - 1 * static_cast<int32_t>(in_row[4])
                    - 1 * static_cast<int32_t>(in_row[5]);
        sum = (sum + 4) >> 3;
        highpass[y * high_stride + 0] = CUDASaturate16S(sum);
    } else if (c < half_w - 1) {
        // Interior filter: [-1, -1, 8, -8, 1, 1] / 8
        int32_t hp_sum = - static_cast<int32_t>(in_row[x - 2])
                         - static_cast<int32_t>(in_row[x - 1])
                         + static_cast<int32_t>(in_row[x + 2])
                         + static_cast<int32_t>(in_row[x + 3]);
        hp_sum = (hp_sum + 4) >> 3;
        int32_t sum = hp_sum + static_cast<int32_t>(in_row[x]) - static_cast<int32_t>(in_row[x + 1]);
        highpass[y * high_stride + c] = CUDASaturate16S(sum);
    } else {
        // Right border filter: [1, 1, -4, -4, 11, -5] / 8
        int32_t sum = 11 * static_cast<int32_t>(in_row[x])
                    - 5 * static_cast<int32_t>(in_row[x + 1])
                    - 4 * static_cast<int32_t>(in_row[x - 1])
                    - 4 * static_cast<int32_t>(in_row[x - 2])
                    + 1 * static_cast<int32_t>(in_row[x - 3])
                    + 1 * static_cast<int32_t>(in_row[x - 4]);
        sum = (sum + 4) >> 3;
        highpass[y * high_stride + c] = CUDASaturate16S(sum);
    }
}

__global__ void ForwardVertical26_RefKernel(
    const int16_t* __restrict__ src, int width, int height, int src_stride,
    int16_t* __restrict__ lowpass, int low_stride,
    int16_t* __restrict__ highpass, int high_stride
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int r = blockIdx.y * blockDim.y + threadIdx.y;
    const int half_h = height / 2;

    if (x >= width || r >= half_h) return;

    const int y = r * 2;

    // Lowpass: sum of adjacent pair in column
    int32_t low_sum = static_cast<int32_t>(src[y * src_stride + x])
                    + static_cast<int32_t>(src[(y + 1) * src_stride + x]);
    lowpass[r * low_stride + x] = CUDASaturate16S(low_sum);

    // Highpass
    if (r == 0) {
        // Top border filter
        int32_t sum = 5 * static_cast<int32_t>(src[0 * src_stride + x])
                    - 11 * static_cast<int32_t>(src[1 * src_stride + x])
                    + 4 * static_cast<int32_t>(src[2 * src_stride + x])
                    + 4 * static_cast<int32_t>(src[3 * src_stride + x])
                    - 1 * static_cast<int32_t>(src[4 * src_stride + x])
                    - 1 * static_cast<int32_t>(src[5 * src_stride + x]);
        sum = (sum + 4) >> 3;
        highpass[r * high_stride + x] = CUDASaturate16S(sum);
    } else if (r < half_h - 1) {
        // Interior filter
        int32_t hp_sum = - static_cast<int32_t>(src[(y - 2) * src_stride + x])
                         - static_cast<int32_t>(src[(y - 1) * src_stride + x])
                         + static_cast<int32_t>(src[(y + 2) * src_stride + x])
                         + static_cast<int32_t>(src[(y + 3) * src_stride + x]);
        hp_sum = (hp_sum + 4) >> 3;
        int32_t sum = hp_sum + static_cast<int32_t>(src[y * src_stride + x])
                             - static_cast<int32_t>(src[(y + 1) * src_stride + x]);
        highpass[r * high_stride + x] = CUDASaturate16S(sum);
    } else {
        // Bottom border filter
        int32_t sum = 11 * static_cast<int32_t>(src[y * src_stride + x])
                    - 5 * static_cast<int32_t>(src[(y + 1) * src_stride + x])
                    - 4 * static_cast<int32_t>(src[(y - 1) * src_stride + x])
                    - 4 * static_cast<int32_t>(src[(y - 2) * src_stride + x])
                    + 1 * static_cast<int32_t>(src[(y - 3) * src_stride + x])
                    + 1 * static_cast<int32_t>(src[(y - 4) * src_stride + x]);
        sum = (sum + 4) >> 3;
        highpass[r * high_stride + x] = CUDASaturate16S(sum);
    }
}

__global__ void InverseVertical26_RefKernel(
    const int16_t* __restrict__ lowpass, int low_stride,
    const int16_t* __restrict__ highpass, int high_stride,
    int width, int height,
    int16_t* __restrict__ dst, int dst_stride
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int r = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || r >= height) return;

    int32_t even = 0;
    int32_t odd = 0;

    if (r == 0) {
        even = (11 * static_cast<int32_t>(lowpass[0 * low_stride + x])
               - 4 * static_cast<int32_t>(lowpass[1 * low_stride + x])
               + 1 * static_cast<int32_t>(lowpass[2 * low_stride + x])
               + 4) >> 3;
        even += static_cast<int32_t>(highpass[0 * high_stride + x]);

        odd = (5 * static_cast<int32_t>(lowpass[0 * low_stride + x])
              + 4 * static_cast<int32_t>(lowpass[1 * low_stride + x])
              - 1 * static_cast<int32_t>(lowpass[2 * low_stride + x])
              + 4) >> 3;
        odd -= static_cast<int32_t>(highpass[0 * high_stride + x]);
    } else if (r < height - 1) {
        even = (static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
               - static_cast<int32_t>(lowpass[(r + 1) * low_stride + x])
               + 4) >> 3;
        even += static_cast<int32_t>(lowpass[r * low_stride + x])
              + static_cast<int32_t>(highpass[r * high_stride + x]);

        odd = (- static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
               + static_cast<int32_t>(lowpass[(r + 1) * low_stride + x])
               + 4) >> 3;
        odd += static_cast<int32_t>(lowpass[r * low_stride + x])
             - static_cast<int32_t>(highpass[r * high_stride + x]);
    } else {
        even = (5 * static_cast<int32_t>(lowpass[r * low_stride + x])
               + 4 * static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
               - 1 * static_cast<int32_t>(lowpass[(r - 2) * low_stride + x])
               + 4) >> 3;
        even += static_cast<int32_t>(highpass[r * high_stride + x]);

        odd = (11 * static_cast<int32_t>(lowpass[r * low_stride + x])
              - 4 * static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
              + 1 * static_cast<int32_t>(lowpass[(r - 2) * low_stride + x])
              + 4) >> 3;
        odd -= static_cast<int32_t>(highpass[r * high_stride + x]);
    }

    dst[(2 * r) * dst_stride + x] = CUDASaturate16S(even >> 1);
    dst[(2 * r + 1) * dst_stride + x] = CUDASaturate16S(odd >> 1);
}

__global__ void InverseHorizontal26_RefKernel(
    const int16_t* __restrict__ lowpass, int low_stride,
    const int16_t* __restrict__ highpass, int high_stride,
    int width, int height,
    int16_t* __restrict__ dst, int dst_stride
) {
    const int c = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (c >= width || y >= height) return;

    const int16_t* low_row = lowpass + y * low_stride;
    const int16_t* high_row = highpass + y * high_stride;
    int16_t* out_row = dst + y * dst_stride;

    int32_t even = 0;
    int32_t odd = 0;

    if (c == 0) {
        even = (11 * static_cast<int32_t>(low_row[0])
               - 4 * static_cast<int32_t>(low_row[1])
               + 1 * static_cast<int32_t>(low_row[2])
               + 4) >> 3;
        even += static_cast<int32_t>(high_row[0]);

        odd = (5 * static_cast<int32_t>(low_row[0])
              + 4 * static_cast<int32_t>(low_row[1])
              - 1 * static_cast<int32_t>(low_row[2])
              + 4) >> 3;
        odd -= static_cast<int32_t>(high_row[0]);
    } else if (c < width - 1) {
        even = (static_cast<int32_t>(low_row[c - 1])
               - static_cast<int32_t>(low_row[c + 1])
               + 4) >> 3;
        even += static_cast<int32_t>(low_row[c])
              + static_cast<int32_t>(high_row[c]);

        odd = (- static_cast<int32_t>(low_row[c - 1])
               + static_cast<int32_t>(low_row[c + 1])
               + 4) >> 3;
        odd += static_cast<int32_t>(low_row[c])
             - static_cast<int32_t>(high_row[c]);
    } else {
        even = (5 * static_cast<int32_t>(low_row[c])
               + 4 * static_cast<int32_t>(low_row[c - 1])
               - 1 * static_cast<int32_t>(low_row[c - 2])
               + 4) >> 3;
        even += static_cast<int32_t>(high_row[c]);

        odd = (11 * static_cast<int32_t>(low_row[c])
              - 4 * static_cast<int32_t>(low_row[c - 1])
              + 1 * static_cast<int32_t>(low_row[c - 2])
              + 4) >> 3;
        odd -= static_cast<int32_t>(high_row[c]);
    }

    out_row[2 * c] = CUDASaturate16S(even >> 1);
    out_row[2 * c + 1] = CUDASaturate16S(odd >> 1);
}

HyperCineStatus ForwardHorizontal26_Reference(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
) {
    const int half_w = width / 2;
    dim3 block(16, 16);
    dim3 grid((half_w + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    ForwardHorizontal26_RefKernel<<<grid, block, 0, stream>>>(
        d_src, width, height, src_stride,
        d_low, low_stride,
        d_high, high_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus ForwardVertical26_Reference(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
) {
    const int half_h = height / 2;
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (half_h + block.y - 1) / block.y);

    ForwardVertical26_RefKernel<<<grid, block, 0, stream>>>(
        d_src, width, height, src_stride,
        d_low, low_stride,
        d_high, high_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus ForwardVerticalQuant26_Reference(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_ll, int ll_stride,
    int16_t* d_lh, int lh_stride,
    int16_t* d_hl, int hl_stride,
    int16_t* d_hh, int hh_stride,
    int quant_ll, int quant_lh, int quant_hl, int quant_hh,
    int midpoint,
    cudaStream_t stream
) {
    const int half_h = height / 2;
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (half_h + block.y - 1) / block.y);

    ForwardVerticalQuant26_RefKernel<<<grid, block, 0, stream>>>(
        d_low, d_high,
        width, height,
        low_stride, high_stride,
        d_ll, ll_stride,
        d_lh, lh_stride,
        d_hl, hl_stride,
        d_hh, hh_stride,
        quant_ll, quant_lh, quant_hl, quant_hh,
        midpoint
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus InverseVertical26_Reference(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
) {
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    InverseVertical26_RefKernel<<<grid, block, 0, stream>>>(
        d_low, low_stride,
        d_high, high_stride,
        width, height,
        d_dst, dst_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus InverseHorizontal26_Reference(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
) {
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    InverseHorizontal26_RefKernel<<<grid, block, 0, stream>>>(
        d_low, low_stride,
        d_high, high_stride,
        width, height,
        d_dst, dst_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}
