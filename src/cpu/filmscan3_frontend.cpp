// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "../core/filmscan3_backend.h"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <algorithm>
#include <stdexcept>
#include <string>
namespace hypercine { namespace standalone {
// The current three-level YUV422 wavelet shape and observed 16-bit dimension
// tags are independent of this implementation's bounded session allocation.
// This is NOT a proof of an intrinsic upper limit of every CineForm family.
void ValidateFilmScan3Geometry(FilmScan3Geometry g) {
    if (g.width < 128 || g.width % 128 || g.height < 24 || g.height % 8 ||
        g.width > kMaxWireDimension || g.height > kMaxWireDimension)
        throw std::invalid_argument(
            "Current FilmScan3 YUV422 pipeline requires width %128 in [128,65535] and height %8 in [24,65535]");
    // Budget: checked after legality so the two failure modes stay distinguishable.
    if (static_cast<std::size_t>(g.width) >
            kMaxPlaneElements / static_cast<std::size_t>(g.height))
        throw std::length_error(
            "FilmScan3 wavelet-aligned geometry exceeds this build's pixel budget: "
            + std::to_string(g.width) + "x" + std::to_string(g.height)
            + " = " + std::to_string(static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height))
            + " px > " + std::to_string(kMaxPlaneElements) + " px");
    const auto pixels=static_cast<std::size_t>(g.width)*g.height;
    if (g.sample_capacity_bytes > 8*pixels+16384)
        throw std::length_error("FilmScan3 sample capacity exceeds geometric bound");
}
// Frozen CG709 conversion semantics from hybrid_kernels.cu, kept SDK-free.
// Explicit signed floors and uint16 wrapping reproduce the documented SIMD
// arithmetic without platform intrinsics or implementation-defined shifts.
int FloorDivide(int v, int divisor) { return v >= 0 ? v/divisor : -((-v+divisor-1)/divisor); }
int Sat(int v) { return std::clamp(v,-32768,32767); }
int ReferenceChannel(int r, int g, int b, int cr, int cg, int cb, int offset) {
    int v=FloorDivide((r<<7)*cr,65536);
    v=Sat(v+FloorDivide((g<<7)*cg,65536));
    v=Sat(v+FloorDivide((b<<7)*cb,65536));
    const auto wrapped=static_cast<uint16_t>(static_cast<uint16_t>(v)*4u);
    v=wrapped<=32767 ? wrapped : static_cast<int>(wrapped)-65536;
    v=Sat(v+offset); v=Sat(v+16383);
    const auto u=static_cast<uint16_t>(v);
    const unsigned nonnegative=u<16383u ? 0u : static_cast<unsigned>(u)-16383u;
    return static_cast<uint16_t>(nonnegative*4u)>>6;
}
Planes ConvertFilmScan3Bgra(const uint8_t* input, std::size_t bytes, int width, int height, std::size_t stride) {
    detail::ValidateFilmScan3Input(input,bytes,width,height,stride);
    Planes out{Plane(width,height),Plane(width/2,height),Plane(width/2,height)};
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        const auto* p=input+static_cast<std::size_t>(height-1-y)*stride+static_cast<std::size_t>(x)*4;
        const int b=p[0],g=p[1],r=p[2];
        out[0].pixels[y*out[0].stride+x]=static_cast<int16_t>(ReferenceChannel(r,g,b,1499,5029,507,1024));
        if (!(x&1)) {
            out[1].pixels[y*out[1].stride+x/2]=static_cast<int16_t>(ReferenceChannel(r,g,b,3596,-3268,-327,8192));
            out[2].pixels[y*out[2].stride+x/2]=static_cast<int16_t>(ReferenceChannel(r,g,b,-827,-2768,3596,8192));
        }
    }
    return out;
}
// NEON quantisation (issue #70, performance follow-up).
//
// The scalar rule is
//   result = ((|v| + q/2-1) * (65536/q)) >> 16, sign reapplied
// and it is per-coefficient independent, so it vectorises without changing a
// single value.
//
// Two facts checked before writing any vector code:
//   * the largest possible product is 2^30 (q=2, |v|=32768), so 32-bit lanes
//     cannot overflow;
//   * the largest possible result is 16384, so the saturating narrow never
//     actually saturates. It is used anyway because it is the exact equivalent.
//
// q == 1 is the identity and is short-circuited: with q == 1 the scalar
// midpoint `q/2-1` would underflow unsigned, which is why the reference
// returns early there too.
void QuantizeFilmScan3Block(int16_t* data, std::size_t count, unsigned q) {
    if (!data || !count) return;
    if (q < 1 || q > 32768) throw std::invalid_argument("invalid FilmScan3 divisor");
    if (q == 1) return;
    const int scale = static_cast<int>(65536u / q);
    const int midpoint = static_cast<int>(q / 2 - 1);

#if defined(__ARM_NEON)
    std::size_t i = 0;
    const int32x4_t vmid = vdupq_n_s32(midpoint);
    const int32x4_t vscale = vdupq_n_s32(scale);
    for (; i + 8 <= count; i += 8) {
        const int16x8_t in = vld1q_s16(data + i);
        const int32x4_t lo = vmovl_s16(vget_low_s16(in));
        const int32x4_t hi = vmovl_s16(vget_high_s16(in));
        // |v| works on the widened lanes, so the -32768 case cannot overflow.
        const int32x4_t mag_lo = vabsq_s32(lo);
        const int32x4_t mag_hi = vabsq_s32(hi);
        const int32x4_t res_lo = vshrq_n_s32(
            vmulq_s32(vaddq_s32(mag_lo, vmid), vscale), 16);
        const int32x4_t res_hi = vshrq_n_s32(
            vmulq_s32(vaddq_s32(mag_hi, vmid), vscale), 16);
        // Reapply the sign. vcltq gives an all-ones lane where v < 0.
        const int32x4_t sign_lo = vcltq_s32(lo, vdupq_n_s32(0));
        const int32x4_t sign_hi = vcltq_s32(hi, vdupq_n_s32(0));
        const int32x4_t out_lo = vsubq_s32(veorq_s32(res_lo, sign_lo), sign_lo);
        const int32x4_t out_hi = vsubq_s32(veorq_s32(res_hi, sign_hi), sign_hi);
        vst1q_s16(data + i, vqmovn_high_s32(vqmovn_s32(out_lo), out_hi));
    }
    for (; i < count; ++i) data[i] = QuantizeFilmScan3(data[i], q);
    return;
#else
    for (std::size_t i = 0; i < count; ++i) data[i] = QuantizeFilmScan3(data[i], q);
#endif
}
int16_t QuantizeFilmScan3(int16_t v, unsigned q) {
    if(q<1 || q>32768) throw std::invalid_argument("invalid FilmScan3 divisor");
    if(q==1) return v;
    const unsigned magnitude=v<0 ? -static_cast<int>(v):v;
    const unsigned midpoint=q/2-1; // default pre-emphasis=0 -> midpoint divisor 2
    const int result=static_cast<int>(((magnitude+midpoint)*(65536u/q))/65536u);
    return static_cast<int16_t>(Sat(v<0 ? -result:result));
}
}}