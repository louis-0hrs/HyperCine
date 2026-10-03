// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_inverse_cuda.h"
#include <cuda_runtime.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>

namespace HyperCine {

namespace {

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
__global__ void VerticalInverseKernel(
    const int16_t* __restrict__ lowlow, int ll_pitch,
    const int16_t* __restrict__ lowhigh, int lh_pitch,
    const int16_t* __restrict__ highlow, int hl_pitch,
    const int16_t* __restrict__ highhigh, int hh_pitch,
    int16_t* __restrict__ vert_low, int vl_pitch,
    int16_t* __restrict__ vert_high, int vh_pitch,
    int width, int height, int descale)
{
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
__global__ void HorizontalInverseKernel(
    const int16_t* __restrict__ vert_low, int vl_pitch,
    const int16_t* __restrict__ vert_high, int vh_pitch,
    int16_t* __restrict__ out, int out_pitch,
    int width, int height2, int descale)
{
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

} // anonymous namespace

struct ChannelBuffers {
    // Input uploaded bands
    // Level 2: bands 0..3 (LL, LH, HL, HH)
    int16_t* d_l2_band[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t l2_pitch = 0;

    // Level 1: bands 1..3 (LH, HL, HH); band 0 is d_ll1
    int16_t* d_l1_band[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t l1_pitch = 0;

    // Level 0: bands 1..3 (LH, HL, HH); band 0 is d_ll0
    int16_t* d_l0_band[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t l0_pitch = 0;

    // Intermediate LL buffers
    int16_t* d_ll1 = nullptr; // Level 1 LL (output of Level 2)
    size_t ll1_pitch = 0;

    int16_t* d_ll0 = nullptr; // Level 0 LL (output of Level 1)
    size_t ll0_pitch = 0;

    // Vertical intermediate strip buffers (sized for Level 0, reused across levels)
    int16_t* d_vert_low = nullptr;
    size_t vert_low_pitch = 0;

    int16_t* d_vert_high = nullptr;
    size_t vert_high_pitch = 0;

    // Final reconstructed planar channel output
    int16_t* d_final_plane = nullptr;
    size_t final_plane_pitch = 0;

    int ch_width = 0;
    int ch_height = 0;
};

struct CudaInverseWaveletPipeline::Impl {
    bool initialized = false;
    int frame_width = 0;
    int frame_height = 0;
    int precision = 10;

    ChannelBuffers channels[3]; // 0: Y, 1: V, 2: U
    cudaStream_t streams[3] = {nullptr, nullptr, nullptr};

    cudaEvent_t ev_h2d_start = nullptr;
    cudaEvent_t ev_h2d_stop = nullptr;
    cudaEvent_t ev_inv_start = nullptr;
    cudaEvent_t ev_inv_stop = nullptr;
    cudaEvent_t ev_d2h_start = nullptr;
    cudaEvent_t ev_d2h_stop = nullptr;

    float time_h2d_ms = 0.0f;
    float time_inv_ms = 0.0f;
    float time_d2h_ms = 0.0f;

    void FreeBuffers() {
        for (int ch = 0; ch < 3; ++ch) {
            auto& c = channels[ch];
            for (int b = 0; b < 4; ++b) {
                if (c.d_l2_band[b]) { cudaFree(c.d_l2_band[b]); c.d_l2_band[b] = nullptr; }
            }
            for (int b = 1; b < 4; ++b) {
                if (c.d_l1_band[b]) { cudaFree(c.d_l1_band[b]); c.d_l1_band[b] = nullptr; }
                if (c.d_l0_band[b]) { cudaFree(c.d_l0_band[b]); c.d_l0_band[b] = nullptr; }
            }
            if (c.d_ll1) { cudaFree(c.d_ll1); c.d_ll1 = nullptr; }
            if (c.d_ll0) { cudaFree(c.d_ll0); c.d_ll0 = nullptr; }
            if (c.d_vert_low) { cudaFree(c.d_vert_low); c.d_vert_low = nullptr; }
            if (c.d_vert_high) { cudaFree(c.d_vert_high); c.d_vert_high = nullptr; }
            if (c.d_final_plane) { cudaFree(c.d_final_plane); c.d_final_plane = nullptr; }
        }
        for (int ch = 0; ch < 3; ++ch) {
            if (streams[ch]) { cudaStreamDestroy(streams[ch]); streams[ch] = nullptr; }
        }
        if (ev_h2d_start) { cudaEventDestroy(ev_h2d_start); ev_h2d_start = nullptr; }
        if (ev_h2d_stop)  { cudaEventDestroy(ev_h2d_stop);  ev_h2d_stop = nullptr; }
        if (ev_inv_start) { cudaEventDestroy(ev_inv_start); ev_inv_start = nullptr; }
        if (ev_inv_stop)  { cudaEventDestroy(ev_inv_stop);  ev_inv_stop = nullptr; }
        if (ev_d2h_start) { cudaEventDestroy(ev_d2h_start); ev_d2h_start = nullptr; }
        if (ev_d2h_stop)  { cudaEventDestroy(ev_d2h_stop);  ev_d2h_stop = nullptr; }
        initialized = false;
    }

    ~Impl() {
        FreeBuffers();
    }
};

CudaInverseWaveletPipeline::CudaInverseWaveletPipeline() : impl_(std::make_unique<Impl>()) {}
CudaInverseWaveletPipeline::~CudaInverseWaveletPipeline() = default;
CudaInverseWaveletPipeline::CudaInverseWaveletPipeline(CudaInverseWaveletPipeline&&) noexcept = default;
CudaInverseWaveletPipeline& CudaInverseWaveletPipeline::operator=(CudaInverseWaveletPipeline&&) noexcept = default;

bool CudaInverseWaveletPipeline::IsInitialized() const {
    return impl_ && impl_->initialized;
}

bool CudaInverseWaveletPipeline::Initialize(int frame_width, int frame_height, int precision) {
    if (!impl_) return false;
    if (impl_->initialized) {
        impl_->FreeBuffers();
    }

    impl_->frame_width = frame_width;
    impl_->frame_height = frame_height;
    impl_->precision = precision;

    cudaError_t err;
    for (int ch = 0; ch < 3; ++ch) {
        err = cudaStreamCreateWithFlags(&impl_->streams[ch], cudaStreamNonBlocking);
        if (err != cudaSuccess) return false;
    }

    err = cudaEventCreate(&impl_->ev_h2d_start); if (err != cudaSuccess) return false;
    err = cudaEventCreate(&impl_->ev_h2d_stop);  if (err != cudaSuccess) return false;
    err = cudaEventCreate(&impl_->ev_inv_start); if (err != cudaSuccess) return false;
    err = cudaEventCreate(&impl_->ev_inv_stop);  if (err != cudaSuccess) return false;
    err = cudaEventCreate(&impl_->ev_d2h_start); if (err != cudaSuccess) return false;
    err = cudaEventCreate(&impl_->ev_d2h_stop);  if (err != cudaSuccess) return false;

    for (int ch = 0; ch < 3; ++ch) {
        auto& c = impl_->channels[ch];
        c.ch_width = (ch == 0) ? frame_width : (frame_width / 2);
        c.ch_height = frame_height;

        int w_l2 = c.ch_width / 8;
        int h_l2 = c.ch_height / 8;
        int w_l1 = c.ch_width / 4;
        int h_l1 = c.ch_height / 4;
        int w_l0 = c.ch_width / 2;
        int h_l0 = c.ch_height / 2;

        // Level 2 bands (LL, LH, HL, HH)
        for (int b = 0; b < 4; ++b) {
            err = cudaMallocPitch(&c.d_l2_band[b], &c.l2_pitch, w_l2 * sizeof(int16_t), h_l2);
            if (err != cudaSuccess) return false;
        }

        // Level 1 bands (LH, HL, HH); band 0 is d_ll1
        for (int b = 1; b < 4; ++b) {
            err = cudaMallocPitch(&c.d_l1_band[b], &c.l1_pitch, w_l1 * sizeof(int16_t), h_l1);
            if (err != cudaSuccess) return false;
        }

        // Level 0 bands (LH, HL, HH); band 0 is d_ll0
        for (int b = 1; b < 4; ++b) {
            err = cudaMallocPitch(&c.d_l0_band[b], &c.l0_pitch, w_l0 * sizeof(int16_t), h_l0);
            if (err != cudaSuccess) return false;
        }

        // Intermediates
        err = cudaMallocPitch(&c.d_ll1, &c.ll1_pitch, w_l1 * sizeof(int16_t), h_l1);
        if (err != cudaSuccess) return false;
        c.d_l1_band[0] = c.d_ll1;

        err = cudaMallocPitch(&c.d_ll0, &c.ll0_pitch, w_l0 * sizeof(int16_t), h_l0);
        if (err != cudaSuccess) return false;
        c.d_l0_band[0] = c.d_ll0;

        // Vertical intermediate strip buffer: sized for maximum level (Level 0: w_l0 x (2*h_l0) = (c.ch_width/2) x c.ch_height)
        err = cudaMallocPitch(&c.d_vert_low, &c.vert_low_pitch, w_l0 * sizeof(int16_t), c.ch_height);
        if (err != cudaSuccess) return false;

        err = cudaMallocPitch(&c.d_vert_high, &c.vert_high_pitch, w_l0 * sizeof(int16_t), c.ch_height);
        if (err != cudaSuccess) return false;

        // Final planar channel
        err = cudaMallocPitch(&c.d_final_plane, &c.final_plane_pitch, c.ch_width * sizeof(int16_t), c.ch_height);
        if (err != cudaSuccess) return false;
    }

    impl_->initialized = true;
    return true;
}

void CudaInverseWaveletPipeline::Release() {
    if (impl_) {
        impl_->FreeBuffers();
    }
}

bool CudaInverseWaveletPipeline::UploadCoefficients(const std::vector<DecodeBandDescriptor>& bands) {
    if (!impl_ || !impl_->initialized) return false;

    cudaEventRecord(impl_->ev_h2d_start, impl_->streams[0]);

    for (const auto& b : bands) {
        if (b.channel < 0 || b.channel >= 3) return false;
        auto& c = impl_->channels[b.channel];
        cudaStream_t s = impl_->streams[b.channel];

        int16_t* dst = nullptr;
        size_t dst_pitch = 0;

        if (b.level == 2) {
            if (b.band < 0 || b.band >= 4) return false;
            dst = c.d_l2_band[b.band];
            dst_pitch = c.l2_pitch;
        } else if (b.level == 1) {
            if (b.band < 1 || b.band >= 4) return false;
            dst = c.d_l1_band[b.band];
            dst_pitch = c.l1_pitch;
        } else if (b.level == 0) {
            if (b.band < 1 || b.band >= 4) return false;
            dst = c.d_l0_band[b.band];
            dst_pitch = c.l0_pitch;
        } else {
            return false;
        }

        cudaError_t err = cudaMemcpy2DAsync(
            dst, dst_pitch,
            b.host_data, b.pitch,
            b.width * sizeof(int16_t), b.height,
            cudaMemcpyHostToDevice, s
        );
        if (err != cudaSuccess) return false;
    }

    for (int ch = 0; ch < 3; ++ch) {
        cudaStreamSynchronize(impl_->streams[ch]);
    }
    cudaEventRecord(impl_->ev_h2d_stop, impl_->streams[0]);
    cudaEventSynchronize(impl_->ev_h2d_stop);

    cudaEventElapsedTime(&impl_->time_h2d_ms, impl_->ev_h2d_start, impl_->ev_h2d_stop);
    return true;
}

bool CudaInverseWaveletPipeline::Reconstruct() {
    if (!impl_ || !impl_->initialized) return false;

    cudaEventRecord(impl_->ev_inv_start, impl_->streams[0]);

    for (int ch = 0; ch < 3; ++ch) {
        auto& c = impl_->channels[ch];
        cudaStream_t s = impl_->streams[ch];

        int w_l2 = c.ch_width / 8;
        int h_l2 = c.ch_height / 8;
        int w_l1 = c.ch_width / 4;
        int h_l1 = c.ch_height / 4;
        int w_l0 = c.ch_width / 2;
        int h_l0 = c.ch_height / 2;

        dim3 block(16, 16);

        // --- LEVEL 2: w_l2 x h_l2, descale = 0 -> LL1 (w_l1 x h_l1) ---
        {
            dim3 grid_v((w_l2 + block.x - 1) / block.x, (h_l2 + block.y - 1) / block.y);
            VerticalInverseKernel<<<grid_v, block, 0, s>>>(
                c.d_l2_band[0], int(c.l2_pitch),
                c.d_l2_band[1], int(c.l2_pitch),
                c.d_l2_band[2], int(c.l2_pitch),
                c.d_l2_band[3], int(c.l2_pitch),
                c.d_vert_low, int(c.vert_low_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                w_l2, h_l2, 0
            );

            dim3 block_h(64, 4);
            dim3 grid_h((w_l2 + block_h.x - 1) / block_h.x, (2 * h_l2 + block_h.y - 1) / block_h.y);
            HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
                c.d_vert_low, int(c.vert_low_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                c.d_ll1, int(c.ll1_pitch),
                w_l2, 2 * h_l2, 0
            );
        }

        // --- LEVEL 1: w_l1 x h_l1, descale = 2 -> LL0 (w_l0 x h_l0) ---
        {
            dim3 grid_v((w_l1 + block.x - 1) / block.x, (h_l1 + block.y - 1) / block.y);
            VerticalInverseKernel<<<grid_v, block, 0, s>>>(
                c.d_ll1, int(c.ll1_pitch),
                c.d_l1_band[1], int(c.l1_pitch),
                c.d_l1_band[2], int(c.l1_pitch),
                c.d_l1_band[3], int(c.l1_pitch),
                c.d_vert_low, int(c.vert_low_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                w_l1, h_l1, 2
            );

            dim3 block_h(64, 4);
            dim3 grid_h((w_l1 + block_h.x - 1) / block_h.x, (2 * h_l1 + block_h.y - 1) / block_h.y);
            HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
                c.d_vert_low, int(c.vert_low_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                c.d_ll0, int(c.ll0_pitch),
                w_l1, 2 * h_l1, 2
            );
        }

        // --- LEVEL 0: w_l0 x h_l0, descale = 0 -> Final Planar Channel (ch_width x ch_height) ---
        {
            dim3 grid_v((w_l0 + block.x - 1) / block.x, (h_l0 + block.y - 1) / block.y);
            VerticalInverseKernel<<<grid_v, block, 0, s>>>(
                c.d_ll0, int(c.ll0_pitch),
                c.d_l0_band[1], int(c.l0_pitch),
                c.d_l0_band[2], int(c.l0_pitch),
                c.d_l0_band[3], int(c.l0_pitch),
                c.d_vert_low, int(c.vert_low_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                w_l0, h_l0, 0
            );

            dim3 block_h(64, 4);
            dim3 grid_h((w_l0 + block_h.x - 1) / block_h.x, (2 * h_l0 + block_h.y - 1) / block_h.y);
            HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
                c.d_vert_low, int(c.vert_low_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                c.d_final_plane, int(c.final_plane_pitch),
                w_l0, 2 * h_l0, 0
            );
        }
    }

    for (int ch = 0; ch < 3; ++ch) {
        cudaStreamSynchronize(impl_->streams[ch]);
    }
    cudaEventRecord(impl_->ev_inv_stop, impl_->streams[0]);
    cudaEventSynchronize(impl_->ev_inv_stop);

    cudaEventElapsedTime(&impl_->time_inv_ms, impl_->ev_inv_start, impl_->ev_inv_stop);
    return true;
}

bool CudaInverseWaveletPipeline::DownloadPlanes(
    int16_t* host_y, int y_pitch,
    int16_t* host_v, int v_pitch,
    int16_t* host_u, int u_pitch)
{
    if (!impl_ || !impl_->initialized) return false;

    cudaEventRecord(impl_->ev_d2h_start, impl_->streams[0]);

    int16_t* host_ptrs[3] = {host_y, host_v, host_u};
    int pitches[3] = {y_pitch, v_pitch, u_pitch};

    for (int ch = 0; ch < 3; ++ch) {
        auto& c = impl_->channels[ch];
        cudaError_t err = cudaMemcpy2DAsync(
            host_ptrs[ch], pitches[ch],
            c.d_final_plane, c.final_plane_pitch,
            c.ch_width * sizeof(int16_t), c.ch_height,
            cudaMemcpyDeviceToHost, impl_->streams[ch]
        );
        if (err != cudaSuccess) return false;
    }

    for (int ch = 0; ch < 3; ++ch) {
        cudaStreamSynchronize(impl_->streams[ch]);
    }
    cudaEventRecord(impl_->ev_d2h_stop, impl_->streams[0]);
    cudaEventSynchronize(impl_->ev_d2h_stop);

    cudaEventElapsedTime(&impl_->time_d2h_ms, impl_->ev_d2h_start, impl_->ev_d2h_stop);
    return true;
}

bool CudaInverseWaveletPipeline::DownloadIntermediateLL(int level, int channel, int16_t* host_dst, int dst_pitch) {
    if (!impl_ || !impl_->initialized) return false;
    if (channel < 0 || channel >= 3) return false;

    auto& c = impl_->channels[channel];
    cudaStream_t s = impl_->streams[channel];

    const int16_t* src = nullptr;
    size_t src_pitch = 0;
    int w = 0, h = 0;

    if (level == 1) { // LL1 (output of L2)
        src = c.d_ll1;
        src_pitch = c.ll1_pitch;
        w = c.ch_width / 4;
        h = c.ch_height / 4;
    } else if (level == 0) { // LL0 (output of L1)
        src = c.d_ll0;
        src_pitch = c.ll0_pitch;
        w = c.ch_width / 2;
        h = c.ch_height / 2;
    } else {
        return false;
    }

    cudaError_t err = cudaMemcpy2DAsync(
        host_dst, dst_pitch,
        src, src_pitch,
        w * sizeof(int16_t), h,
        cudaMemcpyDeviceToHost, s
    );
    if (err != cudaSuccess) return false;
    cudaStreamSynchronize(s);
    return true;
}

float CudaInverseWaveletPipeline::GetH2DTimeMs() const {
    return impl_ ? impl_->time_h2d_ms : 0.0f;
}

float CudaInverseWaveletPipeline::GetInverseTimeMs() const {
    return impl_ ? impl_->time_inv_ms : 0.0f;
}

float CudaInverseWaveletPipeline::GetD2HTimeMs() const {
    return impl_ ? impl_->time_d2h_ms : 0.0f;
}

const int16_t* CudaInverseWaveletPipeline::GetDevicePlane(int channel) const {
    if (!impl_ || !impl_->initialized || channel < 0 || channel >= 3) return nullptr;
    return impl_->channels[channel].d_final_plane;
}

int CudaInverseWaveletPipeline::GetDevicePlanePitch(int channel) const {
    if (!impl_ || !impl_->initialized || channel < 0 || channel >= 3) return 0;
    return int(impl_->channels[channel].final_plane_pitch);
}

} // namespace HyperCine
