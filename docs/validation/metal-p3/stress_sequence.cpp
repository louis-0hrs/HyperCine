// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "../../../tests/reference_frame_generator.h"
#include <iostream>
#include <string>
using namespace hypercine::standalone;
int main(){try{
    FilmScan3Encoder cpu(FilmScan3Backend::Cpu),metal(FilmScan3Backend::Metal);
    AlignedVector<uint8_t> input(1920*1080*4);
    for(int i=0;i<1001;++i){
        reference_fixture::GeneratePhase3AFrame(1920,1080,i,
            static_cast<reference_fixture::Phase3APattern>(i%127==0?7:1),input.data(),7680,0x1337beef);
        if(i==500||i==800){cpu.Reset();metal.Reset();}
        auto a=cpu.Encode(input.data(),input.size(),7680);
        auto b=metal.Encode(input.data(),input.size(),7680);
        if(a.sample!=b.sample||a.coefficient_fnv64!=b.coefficient_fnv64||a.tables.quant!=b.tables.quant||a.tables.scale!=b.tables.scale)
            throw std::runtime_error("stress CPU/Metal mismatch frame "+std::to_string(i));
    }
    std::cout<<"{\"frames_per_encoder\":1001,\"independent_instances\":2,\"resets\":[500,800],\"raw_packets_and_30_band_hashes\":\"PASS\"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
