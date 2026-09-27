// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/gpu_pyramid.h"
#include "hypercine/phase2a_gpu.h"

#include <cuda_runtime.h>
#include <algorithm>
#include <cstring>
#include <vector>

extern "C" int g_midpoint_prequant;

namespace hybrid {

constexpr int kChannels = 3;
constexpr int kLevels = 3;
constexpr int kBands = 4;

int MidpointFor(int divisor) {
    if (divisor <= 1) return 0;
    int midpoint = 0;
    if (g_midpoint_prequant >= 2 && g_midpoint_prequant < 9) {
        midpoint = divisor / g_midpoint_prequant;
        if (g_midpoint_prequant == 2 && midpoint > 0) --midpoint;
    }
    return midpoint;
}

__device__ __forceinline__ int16_t Sat16(int value) {
    return static_cast<int16_t>(max(-32768, min(32767, value)));
}

__device__ __forceinline__ int16_t Quantize(int value, int divisor, int midpoint) {
    if (divisor <= 1) return Sat16(value);
    const int sign = value < 0 ? -1 : 1;
    const int magnitude = value < 0 ? -value : value;
    const unsigned multiplier = (1u << 16) / static_cast<unsigned>(divisor);
    const unsigned q = (static_cast<unsigned>(magnitude + midpoint) * multiplier) >> 16;
    return Sat16(sign * static_cast<int>(q));
}

__device__ __forceinline__ int Prescale2(int sample) {
    return (sample + 3) >> 2;
}

template <int kPrescale>
__global__ void HorizontalKernel(const int16_t* __restrict__ src, int width, int height, int src_stride,
                                 int16_t* __restrict__ low, int low_stride,
                                 int16_t* __restrict__ high, int high_stride) {
    const int c = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int half_w = width >> 1;
    if (c >= half_w || y >= height) return;

    const int16_t* row = src + static_cast<size_t>(y) * src_stride;
    const int x = c * 2;

    int low_sum;
    if (kPrescale == 2) {
        low_sum = (static_cast<int>(row[x]) + static_cast<int>(row[x + 1]) + 3) >> 2;
    } else {
        low_sum = static_cast<int>(row[x]) + static_cast<int>(row[x + 1]);
    }
    low[static_cast<size_t>(y) * low_stride + c] = Sat16(low_sum);

    int s0, s1, s2, s3, s4, s5, sm2, sm1, sp2, sp3, sm3, sm4;
    if (kPrescale == 2) {
        s0 = Prescale2(row[x]);
        s1 = Prescale2(row[x + 1]);
    } else {
        s0 = row[x];
        s1 = row[x + 1];
    }
    int high_sum;
    if (c == 0) {
        if (kPrescale == 2) {
            s0 = Prescale2(row[0]); s1 = Prescale2(row[1]); s2 = Prescale2(row[2]);
            s3 = Prescale2(row[3]); s4 = Prescale2(row[4]); s5 = Prescale2(row[5]);
        } else {
            s0 = row[0]; s1 = row[1]; s2 = row[2]; s3 = row[3]; s4 = row[4]; s5 = row[5];
        }
        high_sum = 5 * s0 - 11 * s1 + 4 * s2 + 4 * s3 - s4 - s5;
        high_sum = (high_sum + 4) >> 3;
    } else if (c < half_w - 1) {
        if (kPrescale == 2) {
            sm2 = Prescale2(row[x - 2]); sm1 = Prescale2(row[x - 1]);
            sp2 = Prescale2(row[x + 2]); sp3 = Prescale2(row[x + 3]);
            s0 = Prescale2(row[x]); s1 = Prescale2(row[x + 1]);
        } else {
            sm2 = row[x - 2]; sm1 = row[x - 1]; sp2 = row[x + 2]; sp3 = row[x + 3];
            s0 = row[x]; s1 = row[x + 1];
        }
        int hp = -sm2 - sm1 + sp2 + sp3;
        hp = (hp + 4) >> 3;
        high_sum = hp + s0 - s1;
    } else {
        if (kPrescale == 2) {
            s0 = Prescale2(row[x]); s1 = Prescale2(row[x + 1]);
            sm1 = Prescale2(row[x - 1]); sm2 = Prescale2(row[x - 2]);
            sm3 = Prescale2(row[x - 3]); sm4 = Prescale2(row[x - 4]);
        } else {
            s0 = row[x]; s1 = row[x + 1]; sm1 = row[x - 1]; sm2 = row[x - 2];
            sm3 = row[x - 3]; sm4 = row[x - 4];
        }
        high_sum = 11 * s0 - 5 * s1 - 4 * sm1 - 4 * sm2 + sm3 + sm4;
        high_sum = (high_sum + 4) >> 3;
    }
    high[static_cast<size_t>(y) * high_stride + c] = Sat16(high_sum);
}

__global__ void VerticalQuantKernel(const int16_t* __restrict__ lowpass, const int16_t* __restrict__ highpass,
                                    int width, int height, int low_stride, int high_stride,
                                    int16_t* __restrict__ ll, int ll_stride,
                                    int16_t* __restrict__ lh, int lh_stride,
                                    int16_t* __restrict__ hl, int hl_stride,
                                    int16_t* __restrict__ hh, int hh_stride,
                                    int qll, int qlh, int qhl, int qhh,
                                    int mll, int mlh, int mhl, int mhh) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int r = blockIdx.y * blockDim.y + threadIdx.y;
    const int half_h = height >> 1;
    if (x >= width || r >= half_h) return;
    const int y = r * 2;

    int odd_low;
    int odd_high;
    if (r == 0) {
        odd_low = 5 * lowpass[0 * low_stride + x] - 11 * lowpass[1 * low_stride + x]
                + 4 * lowpass[2 * low_stride + x] + 4 * lowpass[3 * low_stride + x]
                - lowpass[4 * low_stride + x] - lowpass[5 * low_stride + x];
        odd_low = (odd_low + 4) >> 3;
        odd_high = 5 * highpass[0 * high_stride + x] - 11 * highpass[1 * high_stride + x]
                 + 4 * highpass[2 * high_stride + x] + 4 * highpass[3 * high_stride + x]
                 - highpass[4 * high_stride + x] - highpass[5 * high_stride + x];
        odd_high = (odd_high + 4) >> 3;
    } else if (r < half_h - 1) {
        int hp = -lowpass[(y - 2) * low_stride + x] - lowpass[(y - 1) * low_stride + x]
               + lowpass[(y + 2) * low_stride + x] + lowpass[(y + 3) * low_stride + x];
        odd_low = ((hp + 4) >> 3) + lowpass[y * low_stride + x] - lowpass[(y + 1) * low_stride + x];
        hp = -highpass[(y - 2) * high_stride + x] - highpass[(y - 1) * high_stride + x]
           + highpass[(y + 2) * high_stride + x] + highpass[(y + 3) * high_stride + x];
        odd_high = ((hp + 4) >> 3) + highpass[y * high_stride + x] - highpass[(y + 1) * high_stride + x];
    } else {
        odd_low = 11 * lowpass[y * low_stride + x] - 5 * lowpass[(y + 1) * low_stride + x]
                - 4 * lowpass[(y - 1) * low_stride + x] - 4 * lowpass[(y - 2) * low_stride + x]
                + lowpass[(y - 3) * low_stride + x] + lowpass[(y - 4) * low_stride + x];
        odd_low = (odd_low + 4) >> 3;
        odd_high = 11 * highpass[y * high_stride + x] - 5 * highpass[(y + 1) * high_stride + x]
                 - 4 * highpass[(y - 1) * high_stride + x] - 4 * highpass[(y - 2) * high_stride + x]
                 + highpass[(y - 3) * high_stride + x] + highpass[(y - 4) * high_stride + x];
        odd_high = (odd_high + 4) >> 3;
    }

    const int even_low = static_cast<int>(lowpass[y * low_stride + x]) + static_cast<int>(lowpass[(y + 1) * low_stride + x]);
    const int even_high = static_cast<int>(highpass[y * high_stride + x]) + static_cast<int>(highpass[(y + 1) * high_stride + x]);

    ll[static_cast<size_t>(r) * ll_stride + x] = Quantize(even_low, qll, mll);
    hl[static_cast<size_t>(r) * hl_stride + x] = Quantize(odd_low, qhl, mhl);
    lh[static_cast<size_t>(r) * lh_stride + x] = Quantize(even_high, qlh, mlh);
    hh[static_cast<size_t>(r) * hh_stride + x] = Quantize(odd_high, qhh, mhh);
}

struct RgbCoeff {
    int yr, yg, yb, yo;
    int ur, ug, ub, uo;
    int vr, vg, vb, vo;
};

RgbCoeff MakeCoeff(int color_space) {
    const float precision = static_cast<float>(1 << 13);
    float yr, yg, yb, yo, ur, ug, ub, uo, vr, vg, vb, vo;
    switch (color_space & 7) {
    case 1:  // CG 601
        yr = 0.257f; yg = 0.504f; yb = 0.098f; yo = static_cast<float>((65536 * 16) >> 8);
        ur = 0.148f; ug = 0.291f; ub = 0.439f; uo = 32768.f;
        vr = 0.439f; vg = 0.368f; vb = 0.071f; vo = 32768.f;
        break;
    case 6:  // VS 709 = 2|4
        yr = 0.213f; yg = 0.715f; yb = 0.072f; yo = 0.f;
        ur = 0.117f; ug = 0.394f; ub = 0.511f; uo = 32768.f;
        vr = 0.511f; vg = 0.464f; vb = 0.047f; vo = 32768.f;
        break;
    case 5:  // VS 601 = 1|4
        yr = 0.299f; yg = 0.587f; yb = 0.114f; yo = 0.f;
        ur = 0.172f; ug = 0.339f; ub = 0.511f; uo = 32768.f;
        vr = 0.511f; vg = 0.428f; vb = 0.083f; vo = 32768.f;
        break;
    case 2:  // CG 709
    default:
        yr = 0.183f; yg = 0.614f; yb = 0.062f; yo = static_cast<float>((65536 * 16) >> 8);
        ur = 0.101f; ug = 0.338f; ub = 0.439f; uo = 32768.f;
        vr = 0.439f; vg = 0.399f; vb = 0.040f; vo = 32768.f;
        break;
    }
    RgbCoeff c;
    c.yr = static_cast<int>(precision * yr);
    c.yg = static_cast<int>(precision * yg);
    c.yb = static_cast<int>(precision * yb);
    c.yo = static_cast<int>(yo) >> 2;
    c.ur = static_cast<int>(precision * ur);
    c.ug = static_cast<int>(precision * ug);
    c.ub = static_cast<int>(precision * ub);
    c.uo = static_cast<int>(uo) >> 2;
    c.vr = static_cast<int>(precision * vr);
    c.vg = static_cast<int>(precision * vg);
    c.vb = static_cast<int>(precision * vb);
    c.vo = static_cast<int>(vo) >> 2;
    return c;
}

__device__ __forceinline__ int16_t SatAdd16(int a, int b) {
    int s = a + b;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return static_cast<int16_t>(s);
}

__device__ __forceinline__ int16_t MulHi16(int a, int b) {
    return static_cast<int16_t>((a * b) >> 16);
}

__device__ __forceinline__ int16_t FinishChannel(int16_t value, int offset) {
    value = static_cast<int16_t>(static_cast<uint16_t>(value) << 2);
    value = SatAdd16(value, offset);
    value = SatAdd16(value, 0x3fff);
    uint16_t u = static_cast<uint16_t>(value);
    u = (u < 0x3fff) ? 0 : static_cast<uint16_t>(u - 0x3fff);
    value = static_cast<int16_t>(u);
    value = static_cast<int16_t>(static_cast<uint16_t>(value) << 2);
    return value;
}

__global__ void BgraToYuvKernel(const uint8_t* __restrict__ bgra, int width, int height, int pitch,
                                RgbCoeff coeff, int16_t* __restrict__ y, int16_t* __restrict__ u,
                                int16_t* __restrict__ v) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || row >= height) return;
    const int src_row = height - 1 - row;
    const uint8_t* pix = bgra + static_cast<size_t>(src_row) * pitch + static_cast<size_t>(x) * 4u;
    const int R = static_cast<int>(pix[2]) << 7;
    const int G = static_cast<int>(pix[1]) << 7;
    const int B = static_cast<int>(pix[0]) << 7;

    int16_t acc = MulHi16(R, coeff.yr);
    acc = SatAdd16(acc, MulHi16(G, coeff.yg));
    acc = SatAdd16(acc, MulHi16(B, coeff.yb));
    const int16_t Y = FinishChannel(acc, coeff.yo);
    y[static_cast<size_t>(row) * width + x] = static_cast<int16_t>(static_cast<uint16_t>(Y) >> 6);
    if ((x & 1) == 0) {
        acc = MulHi16(R, -coeff.ur);
        acc = SatAdd16(acc, MulHi16(G, -coeff.ug));
        acc = SatAdd16(acc, MulHi16(B, coeff.ub));
        const int16_t U = FinishChannel(acc, coeff.uo);
        acc = MulHi16(R, coeff.vr);
        acc = SatAdd16(acc, MulHi16(G, -coeff.vg));
        acc = SatAdd16(acc, MulHi16(B, -coeff.vb));
        const int16_t V = FinishChannel(acc, coeff.vo);
        const int cx = x >> 1;
        u[static_cast<size_t>(row) * (width >> 1) + cx] = static_cast<int16_t>(static_cast<uint16_t>(U) >> 6);
        v[static_cast<size_t>(row) * (width >> 1) + cx] = static_cast<int16_t>(static_cast<uint16_t>(V) >> 6);
    }
}

struct BandBuf {
    int channel = 0;
    int level = 0;
    int band = 0;
    int width = 0;
    int height = 0;
    int capacity_w = 0;
    int capacity_h = 0;
    int16_t* device = nullptr;
    int16_t* host = nullptr;
    bool publish = false;
};

struct HyperCineGpuSlotImpl {
    int max_width = 0;
    int max_height = 0;
    cudaStream_t stream = nullptr;
    cudaEvent_t ev_h2d0 = nullptr, ev_h2d1 = nullptr;
    cudaEvent_t ev_k0 = nullptr, ev_k1 = nullptr;
    cudaEvent_t ev_d0 = nullptr, ev_d1 = nullptr;
    int16_t* d_horiz_low = nullptr;
    int16_t* d_horiz_high = nullptr;
    size_t horiz_elems = 0;
    std::vector<BandBuf> bands;
    int16_t* source[kChannels] = {};
    int16_t* host_plane[kChannels] = {};
    int host_plane_elems[kChannels] = {};
    HyperCineGpuTiming timing{};
    int ready = 0;

    cudaStream_t copy_stream = nullptr;
    cudaStream_t frontend_stream = nullptr;
    uint8_t* d_bgra = nullptr;
    size_t bgra_capacity = 0;
    cudaEvent_t level_done[kChannels][kLevels] = {};
    cudaEvent_t ev_color0 = nullptr, ev_color1 = nullptr;
    cudaEvent_t ev_comp0 = nullptr, ev_comp1 = nullptr;
    cudaEvent_t ev_all_host = nullptr;
    cudaEvent_t ev_d2h0 = nullptr, ev_d2h1 = nullptr;
    cudaEvent_t ev_range0 = nullptr, ev_range1 = nullptr;
    cudaEvent_t ev_pack0 = nullptr, ev_pack1 = nullptr;
    bool pack_needs_elapsed = false;
    bool range_recorded = false;
    int gpu_pack_lowpass = 0;

    struct BandX {
        cudaEvent_t device_ready = nullptr;
        cudaEvent_t d2h_begin = nullptr;
        cudaEvent_t d2h_end = nullptr;
        int* d_minmax = nullptr;
        int* h_minmax = nullptr;
        int8_t* d_i8 = nullptr;
        // 0 idle, 1 waiting for header, 2 copy queued, 3 host ready, 4 consumed
        int state = 0;
        int elem = 2;
        int vmin = 0;
        int vmax = 0;
        int fits = 0;
        size_t bytes = 0;
        float copy_ms = 0.f;
    };
    std::vector<BandX> extra;

    int run_input = 0;
    int run_transport = 0;
    int run_phase = 0; // 0 idle, 1 wait level, 2 all queued, 3 error
    int run_channel = 0;
    int run_level = 0;
    int run_error = 0;
    int run_width = 0;
    int run_height = 0;
    int run_pitch = 0;
    int run_color_space = 0;
    const uint8_t* run_bgra = nullptr;
    int run_widths[kChannels] = {};
    int run_heights[kChannels] = {};
    int run_prescale[kLevels] = {};
    int run_quant[36] = {};
    int run_int8 = 0;
    int run_int16 = 0;
    size_t run_d2h_bytes = 0;
    size_t run_h2d_bytes = 0;
    float run_range_ms = 0.f;
    float run_pack_ms = 0.f;
    bool run_d2h_open = false;
    bool run_hold = false;
};

size_t ElemCount(int w, int h) {
    return static_cast<size_t>(w) * static_cast<size_t>(h);
}

int LaunchLevel(HyperCineGpuSlotImpl* slot, int16_t* src, int width, int height,
                int prescale, const int quant[4], BandBuf* out) {
    const int half_w = width >> 1;
    const int half_h = height >> 1;
    if (half_w < 3 || half_h < 3) return -1;
    const size_t horiz = ElemCount(half_w, height);
    if (horiz > slot->horiz_elems) return -2;

    dim3 block(32, 4);
    dim3 hgrid((half_w + 31) / 32, (height + 3) / 4);
    if (prescale == 2) {
        HorizontalKernel<2><<<hgrid, block, 0, slot->stream>>>(src, width, height, width,
            slot->d_horiz_low, half_w, slot->d_horiz_high, half_w);
    } else {
        HorizontalKernel<0><<<hgrid, block, 0, slot->stream>>>(src, width, height, width,
            slot->d_horiz_low, half_w, slot->d_horiz_high, half_w);
    }

    dim3 vgrid((half_w + 31) / 32, (half_h + 3) / 4);
    VerticalQuantKernel<<<vgrid, block, 0, slot->stream>>>(
        slot->d_horiz_low, slot->d_horiz_high, half_w, height, half_w, half_w,
        out[0].device, half_w, out[1].device, half_w, out[2].device, half_w, out[3].device, half_w,
        quant[0], quant[1], quant[2], quant[3],
        MidpointFor(quant[0]), MidpointFor(quant[1]), MidpointFor(quant[2]), MidpointFor(quant[3]));
    return 0;
}

}  // namespace hybrid

struct HyperCineGpuSlot : hybrid::HyperCineGpuSlotImpl {};

using hybrid::BandBuf;
using hybrid::ElemCount;
using hybrid::LaunchLevel;
using hybrid::MakeCoeff;
using hybrid::RgbCoeff;
using hybrid::BgraToYuvKernel;
using hybrid::kChannels;
using hybrid::kLevels;
using hybrid::kBands;

extern "C" HyperCineGpuSlot* HyperCine_GpuSlotCreate(int max_width, int max_height) {
    if (max_width < 16 || max_height < 16) return nullptr;
    HyperCineGpuSlot* slot = new HyperCineGpuSlot();
    slot->max_width = max_width;
    slot->max_height = max_height;
    if (cudaStreamCreate(&slot->stream) != cudaSuccess) {
        delete slot;
        return nullptr;
    }
    if (cudaStreamCreate(&slot->copy_stream) != cudaSuccess) {
        cudaStreamDestroy(slot->stream);
        delete slot;
        return nullptr;
    }
    cudaEventCreate(&slot->ev_h2d0);
    cudaEventCreate(&slot->ev_h2d1);
    cudaEventCreate(&slot->ev_k0);
    cudaEventCreate(&slot->ev_k1);
    cudaEventCreate(&slot->ev_d0);
    cudaEventCreate(&slot->ev_d1);
    cudaEventCreate(&slot->ev_color0);
    cudaEventCreate(&slot->ev_color1);
    cudaEventCreate(&slot->ev_comp0);
    cudaEventCreate(&slot->ev_comp1);
    cudaEventCreate(&slot->ev_all_host);
    cudaEventCreate(&slot->ev_d2h0);
    cudaEventCreate(&slot->ev_d2h1);
    cudaEventCreate(&slot->ev_range0);
    cudaEventCreate(&slot->ev_range1);
    cudaEventCreate(&slot->ev_pack0);
    cudaEventCreate(&slot->ev_pack1);
    for (int c = 0; c < kChannels; ++c) {
        for (int level = 0; level < kLevels; ++level) cudaEventCreate(&slot->level_done[c][level]);
    }
    slot->bgra_capacity = ElemCount(max_width, max_height) * 4u;
    cudaMalloc(&slot->d_bgra, slot->bgra_capacity);

    slot->horiz_elems = ElemCount(max_width / 2, max_height);
    cudaMalloc(&slot->d_horiz_low, slot->horiz_elems * sizeof(int16_t));
    cudaMalloc(&slot->d_horiz_high, slot->horiz_elems * sizeof(int16_t));

    for (int c = 0; c < kChannels; ++c) {
        const int cw = (c == 0) ? max_width : max_width / 2;
        const int ch = max_height;
        cudaMalloc(&slot->source[c], ElemCount(cw, ch) * sizeof(int16_t));
        slot->host_plane_elems[c] = static_cast<int>(ElemCount(cw, ch));
        cudaHostAlloc(reinterpret_cast<void**>(&slot->host_plane[c]),
                      slot->host_plane_elems[c] * sizeof(int16_t), cudaHostAllocDefault);
        int w = cw;
        int h = ch;
        for (int level = 0; level < kLevels; ++level) {
            const int bw = w / 2;
            const int bh = h / 2;
            for (int band = 0; band < kBands; ++band) {
                BandBuf buf;
                buf.channel = c;
                buf.level = level;
                buf.band = band;
                buf.width = bw;
                buf.height = bh;
                buf.capacity_w = bw;
                buf.capacity_h = bh;
                buf.publish = (band != 0) || (level == kLevels - 1);
                cudaMalloc(&buf.device, ElemCount(bw, bh) * sizeof(int16_t));
                if (buf.publish) {
                    cudaHostAlloc(reinterpret_cast<void**>(&buf.host), ElemCount(bw, bh) * sizeof(int16_t), cudaHostAllocDefault);
                }
                slot->bands.push_back(buf);
            }
            w = bw;
            h = bh;
        }
    }
    slot->extra.resize(slot->bands.size());
    for (size_t i = 0; i < slot->bands.size(); ++i) {
        hybrid::HyperCineGpuSlotImpl::BandX& extra = slot->extra[i];
        cudaEventCreate(&extra.device_ready);
        cudaEventCreate(&extra.d2h_begin);
        cudaEventCreate(&extra.d2h_end);
        cudaMalloc(&extra.d_minmax, 3 * sizeof(int));
        cudaHostAlloc(reinterpret_cast<void**>(&extra.h_minmax), 3 * sizeof(int), cudaHostAllocDefault);
        if (slot->bands[i].publish) {
            cudaMalloc(&extra.d_i8, ElemCount(slot->bands[i].capacity_w, slot->bands[i].capacity_h));
        }
    }
    return slot;
}

extern "C" HyperCineGpuSlot* HyperCine_GpuSlotCreateForPhase2C(int max_width, int max_height, int alloc_device_bgra) {
    if (max_width < 16 || max_height < 16) return nullptr;
    HyperCineGpuSlot* slot = new HyperCineGpuSlot();
    slot->max_width = max_width;
    slot->max_height = max_height;
    int least_prio = 0, greatest_prio = 0;
    cudaDeviceGetStreamPriorityRange(&least_prio, &greatest_prio);
    if (cudaStreamCreateWithPriority(&slot->stream, cudaStreamNonBlocking, greatest_prio) != cudaSuccess) {
        delete slot;
        return nullptr;
    }
    if (cudaStreamCreateWithPriority(&slot->frontend_stream, cudaStreamNonBlocking, greatest_prio) != cudaSuccess) {
        cudaStreamDestroy(slot->stream);
        delete slot;
        return nullptr;
    }
    if (cudaStreamCreateWithPriority(&slot->copy_stream, cudaStreamNonBlocking, greatest_prio) != cudaSuccess) {
        cudaStreamDestroy(slot->frontend_stream);
        cudaStreamDestroy(slot->stream);
        delete slot;
        return nullptr;
    }
    cudaEventCreate(&slot->ev_h2d0);
    cudaEventCreate(&slot->ev_h2d1);
    cudaEventCreate(&slot->ev_k0);
    cudaEventCreate(&slot->ev_k1);
    cudaEventCreate(&slot->ev_d0);
    cudaEventCreate(&slot->ev_d1);
    cudaEventCreate(&slot->ev_color0);
    cudaEventCreateWithFlags(&slot->ev_color1, cudaEventBlockingSync);
    cudaEventCreate(&slot->ev_comp0);
    cudaEventCreateWithFlags(&slot->ev_comp1, cudaEventBlockingSync);
    cudaEventCreateWithFlags(&slot->ev_all_host, cudaEventBlockingSync);
    cudaEventCreate(&slot->ev_d2h0);
    cudaEventCreateWithFlags(&slot->ev_d2h1, cudaEventBlockingSync);
    cudaEventCreate(&slot->ev_range0);
    cudaEventCreate(&slot->ev_range1);
    cudaEventCreate(&slot->ev_pack0);
    cudaEventCreateWithFlags(&slot->ev_pack1, cudaEventBlockingSync);
    for (int c = 0; c < kChannels; ++c) {
        for (int level = 0; level < kLevels; ++level) cudaEventCreate(&slot->level_done[c][level]);
    }
    slot->bgra_capacity = ElemCount(max_width, max_height) * 4u;
    if (alloc_device_bgra) {
        if (cudaMalloc(&slot->d_bgra, slot->bgra_capacity) != cudaSuccess) {
            HyperCine_GpuSlotDestroy(slot);
            return nullptr;
        }
    }

    slot->horiz_elems = ElemCount(max_width / 2, max_height);
    if (cudaMalloc(&slot->d_horiz_low, slot->horiz_elems * sizeof(int16_t)) != cudaSuccess ||
        cudaMalloc(&slot->d_horiz_high, slot->horiz_elems * sizeof(int16_t)) != cudaSuccess) {
        HyperCine_GpuSlotDestroy(slot);
        return nullptr;
    }

    for (int c = 0; c < kChannels; ++c) {
        const int cw = (c == 0) ? max_width : max_width / 2;
        const int ch = max_height;
        if (cudaMalloc(&slot->source[c], ElemCount(cw, ch) * sizeof(int16_t)) != cudaSuccess) {
            HyperCine_GpuSlotDestroy(slot);
            return nullptr;
        }
        slot->host_plane_elems[c] = 0;
        slot->host_plane[c] = nullptr;
        int w = cw;
        int h = ch;
        for (int level = 0; level < kLevels; ++level) {
            const int bw = w / 2;
            const int bh = h / 2;
            for (int band = 0; band < kBands; ++band) {
                BandBuf buf;
                buf.channel = c;
                buf.level = level;
                buf.band = band;
                buf.width = bw;
                buf.height = bh;
                buf.capacity_w = bw;
                buf.capacity_h = bh;
                buf.publish = (band != 0) || (level == kLevels - 1);
                if (cudaMalloc(&buf.device, ElemCount(bw, bh) * sizeof(int16_t)) != cudaSuccess) {
                    HyperCine_GpuSlotDestroy(slot);
                    return nullptr;
                }
                const bool is_lowpass = (band == 0) && (level == kLevels - 1);
                if (is_lowpass) {
                    if (cudaHostAlloc(reinterpret_cast<void**>(&buf.host), ElemCount(bw, bh) * sizeof(int16_t), cudaHostAllocDefault) != cudaSuccess) {
                        cudaFree(buf.device);
                        HyperCine_GpuSlotDestroy(slot);
                        return nullptr;
                    }
                }
                slot->bands.push_back(buf);
            }
            w = bw;
            h = bh;
        }
    }
    slot->extra.resize(slot->bands.size());
    for (size_t i = 0; i < slot->bands.size(); ++i) {
        hybrid::HyperCineGpuSlotImpl::BandX& extra = slot->extra[i];
        cudaEventCreate(&extra.device_ready);
        cudaEventCreate(&extra.d2h_begin);
        cudaEventCreateWithFlags(&extra.d2h_end, cudaEventBlockingSync);
    }
    return slot;
}

extern "C" void HyperCine_GpuSlotDestroy(HyperCineGpuSlot* slot) {
    if (!slot) return;
    for (BandBuf& band : slot->bands) {
        if (band.device) cudaFree(band.device);
        if (band.host) cudaFreeHost(band.host);
    }
    for (int c = 0; c < kChannels; ++c) {
        cudaFree(slot->source[c]);
        if (slot->host_plane[c]) cudaFreeHost(slot->host_plane[c]);
    }
    for (auto& extra : slot->extra) {
        cudaEventDestroy(extra.device_ready);
        cudaEventDestroy(extra.d2h_begin);
        cudaEventDestroy(extra.d2h_end);
        if (extra.d_minmax) cudaFree(extra.d_minmax);
        if (extra.h_minmax) cudaFreeHost(extra.h_minmax);
        if (extra.d_i8) cudaFree(extra.d_i8);
    }
    if (slot->d_bgra) cudaFree(slot->d_bgra);
    for (int c = 0; c < kChannels; ++c) {
        for (int level = 0; level < kLevels; ++level) cudaEventDestroy(slot->level_done[c][level]);
    }
    cudaEventDestroy(slot->ev_color0);
    cudaEventDestroy(slot->ev_color1);
    cudaEventDestroy(slot->ev_comp0);
    cudaEventDestroy(slot->ev_comp1);
    cudaEventDestroy(slot->ev_all_host);
    cudaEventDestroy(slot->ev_d2h0);
    cudaEventDestroy(slot->ev_d2h1);
    cudaEventDestroy(slot->ev_range0);
    cudaEventDestroy(slot->ev_range1);
    cudaEventDestroy(slot->ev_pack0);
    cudaEventDestroy(slot->ev_pack1);
    if (slot->copy_stream) cudaStreamDestroy(slot->copy_stream);
    if (slot->frontend_stream) cudaStreamDestroy(slot->frontend_stream);
    cudaFree(slot->d_horiz_low);
    cudaFree(slot->d_horiz_high);
    cudaEventDestroy(slot->ev_h2d0);
    cudaEventDestroy(slot->ev_h2d1);
    cudaEventDestroy(slot->ev_k0);
    cudaEventDestroy(slot->ev_k1);
    cudaEventDestroy(slot->ev_d0);
    cudaEventDestroy(slot->ev_d1);
    cudaStreamDestroy(slot->stream);
    delete slot;
}

extern "C" int HyperCine_GpuSlotLaunch(HyperCineGpuSlot* slot,
                                       const int16_t* const planes[3],
                                       const int widths[3],
                                       const int heights[3],
                                       const int prescale[3],
                                       const int quant[36],
                                       int async_launch) {
    if (!slot || !planes || !widths || !heights || !prescale || !quant) return -1;
    slot->ready = 0;
    cudaEventRecord(slot->ev_h2d0, slot->stream);
    size_t h2d_bytes = 0;
    for (int c = 0; c < kChannels; ++c) {
        if (widths[c] > ((c == 0) ? slot->max_width : slot->max_width / 2) || heights[c] > slot->max_height) return -2;
        const size_t bytes = ElemCount(widths[c], heights[c]) * sizeof(int16_t);
        cudaMemcpyAsync(slot->source[c], planes[c], bytes, cudaMemcpyHostToDevice, slot->stream);
        h2d_bytes += bytes;
    }
    cudaEventRecord(slot->ev_h2d1, slot->stream);
    cudaEventRecord(slot->ev_k0, slot->stream);

    for (int c = 0; c < kChannels; ++c) {
        int16_t* src = slot->source[c];
        int w = widths[c];
        int h = heights[c];
        for (int level = 0; level < kLevels; ++level) {
            BandBuf* out = &slot->bands[static_cast<size_t>((c * kLevels + level) * kBands)];
            const int* q = quant + (c * kLevels + level) * kBands;
            if (LaunchLevel(slot, src, w, h, prescale[level], q, out) != 0) return -3;
            for (int band = 0; band < 4; ++band) {
                out[band].width = w / 2;
                out[band].height = h / 2;
            }
            src = out[0].device;
            w = out[0].width;
            h = out[0].height;
        }
    }
    cudaEventRecord(slot->ev_k1, slot->stream);
    cudaEventRecord(slot->ev_d0, slot->stream);
    size_t d2h_bytes = 0;
    int published = 0;
    for (BandBuf& band : slot->bands) {
        if (!band.publish || !band.host) continue;
        const size_t bytes = ElemCount(band.width, band.height) * sizeof(int16_t);
        cudaMemcpyAsync(band.host, band.device, bytes, cudaMemcpyDeviceToHost, slot->stream);
        d2h_bytes += bytes;
        ++published;
    }
    cudaEventRecord(slot->ev_d1, slot->stream);
    slot->timing.h2d_bytes = h2d_bytes;
    slot->timing.d2h_bytes = d2h_bytes;
    slot->timing.band_count = published;
    if (!async_launch) return HyperCine_GpuSlotSync(slot, &slot->timing);
    return 0;
}

extern "C" int HyperCine_GpuSlotSync(HyperCineGpuSlot* slot, HyperCineGpuTiming* timing) {
    if (!slot) return -1;
    cudaError_t err = cudaEventSynchronize(slot->ev_d1);
    if (err != cudaSuccess) return -2;
    err = cudaGetLastError();
    if (err != cudaSuccess) return -3;
    float h2d = 0, comp = 0, d2h = 0;
    cudaEventElapsedTime(&h2d, slot->ev_h2d0, slot->ev_h2d1);
    cudaEventElapsedTime(&comp, slot->ev_k0, slot->ev_k1);
    cudaEventElapsedTime(&d2h, slot->ev_d0, slot->ev_d1);
    slot->timing.h2d_ms = h2d;
    slot->timing.compute_ms = comp;
    slot->timing.d2h_ms = d2h;
    slot->ready = 1;
    if (timing) *timing = slot->timing;
    return 0;
}

extern "C" int HyperCine_GpuSlotBandCount(const HyperCineGpuSlot* slot) {
    if (!slot) return 0;
    int count = 0;
    for (const BandBuf& band : slot->bands) if (band.publish) ++count;
    return count;
}

extern "C" int HyperCine_GpuSlotBand(const HyperCineGpuSlot* slot, int index,
                                     int* channel, int* level, int* band,
                                     int* width, int* height, const int16_t** data) {
    if (!slot) return -1;
    int seen = 0;
    for (const BandBuf& item : slot->bands) {
        if (!item.publish) continue;
        if (seen == index) {
            if (channel) *channel = item.channel;
            if (level) *level = item.level;
            if (band) *band = item.band;
            if (width) *width = item.width;
            if (height) *height = item.height;
            if (data) *data = item.host;
            return 0;
        }
        ++seen;
    }
    return -1;
}

extern "C" int16_t* HyperCine_GpuSlotHostPlane(HyperCineGpuSlot* slot, int channel, int* capacity_elems) {
    if (!slot || channel < 0 || channel >= 3) return nullptr;
    if (capacity_elems) *capacity_elems = slot->host_plane_elems[channel];
    return slot->host_plane[channel];
}

extern "C" int HyperCine_BgraToYuv10_CUDA(const uint8_t* bgra, int width, int height, int pitch,
                                          int color_space, int16_t* y, int16_t* u, int16_t* v,
                                          HyperCineColorTiming* timing) {
    if (!bgra || !y || !u || !v || width < 2 || height < 2 || (width & 1)) return -1;
    const size_t bgra_bytes = static_cast<size_t>(pitch) * static_cast<size_t>(height);
    const size_t y_bytes = ElemCount(width, height) * sizeof(int16_t);
    const size_t c_bytes = ElemCount(width / 2, height) * sizeof(int16_t);
    uint8_t* d_bgra = nullptr;
    int16_t* d_y = nullptr;
    int16_t* d_u = nullptr;
    int16_t* d_v = nullptr;
    cudaStream_t stream = nullptr;
    cudaStreamCreate(&stream);
    cudaMalloc(&d_bgra, bgra_bytes);
    cudaMalloc(&d_y, y_bytes);
    cudaMalloc(&d_u, c_bytes);
    cudaMalloc(&d_v, c_bytes);

    cudaEvent_t e0, e1, e2, e3, e4;
    cudaEventCreate(&e0); cudaEventCreate(&e1); cudaEventCreate(&e2);
    cudaEventCreate(&e3); cudaEventCreate(&e4);
    cudaEventRecord(e0, stream);
    cudaMemcpyAsync(d_bgra, bgra, bgra_bytes, cudaMemcpyHostToDevice, stream);
    cudaEventRecord(e1, stream);
    const RgbCoeff coeff = MakeCoeff(color_space);
    dim3 block(32, 8);
    dim3 grid((width + 31) / 32, (height + 7) / 8);
    BgraToYuvKernel<<<grid, block, 0, stream>>>(d_bgra, width, height, pitch, coeff, d_y, d_u, d_v);
    cudaEventRecord(e2, stream);
    BgraToYuvKernel<<<grid, block, 0, stream>>>(d_bgra, width, height, pitch, coeff, d_y, d_u, d_v);
    cudaEventRecord(e3, stream);
    cudaMemcpyAsync(y, d_y, y_bytes, cudaMemcpyDeviceToHost, stream);
    cudaMemcpyAsync(u, d_u, c_bytes, cudaMemcpyDeviceToHost, stream);
    cudaMemcpyAsync(v, d_v, c_bytes, cudaMemcpyDeviceToHost, stream);
    cudaEventRecord(e4, stream);
    const cudaError_t err = cudaEventSynchronize(e4);
    float h2d = 0, kern = 0, resident = 0, d2h = 0;
    cudaEventElapsedTime(&h2d, e0, e1);
    cudaEventElapsedTime(&kern, e1, e2);
    cudaEventElapsedTime(&resident, e2, e3);
    cudaEventElapsedTime(&d2h, e3, e4);
    if (timing) {
        timing->h2d_ms = h2d;
        timing->kernel_ms = kern;
        timing->resident_kernel_ms = resident;
        timing->d2h_ms = d2h;
        timing->mismatch_vs_reference = 0;
    }
    cudaEventDestroy(e0); cudaEventDestroy(e1); cudaEventDestroy(e2);
    cudaEventDestroy(e3); cudaEventDestroy(e4);
    cudaFree(d_bgra); cudaFree(d_y); cudaFree(d_u); cudaFree(d_v);
    cudaStreamDestroy(stream);
    return err == cudaSuccess ? 0 : -2;
}

namespace {

constexpr int kTransportFull = HYPERCINE_XPORT_FULL_INT16;
constexpr int kTransportCompact = HYPERCINE_XPORT_COMPACT_BAND;
constexpr int kTransportDevice = HYPERCINE_XPORT_DEVICE;

__global__ void InitMinMaxKernel(int* mm) {
    mm[0] = 32767;
    mm[1] = -32768;
    mm[2] = 0;
}

__global__ void BandRangeKernel(const int16_t* __restrict__ src, int n, int* mm) {
    __shared__ int smin[256];
    __shared__ int smax[256];
    int mn = 32767;
    int mx = -32768;
    const int stride = blockDim.x * gridDim.x;
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        const int value = src[i];
        mn = min(mn, value);
        mx = max(mx, value);
    }
    smin[threadIdx.x] = mn;
    smax[threadIdx.x] = mx;
    __syncthreads();
    for (int step = blockDim.x >> 1; step > 0; step >>= 1) {
        if (threadIdx.x < step) {
            smin[threadIdx.x] = min(smin[threadIdx.x], smin[threadIdx.x + step]);
            smax[threadIdx.x] = max(smax[threadIdx.x], smax[threadIdx.x + step]);
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        atomicMin(mm, smin[0]);
        atomicMax(mm + 1, smax[0]);
    }
}

__global__ void PackI8Kernel(const int16_t* __restrict__ src, int n, int8_t* __restrict__ dst, int* bad) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const int value = src[i];
    if (value < -128 || value > 127) {
        atomicExch(bad, 1);
        return;
    }
    dst[i] = static_cast<int8_t>(value);
}

__global__ void BswapLowpass16Kernel(uint16_t* __restrict__ data, int n) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const uint16_t v = data[i];
    data[i] = static_cast<uint16_t>((v >> 8) | (v << 8));
}

int BandSlot(int channel, int level, int band) {
    return (channel * kLevels + level) * kBands + band;
}

void QueueD2H(HyperCineGpuSlot* slot, int idx, const void* src, size_t bytes, cudaEvent_t wait_on,
              int elem, int vmin, int vmax, int fits) {
    BandBuf& band = slot->bands[static_cast<size_t>(idx)];
    auto& extra = slot->extra[static_cast<size_t>(idx)];
    if (wait_on) cudaStreamWaitEvent(slot->copy_stream, wait_on, 0);
    cudaEventRecord(extra.d2h_begin, slot->copy_stream);
    if (!slot->run_d2h_open) {
        cudaEventRecord(slot->ev_d2h0, slot->copy_stream);
        slot->run_d2h_open = true;
    }
    cudaMemcpyAsync(band.host, src, bytes, cudaMemcpyDeviceToHost, slot->copy_stream);
    cudaEventRecord(extra.d2h_end, slot->copy_stream);
    cudaEventRecord(slot->ev_d2h1, slot->copy_stream);
    extra.state = 2;
    extra.bytes = bytes;
    extra.elem = elem;
    extra.vmin = vmin;
    extra.vmax = vmax;
    extra.fits = fits;
    extra.copy_ms = 0.f;
    slot->run_d2h_bytes += bytes;
    if (elem == 1) ++slot->run_int8;
    else ++slot->run_int16;
}

int QueueLevel(HyperCineGpuSlot* slot, int channel, int level) {
    int width = 0;
    int height = 0;
    int16_t* src = nullptr;
    if (level == 0) {
        width = slot->run_widths[channel];
        height = slot->run_heights[channel];
        src = slot->source[channel];
    } else {
        BandBuf& low = slot->bands[static_cast<size_t>(BandSlot(channel, level - 1, 0))];
        width = low.width;
        height = low.height;
        src = low.device;
    }
    BandBuf* out = &slot->bands[static_cast<size_t>(BandSlot(channel, level, 0))];
    const int* quant = slot->run_quant + (channel * kLevels + level) * kBands;
    if (LaunchLevel(slot, src, width, height, slot->run_prescale[level], quant, out) != 0) return -3;
    for (int band = 0; band < kBands; ++band) {
        out[band].width = width / 2;
        out[band].height = height / 2;
    }
    const bool compact = slot->run_transport == kTransportCompact;
    if (compact) cudaEventRecord(slot->ev_range0, slot->stream);
    for (int band = 0; band < kBands; ++band) {
        const int idx = BandSlot(channel, level, band);
        if (!slot->bands[static_cast<size_t>(idx)].publish) continue;
        auto& extra = slot->extra[static_cast<size_t>(idx)];
        if (compact) {
            const int n = out[band].width * out[band].height;
            InitMinMaxKernel<<<1, 1, 0, slot->stream>>>(extra.d_minmax);
            const int threads = 256;
            int blocks = (n + threads - 1) / threads;
            if (blocks > 128) blocks = 128;
            if (blocks < 1) blocks = 1;
            BandRangeKernel<<<blocks, threads, 0, slot->stream>>>(out[band].device, n, extra.d_minmax);
            cudaMemcpyAsync(extra.h_minmax, extra.d_minmax, 3 * sizeof(int), cudaMemcpyDeviceToHost, slot->stream);
            extra.state = 1;
        }
    }
    if (compact) {
        cudaEventRecord(slot->ev_range1, slot->stream);
        slot->range_recorded = true;
    }
    if (slot->run_transport == HYPERCINE_XPORT_DEVICE_LOWPASS_D2H && level == kLevels - 1 && slot->gpu_pack_lowpass) {
        const int idx = BandSlot(channel, level, 0);
        BandBuf& item = slot->bands[static_cast<size_t>(idx)];
        const int n = item.width * item.height;
        const int threads = 256;
        const int blocks = (n + threads - 1) / threads;
        BswapLowpass16Kernel<<<blocks, threads, 0, slot->stream>>>(reinterpret_cast<uint16_t*>(item.device), n);
    }
    cudaEventRecord(slot->level_done[channel][level], slot->stream);
    if (slot->run_transport == HYPERCINE_XPORT_DEVICE_LOWPASS_D2H) {
        if (level == kLevels - 1) {
            const int idx = BandSlot(channel, level, 0);
            BandBuf& item = slot->bands[static_cast<size_t>(idx)];
            if (item.publish && item.host) {
                const size_t bytes = ElemCount(item.width, item.height) * sizeof(int16_t);
                QueueD2H(slot, idx, item.device, bytes, slot->level_done[channel][level], 2, 0, 0, 0);
            }
        }
    } else if (!compact && slot->run_transport != kTransportDevice) {
        for (int band = 0; band < kBands; ++band) {
            const int idx = BandSlot(channel, level, band);
            BandBuf& item = slot->bands[static_cast<size_t>(idx)];
            if (!item.publish) continue;
            const size_t bytes = ElemCount(item.width, item.height) * sizeof(int16_t);
            QueueD2H(slot, idx, item.device, bytes, slot->level_done[channel][level], 2, 0, 0, 0);
        }
    }
    return 0;
}

int QueueFrontOn(HyperCineGpuSlot* slot, cudaStream_t f_stream) {
    const uint8_t* device_bgra = nullptr;
    cudaEventRecord(slot->ev_h2d0, f_stream);
    if (slot->run_input == HYPERCINE_INPUT_DEVICE_BGRA) {
        device_bgra = slot->run_bgra;
        slot->run_h2d_bytes = 0;
    } else {
        const size_t bytes = static_cast<size_t>(slot->run_pitch) * static_cast<size_t>(slot->run_height);
        if (bytes > slot->bgra_capacity || !slot->d_bgra) return -2;
        cudaMemcpyAsync(slot->d_bgra, slot->run_bgra, bytes, cudaMemcpyHostToDevice, f_stream);
        device_bgra = slot->d_bgra;
        slot->run_h2d_bytes = bytes;
    }
    cudaEventRecord(slot->ev_h2d1, f_stream);
    cudaEventRecord(slot->ev_color0, f_stream);
    const RgbCoeff coeff = MakeCoeff(slot->run_color_space);
    dim3 block(32, 8);
    dim3 grid((slot->run_width + 31) / 32, (slot->run_height + 7) / 8);
    // Encoder channel 1 stores Cr and channel 2 stores Cb. The kernel's natural
    // order is Cb then Cr, so the chroma destinations are swapped here.
    BgraToYuvKernel<<<grid, block, 0, f_stream>>>(device_bgra, slot->run_width, slot->run_height,
                                                  slot->run_pitch, coeff, slot->source[0], slot->source[2],
                                                  slot->source[1]);
    cudaEventRecord(slot->ev_color1, f_stream);
    return 0;
}

int QueueFront(HyperCineGpuSlot* slot) {
    if (QueueFrontOn(slot, slot->stream) != 0) return -2;
    cudaEventRecord(slot->ev_comp0, slot->stream);
    return 0;
}

void IssueCompactLevel(HyperCineGpuSlot* slot, int channel, int level) {
    if (slot->pack_needs_elapsed) {
        float pack_ms = 0.f;
        cudaEventElapsedTime(&pack_ms, slot->ev_pack0, slot->ev_pack1);
        slot->run_pack_ms += pack_ms;
        slot->pack_needs_elapsed = false;
    }
    if (slot->range_recorded) {
        float range_ms = 0.f;
        cudaEventElapsedTime(&range_ms, slot->ev_range0, slot->ev_range1);
        slot->run_range_ms += range_ms;
        slot->range_recorded = false;
    }
    bool any_pack = false;
    for (int band = 0; band < kBands; ++band) {
        const int idx = BandSlot(channel, level, band);
        BandBuf& item = slot->bands[static_cast<size_t>(idx)];
        auto& extra = slot->extra[static_cast<size_t>(idx)];
        if (!item.publish || extra.state != 1) continue;
        const int n = item.width * item.height;
        const int vmin = extra.h_minmax[0];
        const int vmax = extra.h_minmax[1];
        const bool fits = vmin >= -128 && vmax <= 127;
        if (fits) {
            if (!any_pack) {
                cudaEventRecord(slot->ev_pack0, slot->stream);
                any_pack = true;
            }
            const int threads = 256;
            const int blocks = (n + threads - 1) / threads;
            PackI8Kernel<<<blocks, threads, 0, slot->stream>>>(item.device, n, extra.d_i8, extra.d_minmax + 2);
            cudaMemcpyAsync(extra.h_minmax + 2, extra.d_minmax + 2, sizeof(int), cudaMemcpyDeviceToHost, slot->stream);
            cudaEventRecord(extra.device_ready, slot->stream);
            QueueD2H(slot, idx, extra.d_i8, static_cast<size_t>(n), extra.device_ready, 1, vmin, vmax, 1);
        } else {
            QueueD2H(slot, idx, item.device, ElemCount(item.width, item.height) * sizeof(int16_t),
                     slot->level_done[channel][level], 2, vmin, vmax, 0);
        }
    }
    if (any_pack) {
        cudaEventRecord(slot->ev_pack1, slot->stream);
        slot->pack_needs_elapsed = true;
        cudaEventRecord(slot->ev_comp1, slot->stream);
    }
}

}  // namespace

extern "C" int HyperCine_GpuSlotBegin(HyperCineGpuSlot* slot, int input_kind, int transport,
                                      const uint8_t* host_or_device_bgra, int pitch, int width, int height,
                                      int color_space, const int widths[3], const int heights[3],
                                      const int prescale[3], const int quant[36]) {
    if (!slot || !host_or_device_bgra || !widths || !heights || !prescale || !quant) return -1;
    if (width < 16 || height < 16 || pitch < width * 4 || (width & 1)) return -2;
    slot->run_input = input_kind;
    slot->run_transport = transport;
    slot->run_phase = 0;
    slot->run_channel = 0;
    slot->run_level = 0;
    slot->run_error = 0;
    slot->run_width = width;
    slot->run_height = height;
    slot->run_pitch = pitch;
    slot->run_color_space = color_space;
    slot->run_bgra = host_or_device_bgra;
    slot->run_int8 = 0;
    slot->run_int16 = 0;
    slot->run_d2h_bytes = 0;
    slot->run_h2d_bytes = 0;
    slot->run_range_ms = 0.f;
    slot->run_pack_ms = 0.f;
    slot->run_d2h_open = false;
    slot->run_hold = transport == kTransportFull;
    slot->pack_needs_elapsed = false;
    slot->range_recorded = false;
    slot->gpu_pack_lowpass = 0;
    for (int i = 0; i < 3; ++i) {
        slot->run_widths[i] = widths[i];
        slot->run_heights[i] = heights[i];
        slot->run_prescale[i] = prescale[i];
    }
    for (int i = 0; i < 36; ++i) slot->run_quant[i] = quant[i];
    for (auto& extra : slot->extra) {
        extra.state = 0;
        extra.elem = 2;
        extra.bytes = 0;
        extra.fits = 0;
        extra.vmin = 0;
        extra.vmax = 0;
        extra.copy_ms = 0.f;
    }
    return 0;
}

extern "C" int HyperCine_GpuSlotKickStaged(HyperCineGpuSlot* slot, void* wait_frontend_event, void* wait_wavelet_event, int gpu_pack_lowpass) {
    if (!slot) return -1;
    if (slot->run_phase == 3) return -1;
    if (slot->run_phase == 2) return 0;
    slot->gpu_pack_lowpass = gpu_pack_lowpass;
    cudaStream_t f_stream = slot->frontend_stream ? slot->frontend_stream : slot->stream;
    if (wait_frontend_event) {
        cudaStreamWaitEvent(f_stream, static_cast<cudaEvent_t>(wait_frontend_event), 0);
    }
    if (QueueFrontOn(slot, f_stream) != 0) {
        slot->run_phase = 3;
        slot->run_error = -2;
        return -1;
    }
    if (f_stream != slot->stream) {
        cudaStreamWaitEvent(slot->stream, slot->ev_color1, 0);
    }
    if (wait_wavelet_event) {
        cudaStreamWaitEvent(slot->stream, static_cast<cudaEvent_t>(wait_wavelet_event), 0);
    }
    cudaEventRecord(slot->ev_comp0, slot->stream);
    for (int channel = 0; channel < kChannels; ++channel) {
        for (int level = 0; level < kLevels; ++level) {
            if (QueueLevel(slot, channel, level) != 0) {
                slot->run_phase = 3;
                slot->run_error = -3;
                return -1;
            }
        }
    }
    cudaEventRecord(slot->ev_comp1, slot->stream);
    if (slot->run_d2h_open) cudaEventRecord(slot->ev_all_host, slot->copy_stream);
    slot->run_phase = 2;
    if (cudaGetLastError() != cudaSuccess) {
        slot->run_phase = 3;
        slot->run_error = -4;
        return -1;
    }
    return 0;
}

extern "C" int HyperCine_GpuSlotKickFrontendOnly(HyperCineGpuSlot* slot, void* wait_frontend_event) {
    if (!slot || slot->run_phase == 3) return -1;
    if (slot->run_phase >= 1) return 0;
    cudaStream_t f_stream = slot->frontend_stream ? slot->frontend_stream : slot->stream;
    if (wait_frontend_event) {
        cudaStreamWaitEvent(f_stream, static_cast<cudaEvent_t>(wait_frontend_event), 0);
    }
    if (QueueFrontOn(slot, f_stream) != 0) {
        slot->run_phase = 3;
        slot->run_error = -2;
        return -1;
    }
    slot->run_phase = 1;
    if (cudaGetLastError() != cudaSuccess) {
        slot->run_phase = 3;
        slot->run_error = -4;
        return -1;
    }
    return 0;
}

extern "C" int HyperCine_GpuSlotKickWaveletWithQuant(HyperCineGpuSlot* slot, const int quant[36],
                                                     void* wait_wavelet_event, int gpu_pack_lowpass) {
    if (!slot || !quant || slot->run_phase == 3) return -1;
    if (slot->run_phase == 2) return 0;
    if (slot->run_phase == 0) {
        if (HyperCine_GpuSlotKickFrontendOnly(slot, nullptr) != 0) return -1;
    }
    for (int i = 0; i < 36; ++i) slot->run_quant[i] = quant[i];
    slot->gpu_pack_lowpass = gpu_pack_lowpass;
    cudaStream_t f_stream = slot->frontend_stream ? slot->frontend_stream : slot->stream;
    if (f_stream != slot->stream) {
        cudaStreamWaitEvent(slot->stream, slot->ev_color1, 0);
    }
    if (wait_wavelet_event) {
        cudaStreamWaitEvent(slot->stream, static_cast<cudaEvent_t>(wait_wavelet_event), 0);
    }
    cudaEventRecord(slot->ev_comp0, slot->stream);
    for (int channel = 0; channel < kChannels; ++channel) {
        for (int level = 0; level < kLevels; ++level) {
            if (QueueLevel(slot, channel, level) != 0) {
                slot->run_phase = 3;
                slot->run_error = -3;
                return -1;
            }
        }
    }
    cudaEventRecord(slot->ev_comp1, slot->stream);
    if (slot->run_d2h_open) cudaEventRecord(slot->ev_all_host, slot->copy_stream);
    slot->run_phase = 2;
    if (cudaGetLastError() != cudaSuccess) {
        slot->run_phase = 3;
        slot->run_error = -4;
        return -1;
    }
    return 0;
}

extern "C" int HyperCine_GpuSlotKick(HyperCineGpuSlot* slot) {
    if (!slot) return -1;
    if (slot->run_phase == 3) return -1;
    if (slot->run_phase == 2) return 0;
    if (slot->run_phase == 0) {
        if (QueueFront(slot) != 0) {
            slot->run_phase = 3;
            slot->run_error = -2;
            return -1;
        }
        if (slot->run_transport == kTransportCompact) {
            if (QueueLevel(slot, 0, 0) != 0) {
                slot->run_phase = 3;
                slot->run_error = -3;
                return -1;
            }
            slot->run_channel = 0;
            slot->run_level = 0;
            slot->run_phase = 1;
            return 0;
        }
        for (int channel = 0; channel < kChannels; ++channel) {
            for (int level = 0; level < kLevels; ++level) {
                if (QueueLevel(slot, channel, level) != 0) {
                    slot->run_phase = 3;
                    slot->run_error = -3;
                    return -1;
                }
            }
        }
        cudaEventRecord(slot->ev_comp1, slot->stream);
        if (slot->run_d2h_open) cudaEventRecord(slot->ev_all_host, slot->copy_stream);
        slot->run_phase = 2;
        if (cudaGetLastError() != cudaSuccess) {
            slot->run_phase = 3;
            slot->run_error = -4;
            return -1;
        }
        return 0;
    }

    const cudaError_t query = cudaEventQuery(slot->level_done[slot->run_channel][slot->run_level]);
    if (query == cudaErrorNotReady) return 0;
    if (query != cudaSuccess) {
        slot->run_phase = 3;
        slot->run_error = -5;
        return -1;
    }
    IssueCompactLevel(slot, slot->run_channel, slot->run_level);
    int channel = slot->run_channel;
    int level = slot->run_level + 1;
    if (level >= kLevels) {
        level = 0;
        ++channel;
    }
    if (channel >= kChannels) {
        cudaEventRecord(slot->ev_comp1, slot->stream);
        if (slot->run_d2h_open) cudaEventRecord(slot->ev_all_host, slot->copy_stream);
        slot->run_phase = 2;
        return slot->run_error == 0 ? 0 : -1;
    }
    if (QueueLevel(slot, channel, level) != 0) {
        slot->run_phase = 3;
        slot->run_error = -3;
        return -1;
    }
    slot->run_channel = channel;
    slot->run_level = level;
    if (cudaGetLastError() != cudaSuccess) {
        slot->run_phase = 3;
        slot->run_error = -4;
        return -1;
    }
    return 0;
}

extern "C" int HyperCine_GpuSlotPollBand(HyperCineGpuSlot* slot, HyperCineReadyBand* band) {
    if (!slot || !band) return -1;
    if (slot->run_phase == 3) return -1;
    if (slot->run_hold) {
        if (slot->run_phase != 2) return 0;
        const cudaError_t query = cudaEventQuery(slot->ev_all_host);
        if (query == cudaErrorNotReady) return 0;
        if (query != cudaSuccess) {
            slot->run_error = -6;
            slot->run_phase = 3;
            return -1;
        }
    }
    for (size_t i = 0; i < slot->bands.size(); ++i) {
        BandBuf& item = slot->bands[i];
        auto& extra = slot->extra[i];
        if (!item.publish || extra.state != 2) continue;
        if (!slot->run_hold) {
            const cudaError_t query = cudaEventQuery(extra.d2h_end);
            if (query == cudaErrorNotReady) continue;
            if (query != cudaSuccess) {
                slot->run_error = -6;
                slot->run_phase = 3;
                return -1;
            }
        }
        if (extra.fits && extra.h_minmax && extra.h_minmax[2] != 0) {
            slot->run_error = -7;
            slot->run_phase = 3;
            return -1;
        }
        cudaEventElapsedTime(&extra.copy_ms, extra.d2h_begin, extra.d2h_end);
        extra.state = 4;
        band->channel = item.channel;
        band->level = item.level;
        band->band = item.band;
        band->width = item.width;
        band->height = item.height;
        band->elem_bytes = extra.elem;
        band->value_min = extra.vmin;
        band->value_max = extra.vmax;
        band->fits_int8 = extra.fits;
        band->data = item.host;
        band->bytes = extra.bytes;
        band->copy_ms = extra.copy_ms;
        return 1;
    }
    return 0;
}

extern "C" int HyperCine_GpuSlotWaveletIdle(HyperCineGpuSlot* slot) {
    if (!slot) return -1;
    if (slot->run_phase == 3) return -1;
    if (slot->run_phase != 2) return 0;
    const cudaError_t query = cudaEventQuery(slot->ev_comp1);
    if (query == cudaErrorNotReady) return 0;
    return query == cudaSuccess ? 1 : -1;
}

extern "C" int HyperCine_GpuSlotFrontendIdle(HyperCineGpuSlot* slot) {
    if (!slot) return -1;
    if (slot->run_phase == 3) return -1;
    if (slot->run_phase == 0) return 0;
    const cudaError_t query = cudaEventQuery(slot->ev_color1);
    if (query == cudaErrorNotReady) return 0;
    return query == cudaSuccess ? 1 : -1;
}

extern "C" void* HyperCine_GpuSlotLevelEvent(const HyperCineGpuSlot* slot, int channel, int level) {
    if (!slot || channel < 0 || channel >= kChannels || level < 0 || level >= kLevels) return nullptr;
    return slot->level_done[channel][level];
}

extern "C" void* HyperCine_GpuSlotWaveletEvent(const HyperCineGpuSlot* slot) {
    return slot ? slot->ev_comp1 : nullptr;
}

extern "C" void* HyperCine_GpuSlotFrontendEvent(const HyperCineGpuSlot* slot) {
    return slot ? slot->ev_color1 : nullptr;
}

extern "C" void* HyperCine_GpuSlotCopyStream(const HyperCineGpuSlot* slot) {
    return slot ? slot->copy_stream : nullptr;
}

extern "C" const int16_t* HyperCine_GpuSlotBandDevicePtr(const HyperCineGpuSlot* slot, int channel, int level, int band) {
    if (!slot || channel < 0 || channel >= kChannels || level < 0 || level >= kLevels || band < 0 || band >= kBands) return nullptr;
    return slot->bands[static_cast<size_t>(BandSlot(channel, level, band))].device;
}

extern "C" const int16_t* HyperCine_GpuSlotBandHostPtr(const HyperCineGpuSlot* slot, int channel, int level, int band) {
    if (!slot || channel < 0 || channel >= kChannels || level < 0 || level >= kLevels || band < 0 || band >= kBands) return nullptr;
    return slot->bands[static_cast<size_t>(BandSlot(channel, level, band))].host;
}

extern "C" int HyperCine_GpuSlotGpuIdle(HyperCineGpuSlot* slot) {
    if (!slot) return -1;
    if (slot->run_phase == 3) return -1;
    if (slot->run_phase != 2) return 0;
    if (slot->run_transport == HYPERCINE_XPORT_DEVICE) {
        const cudaError_t query = cudaEventQuery(slot->ev_comp1);
        if (query == cudaErrorNotReady) return 0;
        return query == cudaSuccess ? 1 : -1;
    }
    if (slot->run_hold) {
        const cudaError_t query = cudaEventQuery(slot->ev_all_host);
        if (query == cudaErrorNotReady) return 0;
        return query == cudaSuccess ? 1 : -1;
    }
    for (size_t i = 0; i < slot->bands.size(); ++i) {
        if (!slot->bands[i].publish) continue;
        const int state = slot->extra[i].state;
        if (state == 1) return 0;
        if (state == 2) {
            const cudaError_t query = cudaEventQuery(slot->extra[i].d2h_end);
            if (query != cudaSuccess) return 0;
        }
    }
    return 1;
}

extern "C" int HyperCine_GpuSlotReadTiming(HyperCineGpuSlot* slot, HyperCineStreamTiming* timing) {
    if (!slot || !timing) return -1;
    if (slot->pack_needs_elapsed) {
        cudaEventSynchronize(slot->ev_pack1);
        float pack_ms = 0.f;
        cudaEventElapsedTime(&pack_ms, slot->ev_pack0, slot->ev_pack1);
        slot->run_pack_ms += pack_ms;
        slot->pack_needs_elapsed = false;
    }
    if (slot->run_d2h_open) cudaEventSynchronize(slot->ev_d2h1);
    cudaEventSynchronize(slot->ev_comp1);
    float h2d = 0.f, color = 0.f, compute = 0.f, d2h = 0.f;
    cudaEventElapsedTime(&h2d, slot->ev_h2d0, slot->ev_h2d1);
    cudaEventElapsedTime(&color, slot->ev_color0, slot->ev_color1);
    cudaEventElapsedTime(&compute, slot->ev_comp0, slot->ev_comp1);
    if (slot->run_d2h_open) cudaEventElapsedTime(&d2h, slot->ev_d2h0, slot->ev_d2h1);
    float overlap = 0.f;
    if (slot->run_d2h_open) {
        float d0 = 0.f, d1 = 0.f;
        cudaEventElapsedTime(&d0, slot->ev_comp0, slot->ev_d2h0);
        cudaEventElapsedTime(&d1, slot->ev_comp0, slot->ev_d2h1);
        const float start = d0 > 0.f ? d0 : 0.f;
        const float end = d1 < compute ? d1 : compute;
        if (end > start) overlap = end - start;
    }
    std::memset(timing, 0, sizeof(*timing));
    timing->h2d_ms = h2d;
    timing->color_ms = color;
    timing->compute_ms = compute;
    timing->range_ms = slot->run_range_ms;
    timing->pack_ms = slot->run_pack_ms;
    timing->d2h_ms = d2h;
    timing->overlap_compute_d2h_ms = overlap;
    timing->h2d_bytes = slot->run_h2d_bytes;
    timing->d2h_bytes = slot->run_d2h_bytes;
    timing->int8_bands = slot->run_int8;
    timing->int16_bands = slot->run_int16;
    timing->band_count = slot->run_int8 + slot->run_int16;
    timing->error = slot->run_error;
    return slot->run_error == 0 ? 0 : -1;
}

extern "C" int HyperCine_GpuSlotDeviceBandCount(const HyperCineGpuSlot* slot) {
    if (!slot) return 0;
    int count = 0;
    for (const BandBuf& band : slot->bands) if (band.publish) ++count;
    return count;
}

extern "C" int HyperCine_GpuSlotDeviceBand(const HyperCineGpuSlot* slot, int index, HyperCineDeviceBand* band) {
    if (!slot || !band || index < 0) return -1;
    int seen = 0;
    for (const BandBuf& item : slot->bands) {
        if (!item.publish) continue;
        if (seen == index) {
            band->channel = item.channel;
            band->level = item.level;
            band->band = item.band;
            band->width = item.width;
            band->height = item.height;
            band->device = item.device;
            return 0;
        }
        ++seen;
    }
    return -4;
}

extern "C" int HyperCine_GpuSlotUploadBgra(HyperCineGpuSlot* slot, const uint8_t* host, int pitch,
                                           int width, int height, const void** device_out) {
    if (!slot || !host || !device_out) return -1;
    const size_t bytes = static_cast<size_t>(pitch) * static_cast<size_t>(height);
    if (width > slot->max_width || height > slot->max_height || bytes > slot->bgra_capacity) return -2;
    if (cudaMemcpyAsync(slot->d_bgra, host, bytes, cudaMemcpyHostToDevice, slot->stream) != cudaSuccess) return -3;
    if (cudaStreamSynchronize(slot->stream) != cudaSuccess) return -3;
    *device_out = slot->d_bgra;
    return 0;
}

extern "C" int HyperCine_RegisterHost(void* ptr, size_t bytes) {
    if (!ptr || bytes == 0) return -1;
    const cudaError_t err = cudaHostRegister(ptr, bytes, cudaHostRegisterDefault);
    if (err == cudaSuccess || err == cudaErrorHostMemoryAlreadyRegistered) return 0;
    return -2;
}

extern "C" int HyperCine_UnregisterHost(void* ptr) {
    if (!ptr) return -1;
    const cudaError_t err = cudaHostUnregister(ptr);
    if (err == cudaSuccess || err == cudaErrorHostMemoryNotRegistered) return 0;
    return -2;
}

extern "C" int HyperCine_GpuSlotDownloadInt16(HyperCineGpuSlot* slot, int index, int16_t* dst, int capacity,
                                              int* width, int* height, int* channel, int* level, int* band) {
    if (!slot || !dst || index < 0) return -1;
    cudaStreamSynchronize(slot->stream);
    int seen = 0;
    for (const BandBuf& item : slot->bands) {
        if (!item.publish) continue;
        if (seen == index) {
            const int n = item.width * item.height;
            if (n > capacity) return -2;
            if (cudaMemcpy(dst, item.device, static_cast<size_t>(n) * sizeof(int16_t), cudaMemcpyDeviceToHost) != cudaSuccess) {
                return -3;
            }
            if (width) *width = item.width;
            if (height) *height = item.height;
            if (channel) *channel = item.channel;
            if (level) *level = item.level;
            if (band) *band = item.band;
            return 0;
        }
        ++seen;
    }
    return -4;
}
