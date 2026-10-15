// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "hypercine/filmscan3.h"
#include "hypercine/canonical.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) {
    try {
        bool filmscan3=false;
        if(argc>=3 && std::string(argv[1])=="--profile") {
            const std::string profile=argv[2];
            if(profile!="filmscan3" && profile!="fs3-q2")throw std::invalid_argument("unknown encoding profile");
            filmscan3=profile=="filmscan3";argv+=2;argc-=2;
        }
        if(argc!=3 && argc!=5) throw std::invalid_argument("usage: hypercine_encode_smoke [--profile fs3-q2|filmscan3] flat|gradient|canonical|bgra:input.raw output.cfhd [width height]");
        const std::string name=argv[1];
        auto pattern=hypercine::CanonicalPattern::Flat128;
        if(name=="gradient") pattern=hypercine::CanonicalPattern::GradientChecker;
        else if(name=="canonical") pattern=hypercine::CanonicalPattern::Canonical;
        else if(name!="flat" && name.rfind("bgra:",0)!=0) throw std::invalid_argument("unknown pattern");
        int width=128,height=128;
        if(argc==5) {
            std::size_t consumed=0;
            width=std::stoi(argv[3],&consumed);
            if(consumed!=std::string(argv[3]).size()) throw std::invalid_argument("invalid width");
            height=std::stoi(argv[4],&consumed);
            if(consumed!=std::string(argv[4]).size()) throw std::invalid_argument("invalid height");
        }
        hypercine::standalone::ValidateGeometry(width,height);
        hypercine::standalone::AlignedVector<uint8_t> aligned;
        if(name.rfind("bgra:",0)==0) {
            aligned.resize(static_cast<std::size_t>(width)*height*4);
            std::ifstream input(name.substr(5),std::ios::binary);
            input.read(reinterpret_cast<char*>(aligned.data()),static_cast<std::streamsize>(aligned.size()));
            if(!input || input.peek()!=std::char_traits<char>::eof())
                throw std::invalid_argument("raw BGRA input must contain exactly width*height*4 bytes");
        } else {
            auto frame=hypercine::MakeFrame(width,height,pattern);
            aligned.assign(frame.begin(),frame.end());
        }
        if(filmscan3 && (width!=1920 || height!=1080))throw std::invalid_argument("FilmScan3 currently supports the 1920x1080 oracle domain");
        hypercine::standalone::FilmScan3Encoder fs3;
        auto sample=filmscan3 ? fs3.Encode(aligned.data(),aligned.size(),width*4).sample : hypercine::standalone::EncodeBgra(aligned.data(),aligned.size(),width,height,width*4);
        std::ofstream output(argv[2],std::ios::binary);
        output.write(reinterpret_cast<const char*>(sample.data()),static_cast<std::streamsize>(sample.size()));
        output.close();
        if(!output) throw std::runtime_error("sample write failed");
        std::cout<<name<<": "<<width<<"x"<<height<<" YUV422 p10, "<<sample.size()<<" bytes\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
