// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "cpu/wavelet_simd.h"
#include "hypercine/filmscan3.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#include "../tests/wavelet_simd_frontend.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#endif
using namespace hypercine::detail;
using namespace hypercine::standalone;
namespace {
using Clock=std::chrono::steady_clock;
std::size_t PeakMemory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters))) return counters.PeakWorkingSetSize;
#endif
    return 0; // unavailable on this platform; never infer a measurement
}
template<class F> void Measure(const char* stage,int isa,int w,int h,int workers,int frames,F run) {
    for (int i=0;i<3;++i) run();
    std::vector<double> times;
    for (int i=0;i<frames;++i) {
        const auto start=Clock::now();run();
        times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
    }
    std::sort(times.begin(),times.end());
    const double avg=std::accumulate(times.begin(),times.end(),0.0)/times.size();
    std::cout<<"{\"stage\":\""<<stage<<"\",\"isa\":"<<isa<<",\"width\":"<<w<<",\"height\":"<<h
        <<",\"workers\":"<<workers<<",\"frames\":"<<frames<<",\"warmups\":3,\"avg_ms\":"<<avg
        <<",\"min_ms\":"<<times.front()<<",\"max_ms\":"<<times.back()<<",\"median_ms\":"<<times[times.size()/2]
        <<",\"p95_ms\":"<<times[(times.size()*95+99)/100-1]<<",\"fps\":"<<1000/avg
        <<",\"megapixels_per_sec\":"<<double(w)*h/(1000*avg)<<",\"peak_process_bytes\":"<<PeakMemory()<<"}\n";
}
}
int main(int argc,char** argv) { try {
    const int workers=argc>1?std::stoi(argv[1]):1;
    if (workers<1 || workers>64) return 2;
#ifdef _WIN32
    int regs[4]; char brand[49]={};
    for (int i=0;i<3;++i) { __cpuid(regs,static_cast<int>(0x80000002u+i));std::copy_n(reinterpret_cast<char*>(regs),16,brand+16*i); }
    std::cout<<"CPU: "<<brand<<"; logical_threads="<<std::thread::hardware_concurrency()<<'\n';
#endif
    std::cout<<"Timing includes per-call worker creation/join when workers>1; clocks/power OS-managed, affinity unpinned.\n";
    for (auto size:{std::pair<int,int>{1920,1080},{3840,2160}}) {
        const int w=size.first,h=size.second;
        AlignedVector<int16_t> src(std::size_t(w)*h),low(std::size_t(w)*h),high(low.size());
        uint32_t seed=0x1337beefu;
        for (auto& v:src) { seed=seed*1664525u+1013904223u;v=static_cast<int16_t>(int(seed>>16)-32768); }
        for (auto isa:{CpuIsa::Scalar,CpuIsa::Avx2,CpuIsa::Avx512}) {
            if (!WaveletSimdAvailable(isa)) continue;
            for (bool vertical:{false,true}) Measure(vertical?"vertical":"horizontal",int(isa),w,h,workers,31,[&] {
                auto run=[&](int worker) {
                    HyperCineStatus status;
                    if (vertical) {
                        const int begin=w*worker/workers,end=w*(worker+1)/workers;
                        status=ForwardVertical26(isa,src.data()+begin,end-begin,h,w,low.data()+begin,w,high.data()+begin,w);
                    } else {
                        const int begin=h*worker/workers,end=h*(worker+1)/workers;
                        status=ForwardHorizontal26(isa,src.data()+std::size_t(begin)*w,w,end-begin,w,
                            low.data()+std::size_t(begin)*w/2,w/2,high.data()+std::size_t(begin)*w/2,w/2);
                    }
                    if (status!=HYPERCINE_STATUS_SUCCESS) std::terminate();
                };
                if (workers==1) run(0);
                else {
                    std::vector<std::thread> threads;
                    for (int i=0;i<workers;++i) threads.emplace_back(run,i);
                    for (auto& thread:threads) thread.join();
                }
            });
        }
    }
    // Complete FS3 sessions via test-only frontend injection, separately from
    // isolated stages. Production dispatch remains scalar.
    AlignedVector<uint8_t> bgra(1920*1080*4);
    for (std::size_t i=0;i<bgra.size();++i) bgra[i]=static_cast<uint8_t>((i/4+i/(1920*4))%256);
    for (auto isa:{CpuIsa::Scalar,CpuIsa::Avx2,CpuIsa::Avx512}) {
        if (!WaveletSimdAvailable(isa)) continue;
        FilmScan3Encoder encoder;
        if (isa!=CpuIsa::Scalar) hypercine::standalone::detail::FrontendAccess::Use(encoder,
            std::make_shared<hypercine_test::SimdFrontend>(isa));
        Measure("test_injected_fs3_session",int(isa),1920,1080,1,16,[&] {
            const auto packet=encoder.Encode(bgra.data(),bgra.size(),1920*4);
            if (packet.sample.empty()) std::terminate();
        });
    }
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
