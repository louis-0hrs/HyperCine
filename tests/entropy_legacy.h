// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Frozen from PR41 db98869012a75fa628110e14fb25f93697a78754.
// Test-only oracle: retain implementation independently of the production writer.
#pragma once
#include "hypercine/standalone.h"
#include "../src/core/standalone_codebook.h"
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace hypercine { namespace standalone { namespace legacy {
class Bits {
    std::size_t capacity_, count_ = 0;
public:
    std::vector<uint8_t> bytes;
    explicit Bits(std::size_t capacity) : capacity_(capacity) {}
    void Put(uint32_t code, unsigned size) {
        if (size < 1 || size > 31 || (code >> size)) throw std::invalid_argument("invalid VLC codeword");
        for (unsigned n=size;n>0;--n) {
            if (count_ % 8 == 0) {
                if (bytes.size() >= capacity_) throw std::length_error("entropy capacity exceeded");
                bytes.push_back(0);
            }
            bytes.back() |= static_cast<uint8_t>(((code >> (n-1)) & 1u) << (7-count_%8));
            ++count_;
        }
    }
    void Finish() {
        while (count_ % 32) Put(0,1);
    }
};
std::vector<uint8_t> PackHighpass(const Plane& p, std::size_t capacity) {
    if (p.width<1 || p.width>kMaxDimension || p.width%8 || p.height<1 || p.height>kMaxDimension || p.stride<p.width || p.stride%8 ||
        static_cast<std::size_t>(p.stride)*p.height>kMaxPixels ||
        p.pixels.size()<static_cast<std::size_t>(p.stride)*p.height)
        throw std::invalid_argument("invalid entropy plane");
    Bits bits(capacity);
    // Inverse of table17's integer cubic decompanding, identical tie policy to
    // FillVleTable: largest mapped index <= magnitude, clamped to 1023.
    std::array<unsigned,1024> cubic{};
    for (unsigned i=1;i<256;++i) {
        const unsigned m=std::min(1023u,i+static_cast<unsigned>((uint64_t{768}*i*i*i)/(256u*256u*256u)));
        cubic[m]=i;
    }
    unsigned previous=0;
    for(auto& x:cubic) { if (x) previous=x; else x=previous; }
    unsigned run=0;
    auto flush=[&] {
        for(const auto& r:detail::runs17) while(run>=r.count) {
            bits.Put(r.code.bits,r.code.size); run-=r.count;
        }
    };
    for(int y=0;y<p.height;++y) for(int x=0;x<p.width;++x) {
        const int v=p.pixels[y*p.stride+x];
        if (!v) { ++run; continue; }
        flush();
        const auto magnitude=static_cast<unsigned>(v<0 ? -v:v);
        const auto code=detail::magnitude17[cubic[std::min(1023u,magnitude)]];
        bits.Put((code.bits<<1) | (v<0 ? 1u:0u),code.size+1);
    }
    flush(); bits.Put(detail::band_end17.bits,detail::band_end17.size); bits.Finish();
    return std::move(bits.bytes);
}
}}}
