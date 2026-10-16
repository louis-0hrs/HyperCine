// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "../src/core/filmscan3_backend.h"
#include <iostream>
#include <algorithm>
#include <sstream>
namespace metal_test {
using namespace hypercine::standalone;
inline void Equal(const Plane& cpu,const Plane& metal,int plane,int level,int band) {
    if(cpu.width!=metal.width || cpu.height!=metal.height || cpu.stride!=metal.stride || cpu.pixels.size()!=metal.pixels.size())
        throw std::runtime_error("Metal plane layout differs");
    // Include padding, not only visible samples or hashes.
    for(std::size_t i=0;i<cpu.pixels.size();++i)if(cpu.pixels[i]!=metal.pixels[i]) {
        std::ostringstream s;s<<"plane="<<plane<<" level="<<level<<" band="<<band<<" x="<<i%cpu.stride
            <<" y="<<i/cpu.stride<<" cpu="<<cpu.pixels[i]<<" metal="<<metal.pixels[i];throw std::runtime_error(s.str());
    }
}
inline void EqualPacket(const std::vector<uint8_t>& cpu,const std::vector<uint8_t>& metal) {
    const auto n=std::min(cpu.size(),metal.size());
    std::size_t at=0;while(at<n && cpu[at]==metal[at])++at;
    if(at!=n || cpu.size()!=metal.size()) {
        std::ostringstream s;s<<"CFHD byte="<<at<<" cpu_size="<<cpu.size()<<" metal_size="<<metal.size();
        if(at<n)s<<" cpu="<<unsigned(cpu[at])<<" metal="<<unsigned(metal[at]);
        throw std::runtime_error(s.str());
    }
}
class Audit final : public detail::Frontend {
    unsigned wave_=0;
public:
    std::shared_ptr<detail::Frontend> gpu=detail::MetalFrontend();
    unsigned frames=0,levels=0;
    detail::FrontendFrame Transform(const uint8_t* p,std::size_t n,int w,int h,std::size_t s,bool=false) override {
        auto cpu=detail::CpuFrontend()->Transform(p,n,w,h,s,true);
        auto metal=gpu->Transform(p,n,w,h,s,true);
        for(int c=0;c<3;++c) {
            Equal(cpu.color[c],metal.color[c],c,0,0);
            for(int l=0;l<3;++l)for(int b=0;b<4;++b)
                Equal(cpu.levels[c][l].bands[b],metal.levels[c][l].bands[b],c,l+1,b);
        }
        ++frames;levels+=9;return metal;
    }
    Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {
        auto cpu=ConvertFilmScan3Bgra(p,n,w,h,s);auto metal=gpu->Convert(p,n,w,h,s);
        for(int c=0;c<3;++c)Equal(cpu[c],metal[c],c,0,0);
        wave_=0;++frames;return metal;
    }
    WaveletLevel Forward(const Plane& p,int s) override {
        auto cpu=ForwardLevel(p,s);auto metal=gpu->Forward(p,s);
        for(int b=0;b<4;++b)Equal(cpu.bands[b],metal.bands[b],wave_/3,wave_%3+1,b);
        ++wave_;++levels;return metal;
    }
    std::string DeviceName() const override {return gpu->DeviceName();}
    void Report() const {
        const auto t=detail::ReadMetalTiming(*gpu);
        std::cout<<"Metal device: "<<DeviceName()<<"; audited frames="<<frames<<" levels="<<levels
            <<" color_gpu_ms="<<t.color_gpu_ms<<" wavelet_gpu_ms="<<t.wavelet_gpu_ms
            <<" upload_ms="<<t.upload_ms<<" readback_ms="<<t.readback_ms<<" submit_ms="<<t.submit_ms
            <<" wait_ms="<<t.wait_ms<<" peak_shared_buffer_bytes="<<t.peak_buffer_bytes<<" peak_device_allocated_bytes="<<t.peak_device_allocated_bytes<<'\n';
    }
};
}
