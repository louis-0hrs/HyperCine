// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "../core/filmscan3_backend.h"
#include <algorithm>
#include <stdexcept>
#include <string>
namespace hypercine { namespace standalone {
// Format legality is deliberately separate from the resource budget. A legal
// geometry that this build cannot afford must fail as a budget error, not as an
// invalid format -- otherwise 4096x2160 and 8K look like format bugs when they are
// only allocation limits.
//
// Illegal: shape does not match the 3-level YUV422 wavelet, or exceeds the 16-bit
// wire field. Legal but unaffordable: passes the shape, exceeds the session budget.
void ValidateFilmScan3Geometry(FilmScan3Geometry g) {
    if (g.width < 128 || g.width % 128 || g.height < 24 || g.height % 8 ||
        g.width > kMaxWireDimension || g.height > kMaxWireDimension)
        throw std::invalid_argument(
            "FilmScan3 format requires width %128 in [128,65535] and height %8 in [24,65535]");
    // Budget: checked after legality so the two failure modes stay distinguishable.
    if (static_cast<std::size_t>(g.width) >
            kMaxPlaneElements / static_cast<std::size_t>(g.height))
        throw std::length_error(
            "FilmScan3 geometry is legal but exceeds this build's pixel budget: "
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
int16_t QuantizeFilmScan3(int16_t v, unsigned q) {
    if(q<1 || q>32768) throw std::invalid_argument("invalid FilmScan3 divisor");
    if(q==1) return v;
    const unsigned magnitude=v<0 ? -static_cast<int>(v):v;
    const unsigned midpoint=q/2-1; // default pre-emphasis=0 -> midpoint divisor 2
    const int result=static_cast<int>(((magnitude+midpoint)*(65536u/q))/65536u);
    return static_cast<int16_t>(Sat(v<0 ? -result:result));
}
}}
