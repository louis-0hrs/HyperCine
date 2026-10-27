// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Standalone reproduction of the extended issue #44 / #48 fail-closed audit.
#include "hypercine/filmscan3.h"
#include <iostream>
#include <stdexcept>
using namespace hypercine::standalone;
uint64_t Mul(uint64_t a,uint64_t b) {
    if(b && a>std::numeric_limits<uint64_t>::max()/b)throw std::overflow_error("multiply");
    return a*b;
}
uint64_t Add(uint64_t a,uint64_t b) {
    if(a>std::numeric_limits<uint64_t>::max()-b)throw std::overflow_error("add");
    return a+b;
}
int main() {
    try {
        bool multiplication=false,addition=false;
        try {Mul(std::numeric_limits<uint64_t>::max(),2);}catch(const std::overflow_error&){multiplication=true;}
        try {Add(std::numeric_limits<uint64_t>::max(),1);}catch(const std::overflow_error&){addition=true;}
        if(!multiplication || !addition)throw std::runtime_error("checked audit arithmetic");
        FilmScan3Encoder hd;const auto before=hd.state();
        std::cout<<"{\"arithmetic\":\"checked uint64 multiplication/addition; no future buffers allocated\",\"cases\":[";
        bool first=true;
        for(auto g:{FilmScan3Geometry{3840,2160},{7680,4320},{12288,8040},{17520,8040}}) {
            bool accepted=false;
            try {FilmScan3Encoder trial(g);accepted=true;}catch(const std::invalid_argument&){}catch(const std::length_error&){}
            if(accepted!=(g.width==3840))throw std::runtime_error("unexpected geometry policy");
            if(hd.state().frame_index!=before.frame_index || hd.state().fs_rate_limiter!=before.fs_rate_limiter || hd.state().previous_sample_bytes!=before.previous_sample_bytes)
                throw std::runtime_error("independent session changed");
            const auto p=Mul(g.width,g.height);
            if(!first)std::cout<<',';first=false;
            std::cout<<"{\"width\":"<<g.width<<",\"height\":"<<g.height<<",\"accepted\":"<<(accepted?"true":"false")
                     <<",\"width_mod_128\":"<<g.width%128<<",\"width_mod_16\":"<<g.width%16
                     <<",\"ll3_chroma_width\":"<<g.width/16<<",\"ll3_chroma_width_mod_8\":"<<(g.width/16)%8
                     <<",\"sample_bound_bytes\":"<<Add(Mul(p,8),16384)<<",\"conservative_live_bytes\":"<<Add(Mul(p,36),1048576)
                     <<",\"rate_numerator_before_div8\":"<<Mul(p,30)<<",\"session_unchanged\":true}";
        }
        std::cout<<"]}\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
