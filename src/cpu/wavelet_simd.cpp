// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "wavelet_simd.h"
#include <cstddef>
#include <limits>

namespace hypercine { namespace detail {
#ifdef HYPERCINE_WAVELET_X86
#define DECLARE_KERNELS(suffix) \
void Horizontal26_##suffix(const int16_t*, int, int, int, int16_t*, int, int16_t*, int); \
void Vertical26_##suffix(const int16_t*, int, int, int, int16_t*, int, int16_t*, int);
DECLARE_KERNELS(Avx2)
DECLARE_KERNELS(Avx512)
#undef DECLARE_KERNELS
#endif
bool WaveletSimdAvailable(CpuIsa isa) noexcept {
    if (isa == CpuIsa::Scalar || isa == CpuIsa::Auto) return true;
#ifdef HYPERCINE_WAVELET_X86
    static const auto caps = DetectCpuIsa();
    return CpuIsaUsable(isa, caps, {true, true, false});
#else
    return false;
#endif
}
namespace {
bool Fits(int rows, int stride, int columns) {
    if (rows <= 0 || columns <= 0 || stride < columns) return false;
    const auto limit = static_cast<std::size_t>(
        (std::numeric_limits<std::ptrdiff_t>::max)()) / sizeof(int16_t);
    // Scalar reference uses int row products; reject instead of overflowing.
    return static_cast<std::size_t>(rows) <= limit / static_cast<std::size_t>(stride) &&
        static_cast<std::size_t>(rows) <=
            static_cast<std::size_t>((std::numeric_limits<int>::max)()) / static_cast<std::size_t>(stride);
}
}
HyperCineStatus ForwardHorizontal26(CpuIsa isa, const int16_t* src,
    int w, int h, int ss, int16_t* low, int ls, int16_t* high, int hs) {
    if (!src || !low || !high || w < 6 || w % 2 ||
        !Fits(h,ss,w) || !Fits(h,ls,w/2) || !Fits(h,hs,w/2))
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (!WaveletSimdAvailable(isa)) return HYPERCINE_STATUS_NOT_SUPPORTED;
#ifdef HYPERCINE_WAVELET_X86
    if (isa == CpuIsa::Avx2) Horizontal26_Avx2(src,w,h,ss,low,ls,high,hs);
    else if (isa == CpuIsa::Avx512) Horizontal26_Avx512(src,w,h,ss,low,ls,high,hs);
    else
#endif
        return HyperCine_ForwardHorizontal26_CPU(src,w,h,ss,low,ls,high,hs);
    return HYPERCINE_STATUS_SUCCESS;
}
HyperCineStatus ForwardVertical26(CpuIsa isa, const int16_t* src,
    int w, int h, int ss, int16_t* low, int ls, int16_t* high, int hs) {
    if (!src || !low || !high || h < 6 || h % 2 ||
        !Fits(h,ss,w) || !Fits(h/2,ls,w) || !Fits(h/2,hs,w))
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (!WaveletSimdAvailable(isa)) return HYPERCINE_STATUS_NOT_SUPPORTED;
#ifdef HYPERCINE_WAVELET_X86
    if (isa == CpuIsa::Avx2) Vertical26_Avx2(src,w,h,ss,low,ls,high,hs);
    else if (isa == CpuIsa::Avx512) Vertical26_Avx512(src,w,h,ss,low,ls,high,hs);
    else
#endif
        return HyperCine_ForwardVertical26_CPU(src,w,h,ss,low,ls,high,hs);
    return HYPERCINE_STATUS_SUCCESS;
}
}} // namespace hypercine::detail
