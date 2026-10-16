// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../../../tests/entropy_legacy.h"
#include <chrono>
#include <iostream>
#include <random>
using namespace hypercine::standalone;
using Clock=std::chrono::steady_clock;
int main(){try{
    std::cout<<"{\"seed\":322420463,\"runs\":[";bool first=true;
    std::mt19937 rng(0x1337beef);
    for(int pattern=0;pattern<5;++pattern){
        Plane p(960,540);
        for(std::size_t i=0;i<p.pixels.size();++i){
            int v=0;
            if(pattern==1)v=(i%2)?-32768:32767;
            if(pattern==2)v=(i%2)?1:0;
            if(pattern==3)v=static_cast<int>(rng()%2049)-1024;
            if(pattern==4)v=(i%321)?0:1;
            p.pixels[i]=static_cast<int16_t>(v);
        }
        for(int trial=-2;trial<30;++trial){
            std::vector<uint8_t> a,b;double ta=0,tb=0;
            auto old=[&]{auto t=Clock::now();a=legacy::PackHighpass(p,4*(p.width*p.height+1));ta=std::chrono::duration<double,std::milli>(Clock::now()-t).count();};
            auto fast=[&]{auto t=Clock::now();b=PackHighpass(p,4*(p.width*p.height+1));tb=std::chrono::duration<double,std::milli>(Clock::now()-t).count();};
            if(trial%2){fast();old();}else{old();fast();}
            if(a!=b)throw std::runtime_error("microbenchmark oracle mismatch");
            if(trial<0)continue;
            if(!first)std::cout<<',';first=false;
            std::cout<<"{\"pattern\":"<<pattern<<",\"trial\":"<<trial<<",\"legacy_ms\":"<<ta<<",\"fast_ms\":"<<tb<<",\"bytes\":"<<b.size()<<"}";
        }
    }
    std::cout<<"]}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
