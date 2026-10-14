// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "hypercine/canonical.h"
#include "hypercine/wavelet.h"
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace hypercine::standalone;
namespace {
void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void Reject(F f) { bool caught=false; try{f();}catch(const std::exception&){caught=true;} Require(caught,"invalid input accepted"); }
AlignedVector<uint8_t> Frame(hypercine::CanonicalPattern p) {
    auto raw=hypercine::MakeFrame(128,128,p); return {raw.begin(),raw.end()};
}
void Color() {
    auto frame=Frame(hypercine::CanonicalPattern::Flat128);
    auto planes=ConvertBgra128(frame.data(),frame.size(),512);
    for(int c=0;c<3;++c) for(auto v:planes[c].pixels) Require(v==(c==0?504:512),"neutral conversion");
    for(std::size_t i=0;i<frame.size();i+=4) { frame[i]=0;frame[i+1]=0;frame[i+2]=255; }
    planes=ConvertBgra128(frame.data(),frame.size(),512);
    Require(planes[0].pixels[0]==250 && planes[1].pixels[0]==960 && planes[2].pixels[0]==409,"red 709 conversion");
    AlignedVector<uint8_t> padded(544*128,0xEE);
    for(int y=0;y<128;++y) std::copy(frame.begin()+y*512,frame.begin()+(y+1)*512,padded.begin()+y*544);
    auto padded_planes=ConvertBgra128(padded.data(),padded.size(),544);
    for(int c=0;c<3;++c) Require(padded_planes[c].pixels==planes[c].pixels,"padded input stride");
    Reject([&]{ConvertBgra128(frame.data()+1,frame.size()-1,512);});
    Reject([&]{ConvertBgra128(frame.data(),frame.size()-1,512);});
    Reject([&]{ConvertBgra128(frame.data(),frame.size(),513);});
    Reject([&]{ConvertBgra128(frame.data(),frame.size(),std::numeric_limits<std::size_t>::max()-15);});
}
void Wavelet() {
    Plane p(128,128); std::fill(p.pixels.begin(),p.pixels.end(),100);
    for(int lv=0;lv<3;++lv) {
        auto level=ForwardLevel(p,lv==1?2:0);
        for(auto v:level.bands[0].pixels) Require(v==(lv==2?1600:400),"constant pyramid scaling");
        for(int b=1;b<4;++b) for(auto v:level.bands[b].pixels) Require(v==0,"constant detail");
        p=std::move(level.bands[0]);
    }
    // Analytic affine ramp: interior/border highpass is zero in both axes.
    Plane ramp(16,16);
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) ramp.pixels[y*ramp.stride+x]=x+2*y;
    auto r=ForwardLevel(ramp,0);
    Require(r.bands[0].pixels[0]==6 && r.bands[0].pixels[7+7*r.bands[0].stride]==174,"ramp lowpass");
    for(int b=1;b<4;++b) for(auto v:r.bands[b].pixels) Require(v==0,"affine highpass");
    // Hand-calculated impulse vector exercises both one-sided borders and a
    // negative interior coefficient, independently of the 2D implementation.
    alignas(32) int16_t impulse[8]={0,0,0,100,0,0,0,0};
    alignas(32) int16_t low[8]={},high[8]={};
    Require(HyperCine_ForwardHorizontal26_CPU(impulse,8,1,8,low,8,high,8)==HYPERCINE_STATUS_SUCCESS,"impulse transform");
    const int expected_low[]={0,100,0,0}, expected_high[]={50,-100,-12,13};
    for(int i=0;i<4;++i) Require(low[i]==expected_low[i] && high[i]==expected_high[i],"impulse oracle");
    Reject([&]{ForwardLevel(ramp,1);});
    ramp.pixels.clear(); Reject([&]{ForwardLevel(ramp,0);});
}
void Quantization() {
    for(int v=-32768;v<=32767;++v) {
        Require(Quantize(static_cast<int16_t>(v),1)==v,"identity quantization");
        const int q=(v<0?-1:1)*(std::abs(v)/2);
        Require(Quantize(static_cast<int16_t>(v),2)==q,"signed quantization");
    }
    Require(Quantize(12,3)==3 && Quantize(-12,3)==-3,"reciprocal truncation");
    Reject([]{Quantize(1,0);});
}
void Entropy() {
    Plane p(8,1); auto zero=PackHighpass(p,100);
    // Eight zero bits, followed by table17 c256 (26 bits), 30 pad bits.
    Require(zero==std::vector<uint8_t>({0,0xC4,0x52,0xE8,0xC0,0,0,0}),"zero VLC golden");
    p.pixels[0]=1; auto positive=PackHighpass(p,100);
    p.pixels[0]=-1; auto negative=PackHighpass(p,100);
    Require(positive!=negative && positive[0]==0x80 && negative[0]==0xA0,"VLC sign bit");
    Reject([&]{PackHighpass(p,negative.size()-1);});
    Require(PackHighpass(p,negative.size())==negative,"exact capacity");
    Plane worst(128,128);
    for(std::size_t i=0;i<worst.pixels.size();++i) worst.pixels[i]=(i&1)?-32768:32767;
    const auto bound=4*(worst.pixels.size()+1);
    auto packed=PackHighpass(worst,bound);
    Require(packed.size()<=bound && packed.size()%4==0,"analytical VLC capacity");
    Reject([&]{PackHighpass(worst,packed.size()-1);});
    p.pixels[0]=-32768; const auto saturated=PackHighpass(p,100);
    p.pixels[0]=-1023; Require(PackHighpass(p,100)==saturated,"magnitude clamp");
}
unsigned U16(const std::vector<uint8_t>& v,std::size_t at) { Require(at+2<=v.size(),"truncated tag");return (v[at]<<8)|v[at+1]; }
uint32_t U32(const std::vector<uint8_t>& v,std::size_t at) { return (U16(v,at)<<16)|U16(v,at+2); }
void Sample() {
    for(auto pattern:{hypercine::CanonicalPattern::Flat128,hypercine::CanonicalPattern::GradientChecker,hypercine::CanonicalPattern::Canonical}) {
        auto f=Frame(pattern); const auto sample=EncodeBgra128(f.data(),f.size(),512);
        for(int n=0;n<10;++n) Require(EncodeBgra128(f.data(),f.size(),512)==sample,"nondeterministic sample");
        Require(U32(sample,0)==0x10009 && U32(sample,4)==0x20003,"intra header/index");
        int low=0,high=0,levels=0; std::size_t final_chunk=0;
        // Independently walk every tag, validating sized chunk extents and the
        // mandatory coefficient markers. No encoder structures are consulted.
        for(std::size_t at=20;at<sample.size();) {
            const auto raw=static_cast<int16_t>(U16(sample,at));
            const unsigned tag=raw<0 ? -raw:raw, value=U16(sample,at+2); at+=4;
            if((tag&0xff00)==0x2200) { final_chunk=at+4*(((tag&255)<<16)|value);Require(final_chunk==sample.size(),"sample size"); }
            if((tag&0xff00)==0x2100) { ++levels;Require(at+4*(((tag&255)<<16)|value)<=sample.size(),"level size"); }
            if((tag&0xff00)==0x2000) {
                const auto end=at+4*(((tag&255)<<16)|value);
                Require(end<=sample.size() && end>=at+8,"band size");
                const auto marker=U32(sample,at);
                if(marker==0x00040F0F) { ++low;Require(U32(sample,end-4)==0x00041B4B,"lowpass end"); }
                else { ++high;Require(marker==0x00370000 && U32(sample,end-4)==0x00380000,"detail framing"); }
                at=end;
            }
        }
        Require(low==3 && high==27 && levels==9 && final_chunk==sample.size(),"complete 30-band sample");
        Require(U32(sample,sample.size()-4)==0x00180000,"frame trailer");
        std::cout<<hypercine::PatternName(pattern)<<": "<<sample.size()<<" bytes, 30 bands parsed\n";
    }
}
}
int main(int argc,char** argv) {
    try {
        Require(argc==2,"missing test stage"); const std::string stage=argv[1];
        if(stage=="color") Color(); else if(stage=="wavelet") Wavelet(); else if(stage=="quantization") Quantization();
        else if(stage=="entropy") Entropy(); else if(stage=="sample") Sample(); else throw std::invalid_argument("unknown test");
        std::cout<<stage<<" PASS\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
