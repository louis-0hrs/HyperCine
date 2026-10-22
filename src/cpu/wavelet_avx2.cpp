// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <immintrin.h>
#include "wavelet_simd_impl.h"

namespace hypercine { namespace detail {
namespace {
struct Avx2 {
    using Reg=__m256i;
    static constexpr int lanes=8;
    static Reg Set(int v) { return _mm256_set1_epi32(v); }
    static Reg Add(Reg a,Reg b) { return _mm256_add_epi32(a,b); }
    static Reg Sub(Reg a,Reg b) { return _mm256_sub_epi32(a,b); }
    static Reg Mul(Reg a,int b) { return _mm256_mullo_epi32(a,Set(b)); }
    static Reg Shift3(Reg a) { return _mm256_srai_epi32(a,3); }
    static Reg Load(const int16_t* p) {
        return _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
    }
    static Reg Pairs(const int16_t* p) {
        return _mm256_madd_epi16(_mm256_loadu_si256(reinterpret_cast<const Reg*>(p)),_mm256_set1_epi16(1));
    }
    static Reg Differences(const int16_t* p) {
        return _mm256_madd_epi16(_mm256_loadu_si256(reinterpret_cast<const Reg*>(p)),Set(-65535));
    }
    static void Store(int16_t* p,Reg a) {
        // packs_epi32 is lane-local: combine the two 128-bit halves in order.
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p),
            _mm_packs_epi32(_mm256_castsi256_si128(a),_mm256_extracti128_si256(a,1)));
    }
};
}
void Horizontal26_Avx2(const int16_t* p,int w,int h,int ss,int16_t* l,int ls,int16_t* q,int hs) {
    Horizontal<Avx2>(p,w,h,ss,l,ls,q,hs);
}
void Vertical26_Avx2(const int16_t* p,int w,int h,int ss,int16_t* l,int ls,int16_t* q,int hs) {
    Vertical<Avx2>(p,w,h,ss,l,ls,q,hs);
}
}} // namespace hypercine::detail
