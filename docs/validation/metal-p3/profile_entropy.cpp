// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Diagnostic only: split-pass staging intentionally differs from production.
#include "../../../tests/entropy_legacy.h"
#include "../../../tests/reference_frame_generator.h"
#include "hypercine/filmscan3.h"
#include <chrono>
#include <iostream>
using namespace hypercine::standalone;
using Clock=std::chrono::steady_clock;
double Elapsed(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
struct Symbol {unsigned run;int value;detail::Code code;};
int main(){try{
    AlignedVector<uint8_t> input(1920*1080*4);
    std::cout<<"{\"method\":\"diagnostic split passes, one CPU thread, not production timings\",\"bands\":[";
    bool first=true;
    for(int pattern:{1,3,7}) {
        reference_fixture::GeneratePhase3AFrame(1920,1080,7,static_cast<reference_fixture::Phase3APattern>(pattern),input.data(),7680,0x1337beef);
        auto colors=ConvertFilmScan3Bgra(input.data(),input.size(),1920,1080,7680);
        auto tables=ComputeFilmScan3Tables(0);
        for(int c=0;c<3;++c){
            auto l1=ForwardLevel(colors[c],0);auto l2=ForwardLevel(l1.bands[0],2);auto l3=ForwardLevel(l2.bands[0],0);
            WaveletLevel* levels[]={&l1,&l2,&l3};
            for(int lv=2;lv>=0;--lv)for(int b=1;b<4;++b){
                auto& p=levels[lv]->bands[b];for(auto& v:p.pixels)v=QuantizeFilmScan3(v,tables.quant[c*12+lv*4+b]);
                auto t=Clock::now();auto actual=PackHighpass(p,4*(p.width*p.height+1));double total=Elapsed(t);
                auto reference=legacy::PackHighpass(p,actual.size());if(actual!=reference)throw std::runtime_error("profile oracle mismatch");
                std::vector<Symbol> symbols;symbols.reserve(p.width*p.height);
                std::vector<detail::Code> tokens;tokens.reserve(2*p.width*p.height+1);
                unsigned run=0;t=Clock::now();
                for(int y=0;y<p.height;++y)for(int x=0;x<p.width;++x){int v=p.pixels[y*p.stride+x];if(!v)++run;else{symbols.push_back({run,v,{0,0}});run=0;}}
                double classify=Elapsed(t);
                t=Clock::now();std::array<unsigned,1024> cubic{};
                for(unsigned i=1;i<256;++i)cubic[std::min(1023u,i+unsigned((uint64_t{768}*i*i*i)/(256u*256u*256u)))]=i;
                unsigned prev=0;for(auto& v:cubic){if(v)prev=v;else v=prev;}
                for(auto& s:symbols){auto m=unsigned(s.value<0?-s.value:s.value);s.code=detail::magnitude17[cubic[std::min(1023u,m)]];s.code.bits=(s.code.bits<<1)|(s.value<0);++s.code.size;}
                double mapping=Elapsed(t);t=Clock::now();
                auto flush=[&](unsigned n){for(const auto& r:detail::runs17)while(n>=r.count){tokens.push_back(r.code);n-=r.count;}};
                for(const auto& s:symbols){flush(s.run);tokens.push_back(s.code);}flush(run);tokens.push_back(detail::band_end17);
                double rle=Elapsed(t);t=Clock::now();legacy::Bits bits(actual.size());for(auto code:tokens)bits.Put(code.bits,code.size);bits.Finish();double packing=Elapsed(t);
                if(bits.bytes!=reference)throw std::runtime_error("split-pass mismatch");
                if(!first)std::cout<<',';first=false;
                std::cout<<"{\"pattern\":"<<pattern<<",\"channel\":"<<c<<",\"level\":"<<lv<<",\"band\":"<<b<<",\"coefficients\":"<<p.width*p.height<<",\"nonzero\":"<<symbols.size()<<",\"bytes\":"<<actual.size()<<",\"production_ms\":"<<total<<",\"classify_ms\":"<<classify<<",\"cubic_mapping_ms\":"<<mapping<<",\"run_emission_ms\":"<<rle<<",\"legacy_bitpack_ms\":"<<packing<<",\"diagnostic_vector_reserved_bytes\":"<<symbols.capacity()*sizeof(Symbol)+tokens.capacity()*sizeof(detail::Code)<<"}";
            }
        }
    }
    std::cout<<"]}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
