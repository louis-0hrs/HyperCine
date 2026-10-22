// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <immintrin.h>
#include "wavelet_simd_impl.h"

namespace hypercine { namespace detail {
namespace {
struct Avx512 {
    using Reg=__m512i;
    static constexpr int lanes=16;
    static Reg Set(int v) { return _mm512_set1_epi32(v); }
    static Reg Add(Reg a,Reg b) { return _mm512_add_epi32(a,b); }
    static Reg Sub(Reg a,Reg b) { return _mm512_sub_epi32(a,b); }
    static Reg Mul(Reg a,int b) { return _mm512_mullo_epi32(a,Set(b)); }
    static Reg Shift3(Reg a) { return _mm512_srai_epi32(a,3); }
    static Reg Load(const int16_t* p) {
        return _mm512_cvtepi16_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)));
    }
    static Reg Pairs(const int16_t* p) {
        return _mm512_madd_epi16(_mm512_loadu_si512(p),_mm512_set1_epi16(1));
    }
    static Reg Differences(const int16_t* p) {
        return _mm512_madd_epi16(_mm512_loadu_si512(p),Set(-65535));
    }
    static void Store(int16_t* p,Reg a) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(p),_mm512_cvtsepi32_epi16(a));
    }
};
}
void Horizontal26_Avx512(const int16_t* p,int w,int h,int ss,int16_t* l,int ls,int16_t* q,int hs) {
    Horizontal<Avx512>(p,w,h,ss,l,ls,q,hs);
}
void Vertical26_Avx512(const int16_t* p,int w,int h,int ss,int16_t* l,int ls,int16_t* q,int hs) {
    Vertical<Avx512>(p,w,h,ss,l,ls,q,hs);
}
}} // namespace hypercine::detail
