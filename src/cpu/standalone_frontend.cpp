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
    if (p.width < 1 || p.height < 1 || p.stride < p.width || p.stride % 8 ||
        p.pixels.size() < static_cast<std::size_t>(p.stride) * p.height)
        throw std::invalid_argument("invalid plane storage");
}
// Explicit floor division: negative right shifts are implementation-defined in C++17.
int RoundChroma(int v) { v += 16384; return v >= 0 ? v / 32768 : -((-v + 32767) / 32768); }
}
Plane::Plane(int w, int h) : width(w), height(h), stride(0) {
    if (w < 1 || h < 1 || w > 128 || h > 128) throw std::invalid_argument("plane exceeds fixed profile");
    stride = (w + 7) & ~7;
    pixels.resize(static_cast<std::size_t>(stride) * h);
}
Planes ConvertBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride) {
    if (!data || reinterpret_cast<std::uintptr_t>(data) % 16 || stride < 512 || stride % 16 ||
        stride > (std::numeric_limits<std::size_t>::max() - 512) / 127 || bytes < stride * 127 + 512)
        throw std::invalid_argument("BGRA128 requires aligned pointer, padded stride and complete storage");
    Planes out{Plane(128,128), Plane(64,128), Plane(64,128)};
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; x += 2) {
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
WaveletLevel ForwardLevel(const Plane& in, int prescale) {
    Validate(in);
    if (in.width < 6 || in.height < 6 || in.width % 2 || in.height % 2 || (prescale != 0 && prescale != 2))
        throw std::invalid_argument("invalid spatial dimensions or prescale");
    WaveletLevel out{{Plane(in.width/2,in.height/2), Plane(in.width/2,in.height/2),
                      Plane(in.width/2,in.height/2), Plane(in.width/2,in.height/2)}};
    Plane low(in.width/2,in.height), high(in.width/2,in.height);
    if (prescale == 0) {
        if (HyperCine_ForwardHorizontal26_CPU(in.pixels.data(),in.width,in.height,in.stride,
                low.pixels.data(),low.stride,high.pixels.data(),high.stride) != HYPERCINE_STATUS_SUCCESS)
            throw std::runtime_error("horizontal transform failed");
    } else {
        // Reuse the existing scalar filter for individually rounded highpass taps.
        Plane scaled(in.width,in.height);
        for (int y=0;y<in.height;++y) for (int x=0;x<in.width;++x) {
            int v = in.pixels[y*in.stride+x]+3;
            scaled.pixels[y*scaled.stride+x] = static_cast<int16_t>(v>=0 ? v/4 : -((-v+3)/4));
        }
        HyperCine_ForwardHorizontal26_CPU(scaled.pixels.data(),in.width,in.height,scaled.stride,
                                        low.pixels.data(),low.stride,high.pixels.data(),high.stride);
        // Lowpass rounding is applied to the pair sum, not each individual tap.
        for (int y=0;y<in.height;++y) for (int x=0;x<in.width/2;++x) {
            const int v = in.pixels[y*in.stride+2*x]+in.pixels[y*in.stride+2*x+1]+3;
            low.pixels[y*low.stride+x] = static_cast<int16_t>(v>=0 ? v/4 : -((-v+3)/4));
        }
    }
    auto& b=out.bands;
    HyperCine_ForwardVertical26_CPU(low.pixels.data(),low.width,low.height,low.stride,
                                  b[0].pixels.data(),b[0].stride,b[2].pixels.data(),b[2].stride);
    HyperCine_ForwardVertical26_CPU(high.pixels.data(),high.width,high.height,high.stride,
                                  b[1].pixels.data(),b[1].stride,b[3].pixels.data(),b[3].stride);
    return out;
}
int16_t Quantize(int16_t value, unsigned divisor) {
    if (divisor < 1 || divisor > 32768) throw std::invalid_argument("invalid quantization divisor");
    if (divisor == 1) return value;
    const int v = value;
    const unsigned magnitude = v < 0 ? -v : v;
    const int q = static_cast<int>((magnitude * (65536u / divisor)) / 65536u);
    return static_cast<int16_t>(v < 0 ? -q : q);
}
}}
