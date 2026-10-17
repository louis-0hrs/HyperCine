// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "hypercine/quality_state.h"
#include <algorithm>
#include <array>
#include <climits>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace hypercine::standalone;
namespace {
std::size_t checks=0;
void Require(bool value, const char* why) {
    ++checks;
    if(!value) throw std::runtime_error(why);
}
template<class F> void Reject(F f) {
    try { f(); } catch(const std::invalid_argument&) { ++checks; return; }
    catch(const std::length_error&) { ++checks; return; }
    throw std::runtime_error("invalid geometry/storage accepted");
}
void Geometry() {
    for(auto wh : {std::array<int,2>{128,24},{1920,1080},{4096,512}})
        ValidateGeometry(wh[0],wh[1]);
    for(auto wh : {std::array<int,2>{INT_MIN,24},{128,INT_MAX},{0,24},
        {128,0},{127,24},{129,24},{128,23},{128,25},{3840,2160},
        {7680,4320},{12288,8040},{17520,8040},{4096,520}})
        Reject([&]{ ValidateGeometry(wh[0],wh[1]); });
    // Public mutable Plane descriptors must never make storage trusted.
    Plane valid(14,10);
    for(int field=0;field<6;++field) {
        auto p=valid;
        if(field==0) p.width=INT_MAX;
        if(field==1) p.height=INT_MIN;
        if(field==2) p.stride=INT_MAX;
        if(field==3) p.stride=-8;
        if(field==4) p.pixels.resize(1);
        if(field==5) p.width=7;
        Reject([&]{ ForwardLevel(p,0); });
    }
    Reject([&]{ ForwardLevel(valid,1); });
    Require(valid.width==14 && valid.height==10 && valid.pixels.size()==160,
            "rejections mutated original plane");
}
// Independent quotient/remainder division, widened before each calculation.
int64_t Floor(int64_t v,int64_t d) { return v/d-(v%d<0); }
int16_t Clamp(int64_t v) { return static_cast<int16_t>(std::max<int64_t>(-32768,std::min<int64_t>(32767,v))); }
template<class Tap> std::array<int16_t,2> Pair(Tap raw,int n,int c,int prescale) {
    const int x=2*c;
    auto tap=[&](int i)->int64_t { return prescale ? Floor(int64_t(raw(i))+3,4) : raw(i); };
    int64_t high=0;
    if(c==0) high=Floor(5*tap(0)-11*tap(1)+4*tap(2)+4*tap(3)-tap(4)-tap(5)+4,8);
    else if(c==n/2-1) high=Floor(11*tap(x)-5*tap(x+1)-4*tap(x-1)-4*tap(x-2)+tap(x-3)+tap(x-4)+4,8);
    else high=tap(x)-tap(x+1)+Floor(-tap(x-2)-tap(x-1)+tap(x+2)+tap(x+3)+4,8);
    const auto sum=int64_t(raw(x))+raw(x+1);
    return {Clamp(prescale ? Floor(sum+3,4) : sum),Clamp(high)};
}
void Wavelet() {
    uint32_t seed=0x1337beef;
    // Small signed domains, SIMD tails and a 4K-wide six-row probe; bounded RAM.
    for(int w : {6,8,10,14,16,18,30,34,128,3840})
    for(int h : {6,10,18}) for(int scale : {0,2}) for(int pattern=0;pattern<6;++pattern) {
        Plane p(w,h);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
            const int random=int(seed&65535u)-32768;
            const int values[]={0,-32768,32767,((x+y)&1)?-32768:32767,
                                (x==w-1 && y==h-1)?32767:0,random};
            p.pixels[y*p.stride+x]=static_cast<int16_t>(values[pattern]);
        }
        auto poisoned=p;
        for(int y=0;y<h;++y) for(int x=w;x<p.stride;++x) poisoned.pixels[y*p.stride+x]=-23456;
        const auto result=ForwardLevel(p,scale),other=ForwardLevel(poisoned,scale);
        // Build independent horizontal intermediates, then reference columns.
        std::array<Plane,2> horizontal{Plane(w/2,h),Plane(w/2,h)};
        for(int y=0;y<h;++y) for(int x=0;x<w/2;++x) {
            const auto pair=Pair([&](int k){ return p.pixels[y*p.stride+k]; },w,x,scale);
            for(int b=0;b<2;++b) horizontal[b].pixels[y*horizontal[b].stride+x]=pair[b];
        }
        for(int b=0;b<2;++b) for(int y=0;y<h/2;++y) for(int x=0;x<w/2;++x) {
            const auto& tmp=horizontal[b];
            const auto pair=Pair([&](int k){return tmp.pixels[k*tmp.stride+x];},h,y,0);
            for(int vertical=0;vertical<2;++vertical) {
                const auto& actual=result.bands[b+2*vertical];
                Require(actual.pixels[y*actual.stride+x]==pair[vertical],"independent 2D coefficient mismatch");
            }
        }
        for(int b=0;b<4;++b) {
            Require(result.bands[b].pixels==other.bands[b].pixels,"input row padding influenced transform");
            const auto& band=result.bands[b];
            for(int y=0;y<band.height;++y) for(int x=band.width;x<band.stride;++x)
                Require(band.pixels[y*band.stride+x]==0,"output padding overwritten");
        }
    }
}
void Quality() {
    using namespace hypercine;
    std::array<QualityStateInput,6> state{};
    std::array<std::array<QualityState,120>,6> frozen{};
    auto advance=[](QualityStateInput& in,const QualityState& out,int f) {
        in.frame_index=f+1; in.previous_fs_rate_limiter=out.fs_rate_limiter;
        in.previous_overbitrate=out.overbitrate;
        in.previous_completed_sample_bytes=std::array<uint64_t,4>{0,250000,2117476,8000000}[f%4];
    };
    auto equal=[](const QualityState& a,const QualityState& b) {
        return a.ok==b.ok && a.quality==b.quality && a.frame_index==b.frame_index &&
            a.coding_width==b.coding_width && a.coding_height==b.coding_height &&
            a.encoded_precision==b.encoded_precision && a.frame_rate_numerator==b.frame_rate_numerator &&
            a.frame_rate_denominator==b.frame_rate_denominator && a.gop_length==b.gop_length &&
            a.prescale==b.prescale && a.fs_rate_limiter==b.fs_rate_limiter &&
            a.overbitrate==b.overbitrate && a.previous_completed_sample_bytes==b.previous_completed_sample_bytes &&
            a.quant==b.quant && a.scale==b.scale;
    };
    for(int q=0;q<6;++q) {
        QualityStateInput input; input.quality=static_cast<EncodingQuality>(q+1);
        state[q]=input;
        for(int f=0;f<120;++f) {
            frozen[q][f]=CalculateQualityState(input);
            Require(frozen[q][f].ok,"valid quality sequence rejected");
            advance(input,frozen[q][f],f);
        }
    }
    for(int f=0;f<120;++f) for(int k=0;k<6;++k) {
        const int q=f%2 ? k : 5-k;
        auto out=CalculateQualityState(state[q]);
        Require(equal(out,frozen[q][f]),"interleaved quality state differs");
        auto invalid=state[q];invalid.quality=static_cast<EncodingQuality>(7);
        Require(!CalculateQualityState(invalid).ok,"invalid quality accepted");
        Require(equal(CalculateQualityState(state[q]),out),"invalid call affected independent session");
        advance(state[q],out,f);
    }
}
}
int main(int argc,char** argv) { try {
    if(argc!=2) throw std::invalid_argument("expected geometry, quality or wavelet");
    const std::string mode=argv[1];
    if(mode=="geometry") Geometry(); else if(mode=="quality") Quality();
    else if(mode=="wavelet") Wavelet(); else throw std::invalid_argument("unknown contract");
    std::cout<<mode<<": PASS checks="<<checks<<'\n';
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
