// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Issue #383: the native planes-domain entry must produce the same session,
// tables, rate state and packet bytes as the BGRA entry when the planes are the
// documented conversion of that BGRA frame -- and it must refuse everything the
// converted path cannot produce. No decoder is claimed here.
#include "hypercine/filmscan3.h"
#include "../src/core/filmscan3_backend.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace hypercine::standalone;
namespace {
void Require(bool ok,const std::string& message) { if(!ok)throw std::runtime_error(message); }
template<class F> void Reject(F f,const std::string& message) {
    bool threw=false;
    try{f();}catch(const std::exception&){threw=true;}
    Require(threw,message);
}
AlignedVector<uint8_t> BgraFrame(int width,int height) {
    AlignedVector<uint8_t> bytes(static_cast<std::size_t>(width)*height*4);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const auto at=(static_cast<std::size_t>(y)*width+x)*4;
        bytes[at+0]=static_cast<uint8_t>((x*7+y*13+((x*y)>>3))&255);
        bytes[at+1]=static_cast<uint8_t>((x*3+y*5+17)&255);
        bytes[at+2]=static_cast<uint8_t>((x*11+y*2+91)&255);
        bytes[at+3]=255;
    }
    return bytes;
}
Planes Converted(const AlignedVector<uint8_t>& input,int width,int height) {
    return ConvertFilmScan3Bgra(input.data(),input.size(),width,height,
                                static_cast<std::size_t>(width)*4);
}
bool SameState(const FilmScan3State& a,const FilmScan3State& b) {
    return a.frame_index==b.frame_index && a.fs_rate_limiter==b.fs_rate_limiter &&
           a.previous_sample_bytes==b.previous_sample_bytes && a.fs_overbitrate==b.fs_overbitrate;
}
void RequireSameFrame(const FilmScan3Frame& bgra,const FilmScan3Frame& planes,const std::string& label) {
    Require(bgra.sample==planes.sample,label+": packet bytes differ");
    Require(bgra.tables.quant==planes.tables.quant,label+": quant tables differ");
    Require(bgra.tables.scale==planes.tables.scale,label+": scale tables differ");
    Require(SameState(bgra.state,planes.state),label+": rate state differs");
    Require(bgra.coefficient_hash==planes.coefficient_hash,label+": hash status differs");
    Require(bgra.coefficient_fnv64==planes.coefficient_fnv64,label+": coefficient hashes differ");
    Require(FilmScan3FullCoefficientHashesEqual(bgra,planes),label+": full-hash gate refused equal frames");
}
bool SamePlanes(const Planes& a,const Planes& b) {
    for(int c=0;c<3;++c) {
        if(a[c].width!=b[c].width || a[c].height!=b[c].height || a[c].stride!=b[c].stride) return false;
        if(a[c].pixels!=b[c].pixels) return false;
    }
    return true;
}
class ConvertOnly final : public detail::Frontend {
    FilmScan3Geometry geometry_;
public:
    explicit ConvertOnly(FilmScan3Geometry geometry):geometry_(geometry){}
    FilmScan3Geometry Geometry() const noexcept override {return geometry_;}
    Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {
        return ConvertFilmScan3Bgra(p,n,w,h,s);
    }
    WaveletLevel Forward(const Plane& p,int prescale) override {return ForwardLevel(p,prescale);}
    std::string DeviceName() const override {return "convert-only test backend";}
};
class PlaneFailure final : public detail::Frontend {
    FilmScan3Geometry geometry_;
public:
    explicit PlaneFailure(FilmScan3Geometry geometry):geometry_(geometry){}
    FilmScan3Geometry Geometry() const noexcept override {return geometry_;}
    Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {
        return ConvertFilmScan3Bgra(p,n,w,h,s);
    }
    WaveletLevel Forward(const Plane& p,int prescale) override {return ForwardLevel(p,prescale);}
    std::string DeviceName() const override {return "plane-failure test backend";}
    detail::FrontendFrame TransformPlanes(const Planes&,bool) override {throw std::bad_alloc();}
};
void CheckEquivalence(int width,int height,hypercine::EncodingQuality quality,const char* name) {
    const FilmScan3Geometry geometry{width,height};
    const auto input=BgraFrame(width,height);
    const auto planes=Converted(input,width,height);
    ValidateFilmScan3Planes(planes,geometry);
    FilmScan3Encoder bgra(geometry),native(geometry);
    bgra.SetSessionQuality(quality);native.SetSessionQuality(quality);
    for(unsigned frame=0;frame<3;++frame) {
        const auto from_bgra=bgra.Encode(input.data(),input.size(),static_cast<std::size_t>(width)*4);
        const auto from_planes=native.Encode(planes);
        RequireSameFrame(from_bgra,from_planes,std::string(name)+" frame "+std::to_string(frame));
        Require(bgra.state().frame_index==native.state().frame_index,std::string(name)+": frame counters differ");
    }
}
}
int main() {
    try {
        const auto input=BgraFrame(128,24);
        auto planes=Converted(input,128,24);
        const FilmScan3Geometry geometry{128,24};
        ValidateFilmScan3Planes(planes,geometry);

        // The same 1..3-frame sequence must produce byte-identical packets and
        // the same evolving rate state on a non-canonical geometry and on a
        // second profile, not just the default one.
        CheckEquivalence(128,24,hypercine::EncodingQuality::FilmScan3,"128x24 filmscan3");
        CheckEquivalence(256,40,hypercine::EncodingQuality::FilmScan1,"256x40 filmscan1");

        // A native frame is not allowed to mutate the caller's planes.
        {
            const auto before=planes;
            FilmScan3Encoder encoder(geometry);
            encoder.SetCoefficientHashing(false);
            (void)encoder.Encode(planes);
            Require(SamePlanes(before,planes),"Encode(planes) mutated the input planes");
        }

        // The upper documentable code 1024 is accepted; 1025 and -1 are refused
        // rather than masked or clipped.
        {
            auto boundary=planes;
            for(int y=0;y<boundary[0].height;++y)for(int x=0;x<boundary[0].width;++x)
                boundary[0].pixels[static_cast<std::size_t>(y)*boundary[0].stride+x]=1024;
            FilmScan3Encoder encoder(geometry);
            const auto frame=encoder.Encode(boundary);
            Require(frame.coefficient_hash==FilmScan3CoefficientHash::Full,"1024 boundary frame lost full hashing");
            auto high=boundary;
            high[1].pixels[0]=1025;
            Reject([&]{FilmScan3Encoder e(geometry);e.Encode(high);},"sample 1025 was accepted");
            auto low=boundary;
            low[2].pixels[0]=-1;
            Reject([&]{FilmScan3Encoder e(geometry);e.Encode(low);},"sample -1 was accepted");
        }

        // Shape and storage refusals: every case below can never be the output
        // of ConvertFilmScan3Bgra, so accepting it would create a second,
        // undocumented input dialect.
        {
            auto wrong_y=planes;
            wrong_y[0].width=geometry.width-128;
            Reject([&]{ValidateFilmScan3Planes(wrong_y,geometry);},"wrong luma width accepted");
            auto wrong_chroma=planes;
            wrong_chroma[1].width+=2;
            Reject([&]{ValidateFilmScan3Planes(wrong_chroma,geometry);},"wrong chroma width accepted");
            auto short_storage=planes;
            short_storage[0].pixels.resize(1);
            Reject([&]{ValidateFilmScan3Planes(short_storage,geometry);},"truncated luma storage accepted");
            auto bad_stride=planes;
            bad_stride[2].stride=bad_stride[2].width+1;
            Reject([&]{ValidateFilmScan3Planes(bad_stride,geometry);},"unaligned stride accepted");
            Reject([&]{ValidateFilmScan3Planes(planes,FilmScan3Geometry{128,32});},"foreign geometry accepted");
        }

        // A backend without a native planes path must refuse, not fall back to
        // treating the plane pointer as BGRA. The session must stay untouched.
        {
            FilmScan3Encoder encoder(geometry);
            detail::FrontendAccess::Use(encoder,std::make_shared<ConvertOnly>(geometry));
            const auto before=encoder.state();
            bool refused=false;
            try{encoder.Encode(planes);}
            catch(const std::logic_error& e){
                refused=std::string(e.what())=="planes-domain input is not supported by this backend";
            }
            Require(refused,"non-planes backend did not refuse the native entry");
            Require(SameState(before,encoder.state()),"refused planes entry committed rate state");
        }

        // A failing native transform must leave the session exactly as it was,
        // including the lazy frontend binding.
        {
            FilmScan3Encoder encoder(geometry);
            detail::FrontendAccess::Use(encoder,std::make_shared<PlaneFailure>(geometry));
            const auto before=encoder.state();
            Reject([&]{encoder.Encode(planes);},"native transform failure was swallowed");
            Require(SameState(before,encoder.state()),"failed native transform committed rate state");
            Require(encoder.state().frame_index==0,"failed native transform consumed a frame");
        }

        std::cout<<"PASS: native planes entry equals BGRA packets/state on two geometries and profiles, "
                   "immutable input, 10-bit boundary, shape/storage/value refusals, backend refusal, "
                   "and failed-transform rollback\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
