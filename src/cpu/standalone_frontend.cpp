// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "hypercine/wavelet.h"
#include <algorithm>
#include <cstdint>
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
void ValidateGeometry(int w, int h) {
    if (w < 128 || w > kMaxDimension || w % 128)
        throw std::invalid_argument("frame width must be a multiple of 128 in [128,4096]");
    if (h < 24 || h > kMaxDimension || h % 8)
        throw std::invalid_argument("frame height must be a multiple of 8 in [24,4096]");
    if (static_cast<std::size_t>(w) * h > kMaxPixels)
        throw std::length_error("frame exceeds 2097152-pixel allocation budget");
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
// NEON dispatch (issue #70).
//
// The scalar kernels remain the reference. This only chooses which
// implementation runs; both were proven coefficient-identical before either was
// wired in. A missing NEON build or a disabled switch falls through to scalar
// rather than failing, so a host without the vector path keeps working
// unchanged -- and `HyperCine_IsNEONWaveletEnabled()` tells a caller which one
// actually ran, so a benchmark cannot mislabel the path.
namespace detail {
// Pure dispatch policy (issue #70).
//
// Splitting the decision out of the kernels keeps one rule in one place: a caller
// that asked for a concrete vector path must never be handed a scalar result while
// still being told the vector path ran, and `Auto` may degrade because that is what
// "follow the process-global default" has always meant.
WaveletDispatchDecision DecideWaveletDispatch(WaveletIsa resolved, bool allow_fallback,
                                             bool vector_succeeded) {
    if (resolved != WaveletIsa::Neon) return WaveletDispatchDecision::Scalar;
    if (vector_succeeded) return WaveletDispatchDecision::Vector;
    return allow_fallback ? WaveletDispatchDecision::Scalar
                          : WaveletDispatchDecision::FailClosed;
}
} // namespace detail

namespace {
inline bool UseNeon() {
    return HyperCine_IsNEONWaveletAvailable() && HyperCine_IsNEONWaveletEnabled();
}
// Thread-local dispatch witness (issue #70). A packet cannot reveal the kernel because
// both are bit-exact; a timestamp cannot either once another thread flips the global
// default. These counters are written on the thread that actually entered a kernel, so
// a test can assert its own session's path without perturbing another session's numbers.
thread_local std::uint64_t t_scalar_dispatches = 0;
thread_local std::uint64_t t_neon_dispatches = 0;
// Dispatch against an already-resolved choice. `Scalar` never touches the global
// switch, which is the whole point: a Neon session keeps Neon while another thread
// flips the process-wide default.
inline bool UseNeon(WaveletIsa isa) { return isa == WaveletIsa::Neon; }
inline HyperCineStatus ForwardHorizontal26(const int16_t* src, int width, int height, int src_stride,
                                          int16_t* lowpass, int low_stride,
                                          int16_t* highpass, int high_stride,
                                          WaveletIsa isa, bool allow_fallback) {
    HyperCineStatus vector_status = HYPERCINE_STATUS_NOT_SUPPORTED;
    if (UseNeon(isa)) {
        vector_status = HyperCine_ForwardHorizontal26_NEON(src, width, height, src_stride,
                                                          lowpass, low_stride,
                                                          highpass, high_stride);
    }
    switch (detail::DecideWaveletDispatch(isa, allow_fallback,
                                          vector_status == HYPERCINE_STATUS_SUCCESS)) {
    case detail::WaveletDispatchDecision::Vector:
        ++t_neon_dispatches;
        return vector_status;
    case detail::WaveletDispatchDecision::Scalar:
        break;
    case detail::WaveletDispatchDecision::FailClosed:
        // Explicit vector request: do not launder a scalar result as the requested
        // path. Surface the kernel's own status so the encoder fails closed instead
        // of advancing a successful encode state.
        return vector_status;
    }
    ++t_scalar_dispatches;
    return HyperCine_ForwardHorizontal26_CPU(src, width, height, src_stride,
                                             lowpass, low_stride, highpass, high_stride);
}
inline HyperCineStatus ForwardVertical26(const int16_t* src, int width, int height, int src_stride,
                                        int16_t* lowpass, int low_stride,
                                        int16_t* highpass, int high_stride,
                                        WaveletIsa isa, bool allow_fallback) {
    HyperCineStatus vector_status = HYPERCINE_STATUS_NOT_SUPPORTED;
    if (UseNeon(isa)) {
        vector_status = HyperCine_ForwardVertical26_NEON(src, width, height, src_stride,
                                                        lowpass, low_stride,
                                                        highpass, high_stride);
    }
    switch (detail::DecideWaveletDispatch(isa, allow_fallback,
                                          vector_status == HYPERCINE_STATUS_SUCCESS)) {
    case detail::WaveletDispatchDecision::Vector:
        ++t_neon_dispatches;
        return vector_status;
    case detail::WaveletDispatchDecision::Scalar:
        break;
    case detail::WaveletDispatchDecision::FailClosed:
        return vector_status;
    }
    ++t_scalar_dispatches;
    return HyperCine_ForwardVertical26_CPU(src, width, height, src_stride,
                                           lowpass, low_stride, highpass, high_stride);
}
} // namespace

detail::WaveletDispatchCounts detail::WaveletDispatchSnapshot() noexcept {
    WaveletDispatchCounts counts;
    counts.scalar = t_scalar_dispatches;
    counts.neon = t_neon_dispatches;
    return counts;
}

WaveletIsa detail::ResolveWaveletIsa(WaveletIsa requested) {
    switch (requested) {
    case WaveletIsa::Auto:
        // The only place the process-global default is still read. A session calls
        // this once; a direct ForwardLevel call reads it once per call, as before.
        return UseNeon() ? WaveletIsa::Neon : WaveletIsa::Scalar;
    case WaveletIsa::Scalar:
        return WaveletIsa::Scalar;
    case WaveletIsa::Neon:
        // An explicit request must not degrade silently, otherwise a caller cannot
        // tell "I asked for NEON and got it" from "I asked and got scalar anyway".
        if (!HyperCine_IsNEONWaveletAvailable())
            throw std::invalid_argument("NEON wavelet backend is not available in this build");
        return WaveletIsa::Neon;
    }
    throw std::invalid_argument("unknown wavelet ISA policy");
}

WaveletLevel ForwardLevel(const Plane& in, int prescale) {
    // Pass Auto through rather than a pre-resolved value: the 3-argument form has to
    // see that this call is allowed to degrade, which a resolved `Neon` would forbid.
    return ForwardLevel(in, prescale, WaveletIsa::Auto);
}

WaveletLevel ForwardLevel(const Plane& in, int prescale, WaveletIsa isa) {
    Validate(in);
    if (in.width < 6 || in.height < 6 || in.width % 2 || in.height % 2 || (prescale != 0 && prescale != 2))
        throw std::invalid_argument("invalid spatial dimensions or prescale");
    // `Auto` is documented to follow the process-global default. Resolve it here, once
    // per call, so the 3-argument form dispatches exactly like the 2-argument form. The
    // kernels only understand concrete choices, so passing `Auto` straight down used to
    // run scalar on every host while the caller believed it had asked for the default.
    // Resolve through the shared policy in every case, so an explicit `Neon` on a
    // build without the vector path still throws `invalid_argument` exactly as the
    // encoder constructor does. Only `Auto` is allowed to land on scalar when the
    // kernel itself refuses a specific geometry at run time.
    const bool allow_fallback = isa == WaveletIsa::Auto;
    const WaveletIsa resolved = detail::ResolveWaveletIsa(isa);
    WaveletLevel out{{Plane(in.width/2,in.height/2), Plane(in.width/2,in.height/2),
                      Plane(in.width/2,in.height/2), Plane(in.width/2,in.height/2)}};
    Plane low(in.width/2,in.height), high(in.width/2,in.height);
    if (prescale == 0) {
        if (ForwardHorizontal26(in.pixels.data(),in.width,in.height,in.stride,
                low.pixels.data(),low.stride,high.pixels.data(),high.stride,resolved,allow_fallback) != HYPERCINE_STATUS_SUCCESS)
            throw std::runtime_error("horizontal transform failed");
    } else {
        // Reuse the existing scalar filter for individually rounded highpass taps.
        Plane scaled(in.width,in.height);
        for (int y=0;y<in.height;++y) for (int x=0;x<in.width;++x) {
            int v = in.pixels[y*in.stride+x]+3;
            scaled.pixels[y*scaled.stride+x] = static_cast<int16_t>(v>=0 ? v/4 : -((-v+3)/4));
        }
        // Prescaled path stays scalar on purpose: it rounds each tap individually
        // before filtering, which the NEON interior identity does not model.
        HyperCine_ForwardHorizontal26_CPU(scaled.pixels.data(),in.width,in.height,scaled.stride,
                                        low.pixels.data(),low.stride,high.pixels.data(),high.stride);
        // Lowpass rounding is applied to the pair sum, not each individual tap.
        for (int y=0;y<in.height;++y) for (int x=0;x<in.width/2;++x) {
            const int v = in.pixels[y*in.stride+2*x]+in.pixels[y*in.stride+2*x+1]+3;
            low.pixels[y*low.stride+x] = static_cast<int16_t>(v>=0 ? v/4 : -((-v+3)/4));
        }
    }
    auto& b=out.bands;
    if (ForwardVertical26(low.pixels.data(),low.width,low.height,low.stride,
                          b[0].pixels.data(),b[0].stride,b[2].pixels.data(),b[2].stride,resolved,allow_fallback) != HYPERCINE_STATUS_SUCCESS)
        throw std::runtime_error("vertical lowpass transform failed");
    if (ForwardVertical26(high.pixels.data(),high.width,high.height,high.stride,
                          b[1].pixels.data(),b[1].stride,b[3].pixels.data(),b[3].stride,resolved,allow_fallback) != HYPERCINE_STATUS_SUCCESS)
        throw std::runtime_error("vertical highpass transform failed");
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
