// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "reference_frame_generator.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
using namespace hypercine::standalone;
void Require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
template<class F>void Reject(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}Require(rejected,"invalid input accepted");}
int main(){try{
    Require(QuantizeFilmScan3(5,6)==1 && QuantizeFilmScan3(-5,6)==-1,"FS3 midpoint signed rounding");
    Require(Quantize(5,6)==0,"legacy quantization changed");
    Reject([]{QuantizeFilmScan3(1,0);});Reject([]{QuantizeFilmScan3(1,32769);});
    Reject([]{ComputeFilmScan3Tables(-1);});Reject([]{ComputeFilmScan3Tables(21);});
    const auto tables=ComputeFilmScan3Tables(4);
    const unsigned luma[]={1,6,6,9,1,4,4,2,1,16,16,8};
    const unsigned chroma[]={1,6,6,12,1,4,4,2,1,16,16,8};
    for(unsigned i=0;i<36;++i)Require(tables.quant[i]==(i<12?luma[i]:chroma[i%12]),"golden initial quant tables");
    for(unsigned i=0;i<36;++i)Require(ComputeFilmScan3Tables(20).quant[i]==ComputeFilmScan3Tables(16).quant[i],"FS3 limiter quant clamp");
    AlignedVector<uint32_t> input(1920*1080);
    reference_fixture::GeneratePhase3AFrame(1920,1080,0,reference_fixture::Phase3APattern::FlatBlack,reinterpret_cast<uint8_t*>(input.data()),7680,0x1337beef);
    auto* data=reinterpret_cast<uint8_t*>(input.data());const auto size=input.size()*4;
    FilmScan3Encoder encoder;
    Reject([&]{encoder.Encode(nullptr,size,7680);});Reject([&]{encoder.Encode(data,size-1,7680);});Reject([&]{encoder.Encode(data+1,size-1,7680);});
    Reject([&]{encoder.Encode(data,size,7679);});Reject([&]{encoder.Encode(data,size,(std::numeric_limits<std::size_t>::max()/16)*16);});
    Require(encoder.state().frame_index==0 && encoder.state().previous_sample_bytes==0,"failure mutated encoder state");
    auto first=encoder.Encode(data,size,7680);Require(first.state.frame_index==0 && first.state.fs_rate_limiter==4,"initial state");
    Require(first.sample.size()==185648,"deterministic reference sample size");
    AlignedVector<uint8_t> padded(7696*1080);
    for(int y=0;y<1080;++y)std::memcpy(padded.data()+static_cast<std::size_t>(y)*7696,data+static_cast<std::size_t>(y)*7680,7680);
    FilmScan3Encoder pitched;Require(pitched.Encode(padded.data(),padded.size(),7696).sample==first.sample,"padded stride changed packet");
    auto second=encoder.Encode(data,size,7680);Require(second.state.previous_sample_bytes==first.sample.size() && second.state.fs_rate_limiter==0,"autonomous flat frame rate-control feedback");
    const auto previous=encoder.state();Reject([&]{encoder.Encode(data,0,7680);});
    Require(encoder.state().frame_index==previous.frame_index && encoder.state().fs_rate_limiter==previous.fs_rate_limiter && encoder.state().previous_sample_bytes==previous.previous_sample_bytes,"failure advanced dynamic state");
    encoder.Reset();Require(encoder.Encode(data,size,7680).sample==first.sample,"Reset not deterministic");
    Require(first.coefficient_hash==FilmScan3CoefficientHash::Full,"production frame must declare full coefficient hashing");
    FilmScan3Frame absent_a,absent_b;
    Require(absent_a.coefficient_fnv64==absent_b.coefficient_fnv64,"precondition: default hash arrays are equal");
    Require(!FilmScan3FullCoefficientHashesEqual(absent_a,absent_b),"zero hashes must not pass the full-hash gate");
    struct ByteDiff { int compared=0; int mismatches=0; };
    auto compare_bytes=[&](const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){
        ByteDiff diff;diff.compared=1;
        if(a.size()!=b.size()){diff.mismatches=1;return diff;}
        for(std::size_t i=0;i<a.size();++i) if(a[i]!=b[i]){diff.mismatches=1;return diff;}
        return diff;
    };
    int byte_gates=0;
    FilmScan3Encoder hashed,skipped;
    skipped.SetCoefficientHashing(false);
    Require(hashed.coefficient_hashing() && !skipped.coefficient_hashing(),"hashing switch is per session");
    auto hashed_frame=hashed.Encode(data,size,7680);
    auto skipped_frame=skipped.Encode(data,size,7680);
    Require(hashed_frame.coefficient_hash==FilmScan3CoefficientHash::Full,"default session hashing is not Full");
    Require(skipped_frame.coefficient_hash==FilmScan3CoefficientHash::DiagnosticOff,"disabled hashing is not marked diagnostic_off");
    Require(!FilmScan3FullCoefficientHashesEqual(hashed_frame,skipped_frame),"diagnostic_off must fail the full-hash gate");
    for(auto v:skipped_frame.coefficient_fnv64) Require(v!=0,"diagnostic_off stored a zero hash");
    auto skipped_again=skipped.Encode(data,size,7680);
    Require(skipped_frame.coefficient_fnv64!=skipped_again.coefficient_fnv64,"two disabled frames share one fingerprint");
    Require(!FilmScan3FullCoefficientHashesEqual(skipped_frame,skipped_again),"disabled frames passed the full-hash gate");
    FilmScan3Encoder other_skipped;other_skipped.SetCoefficientHashing(false);
    auto other=other_skipped.Encode(data,size,7680);
    Require(other.coefficient_fnv64!=skipped_frame.coefficient_fnv64,"two sessions share a diagnostic tombstone");
    auto positive=compare_bytes(hashed_frame.sample,skipped_frame.sample);
    if(positive.compared!=1 || positive.mismatches!=0) throw std::runtime_error("disabling coefficient hashing changed CFHD bytes");
    byte_gates+=positive.compared;
    auto mutated=hashed_frame.sample;Require(!mutated.empty(),"empty sample");mutated[mutated.size()/2]^=0x5a;
    auto negative=compare_bytes(hashed_frame.sample,mutated);
    if(negative.compared!=1 || negative.mismatches==0) throw std::runtime_error("byte comparison removed or blind");
    byte_gates+=negative.compared;
    Require(byte_gates==2,"byte-for-byte comparison removed");
    hashed.Reset();skipped.Reset();
    reference_fixture::GeneratePhase3AFrame(1920,1080,3,reference_fixture::Phase3APattern::DeterministicNoise,data,7680,0x1337beef);
    auto noise_on=hashed.Encode(data,size,7680);auto noise_off=skipped.Encode(data,size,7680);
    auto noise_bytes=compare_bytes(noise_on.sample,noise_off.sample);
    Require(noise_bytes.compared==1 && noise_bytes.mismatches==0,"noise sample changed when hashing was disabled");
    Require(noise_on.coefficient_hash==FilmScan3CoefficientHash::Full,"noise frame dropped full hashing");
    Require(noise_off.coefficient_hash==FilmScan3CoefficientHash::DiagnosticOff,"noise frame hid disabled hashing");
    FilmScan3Encoder replay;auto replay_frame=replay.Encode(data,size,7680);
    Require(FilmScan3FullCoefficientHashesEqual(noise_on,replay_frame),"full hashes of the same coefficients differ");
    replay_frame.coefficient_hash=FilmScan3CoefficientHash::Absent;
    Require(!FilmScan3FullCoefficientHashesEqual(noise_on,replay_frame),"clearing Full status must fail the gate even when the words match");
    std::cout<<"PASS: production FS3 API arithmetic, initial tables, feedback, invalid storage and reset\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
