// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "wavelet_simd_frontend.h"
#include "reference_frame_generator.h"
#include <fstream>
#include <iostream>
#include <iterator>
using namespace hypercine::standalone;
using hypercine::detail::CpuIsa;
int main(int argc,char** argv) { try {
    if(argc!=2) return 2;
    FilmScan3Encoder scalar,avx2,avx512;
    const bool has2=hypercine::detail::WaveletSimdAvailable(CpuIsa::Avx2);
    const bool has512=hypercine::detail::WaveletSimdAvailable(CpuIsa::Avx512);
    if(has2) detail::FrontendAccess::Use(avx2,std::make_shared<hypercine_test::SimdFrontend>(CpuIsa::Avx2));
    if(has512) detail::FrontendAccess::Use(avx512,std::make_shared<hypercine_test::SimdFrontend>(CpuIsa::Avx512));
    AlignedVector<uint8_t> bgra(1920*1080*4);
    for (int frame=0;frame<16;++frame) {
        using namespace reference_fixture;
        const Phase3APattern patterns[]={Phase3APattern::FlatBlack,Phase3APattern::HorizontalGradient,Phase3APattern::DeterministicNoise};
        GeneratePhase3AFrame(1920,1080,frame,patterns[frame%3],bgra.data(),1920*4,0x1337beef);
        const auto reference=scalar.Encode(bgra.data(),bgra.size(),1920*4);
        if(frame==0) {
            std::ifstream file(argv[1],std::ios::binary);
            if(!file) throw std::runtime_error("missing frozen FS3 packet");
            const std::vector<uint8_t> golden{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
            if(reference.sample!=golden) throw std::runtime_error("frozen flat packet changed");
        }
        for(auto* encoder:{has2?&avx2:nullptr,has512?&avx512:nullptr}) if(encoder) {
            const auto result=encoder->Encode(bgra.data(),bgra.size(),1920*4);
            if(result.sample!=reference.sample || result.coefficient_fnv64!=reference.coefficient_fnv64 ||
                result.tables.quant!=reference.tables.quant || encoder->state().frame_index!=scalar.state().frame_index ||
                encoder->state().fs_rate_limiter!=scalar.state().fs_rate_limiter ||
                encoder->state().previous_sample_bytes!=scalar.state().previous_sample_bytes)
                throw std::runtime_error("stateful FS3 packet/coefficient/state mismatch");
        }
    }
    std::cout<<"PASS: 16 ordered full FS3 frames; frozen first packet; AVX2="<<has2<<" AVX512="<<has512<<'\n';
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
