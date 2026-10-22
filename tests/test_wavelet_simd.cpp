// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "cpu/wavelet_simd.h"
#include <vector>
#include "test_vectors.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace hypercine::detail;
using Buffer=HyperCineTest::AlignedVector<int16_t>;
namespace {
std::size_t cases=0;
constexpr int16_t canary=12345;
void Check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
using Kernel=HyperCineStatus(*)(CpuIsa,const int16_t*,int,int,int,int16_t*,int,int16_t*,int);
int16_t Pattern(int x,int y,int w,int h,int pattern,uint32_t& seed) {
    seed=seed*1664525u+1013904223u;
    switch(pattern) {
    case 0:return 0;
    case 1:return 32767;
    case 2:return -32768;
    case 3:return (x+y)%2 ? -32768 : 32767;
    case 4:return x==w/2 && y==h/2 ? -32768 : 0;
    case 5:return static_cast<int16_t>((x*19+y*37)%33-16);
    case 6:return static_cast<int16_t>(int(seed>>16)-32768);
    default:return x<w/2 ? -32768 : 32767;
    }
}
void Compare(CpuIsa isa,bool vertical,int w,int h,int pad,int offset,int pattern) {
    const int ss=w+pad, ow=vertical?w:w/2, oh=vertical?h/2:h;
    const int ls=ow+pad, hs=ow+pad+3;
    const std::size_t prefix=32+offset, suffix=32;
    Buffer src(prefix+std::size_t(ss)*h+suffix,canary);
    Buffer l(prefix+std::size_t(ls)*oh+suffix,canary),q(prefix+std::size_t(hs)*oh+suffix,canary);
    auto rl=l,rq=q;
    uint32_t seed=0x1337beefu;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x)
        src[prefix+std::size_t(y)*ss+x]=Pattern(x,y,w,h,pattern,seed);
    const auto original=src;
    const auto kernel=vertical?ForwardVertical26:ForwardHorizontal26;
    const auto scalar=vertical?HyperCine_ForwardVertical26_CPU:HyperCine_ForwardHorizontal26_CPU;
    Check(scalar(src.data()+prefix,w,h,ss,rl.data()+prefix,ls,rq.data()+prefix,hs)==0,"scalar status");
    for (int repeat=0;repeat<3;++repeat) {
        Check(kernel(isa,src.data()+prefix,w,h,ss,l.data()+prefix,ls,q.data()+prefix,hs)==0,"SIMD status");
        Check(l==rl && q==rq,"coefficient or output redzone/padding mismatch");
        Check(src==original,"source modified");
        // Change all source guards/padding: output must never depend on them.
        for (std::size_t i=0;i<src.size();++i) {
            bool active=i>=prefix && i<prefix+std::size_t(ss)*h && (i-prefix)%ss<std::size_t(w);
            if (!active) src[i]=static_cast<int16_t>(repeat*20000-30000);
        }
        // Restore before checking that the next invocation leaves source intact.
        if (repeat<2) {
            auto poisoned=src;
            Check(kernel(isa,src.data()+prefix,w,h,ss,l.data()+prefix,ls,q.data()+prefix,hs)==0,"poison status");
            Check(l==rl && q==rq && src==poisoned,"padding-dependent result or source write");
            src=original;
        }
    }
    ++cases;
}
// Put the final input sample against an inaccessible page. Unlike canaries,
// this detects a vector read that runs beyond the final logical row.
struct Guarded {
    void* allocation=nullptr;
    std::size_t bytes=0;
    int16_t* data=nullptr;
    explicit Guarded(std::size_t samples) {
#ifdef _WIN32
        SYSTEM_INFO info; GetSystemInfo(&info); const auto page=info.dwPageSize;
#else
        const auto page=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
        const auto payload=((samples*2+page-1)/page)*page;
        bytes=payload+page;
#ifdef _WIN32
        allocation=VirtualAlloc(nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if (!allocation) throw std::runtime_error("VirtualAlloc failed");
        DWORD previous=0;
        if (!VirtualProtect(static_cast<char*>(allocation)+payload,page,PAGE_NOACCESS,&previous)) {
            VirtualFree(allocation,0,MEM_RELEASE); throw std::runtime_error("VirtualProtect failed");
        }
#else
        allocation=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (allocation==MAP_FAILED) throw std::runtime_error("mmap failed");
        if (mprotect(static_cast<char*>(allocation)+payload,page,PROT_NONE)) {
            munmap(allocation,bytes); throw std::runtime_error("mprotect failed");
        }
#endif
        data=reinterpret_cast<int16_t*>(static_cast<char*>(allocation)+payload)-samples;
    }
    Guarded(const Guarded&)=delete;
    Guarded& operator=(const Guarded&)=delete;
    ~Guarded() {
#ifdef _WIN32
        VirtualFree(allocation,0,MEM_RELEASE);
#else
        munmap(allocation,bytes);
#endif
    }
};
void PageBounds(CpuIsa isa) {
    for (int w=6;w<=98;w+=2) for (bool vertical:{false,true}) {
        const int h=6,ow=vertical?w:w/2,oh=vertical?h/2:h;
        Guarded src(w*h),low(ow*oh),high(ow*oh);
        std::fill_n(src.data,w*h,int16_t(-32768));
        Check((vertical?ForwardVertical26:ForwardHorizontal26)(isa,src.data,w,h,w,
            low.data,ow,high.data,ow)==0,"page-boundary status");
        ++cases;
    }
}
void Invalid(CpuIsa isa) {
    int16_t src[64]={},low[64]={},high[64]={};
    for (auto kernel:{ForwardHorizontal26,ForwardVertical26}) {
        Check(kernel(isa,nullptr,6,6,6,low,6,high,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"null source accepted");
        Check(kernel(isa,src,6,6,6,nullptr,6,high,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"null low accepted");
        Check(kernel(isa,src,6,6,6,low,6,nullptr,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"null high accepted");
        Check(kernel(isa,src,6,6,5,low,6,high,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"bad source stride accepted");
        Check(kernel(isa,src,6,6,6,low,2,high,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"bad output stride accepted");
        Check(kernel(isa,src,6,6,(std::numeric_limits<int>::max)(),low,6,high,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"overflow extent accepted");
        Check(kernel(isa,src,6,6,6,low,6,high,6)==
            (WaveletSimdAvailable(isa)?HYPERCINE_STATUS_SUCCESS:HYPERCINE_STATUS_NOT_SUPPORTED),"unsupported ISA did not fail closed");
    }
    Check(ForwardHorizontal26(isa,src,5,6,6,low,6,high,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"odd/short width");
    Check(ForwardVertical26(isa,src,6,5,6,low,6,high,6)==HYPERCINE_STATUS_INVALID_ARGUMENT,"odd/short height");
}
}
int main() { try {
    Check(ResolveCpuIsa(CpuIsa::Auto)==CpuIsa::Scalar,"production dispatch changed");
    for (auto isa:{CpuIsa::Scalar,CpuIsa::Auto,CpuIsa::Avx2,CpuIsa::Avx512,CpuIsa::Neon,static_cast<CpuIsa>(999)}) Invalid(isa);
    for (auto isa:{CpuIsa::Scalar,CpuIsa::Avx2,CpuIsa::Avx512}) {
        if (!WaveletSimdAvailable(isa)) { std::cout<<"HW_PENDING ISA="<<int(isa)<<'\n';continue; }
        const auto before=cases;
        for (int n=6;n<=130;n+=2) for (int pattern=0;pattern<8;++pattern) {
            Compare(isa,false,n,6,0,0,pattern);
            Compare(isa,false,n,8,7,1,pattern);
            Compare(isa,true,n-5,n,0,0,pattern);
            Compare(isa,true,n-5,n,9,3,pattern);
        }
        for (const auto size:{std::pair<int,int>{128,128},{256,256},{1920,1080},{3840,2160}})
            for (int pattern=0;pattern<8;++pattern) for (bool vertical:{false,true})
                Compare(isa,vertical,size.first,size.second,8,1,pattern);
        PageBounds(isa);
        std::cout<<"PASS ISA="<<int(isa)<<" cases="<<cases-before<<'\n';
    }
    std::cout<<"PASS: "<<cases<<" deterministic cases; all coefficients, guards and repeated calls\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
