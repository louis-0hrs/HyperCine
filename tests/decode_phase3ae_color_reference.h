// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/decode_color_cuda.h"
#include <algorithm>
#include <array>
#include <cstdint>
namespace DecodeAE {
inline int16_t Sat16(int v) { return int16_t((std::max)(-32768, (std::min)(32767, v))); }
inline int16_t Limit(int16_t v) {
    const uint16_t n = uint16_t(Sat16(int(v) + 0x7f00));
    return n < 0x7f00 ? 0 : int16_t(n - 0x7f00);
}
// Test-only CPU clone of the old GPU integer path. signed_input changes only
// the reduction to eight bits; all saturation, 16-bit multiply and packing
// semantics remain explicit. The pristine upstream decoder is the oracle.
inline std::array<uint8_t, 4> Color(int16_t y, int16_t u, int16_t v, int x, int width,
                                  HyperCine::ColorCoefficients c, bool signed_input) {
    int16_t yy = signed_input ? int16_t(int(y) >> 2) : int16_t(uint16_t(y) >> 2);
    int16_t uu = signed_input ? int16_t(int(u) >> 2) : int16_t(uint16_t(u) >> 2);
    int16_t vv = signed_input ? int16_t(int(v) >> 2) : int16_t(uint16_t(v) >> 2);
    int border = width / 2 - (width / 2) % 16;
    while (border > width / 2 - 4) border -= 16;
    int b, g, r;
    if (x < border * 2) {
        yy = Limit(Sat16(yy - c.y_offset));
        uu = Sat16(Limit(uu) - c.chroma_offset);
        vv = Sat16(Limit(vv) - c.chroma_offset);
        const int16_t luma = int16_t(int16_t((int(int16_t(uint16_t(yy) << 7)) * c.ymult) >> 16) * 2);
        r = Sat16(Sat16(luma + (int16_t(vv * c.r_vmult) >> 1)) + 32) >> 6;
        g = Sat16(Sat16(Sat16(luma - (int16_t(vv * c.g_vmult) >> 2)) - (int16_t(uu * c.g_umult) >> 2)) + 32) >> 6;
        b = Sat16(Sat16(luma + int16_t(uu * c.b_umult)) + 32) >> 6;
    } else {
        const int yscaled = ((int(yy) - c.y_offset) * c.ymult) >> 7;
        uu = int16_t(uu - c.chroma_offset); vv = int16_t(vv - c.chroma_offset);
        r = (yscaled + c.r_vmult * vv + 64) >> 7;
        g = (yscaled * 2 - c.g_umult * uu - c.g_vmult * vv + 128) >> 8;
        b = (yscaled + 2 * c.b_umult * uu + 64) >> 7;
    }
    auto pack = [](int n) { return uint8_t((std::max)(0, (std::min)(255, n))); };
    return {pack(b), pack(g), pack(r), 255};
}
} // namespace DecodeAE
