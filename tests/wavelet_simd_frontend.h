// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "core/filmscan3_backend.h"
#include "cpu/wavelet_simd.h"

namespace hypercine_test {
// Test/benchmark injection only: reuses the existing encoder and frontend
// ownership seam. No global override or production ISA registration.
class SimdFrontend final : public hypercine::standalone::detail::Frontend {
    hypercine::detail::CpuIsa isa_;
    static int Floor4(int v) { return v>=0?v/4:-((-v+3)/4); }
public:
    explicit SimdFrontend(hypercine::detail::CpuIsa isa):isa_(isa) {
        if (!hypercine::detail::WaveletSimdAvailable(isa)) throw std::runtime_error("ISA unavailable");
    }
    std::string DeviceName() const override { return "test-only CPU SIMD"; }
    hypercine::standalone::Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {
        return hypercine::standalone::ConvertFilmScan3Bgra(p,n,w,h,s);
    }
    hypercine::standalone::WaveletLevel Forward(const hypercine::standalone::Plane& in,int prescale) override {
        using namespace hypercine::standalone;
        detail::ValidateWaveletInput(in,prescale);
        WaveletLevel out{{Plane(in.width/2,in.height/2),Plane(in.width/2,in.height/2),
                         Plane(in.width/2,in.height/2),Plane(in.width/2,in.height/2)}};
        Plane low(in.width/2,in.height),high(in.width/2,in.height);
        const Plane* source=&in;
        Plane scaled(1,1);
        if (prescale) {
            scaled=Plane(in.width,in.height);
            for (int y=0;y<in.height;++y) for (int x=0;x<in.width;++x)
                scaled.pixels[y*scaled.stride+x]=static_cast<int16_t>(Floor4(in.pixels[y*in.stride+x]+3));
            source=&scaled;
        }
        auto require=[](HyperCineStatus status) { if(status) throw std::runtime_error("SIMD transform failed"); };
        require(hypercine::detail::ForwardHorizontal26(isa_,source->pixels.data(),in.width,in.height,source->stride,
            low.pixels.data(),low.stride,high.pixels.data(),high.stride));
        if (prescale) for (int y=0;y<in.height;++y) for (int x=0;x<in.width/2;++x)
            low.pixels[y*low.stride+x]=static_cast<int16_t>(Floor4(int(in.pixels[y*in.stride+2*x])+in.pixels[y*in.stride+2*x+1]+3));
        auto& b=out.bands;
        require(hypercine::detail::ForwardVertical26(isa_,low.pixels.data(),low.width,low.height,low.stride,
            b[0].pixels.data(),b[0].stride,b[2].pixels.data(),b[2].stride));
        require(hypercine::detail::ForwardVertical26(isa_,high.pixels.data(),high.width,high.height,high.stride,
            b[1].pixels.data(),b[1].stride,b[3].pixels.data(),b[3].stride));
        return out;
    }
};
}
