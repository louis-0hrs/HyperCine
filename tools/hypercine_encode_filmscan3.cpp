// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "hypercine/quality_state.h"
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
#include <limits>
namespace fs=std::filesystem;
using namespace hypercine::standalone;
using hypercine::EncodingQuality;
using hypercine::EncodingQualityName;
namespace {
// Strict WIDTHxHEIGHT. std::stoi stops at the first unparsable character, which
// would silently accept "3840x2160junk" or " 3840x 2160"; every character must be
// consumed. Range is checked here so an overflow cannot wrap into a valid value,
// and the geometry contract is validated before it is ever used.
int ParseDimension(const std::string& text,const char* what) {
    if(text.empty())throw std::invalid_argument(std::string("--geometry ")+what+" is missing");
    for(char c:text)if(c<'0'||c>'9')throw std::invalid_argument(std::string("--geometry ")+what+" must be decimal digits");
    if(text.size()>9)throw std::invalid_argument(std::string("--geometry ")+what+" is out of range");
    long long value=0;
    for(char c:text)value=value*10+(c-'0');
    if(value<=0||value>std::numeric_limits<int>::max())
        throw std::invalid_argument(std::string("--geometry ")+what+" is out of range");
    return static_cast<int>(value);
}
FilmScan3Geometry ParseGeometry(const std::string& value) {
    const auto at=value.find('x');
    if(at==std::string::npos||value.find('x',at+1)!=std::string::npos)
        throw std::invalid_argument("--geometry expects exactly WIDTHxHEIGHT");
    FilmScan3Geometry geometry;
    geometry.width=ParseDimension(value.substr(0,at),"width");
    geometry.height=ParseDimension(value.substr(at+1),"height");
    ValidateFilmScan3Geometry(geometry);
    return geometry;
}
}
int main(int argc,char** argv) {
    try {
        int begin=1;std::ifstream metadata;bool reference_yuv=false;bool coefficient_hashing=true;
        // The session profile is explicit rather than implied: a caller that
        // asks for another profile must get that profile or a refusal.
        EncodingQuality quality=EncodingQuality::FilmScan3;
        // Default stays 1080p so every existing caller keeps its exact contract.
        FilmScan3Geometry geometry{};
        while(begin<argc && std::string(argv[begin]).rfind("--",0)==0) {
            const std::string flag=argv[begin++];
            if(flag=="--reference-yuv")reference_yuv=true;
            else if(flag=="--geometry" && begin<argc) {
                geometry=ParseGeometry(argv[begin++]);
            }
            else if(flag=="--coefficient-hash" && begin<argc) {
                const std::string mode=argv[begin++];
                if(mode=="full") coefficient_hashing=true;
                else if(mode=="off") coefficient_hashing=false;
                else throw std::invalid_argument("--coefficient-hash expects full or off");
            }
            else if(flag=="--quality" && begin<argc) {
                const std::string name=argv[begin++];
                if(name=="low")quality=EncodingQuality::Low;
                else if(name=="medium")quality=EncodingQuality::Medium;
                else if(name=="high")quality=EncodingQuality::High;
                else if(name=="filmscan1")quality=EncodingQuality::FilmScan1;
                else if(name=="filmscan2")quality=EncodingQuality::FilmScan2;
                else if(name=="filmscan3")quality=EncodingQuality::FilmScan3;
                else throw std::invalid_argument("--quality expects one of low, medium, high, filmscan1, filmscan2, filmscan3");
            }
            else if(flag=="--metadata-binary" && begin<argc) {
                metadata.open(argv[begin++],std::ios::binary);if(!metadata)throw std::runtime_error("cannot read explicit metadata");
            } else throw std::invalid_argument("unknown or incomplete FilmScan3 option");
        }
        if(argc-begin<2)throw std::invalid_argument("usage: hypercine_encode_filmscan3 [--geometry WIDTHxHEIGHT] [--quality PROFILE] [--metadata-binary values.raw] [--coefficient-hash full|off] OUTPUT_DIR INPUT.bgra [INPUT.bgra ...]");
        const fs::path directory=argv[begin++];fs::create_directories(directory);
        std::ofstream trace(directory/"trace.json");trace<<"{\"profile\":\""<<EncodingQualityName(quality)<<"\",\"frames\":[";
        FilmScan3Encoder encoder(geometry);
        encoder.SetCoefficientHashing(coefficient_hashing);
        encoder.SetSessionQuality(quality);
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
                 <<",\"previous_sample_bytes\":"<<result.state.previous_sample_bytes
                 <<",\"fs_overbitrate\":"<<result.state.fs_overbitrate
                 <<",\"sample_bytes\":"<<result.sample.size()<<",\"quant\":[";
            for(unsigned k=0;k<36;++k){if(k)trace<<',';trace<<result.tables.quant[k];}trace<<"],\"scale\":[";
            for(unsigned k=0;k<36;++k){if(k)trace<<',';trace<<result.tables.scale[k];}trace<<"],\"coefficient_hash\":\""<<FilmScan3CoefficientHashName(result.coefficient_hash)<<"\"";
            if(result.coefficient_hash!=FilmScan3CoefficientHash::Full) trace<<",\"coefficient_fnv64\":null}";
            else {trace<<",\"coefficient_fnv64\":[";
            for(unsigned k=0;k<30;++k){if(k)trace<<',';trace<<'"'<<std::hex<<std::setw(16)<<std::setfill('0')<<result.coefficient_fnv64[k]<<'"'<<std::dec;}trace<<"]}";}
        }
        if(metadata.is_open() && metadata.peek()!=std::char_traits<char>::eof())throw std::runtime_error("excess explicit variable metadata");
        trace<<"]}\n";trace.close();if(!trace)throw std::runtime_error("trace write failed");
#ifdef HYPERCINE_METAL_AUDIT
        audit->Report();
#endif
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
