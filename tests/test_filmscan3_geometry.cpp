// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "hypercine/canonical.h"
#include "../src/core/filmscan3_backend.h"
#include "reference_frame_generator.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace hypercine::standalone;
namespace {
void Require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
template<class F> void Reject(F f) { bool threw=false;try{f();}catch(const std::exception&){threw=true;}Require(threw,"invalid submission accepted"); }
bool Same(FilmScan3State a,FilmScan3State b) { return a.frame_index==b.frame_index && a.fs_rate_limiter==b.fs_rate_limiter && a.previous_sample_bytes==b.previous_sample_bytes; }
unsigned U16(const std::vector<uint8_t>& s,std::size_t at) { Require(at<=s.size() && s.size()-at>=2,"truncated tag");return (s[at]<<8)|s[at+1]; }
uint32_t U32(const std::vector<uint8_t>& s,std::size_t at) { return (U16(s,at)<<16)|U16(s,at+2); }
void Inspect(const std::vector<uint8_t>& s,int width,int height) {
    Require(U32(s,0)==0x10009 && U32(s,4)==0x20003,"intra index");
    unsigned low=0,high=0,levels=0,geometry=0,channel=0,wavelet=3;
    std::size_t first=0;
    for(std::size_t at=20;at<s.size();) {
        const unsigned raw=U16(s,at),tag=raw>=32768 ? 65536-raw:raw,value=U16(s,at+2);at+=4;
        if(tag==0x4002) { Require(value<=(s.size()-at)/4,"metadata extent");at+=4*value;continue; }
        if(tag==68)first=at;
        if(tag==62){channel=value;Require(channel<3,"channel");}
        if(tag==38){wavelet=value;Require(wavelet>=1 && wavelet<=3,"wavelet");}
        const unsigned cw=channel?width/2:width;
        if(tag==20){++geometry;Require(value==unsigned(width),"width");}
        if(tag==21 || tag==85){++geometry;Require(value==unsigned(height),"height/display height");}
        if(tag==27)Require(value==cw/8,"LL width");
        if(tag==28)Require(value==unsigned(height/8),"LL height");
        if(tag==41 || tag==49)Require(value==(cw>>wavelet),"HP width");
        if(tag==42 || tag==50)Require(value==unsigned(height>>wavelet),"HP height");
        const unsigned type=tag&0xff00;
        if(type==0x2000 || type==0x2100 || type==0x2200) {
            const auto words=((tag&255)<<16)|value;
            Require(words<=(s.size()-at)/4,"chunk extent");const auto end=at+4*words;
            if(type==0x2200)Require(end==s.size(),"sample extent");
            if(type==0x2100)++levels;
            if(type==0x2000) {
                Require(end>=at+8,"short band");
                if(U32(s,at)==0x40f0f){++low;Require(U32(s,end-4)==0x41b4b,"LL end");}
                else {++high;Require(U32(s,at)==0x370000 && U32(s,end-4)==0x380000,"HP framing");}
                at=end;
            }
        }
    }
    Require(geometry==3 && low==3 && high==27 && levels==9,"complete geometry/bands");
    Require(first+U32(s,8)+U32(s,12)+U32(s,16)+20==s.size(),"channel byte index");
}
void Write(const std::filesystem::path& p,const void* data,std::size_t n) {
    std::ofstream f(p,std::ios::binary);f.write(static_cast<const char*>(data),static_cast<std::streamsize>(n));Require(bool(f),"artifact write");
}
void Export(const std::filesystem::path& dir,int index,const AlignedVector<uint8_t>& input,const FilmScan3Frame& frame) {
    if(dir.empty())return;
    const auto stem=dir/ std::to_string(index);
    Write(stem.string()+".bgra",input.data(),input.size());
    Write(stem.string()+".cfhd",frame.sample.data(),frame.sample.size());
    const auto color=ConvertFilmScan3Bgra(input.data(),input.size(),3840,2160,15360);
    std::ofstream f(stem.string()+".reference.yuv",std::ios::binary);
    for(int c:{0,2,1})for(int y=0;y<color[c].height;++y)for(int x=0;x<color[c].width;++x) {
        const auto v=static_cast<uint16_t>(color[c].pixels[y*color[c].stride+x]);f.put(static_cast<char>(v));f.put(static_cast<char>(v>>8));
    }
    Require(bool(f),"reference write");
    std::ofstream report(stem.string()+".json");
    report<<"{\"frame_index\":"<<frame.state.frame_index<<",\"limiter\":"<<frame.state.fs_rate_limiter<<",\"previous_sample_bytes\":"<<frame.state.previous_sample_bytes<<",\"quant\":[";
    for(int i=0;i<36;++i)report<<(i?",":"")<<frame.tables.quant[i];report<<"],\"scale\":[";
    for(int i=0;i<36;++i)report<<(i?",":"")<<frame.tables.scale[i];report<<"],\"coefficient_fnv64\":[";
    for(int i=0;i<30;++i)report<<(i?",":"")<<frame.coefficient_fnv64[i];report<<"]}\n";
}
// Exercise strong exception safety after the full transform has allocated and
// completed; no global allocator switch or unsafe memory pressure is needed.
class Failure final : public detail::Frontend {
    bool oom_;
public:
    explicit Failure(bool oom):oom_(oom){}
    FilmScan3Geometry Geometry() const noexcept override {return {3840,2160};}
    Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {return ConvertFilmScan3Bgra(p,n,w,h,s);}
    WaveletLevel Forward(const Plane& p,int s) override {return ForwardLevel(p,s);}
    std::string DeviceName() const override {return "test failure";}
    detail::FrontendFrame Transform(const uint8_t* p,std::size_t n,int w,int h,std::size_t s,bool audit) override {
        auto result=Frontend::Transform(p,n,w,h,s,audit);
        if(oom_)throw std::bad_alloc();
        // Force a framing validation failure after transformation.
        result.levels[2][0].bands[3].width=1;return result;
    }
};
}
int main(int argc,char** argv) {try {
    const std::filesystem::path dir=argc==3 && std::string(argv[1])=="--export" ? argv[2]:"";
    Require(argc==1 || !dir.empty(),"usage: test_filmscan3_geometry [--export directory]");
    if(!dir.empty())std::filesystem::create_directories(dir);
    for(const auto g:{FilmScan3Geometry{0,2160},{-128,1080},{3841,2160},{3840,2161},{4096,2160},{7680,4320},{2147483520,2147483640}})
        Reject([&]{FilmScan3Encoder invalid(g);});
    Reject([]{ValidateGeometry(3840,2160);}); // legacy cap stays unchanged
    Reject([]{FilmScan3Encoder invalid(FilmScan3Geometry{3840,2160,std::numeric_limits<std::size_t>::max()});});
    Require(!PlaneStorageFits(2147483640,4096),"oversized plane stride accepted");
    Plane large(3840,2160);Require(large.pixels.size()==8294400,"bounded 4K plane");large.pixels.clear();large.pixels.shrink_to_fit();
    AlignedVector<uint8_t> input(3840u*2160u*4);
    auto flat=hypercine::MakeFrame(3840,2160,hypercine::CanonicalPattern::Flat128);
    std::copy(flat.begin(),flat.end(),input.begin());flat.clear();flat.shrink_to_fit();
    FilmScan3Encoder encoder(FilmScan3Geometry{3840,2160});
    auto first=encoder.Encode(input.data(),input.size(),15360);Inspect(first.sample,3840,2160);Export(dir,0,input,first);
    const auto previous=encoder.state();
    Reject([&]{encoder.Encode(nullptr,input.size(),15360);});Reject([&]{encoder.Encode(input.data()+1,input.size()-1,15360);});
    Reject([&]{encoder.Encode(input.data(),input.size()-1,15360);});Reject([&]{encoder.Encode(input.data(),input.size(),15359);});
    Reject([&]{encoder.Encode(input.data(),input.size(),(std::numeric_limits<std::size_t>::max()/16)*16);});
    for(bool oom:{true,false}) {
        detail::FrontendAccess::Use(encoder,std::make_shared<Failure>(oom));
        Reject([&]{encoder.Encode(input.data(),input.size(),15360);});
        Require(Same(previous,encoder.state()),"failed allocation/framing changed state");
    }
    detail::FrontendAccess::Use(encoder,detail::CpuFrontend({3840,2160}));
    FilmScan3Encoder control(FilmScan3Geometry{3840,2160});control.Encode(input.data(),input.size(),15360);
    Require(encoder.Encode(input.data(),input.size(),15360).sample==control.Encode(input.data(),input.size(),15360).sample,"retry after failure");
    encoder.Reset();Require(encoder.geometry().width==3840 && encoder.geometry().height==2160,"Reset changed geometry");
    AlignedVector<uint8_t> padded(15376u*2160u);
    for(int y=0;y<2160;++y)std::memcpy(padded.data()+std::size_t(y)*15376,input.data()+std::size_t(y)*15360,15360);
    Require(encoder.Encode(padded.data(),padded.size(),15376).sample==first.sample,"padded pitch/reset");
    padded.clear();padded.shrink_to_fit();
    auto gradient=hypercine::MakeFrame(3840,2160,hypercine::CanonicalPattern::GradientChecker);
    std::copy(gradient.begin(),gradient.end(),input.begin());gradient.clear();gradient.shrink_to_fit();
    encoder.Reset();auto second=encoder.Encode(input.data(),input.size(),15360);Inspect(second.sample,3840,2160);Export(dir,1,input,second);
    reference_fixture::GeneratePhase3AFrame(3840,2160,0,reference_fixture::Phase3APattern::DeterministicNoise,input.data(),15360,0x1337beef);
    // A small per-session output budget accepts the first flat frame, then
    // rejects a genuine high-entropy 4K packet during writer assembly.
    FilmScan3Encoder bounded(FilmScan3Geometry{3840,2160,first.sample.size()});
    AlignedVector<uint8_t> uniform(input.size(),128);
    for(std::size_t i=3;i<uniform.size();i+=4)uniform[i]=255;
    bounded.Encode(uniform.data(),uniform.size(),15360);
    const auto bounded_previous=bounded.state();
    Reject([&]{bounded.Encode(input.data(),input.size(),15360);});
    Require(Same(bounded_previous,bounded.state()),"sample budget failure changed state");
    FilmScan3Encoder bounded_control(FilmScan3Geometry{3840,2160,first.sample.size()});
    bounded_control.Encode(uniform.data(),uniform.size(),15360);
    Require(bounded.Encode(uniform.data(),uniform.size(),15360).sample==bounded_control.Encode(uniform.data(),uniform.size(),15360).sample,"sample budget retry");
    uniform.clear();uniform.shrink_to_fit();
    encoder.Reset();auto noise=encoder.Encode(input.data(),input.size(),15360);Inspect(noise.sample,3840,2160);Export(dir,2,input,noise);
    FilmScan3Encoder hd;AlignedVector<uint8_t> hdinput(1920u*1080u*4,128);
    for(std::size_t i=3;i<hdinput.size();i+=4)hdinput[i]=255;
    auto hd0=hd.Encode(hdinput.data(),hdinput.size(),7680);
    FilmScan3Encoder explicit_hd(FilmScan3Geometry{});
    Require(explicit_hd.Encode(hdinput.data(),hdinput.size(),7680).sample==hd0.sample,"default constructor compatibility");
    Require(hd.geometry().width==1920 && hd.state().frame_index==1,"independent HD session");
    encoder.Reset();std::set<std::vector<uint8_t>> identities;std::set<int> limiters;
    std::size_t prev=0;
    for(int i=0;i<16;++i) {
        const auto pattern=i%3==0 ? reference_fixture::Phase3APattern::DeterministicNoise : i%3==1 ? reference_fixture::Phase3APattern::HorizontalGradient : reference_fixture::Phase3APattern::Checkerboard;
        reference_fixture::GeneratePhase3AFrame(3840,2160,i,pattern,input.data(),15360,0x1337beef);
        auto frame=encoder.Encode(input.data(),input.size(),15360);Inspect(frame.sample,3840,2160);
        Require(frame.state.frame_index==unsigned(i) && frame.state.previous_sample_bytes==prev,"sequence feedback");
        Require(frame.coefficient_hash==FilmScan3CoefficientHash::Full,"sequence gate requires full coefficient hashing");
        identities.insert(std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(frame.coefficient_fnv64.data()),reinterpret_cast<const uint8_t*>(frame.coefficient_fnv64.data()+30)));
        limiters.insert(frame.state.fs_rate_limiter);prev=frame.sample.size();Export(dir,3+i,input,frame);
    }
    Require(identities.size()==16 && limiters.size()>1,"sequence is not changing coefficients/state");
    Require(hd.state().frame_index==1,"4K session modified HD state");
    std::cout<<"PASS: bounded CPU 4K geometry, 19 packets, 16 changing frames, rollback, Reset, pitch, independent sessions\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
