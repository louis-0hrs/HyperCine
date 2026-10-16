// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "entropy_legacy.h"
#include "reference_frame_generator.h"
#include "hypercine/filmscan3.h"
#include <iostream>
#include <limits>
#include <random>
using namespace hypercine::standalone;
namespace {
void Require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
template<class F> void Reject(F f) {
    bool caught=false;try{f();}catch(const std::exception&){caught=true;}
    Require(caught,"invalid input/capacity accepted");
}
void Compare(const Plane& p) {
    const auto capacity=4*(static_cast<std::size_t>(p.width)*p.height+1);
    const auto expected=legacy::PackHighpass(p,capacity);
    const auto actual=PackHighpass(p,capacity);
    if(actual!=expected) {
        std::size_t i=0;while(i<std::min(actual.size(),expected.size())&&actual[i]==expected[i])++i;
        throw std::runtime_error("entropy byte mismatch at "+std::to_string(i));
    }
    Require(actual.size()%4==0,"unaligned payload");
    Require(PackHighpass(p,actual.size())==expected,"exact capacity mismatch");
    Reject([&]{PackHighpass(p,actual.size()-1);});
    Reject([&]{PackHighpass(p,0);});
}
}
int main(){try{
    std::mt19937 rng(0x1337BEEF);
    for(int height:{1,2,3,4,7,8,9,15,16,17,31,32,33,63,64,65,127,128,129}) {
        Plane p(16,height);
        p.stride+=8;p.pixels.resize(static_cast<std::size_t>(p.stride)*p.height);
        for(int pattern=0;pattern<6;++pattern) {
            std::fill(p.pixels.begin(),p.pixels.end(),int16_t{12345}); // ignored pitch padding
            for(int y=0;y<p.height;++y)for(int x=0;x<p.width;++x) {
                const auto i=y*p.width+x;
                int v=0;
                if(pattern==1)v=(i%2)?-32768:32767;
                if(pattern==2)v=(i%2)?-1:1;
                if(pattern==3)v=static_cast<int>(rng()%65536)-32768;
                if(pattern==4)v=(i%3)?0:static_cast<int>(rng()%2049)-1024;
                if(pattern==5)v=(i==0||i==p.width*p.height-1)?1023:0;
                p.pixels[y*p.stride+x]=static_cast<int16_t>(v);
            }
            Compare(p);
        }
    }
    // Every signed input, including both sides of cubic ties and saturation.
    Plane full(256,256);
    for(unsigned i=0;i<65536;++i)full.pixels[i]=static_cast<int16_t>(static_cast<int>(i)-32768);
    Compare(full);
    for(int run:{0,1,11,12,13,19,20,21,31,32,33,59,60,61,99,100,101,179,180,181,319,320,321,639,640,641,1023,1024,1025}) {
        Plane p(8,(run+9)/8);p.pixels[run]=1;Compare(p);
    }
    // Actual 1080p FilmScan3 quantized bands, independent of packet equality.
    AlignedVector<uint8_t> input(1920*1080*4);
    for(int pattern:{1,3,7}) {
        reference_fixture::GeneratePhase3AFrame(1920,1080,7,
            static_cast<reference_fixture::Phase3APattern>(pattern),input.data(),7680,0x1337beef);
        auto colors=ConvertFilmScan3Bgra(input.data(),input.size(),1920,1080,7680);
        const auto tables=ComputeFilmScan3Tables(0);
        for(int c=0;c<3;++c) {
            auto l1=ForwardLevel(colors[c],0);
            auto l2=ForwardLevel(l1.bands[0],2);
            auto l3=ForwardLevel(l2.bands[0],0);
            WaveletLevel* levels[]={&l1,&l2,&l3};
            for(int lv=2;lv>=0;--lv)for(int b=1;b<4;++b) {
                auto& band=levels[lv]->bands[b];
                for(auto& v:band.pixels)v=QuantizeFilmScan3(v,tables.quant[c*12+lv*4+b]);
                Compare(band);
            }
        }
    }
    // Reuse and independent calls: no cached bit-buffer state escapes a band.
    Plane repeated(8,8);
    for(int i=0;i<1001;++i){repeated.pixels[i%64]=static_cast<int16_t>(rng()%65536-32768);Compare(repeated);}
    for(int invalid=0;invalid<7;++invalid){
        Plane p(16,16);
        if(invalid==0)p.width=0;
        if(invalid==1)p.width=15;
        if(invalid==2)p.height=0;
        if(invalid==3)p.stride=8;
        if(invalid==4)p.stride=17;
        if(invalid==5)p.pixels.resize(1);
        if(invalid==6)p.height=kMaxDimension+1;
        Reject([&]{PackHighpass(p,100);});
    }
    std::cout<<"PASS: independent frozen legacy, full int16 corpus, RLE boundaries, padding, capacity and 1001 repeated bands\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
