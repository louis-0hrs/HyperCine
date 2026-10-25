// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "metal_audit.h"
#include "reference_frame_generator.h"
#include <chrono>
#include <algorithm>
#include <fstream>
#include <sys/resource.h>
using namespace hypercine::standalone;
using Clock=std::chrono::steady_clock;
double Ms(Clock::time_point s){return std::chrono::duration<double,std::milli>(Clock::now()-s).count();}
template<class F> void Reject(F fn){try{fn();}catch(const std::invalid_argument&){return;}throw std::runtime_error("invalid storage accepted");}
int main(int argc,char** argv){try{
    auto audit=std::make_shared<metal_test::Audit>(); // mandatory GPU; never skip/fallback
    std::cout<<"Required Metal device: "<<audit->DeviceName()<<'\n';
    using namespace reference_fixture;
    // 4K is the P5 target (#45): the CPU session budget and Metal maxBufferLength
    // both admit 3840x2160, so the geometry must be exercised, not assumed.
    for(auto geometry : {std::array<int,2>{128,24},{384,40},{1920,1080},{3840,2160}}) {
        const int w=geometry[0],h=geometry[1],stride=w*4+16;
        AlignedVector<uint8_t> pixels(static_cast<std::size_t>(stride)*h,0xcd);
        for(int pattern=0;pattern<13;++pattern) {
            GeneratePhase3AFrame(w,h,pattern,static_cast<Phase3APattern>(pattern%11),pixels.data(),stride,0x1337beef);
            if(pattern==11) { // one impulse and nonzero odd-pixel chroma
                std::fill(pixels.begin(),pixels.end(),0);pixels[(h/2)*stride+4*(w/2)+2]=255;
                pixels[4+0]=255;
            }
            if(pattern==12)for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<4;++c)
                pixels[static_cast<std::size_t>(y)*stride+4*x+c]=static_cast<uint8_t>((y&1)?(x+c)%256:255-(x+c)%256);
            // Audit the actual batched resident path, including every intermediate
            // LL and all untouched row padding, independently of stage entrypoints.
            audit->Transform(pixels.data(),pixels.size()-16,w,h,stride,true);
            auto planes=audit->Convert(pixels.data(),pixels.size()-16,w,h,stride);
            for(const auto& plane:planes) {
                auto l1=audit->Forward(plane,0);auto l2=audit->Forward(l1.bands[0],2);audit->Forward(l2.bands[0],0);
            }
        }
        Reject([&]{audit->gpu->Convert(pixels.data()+1,pixels.size()-1,w,h,stride);});
        Reject([&]{audit->gpu->Convert(pixels.data(),0,w,h,stride);});
        Reject([&]{audit->gpu->Convert(pixels.data(),pixels.size(),w,h,stride-1);});
    }
    uint32_t seed=0x1337beef;
    for(auto geometry : {std::array<int,2>{6,6},{14,10},{30,270},{240,270}})for(int scale:{0,2}) {
        Plane p(geometry[0],geometry[1]);
        for(auto& v:p.pixels){uint32_t bits=NextXorShift32(seed)&65535u;v=static_cast<int16_t>(bits<=32767 ? int(bits):int(bits)-65536);}
        // Includes signed saturation and all rounding residues, plus nonzero input padding.
        const auto cpu=ForwardLevel(p,scale),gpu=audit->gpu->Forward(p,scale);
        for(int b=0;b<4;++b)metal_test::Equal(cpu.bands[b],gpu.bands[b],0,1,b);
    }
    Plane invalid(16,6);invalid.pixels.resize(1);Reject([&]{audit->gpu->Forward(invalid,0);});
    Plane valid(16,6);Reject([&]{audit->gpu->Forward(valid,1);});
    Plane odd(16,135);Reject([&]{audit->gpu->Forward(odd,0);});
    audit->Report();
    // Uninstrumented CPU/Metal complete encodes. Audit is excluded from these timings.
    AlignedVector<uint8_t> input(1920*1080*4);
    GeneratePhase3AFrame(1920,1080,7,Phase3APattern::DeterministicNoise,input.data(),7680,0x1337beef);
    FilmScan3Encoder cpu,metal(FilmScan3Backend::Metal);
    auto timed=detail::MetalFrontend();detail::FrontendAccess::Use(metal,timed);
    std::vector<double> ct,mt;double host=0;
    for(int i=0;i<6;++i){
        auto start=Clock::now();auto a=cpu.Encode(input.data(),input.size(),7680);double c=Ms(start);
        const auto before=detail::ReadMetalTiming(*timed);
        start=Clock::now();auto b=metal.Encode(input.data(),input.size(),7680);double m=Ms(start);
        const auto after=detail::ReadMetalTiming(*timed);
        metal_test::EqualPacket(a.sample,b.sample);
        if(a.coefficient_fnv64!=b.coefficient_fnv64)throw std::runtime_error("quantized coefficient hashes differ");
        if(i){ct.push_back(c);mt.push_back(m);host+=m-(after.upload_ms-before.upload_ms)-(after.readback_ms-before.readback_ms)-(after.submit_ms-before.submit_ms)-(after.wait_ms-before.wait_ms);}
        if(i==0 && argc==2){std::ofstream f(argv[1],std::ios::binary);f.write(reinterpret_cast<const char*>(b.sample.data()),b.sample.size());if(!f)throw std::runtime_error("sample write failed");}
    }
    metal.Encode(input.data(),input.size(),7680);metal.Reset();cpu.Reset();
    metal_test::EqualPacket(cpu.Encode(input.data(),input.size(),7680).sample,metal.Encode(input.data(),input.size(),7680).sample);
    const auto previous=metal.state();Reject([&]{metal.Encode(input.data(),0,7680);});
    if(metal.state().frame_index!=previous.frame_index || metal.state().previous_sample_bytes!=previous.previous_sample_bytes || metal.state().fs_rate_limiter!=previous.fs_rate_limiter)
        throw std::runtime_error("Metal invalid input advanced sequence state");
    detail::FailNextMetalCompletion(*timed);
    bool failed=false;
    try {metal.Encode(input.data(),input.size(),7680);}catch(const std::runtime_error&){failed=true;}
    if(!failed || metal.state().frame_index!=previous.frame_index ||
       metal.state().previous_sample_bytes!=previous.previous_sample_bytes ||
       metal.state().fs_rate_limiter!=previous.fs_rate_limiter)
        throw std::runtime_error("completion failure did not preserve encoder state");
    // Recover on the same resident buffers. No GPU fallback or stale output.
    const auto before_reuse=detail::ReadMetalTiming(*timed);
    metal.Reset();cpu.Reset();
    for(int f=0;f<105;++f) {
        GeneratePhase3AFrame(1920,1080,f,static_cast<Phase3APattern>(f%11),input.data(),7680,0x1337beef);
        metal_test::EqualPacket(cpu.Encode(input.data(),input.size(),7680).sample,
                                metal.Encode(input.data(),input.size(),7680).sample);
    }
    const auto after_reuse=detail::ReadMetalTiming(*timed);
    if(after_reuse.buffer_allocations!=before_reuse.buffer_allocations ||
       after_reuse.command_buffers-before_reuse.command_buffers!=105 ||
       after_reuse.waits-before_reuse.waits!=105 ||
       after_reuse.upload_copies-before_reuse.upload_copies!=105 ||
       after_reuse.readback_copies-before_reuse.readback_copies!=105*30 ||
       after_reuse.wavelet_dispatches-before_reuse.wavelet_dispatches!=105*27)
        throw std::runtime_error("resident allocation/submission/copy contract failed");
    std::cout<<"PASS 105 sequential changing frames, zero buffer allocations, 1 upload/command/wait and 30 readbacks per frame; completion-failure recovery\n";
    Reject([&]{timed->Transform(input.data(),input.size(),1920,1080,std::numeric_limits<std::size_t>::max()-15);});
    bool oversized=false;
    try {timed->Transform(input.data(),input.size(),4096,4096,16384);}catch(const std::length_error&){oversized=true;}
    if(!oversized)throw std::runtime_error("oversized pyramid accepted");
    FilmScan3Encoder independent(FilmScan3Backend::Metal),independent_cpu;
    metal.Reset();cpu.Reset();
    for(int f=0;f<3;++f) {
        metal_test::EqualPacket(cpu.Encode(input.data(),input.size(),7680).sample,
                                metal.Encode(input.data(),input.size(),7680).sample);
        metal_test::EqualPacket(independent_cpu.Encode(input.data(),input.size(),7680).sample,
                                independent.Encode(input.data(),input.size(),7680).sample);
    }
    independent.Reset();independent_cpu.Reset();
    metal_test::EqualPacket(independent_cpu.Encode(input.data(),input.size(),7680).sample,
                            independent.Encode(input.data(),input.size(),7680).sample);
    // New devices/queues/pipelines repeatedly acquire and release their ARC-owned resources.
    for(int repeat=0;repeat<3;++repeat) {
        auto fresh=detail::MetalFrontend();const auto a=ConvertFilmScan3Bgra(input.data(),input.size(),1920,1080,7680);
        const auto b=fresh->Convert(input.data(),input.size(),1920,1080,7680);
        for(int c=0;c<3;++c)metal_test::Equal(a[c],b[c],c,0,0);
    }
    auto stats=[](const std::vector<double>& v){double sum=0;for(double x:v)sum+=x;std::cout<<" avg_ms="<<sum/v.size()<<" min_ms="<<*std::min_element(v.begin(),v.end())<<" max_ms="<<*std::max_element(v.begin(),v.end())<<" fps="<<1000*v.size()/sum<<" mpix_s="<<2.0736*1000*v.size()/sum;};
    std::cout<<"CPU complete encode (5 measured after 1 warmup)";stats(ct);
    std::cout<<"\nMetal complete encode (5 measured after 1 warmup)";stats(mt);
    std::cout<<"\nMetal host quant/entropy/framing + orchestration avg_ms="<<host/5<<'\n';
    const auto t=detail::ReadMetalTiming(*timed);
    std::cout<<"Accumulated production GPU frames: batch_gpu_ms="<<t.batch_gpu_ms<<" upload_ms="<<t.upload_ms<<" readback_ms="<<t.readback_ms<<" submit_ms="<<t.submit_ms<<" wait_ms="<<t.wait_ms<<" waits="<<t.waits<<" buffer_allocations="<<t.buffer_allocations<<" peak_shared_buffer_bytes="<<t.peak_buffer_bytes<<" peak_device_allocated_bytes="<<t.peak_device_allocated_bytes<<'\n';
    rusage usage{};getrusage(RUSAGE_SELF,&usage);std::cout<<"process_peak_rss_bytes="<<usage.ru_maxrss<<"\nPASS exact full-storage planes, 3 wavelet levels, borders/prescale, packets, repeated encode/reset\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
