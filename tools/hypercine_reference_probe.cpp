// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "../tests/reference_frame_generator.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>
using namespace hypercine::standalone;
namespace fs = std::filesystem;
namespace {
int Number(const char* s) {
    std::size_t n = 0;
    int v = std::stoi(s, &n);
    if (n != std::string(s).size() || v < 0) throw std::invalid_argument("invalid nonnegative integer");
    return v;
}
void Save(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("cannot write " + path.string());
}
// Frozen CG709 conversion semantics from hybrid_kernels.cu, kept SDK-free.
// Explicit signed floors and uint16 wrapping reproduce the documented SIMD
// arithmetic without platform intrinsics or implementation-defined shifts.
int FloorDivide(int v, int divisor) { return v >= 0 ? v/divisor : -((-v+divisor-1)/divisor); }
int Sat(int v) { return std::clamp(v,-32768,32767); }
int ReferenceChannel(int r, int g, int b, int cr, int cg, int cb, int offset) {
    int v=FloorDivide((r<<7)*cr,65536);
    v=Sat(v+FloorDivide((g<<7)*cg,65536));
    v=Sat(v+FloorDivide((b<<7)*cb,65536));
    const auto wrapped=static_cast<uint16_t>(static_cast<uint16_t>(v)*4u);
    v=wrapped<=32767 ? wrapped : static_cast<int>(wrapped)-65536;
    v=Sat(v+offset); v=Sat(v+16383);
    const auto u=static_cast<uint16_t>(v);
    const unsigned nonnegative=u<16383u ? 0u : static_cast<unsigned>(u)-16383u;
    return static_cast<uint16_t>(nonnegative*4u)>>6;
}
Planes ReferenceConvert(const AlignedVector<uint8_t>& input, int width, int height) {
    Planes out{Plane(width,height),Plane(width/2,height),Plane(width/2,height)};
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        const auto* p=input.data()+(static_cast<std::size_t>(height-1-y)*width+x)*4;
        const int b=p[0],g=p[1],r=p[2];
        out[0].pixels[y*out[0].stride+x]=static_cast<int16_t>(ReferenceChannel(r,g,b,1499,5029,507,1024));
        if (!(x&1)) {
            out[1].pixels[y*out[1].stride+x/2]=static_cast<int16_t>(ReferenceChannel(r,g,b,3596,-3268,-327,8192));
            out[2].pixels[y*out[2].stride+x/2]=static_cast<int16_t>(ReferenceChannel(r,g,b,-827,-2768,3596,8192));
        }
    }
    return out;
}
int16_t ReferenceQuantize(int16_t v, unsigned q) {
    if(q==1) return v;
    const unsigned magnitude=v<0 ? -static_cast<int>(v):v;
    const unsigned midpoint=q/2-1; // default pre-emphasis=0 -> midpoint divisor 2
    const int result=static_cast<int>(((magnitude+midpoint)*(65536u/q))/65536u);
    return static_cast<int16_t>(Sat(v<0 ? -result:result));
}
std::vector<uint8_t> Tight(const Plane& p, bool big = false) {
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(p.width)*p.height*2);
    for(int y=0;y<p.height;++y) for(int x=0;x<p.width;++x) {
        const auto v=static_cast<uint16_t>(p.pixels[y*p.stride+x]);
        bytes.push_back(static_cast<uint8_t>(big ? v>>8 : v));
        bytes.push_back(static_cast<uint8_t>(big ? v : v>>8));
    }
    return bytes;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 7) throw std::invalid_argument(
            "usage: hypercine_reference_probe generate W H INDEX PATTERN OUTPUT\n"
            "   or: hypercine_reference_probe diagnose W H INPUT OUTPUT_DIR QUANT_PLAN\n"
            "QUANT_PLAN: 30 divisors in channel/subband order; diagnostic replay only, no SDK profile");
        const std::string mode=argv[1];
        const int width=Number(argv[2]), height=Number(argv[3]);
        ValidateGeometry(width,height);
        if (mode=="generate") {
            const int index=Number(argv[4]), pattern=Number(argv[5]);
            if (index>65535 || pattern>10) throw std::invalid_argument("frame index/pattern out of bounds");
            AlignedVector<uint32_t> words(static_cast<std::size_t>(width)*height);
            reference_fixture::GeneratePhase3AFrame(width,height,pattern,
                static_cast<reference_fixture::Phase3APattern>(pattern),
                reinterpret_cast<uint8_t*>(words.data()),width*4,0x1337BEEFu);
            // VerifyPhase3ASequence120 generates each archetype at index=pattern,
            // then copies it and stamps the sequence index; the pixels are not
            // regenerated at the sequence index. This distinction is hash-critical.
            reference_fixture::StampFrameIdentityBlocks(reinterpret_cast<uint8_t*>(words.data()),
                width,height,width*4,index,static_cast<reference_fixture::Phase3APattern>(pattern));
            // Serialize explicitly as BGRA little endian even on a big endian host.
            std::vector<uint8_t> bytes;
            bytes.reserve(words.size()*4);
            for(auto v:words) for(unsigned shift=0;shift<32;shift+=8)
                bytes.push_back(static_cast<uint8_t>(v>>shift));
            Save(argv[6],bytes);
        } else if (mode=="diagnose" || mode=="diagnose-sdk-replay") {
            AlignedVector<uint8_t> input(static_cast<std::size_t>(width)*height*4);
            std::ifstream f(argv[4],std::ios::binary);
            f.read(reinterpret_cast<char*>(input.data()),static_cast<std::streamsize>(input.size()));
            if (!f || f.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("input length mismatch");
            std::array<unsigned,30> quant{};
            std::ifstream plan(argv[6]);
            for(auto& q:quant) if (!(plan>>q) || q<1 || q>32768) throw std::runtime_error("invalid quant plan");
            std::string extra;
            if (plan>>extra) throw std::runtime_error("extra quant plan entries");
            const fs::path dir(argv[5]); fs::create_directories(dir);
            const bool sdk_replay=mode=="diagnose-sdk-replay";
            auto planes=sdk_replay ? ReferenceConvert(input,width,height) : ConvertBgra(input.data(),input.size(),width,height,width*4);
            for(int c=0;c<3;++c) {
                const std::string prefix="c"+std::to_string(c);
                Save(dir/(prefix+"-converted.i16le"),Tight(planes[c]));
                auto l1=ForwardLevel(planes[c],0), l2=ForwardLevel(l1.bands[0],2), l3=ForwardLevel(l2.bands[0],0);
                WaveletLevel* levels[]={&l1,&l2,&l3};
                for(int lv=0;lv<3;++lv) for(int b=0;b<4;++b)
                    Save(dir/(prefix+"-l"+std::to_string(lv+1)+"-b"+std::to_string(b)+".i16le"),Tight(levels[lv]->bands[b]));
                int sub=0;
                auto dump=[&](Plane& p) {
                    const int sb=sub++;
                    for(auto& v:p.pixels) v=sdk_replay ? ReferenceQuantize(v,quant[c*10+sb]) : Quantize(v,quant[c*10+sb]);
                    const auto name=prefix+"-s"+std::to_string(sb);
                    const auto coefficients=Tight(p);
                    Save(dir/(name+".i16le"),coefficients);
                    uint64_t fnv=14695981039346656037ull;
                    for(auto byte:coefficients) { fnv^=byte; fnv*=1099511628211ull; }
                    std::ofstream hash(dir/(name+".fnv64"));
                    hash<<std::hex<<std::setfill('0')<<std::setw(16)<<fnv;
                    if (!hash) throw std::runtime_error("cannot write coefficient hash");
                    Save(dir/(name+".payload"),sb==0 ? Tight(p,true) : PackHighpass(p,4*(static_cast<std::size_t>(p.width)*p.height+1)));
                };
                dump(l3.bands[0]);
                for(int lv=2;lv>=0;--lv) for(int b=1;b<4;++b) dump(levels[lv]->bands[b]);
            }
        } else throw std::invalid_argument("unknown mode");
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
