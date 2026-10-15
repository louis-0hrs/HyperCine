// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../src/core/filmscan3_backend.h"
#include <iostream>
using namespace hypercine::standalone;
std::shared_ptr<detail::Frontend> Missing(){throw std::runtime_error("injected unavailable device/pipeline");}
int main(){try{
    if(detail::SelectFrontend(FilmScan3Backend::Cpu)!=detail::CpuFrontend() ||
       detail::SelectFrontend(FilmScan3Backend::MetalOrCpu,Missing)!=detail::CpuFrontend())
        throw std::runtime_error("CPU selection/fallback failed");
    bool failed=false;try{detail::SelectFrontend(FilmScan3Backend::Metal,Missing);}catch(const std::runtime_error&){failed=true;}
    if(!failed)throw std::runtime_error("GPU required silently fell back");
    AlignedVector<uint8_t> input(1920*1080*4,128);
    FilmScan3Encoder cpu,selected(FilmScan3Backend::Cpu),fallback;
    detail::FrontendAccess::Use(fallback,detail::SelectFrontend(FilmScan3Backend::MetalOrCpu,Missing));
    const auto a=cpu.Encode(input.data(),input.size(),7680);
    if(selected.Encode(input.data(),input.size(),7680).sample!=a.sample || fallback.Encode(input.data(),input.size(),7680).sample!=a.sample)
        throw std::runtime_error("selected CPU packet changed");
    std::cout<<"PASS CPU default/selection, injected unavailable device/pipeline fallback, required-GPU failure\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
