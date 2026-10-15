// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "hypercine/canonical.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("usage: hypercine_encode_smoke flat|gradient|canonical output.cfhd");
        const std::string name=argv[1];
        auto pattern=hypercine::CanonicalPattern::Flat128;
        if(name=="gradient") pattern=hypercine::CanonicalPattern::GradientChecker;
        else if(name=="canonical") pattern=hypercine::CanonicalPattern::Canonical;
        else if(name!="flat") throw std::invalid_argument("unknown pattern");
        auto frame=hypercine::MakeFrame(128,128,pattern);
        hypercine::standalone::AlignedVector<uint8_t> aligned(frame.begin(),frame.end());
        auto sample=hypercine::standalone::EncodeBgra128(aligned.data(),aligned.size(),512);
        std::ofstream output(argv[2],std::ios::binary);
        output.write(reinterpret_cast<const char*>(sample.data()),static_cast<std::streamsize>(sample.size()));
        output.close();
        if(!output) throw std::runtime_error("sample write failed");
        std::cout<<name<<": 128x128 YUV422 p10, "<<sample.size()<<" bytes\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
