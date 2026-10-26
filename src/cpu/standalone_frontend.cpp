// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "hypercine/wavelet.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hypercine { namespace standalone {
namespace {
void Validate(const Plane& p) {
    if (p.width < 1 || p.height < 1 || p.width > kMaxDimension || p.height > kMaxDimension || p.stride < p.width || p.stride % 8 ||
        !PlaneStorageFits(p.stride,p.height) ||
        p.pixels.size() < static_cast<std::size_t>(p.stride) * p.height)
        throw std::invalid_argument("invalid plane storage");
}
// Explicit floor division: negative right shifts are implementation-defined in C++17.
int RoundChroma(int v) { v += 16384; return v >= 0 ? v / 32768 : -((-v + 32767) / 32768); }
}
// Legacy Fixed-Q2 preserves its pre-existing <=4096 geometry error contract.
// New FilmScan3 geometry/budget diagnostics do not silently change this API.
void ValidateGeometry(int w, int h) {
    // The legacy Fixed-Q2 API keeps its historically observable 4096
    // dimension rejection (invalid_argument), independent of the new
    // FilmScan3 budget/shape classification. Do not silently change callers.
    if (w < 128 || w > kMaxDimension || w % 128)
        throw std::invalid_argument("frame width must be a multiple of 128 in [128,4096]");
    if (h < 24 || h > kMaxDimension || h % 8)
        throw std::invalid_argument("frame height must be a multiple of 8 in [24,4096]");
    if (static_cast<std::size_t>(w) * h > kMaxPixels)
        throw std::length_error("frame is format-legal but exceeds the 2097152-pixel allocation budget");
}
Plane::Plane(int w, int h) : width(w), height(h), stride(0) {
    if (w < 1 || h < 1 || w > kMaxDimension || h > kMaxDimension) throw std::invalid_argument("plane dimensions out of bounds");
    stride = (w + 7) & ~7;
    if (!PlaneStorageFits(stride,h))
        throw std::length_error("plane exceeds allocation budget");
    pixels.resize(static_cast<std::size_t>(stride) * h);
}
Planes ConvertBgra(const uint8_t* data, std::size_t bytes, int width, int height, std::size_t stride) {
    ValidateGeometry(width,height);
    const auto row = static_cast<std::size_t>(width)*4;
    if (!data || reinterpret_cast<std::uintptr_t>(data) % 16 || stride < row || stride % 16 ||
        stride > (std::numeric_limits<std::size_t>::max() - row) / (height-1) || bytes < stride * (height-1) + row)
        throw std::invalid_argument("BGRA requires aligned pointer, padded stride and complete storage");
    Planes out{Plane(width,height), Plane(width/2,height), Plane(width/2,height)};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; x += 2) {
            int cr = 0, cb = 0;
            for (int k = 0; k < 2; ++k) {
                const auto* p = data + stride * y + 4 * (x + k);
                const int r = p[2], g = p[1], b = p[0];
                // BT.709 computer RGB -> studio-range 10-bit Y'CbCr.
                // 16-bit fixed-point coefficients, one final round per component.
                out[0].pixels[y*out[0].stride+x+k] = static_cast<int16_t>(64 +
                    (11966*r + 40254*g + 4064*b + 8192) / 16384);
                cb += -6596*r - 22189*g + 28784*b;
                cr += 28784*r - 26145*g - 2639*b;
            }
            // Horizontal box average; chroma is centered on each pixel pair.
            out[1].pixels[y*out[1].stride+x/2] = static_cast<int16_t>(512 + RoundChroma(cr));
            out[2].pixels[y*out[2].stride+x/2] = static_cast<int16_t>(512 + RoundChroma(cb));
        }
    }
    return out;
}
Planes ConvertBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride) {
    return ConvertBgra(data,bytes,128,128,stride);
}
WaveletLevel ForwardLevel(const Plane& in, int prescale) {
    Validate(in);
    if (in.width < 6 || in.height < 6 || in.width % 2 || in.height % 2 || (prescale != 0 && prescale != 2))
        throw std::invalid_argument("invalid spatial dimensions or prescale");
    WaveletLevel out{{Plane(in.width/2,in.height/2), Plane(in.width/2,in.height/2),