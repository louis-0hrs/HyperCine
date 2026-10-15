// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "reference_frame_generator.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
using namespace hypercine::standalone;
void Require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
template<class F>void Reject(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}Require(rejected,"invalid input accepted");}
int main(){try{
    Require(QuantizeFilmScan3(5,6)==1 && QuantizeFilmScan3(-5,6)==-1,"FS3 midpoint signed rounding");
    Require(Quantize(5,6)==0,"legacy quantization changed");
    Reject([]{QuantizeFilmScan3(1,0);});Reject([]{QuantizeFilmScan3(1,32769);});
    Reject([]{ComputeFilmScan3Tables(-1);});Reject([]{ComputeFilmScan3Tables(21);});
    const auto tables=ComputeFilmScan3Tables(4);
    const unsigned luma[]={1,6,6,9,1,4,4,2,1,16,16,8};
    const unsigned chroma[]={1,6,6,12,1,4,4,2,1,16,16,8};
    for(unsigned i=0;i<36;++i)Require(tables.quant[i]==(i<12?luma[i]:chroma[i%12]),"golden initial quant tables");
    for(unsigned i=0;i<36;++i)Require(ComputeFilmScan3Tables(20).quant[i]==ComputeFilmScan3Tables(16).quant[i],"FS3 limiter quant clamp");
    AlignedVector<uint32_t> input(1920*1080);
    reference_fixture::GeneratePhase3AFrame(1920,1080,0,reference_fixture::Phase3APattern::FlatBlack,reinterpret_cast<uint8_t*>(input.data()),7680,0x1337beef);
    auto* data=reinterpret_cast<uint8_t*>(input.data());const auto size=input.size()*4;
    FilmScan3Encoder encoder;
    Reject([&]{encoder.Encode(nullptr,size,7680);});Reject([&]{encoder.Encode(data,size-1,7680);});Reject([&]{encoder.Encode(data+1,size-1,7680);});
    Reject([&]{encoder.Encode(data,size,7679);});Reject([&]{encoder.Encode(data,size,(std::numeric_limits<std::size_t>::max()/16)*16);});
    Require(encoder.state().frame_index==0 && encoder.state().previous_sample_bytes==0,"failure mutated encoder state");
    auto first=encoder.Encode(data,size,7680);Require(first.state.frame_index==0 && first.state.fs_rate_limiter==4,"initial state");
    Require(first.sample.size()==185648,"deterministic reference sample size");
    AlignedVector<uint8_t> padded(7696*1080);
    for(int y=0;y<1080;++y)std::memcpy(padded.data()+static_cast<std::size_t>(y)*7696,data+static_cast<std::size_t>(y)*7680,7680);
    FilmScan3Encoder pitched;Require(pitched.Encode(padded.data(),padded.size(),7696).sample==first.sample,"padded stride changed packet");
    auto second=encoder.Encode(data,size,7680);Require(second.state.previous_sample_bytes==first.sample.size() && second.state.fs_rate_limiter==0,"autonomous flat frame rate-control feedback");
    const auto previous=encoder.state();Reject([&]{encoder.Encode(data,0,7680);});
    Require(encoder.state().frame_index==previous.frame_index && encoder.state().fs_rate_limiter==previous.fs_rate_limiter && encoder.state().previous_sample_bytes==previous.previous_sample_bytes,"failure advanced dynamic state");
    encoder.Reset();Require(encoder.Encode(data,size,7680).sample==first.sample,"Reset not deterministic");
    std::cout<<"PASS: production FS3 API arithmetic, initial tables, feedback, invalid storage and reset\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
