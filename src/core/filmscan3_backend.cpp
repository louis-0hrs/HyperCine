// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "filmscan3_backend.h"
namespace hypercine { namespace standalone { namespace detail {
namespace {
class Cpu final : public Frontend {
    Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {
        return ConvertFilmScan3Bgra(p,n,w,h,s);
    }
    WaveletLevel Forward(const Plane& p,int s) override { return ForwardLevel(p,s); }
    std::string DeviceName() const override { return "CPU scalar"; }
};
}
std::shared_ptr<Frontend> CpuFrontend() { static auto cpu=std::make_shared<Cpu>();return cpu; }
std::shared_ptr<Frontend> SelectFrontend(FilmScan3Backend backend, MetalFactory factory) {
    switch(backend) {
        case FilmScan3Backend::Cpu: return CpuFrontend();
        case FilmScan3Backend::Metal: return factory();
        case FilmScan3Backend::MetalOrCpu:
            try { return factory(); } catch(const std::runtime_error&) { return CpuFrontend(); }
    }
    throw std::invalid_argument("invalid FilmScan3 backend");
}
#ifndef HYPERCINE_METAL_AVAILABLE
std::shared_ptr<Frontend> MetalFrontend() { throw std::runtime_error("Metal backend not built"); }
MetalTiming ReadMetalTiming(const Frontend&) { throw std::runtime_error("Metal backend not built"); }
#endif
void ValidateFilmScan3Input(const uint8_t* input,std::size_t bytes,int width,int height,std::size_t stride) {
    ValidateGeometry(width,height);
    const auto row=static_cast<std::size_t>(width)*4;
    if(!input || reinterpret_cast<std::uintptr_t>(input)%16 || stride<row || stride%16 ||
       stride>(std::numeric_limits<std::size_t>::max()-row)/(height-1) || bytes<stride*(height-1)+row)
        throw std::invalid_argument("FilmScan3 BGRA requires aligned complete storage");
}
void ValidateWaveletInput(const Plane& p,int prescale) {
    if(p.width<6 || p.height<6 || p.width>kMaxDimension || p.height>kMaxDimension ||
       p.width%2 || p.height%2 || p.stride<p.width || p.stride%8 ||
       static_cast<std::size_t>(p.stride)*p.height>kMaxPixels ||
       p.pixels.size()<static_cast<std::size_t>(p.stride)*p.height || (prescale!=0 && prescale!=2))
        throw std::invalid_argument("invalid spatial dimensions, storage or prescale");
}
}}}
