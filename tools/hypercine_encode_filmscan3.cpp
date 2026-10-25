// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#ifdef HYPERCINE_METAL_AUDIT
#include "../tests/metal_audit.h"
#endif
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
namespace fs=std::filesystem;
using namespace hypercine::standalone;
int main(int argc,char** argv) {
    try {
        int begin=1;std::ifstream metadata;bool reference_yuv=false;
        // Default stays 1080p so every existing caller keeps its exact contract.
        FilmScan3Geometry geometry{};
        while(begin<argc && std::string(argv[begin]).rfind("--",0)==0) {
            const std::string flag=argv[begin++];
            if(flag=="--reference-yuv")reference_yuv=true;
            else if(flag=="--geometry" && begin<argc) {
                const std::string value=argv[begin++];
                const auto at=value.find('x');
                if(at==std::string::npos)throw std::invalid_argument("--geometry expects WIDTHxHEIGHT");
                try {
                    geometry.width=std::stoi(value.substr(0,at));
                    geometry.height=std::stoi(value.substr(at+1));
                } catch(const std::exception&) { throw std::invalid_argument("--geometry expects WIDTHxHEIGHT"); }
                ValidateFilmScan3Geometry(geometry);
            }
            else if(flag=="--metadata-binary" && begin<argc) {
                metadata.open(argv[begin++],std::ios::binary);if(!metadata)throw std::runtime_error("cannot read explicit metadata");
            } else throw std::invalid_argument("unknown or incomplete FilmScan3 option");
        }
        if(argc-begin<2)throw std::invalid_argument("usage: hypercine_encode_filmscan3 [--geometry WIDTHxHEIGHT] [--metadata-binary values.raw] OUTPUT_DIR INPUT.bgra [INPUT.bgra ...]");
        const fs::path directory=argv[begin++];fs::create_directories(directory);
        std::ofstream trace(directory/"trace.json");trace<<"{\"profile\":\"FilmScan3\",\"frames\":[";
        FilmScan3Encoder encoder(geometry);
        const std::size_t frame_bytes=static_cast<std::size_t>(geometry.width)*geometry.height*4;
        const std::size_t stride=static_cast<std::size_t>(geometry.width)*4;
#ifdef HYPERCINE_METAL_AUDIT
        auto audit=std::make_shared<metal_test::Audit>(geometry);
        detail::FrontendAccess::Use(encoder,audit);
        std::cout<<"Required Metal device: "<<audit->DeviceName()<<'\n';
#endif
        for(int i=begin;i<argc;++i) {
            AlignedVector<uint8_t> input(frame_bytes);
            std::ifstream source(argv[i],std::ios::binary);source.read(reinterpret_cast<char*>(input.data()),input.size());
            if(!source || source.peek()!=std::char_traits<char>::eof())
                throw std::runtime_error("input must contain exactly "+std::to_string(geometry.width)+"x"+std::to_string(geometry.height)+" BGRA bytes");
            FilmScan3Metadata values;
            if(metadata.is_open()) {
                metadata.read(reinterpret_cast<char*>(values.guid.data()),values.guid.size());
                metadata.read(values.date.data(),values.date.size());metadata.read(values.time.data(),values.time.size());metadata.read(values.timecode.data(),values.timecode.size());
                if(!metadata)throw std::runtime_error("truncated explicit variable metadata");
            } else {
                const unsigned f=encoder.state().frame_index;
                const unsigned parts[]={f/(30*60*60)%24,f/(30*60)%60,f/30%60,f%30};
                for(unsigned p=0;p<4;++p){values.timecode[3*p]=static_cast<char>('0'+parts[p]/10);values.timecode[3*p+1]=static_cast<char>('0'+parts[p]%10);}
            }
            if(reference_yuv) {
                const auto planes=ConvertFilmScan3Bgra(input.data(),input.size(),geometry.width,geometry.height,stride);
                std::vector<uint8_t> bytes;bytes.reserve(frame_bytes);
                for(int c:{0,2,1}) {const auto& p=planes[c];for(int y=0;y<p.height;++y)for(int x=0;x<p.width;++x){
                    const auto value=static_cast<uint16_t>(p.pixels[y*p.stride+x]);bytes.push_back(static_cast<uint8_t>(value));bytes.push_back(static_cast<uint8_t>(value>>8));}}
                std::ostringstream filename;filename<<"frame_"<<std::setfill('0')<<std::setw(4)<<encoder.state().frame_index<<".reference.yuv";
                std::ofstream f(directory/filename.str(),std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());f.close();if(!f)throw std::runtime_error("reference plane write failed");
            }
            auto result=encoder.Encode(input.data(),input.size(),stride,values);
            std::ostringstream name;name<<"frame_"<<std::setfill('0')<<std::setw(4)<<result.state.frame_index<<".cfhd";
            std::ofstream output(directory/name.str(),std::ios::binary);output.write(reinterpret_cast<const char*>(result.sample.data()),result.sample.size());output.close();
            if(!output)throw std::runtime_error("sample write failed");
            if(i!=begin)trace<<',';
            trace<<"{\"frame_index\":"<<result.state.frame_index<<",\"fs_rate_limiter\":"<<result.state.fs_rate_limiter
                 <<",\"previous_sample_bytes\":"<<result.state.previous_sample_bytes<<",\"sample_bytes\":"<<result.sample.size()<<",\"quant\":[";
            for(unsigned k=0;k<36;++k){if(k)trace<<',';trace<<result.tables.quant[k];}trace<<"],\"scale\":[";
            for(unsigned k=0;k<36;++k){if(k)trace<<',';trace<<result.tables.scale[k];}trace<<"],\"coefficient_fnv64\":[";
            for(unsigned k=0;k<30;++k){if(k)trace<<',';trace<<'"'<<std::hex<<std::setw(16)<<std::setfill('0')<<result.coefficient_fnv64[k]<<'"'<<std::dec;}trace<<"]}";
        }
        if(metadata.is_open() && metadata.peek()!=std::char_traits<char>::eof())throw std::runtime_error("excess explicit variable metadata");
        trace<<"]}\n";trace.close();if(!trace)throw std::runtime_error("trace write failed");
#ifdef HYPERCINE_METAL_AUDIT
        audit->Report();
#endif
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
