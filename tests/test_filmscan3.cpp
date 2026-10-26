// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "reference_frame_generator.h"
#include "../src/core/filmscan3_backend.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
using namespace hypercine::standalone;
void Require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
template<class F>void Reject(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}Require(rejected,"invalid input accepted");}

namespace {
uint32_t Rotr(uint32_t x,uint32_t n){return (x>>n)|(x<<(32-n));}
std::string Sha256(const uint8_t* data,std::size_t len){
    static const uint32_t K[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint64_t bitlen=static_cast<uint64_t>(len)*8;
    std::vector<uint8_t> m(data,data+len);
    m.push_back(0x80);
    while((m.size()%64)!=56)m.push_back(0);
    for(int i=7;i>=0;--i)m.push_back(static_cast<uint8_t>(bitlen>>(i*8)));
    for(std::size_t off=0;off<m.size();off+=64){
        uint32_t w[64];
        for(int i=0;i<16;++i)
            w[i]=(uint32_t(m[off+4*i])<<24)|(uint32_t(m[off+4*i+1])<<16)|(uint32_t(m[off+4*i+2])<<8)|uint32_t(m[off+4*i+3]);
        for(int i=16;i<64;++i){
            const uint32_t s0=Rotr(w[i-15],7)^Rotr(w[i-15],18)^(w[i-15]>>3);
            const uint32_t s1=Rotr(w[i-2],17)^Rotr(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;++i){
            const uint32_t s1=Rotr(e,6)^Rotr(e,11)^Rotr(e,25);
            const uint32_t ch=(e&f)^((~e)&g);
            const uint32_t t1=hh+s1+ch+K[i]+w[i];
            const uint32_t s0=Rotr(a,2)^Rotr(a,13)^Rotr(a,22);
            const uint32_t maj=(a&b)^(a&c)^(b&c);
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+s0+maj;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    static const char* hex="0123456789abcdef";
    std::string out(64,'0');
    for(int i=0;i<8;++i)for(int shift=28;shift>=0;shift-=4)
        out[i*8+(28-shift)/4]=hex[(h[i]>>shift)&15];
    return out;
}
struct HeaderFields {
    unsigned frame_number=0,quality=0;
    int frame_hits=0,quality_hits=0;
    bool saw_sample=false;
};
unsigned U16(const std::vector<uint8_t>& s,std::size_t at){return (unsigned(s[at])<<8)|s[at+1];}
HeaderFields ReadHeader(const std::vector<uint8_t>& s){
    HeaderFields h;
    for(std::size_t at=0;at+4<=s.size();){
        const unsigned raw=U16(s,at),value=U16(s,at+2);
        const unsigned tag=raw>=32768 ? 65536u-raw : raw;
        at+=4;
        if(tag==69){h.frame_number=value;++h.frame_hits;}
        if(tag==80){h.quality=value;++h.quality_hits;}
        if(tag==0x4002){
            Require(value<=(s.size()-at)/4,"metadata extent");
            at+=4*static_cast<std::size_t>(value);
            continue;
        }
        const unsigned type=tag&0xff00u;
        if(type==0x2000 || type==0x2100 || type==0x2200){
            const unsigned words=((tag&255u)<<16)|value;
            Require(words<=(s.size()-at)/4,"chunk extent");
            if(type==0x2200){h.saw_sample=true;break;}
            at+=4*static_cast<std::size_t>(words);
        }
    }
    return h;
}
std::size_t CountFourcc(const std::vector<uint8_t>& s,const char* key){
    std::size_t n=0;
    for(std::size_t i=0;i+4<=s.size();++i)
        if(std::memcmp(s.data()+i,key,4)==0)++n;
    return n;
}
std::string RecordText(const std::vector<uint8_t>& s,const char* key,unsigned size,char type){
    const std::string want(key,4);
    std::string found;
    int hits=0;
    for(std::size_t i=0;i+8+size<=s.size();++i){
        if(std::memcmp(s.data()+i,key,4)!=0 || s[i+7]!=static_cast<uint8_t>(type))continue;
        const unsigned n=unsigned(s[i+4])|(unsigned(s[i+5])<<8)|(unsigned(s[i+6])<<16);
        if(n!=size)continue;
        found.assign(reinterpret_cast<const char*>(s.data()+i+8),size);
        ++hits;
    }
    Require(hits==1,want.c_str());
    return found;
}
uint32_t RecordUfrm(const std::vector<uint8_t>& s){
    int hits=0;uint32_t value=0;
    for(std::size_t i=0;i+12<=s.size();++i){
        if(std::memcmp(s.data()+i,"UFRM",4)!=0 || s[i+7]!='L')continue;
        const unsigned n=unsigned(s[i+4])|(unsigned(s[i+5])<<8)|(unsigned(s[i+6])<<16);
        if(n!=4)continue;
        value=uint32_t(s[i+8])|(uint32_t(s[i+9])<<8)|(uint32_t(s[i+10])<<16)|(uint32_t(s[i+11])<<24);
        ++hits;
    }
    Require(hits==1,"UFRM");
    return value;
}
std::size_t Diffs(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){
    Require(a.size()==b.size(),"metadata override changed packet size");
    std::size_t n=0;for(std::size_t i=0;i<a.size();++i)n+=a[i]!=b[i];return n;
}
void RequireSameState(const FilmScan3State& a,const FilmScan3State& b,const char* label){
    Require(a.frame_index==b.frame_index && a.fs_rate_limiter==b.fs_rate_limiter && a.previous_sample_bytes==b.previous_sample_bytes,label);
}
class Failure final : public detail::Frontend {
    bool corrupt_;
public:
    explicit Failure(bool corrupt):corrupt_(corrupt){}
    FilmScan3Geometry Geometry() const noexcept override {return {128,24};}
    Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {return ConvertFilmScan3Bgra(p,n,w,h,s);}
    WaveletLevel Forward(const Plane& p,int prescale) override {return ForwardLevel(p,prescale);}
    std::string DeviceName() const override {return "rate-state failure";}
    detail::FrontendFrame Transform(const uint8_t* p,std::size_t n,int w,int h,std::size_t s,bool audit) override {
        if(!corrupt_)throw std::bad_alloc();
        auto result=Frontend::Transform(p,n,w,h,s,audit);
        // Quantize runs on the returned bands before PackHighpass. Width 1 is legal
        // for the quantizer and illegal for entropy (width must be a multiple of 8).
        result.levels[2][0].bands[3].width=1;
        return result;
    }
};
void CheckRateState(const std::vector<uint8_t>& golden,const std::vector<uint8_t>& second){
    Require(Sha256(reinterpret_cast<const uint8_t*>("abc"),3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","sha256 abc");
    Require(Sha256(golden.data(),golden.size())=="d75e6af7980558e42a520e8ce3bd3513626a854c3210da1c8b78240f4d75d6df","30fps golden bytes changed");
    const auto g=ReadHeader(golden),s=ReadHeader(second);
    Require(g.saw_sample && g.frame_hits==1 && g.frame_number==1 && g.quality_hits==1 && g.quality==6,"golden header");
    Require(s.saw_sample && s.frame_hits==1 && s.frame_number==2 && s.quality_hits==1 && s.quality==6,"second frame still quality 6");
    Require(RecordText(golden,"TIMC",11,'c')=="00:00:00:00","golden timecode");
    Require(RecordUfrm(golden)==0,"golden UFRM");
    Require(CountFourcc(golden,"TIMB")==0 && CountFourcc(golden,"TIMD")==0,"golden gained TIMB/TIMD");
    AlignedVector<uint8_t> tiny(128u*24u*4,128);
    const FilmScan3Geometry geometry{128,24};
    FilmScan3Encoder clock(geometry);
    std::string tc29,tc30;
    for(unsigned i=0;i<31;++i){
        auto frame=clock.Encode(tiny.data(),tiny.size(),512);
        const auto header=ReadHeader(frame.sample);
        Require(header.frame_hits==1 && header.frame_number==i+1 && header.quality==6,"128x24 header");
        Require(RecordUfrm(frame.sample)==i,"128x24 UFRM");
        if(i==29)tc29=RecordText(frame.sample,"TIMC",11,'c');
        if(i==30)tc30=RecordText(frame.sample,"TIMC",11,'c');
    }
    Require(tc29=="00:00:00:29" && tc30=="00:00:01:00","30fps split");
    FilmScan3Encoder plain(geometry),timecode_only(geometry),guid_only(geometry);
    auto base=plain.Encode(tiny.data(),tiny.size(),512);
    FilmScan3Metadata metadata;
    metadata.timecode={{'0','1',':','0','2',':','0','3',':','0','4'}};
    auto timed=timecode_only.Encode(tiny.data(),tiny.size(),512,metadata);
    Require(Diffs(base.sample,timed.sample)==4,"timecode override spilled");
    Require(RecordText(timed.sample,"TIMC",11,'c')=="01:02:03:04","caller timecode");
    Require(RecordUfrm(timed.sample)==0 && ReadHeader(timed.sample).frame_number==1,"timecode moved frame identity");
    metadata=FilmScan3Metadata{};
    metadata.guid[0]=0x11;
    auto guided=guid_only.Encode(tiny.data(),tiny.size(),512,metadata);
    Require(Diffs(base.sample,guided.sample)==1,"guid override spilled");
    Require(RecordText(guided.sample,"TIMC",11,'c')=="00:00:00:00" && RecordUfrm(guided.sample)==0,"guid moved timecode or UFRM");
    auto fit=base.sample.size();
    Require(fit==3116,"128x24 reference size drifted");
    FilmScan3Encoder almost(FilmScan3Geometry{128,24,fit-1});
    const auto almost_before=almost.state();
    bool capacity=false;
    try{almost.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::length_error& e){capacity=std::string(e.what())=="CFHD capacity exceeded";}
    Require(capacity,"near-full budget did not fail closed");
    RequireSameState(almost_before,almost.state(),"near-full budget committed state");
    FilmScan3Encoder early(FilmScan3Geometry{128,24,4});
    const auto early_before=early.state();
    capacity=false;
    try{early.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::length_error& e){capacity=std::string(e.what())=="CFHD capacity exceeded";}
    Require(capacity,"tiny budget did not fail closed");
    RequireSameState(early_before,early.state(),"tiny budget committed state");
    FilmScan3Encoder injected(geometry);
    const auto injected_before=injected.state();
    detail::FrontendAccess::Use(injected,std::make_shared<Failure>(false));
    bool oom=false;
    try{injected.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::bad_alloc&){oom=true;}
    Require(oom,"transform failure was not bad_alloc");
    RequireSameState(injected_before,injected.state(),"bad_alloc committed state");
    detail::FrontendAccess::Use(injected,std::make_shared<Failure>(true));
    bool entropy=false;
    try{injected.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::invalid_argument& e){entropy=std::string(e.what())=="invalid entropy plane";}
    Require(entropy,"post-quant entropy failure missing");
    RequireSameState(injected_before,injected.state(),"post-quant entropy committed state");
    Reject([&]{Plane bad(1,1);PackHighpass(bad,64);});
    bool bits=false;
    try{Plane band(8,8);PackHighpass(band,0);}
    catch(const std::length_error& e){bits=std::string(e.what())=="entropy capacity exceeded";}
    Require(bits,"zero entropy budget accepted");
    injected.Reset();
    Require(injected.geometry().width==128 && injected.geometry().height==24,"Reset changed geometry");
    detail::FrontendAccess::Use(injected,detail::CpuFrontend(geometry));
    FilmScan3Encoder fresh(geometry);
    Require(injected.Encode(tiny.data(),tiny.size(),512).sample==fresh.Encode(tiny.data(),tiny.size(),512).sample,"Reset after failure");
    FilmScan3Encoder left(geometry),right(geometry);
    auto left0=left.Encode(tiny.data(),tiny.size(),512);
    auto right0=right.Encode(tiny.data(),tiny.size(),512);
    Require(left0.sample==right0.sample,"parallel sessions diverged on frame 0");
    left.Encode(tiny.data(),tiny.size(),512);
    Require(left.state().frame_index==2 && right.state().frame_index==1,"parallel sessions shared the frame counter");
    Require(ReadHeader(left0.sample).quality==6 && ReadHeader(right0.sample).quality==6,"parallel sessions changed quality tag");
    right.Reset();
    Require(left.state().frame_index==2 && right.state().frame_index==0,"Reset crossed sessions");
    Require(right.Encode(tiny.data(),tiny.size(),512).sample==left0.sample,"reset session did not restart");
    FilmScan3Encoder limited(geometry);
    for(unsigned i=0;i<65535u;++i)limited.Encode(tiny.data(),tiny.size(),512);
    const auto exhausted=limited.state();
    Require(exhausted.frame_index==65535u && exhausted.previous_sample_bytes==3116 && exhausted.fs_rate_limiter==20,"65535-frame session state");
    bool stopped=false;
    try{limited.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::length_error& e){stopped=std::string(e.what())=="FilmScan3 frame counter exhausted";}
    Require(stopped,"frame 65536 was encoded or wrapped");
    RequireSameState(exhausted,limited.state(),"exhausted session mutated rate state");
}
class TimingProbe final : public detail::Frontend {
    std::shared_ptr<detail::Frontend> inner_;
    int fail_=0;
public:
    detail::HostTiming timing{};
    explicit TimingProbe(std::shared_ptr<detail::Frontend> inner):inner_(std::move(inner)){}
    void FailOnce(){fail_=1;}
    FilmScan3Geometry Geometry() const noexcept override {return inner_->Geometry();}
    Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {return inner_->Convert(p,n,w,h,s);}
    WaveletLevel Forward(const Plane& p,int prescale) override {return inner_->Forward(p,prescale);}
    std::string DeviceName() const override {return inner_->DeviceName();}
    detail::FrontendFrame Transform(const uint8_t* p,std::size_t n,int w,int h,std::size_t s,bool audit) override {return inner_->Transform(p,n,w,h,s,audit);}
    bool ProfileHost() const override {return true;}
    void RecordHostTiming(const detail::HostTiming& t) override {
        if(fail_){fail_=0;throw std::runtime_error("host timing failed");}
        timing=t;
    }
};
// Removes or weakens FilmScan3FullCoefficientHashesEqual and this throws.
// Full/Full passes only when the 30 words were actually compared. Equal words
// with DiagnosticOff, Absent, or any other status must not pass, including the
// all-zero Absent pair that would otherwise look like a null hash.
void CheckCoefficientGate(){
    FilmScan3Frame full;
    full.coefficient_hash=FilmScan3CoefficientHash::Full;
    full.coefficient_fnv64.fill(0x123456789abcdef0ull);
    auto full_same=full;
    auto full_diff=full;full_diff.coefficient_fnv64[29]^=1ull;
    auto off=full;off.coefficient_hash=FilmScan3CoefficientHash::DiagnosticOff;
    auto off_same=off;
    FilmScan3Frame absent,absent_same;
    auto absent_words=full;absent_words.coefficient_hash=FilmScan3CoefficientHash::Absent;
    auto garbage=full;garbage.coefficient_hash=static_cast<FilmScan3CoefficientHash>(255);
    int passes=0;
    std::string failures;
    auto gate=[&](const FilmScan3Frame& a,const FilmScan3Frame& b,bool expect,const char* label){
        const bool got=FilmScan3FullCoefficientHashesEqual(a,b);
        if(got!=expect) failures+=std::string(label)+"; ";
        if(got)++passes;
    };
    gate(full,full_same,true,"Full vs Full equal words must pass");
    gate(full_same,full,true,"Full vs Full is not symmetric");
    gate(full,full_diff,false,"Full vs Full skipped the word check");
    gate(full,off,false,"Full vs DiagnosticOff passed");
    gate(off,full,false,"DiagnosticOff vs Full passed");
    gate(off,off_same,false,"DiagnosticOff vs DiagnosticOff passed");
    gate(absent,absent_same,false,"absent zero hashes passed");
    gate(absent_words,absent_words,false,"absent nonzero hashes passed");
    gate(full,absent_words,false,"Full vs Absent passed on equal words");
    gate(garbage,garbage,false,"unknown hash status passed");
    gate(full,garbage,false,"Full vs unknown status passed");
    if(!failures.empty()) throw std::runtime_error(failures);
    Require(passes==2,"gate passed something other than the two Full/Full checks");
}
void CompareDiagnosticFrame(FilmScan3Encoder& on,FilmScan3Encoder& off,const uint8_t* data,std::size_t bytes,std::size_t stride,const char* label){
    auto a=on.Encode(data,bytes,stride);
    auto b=off.Encode(data,bytes,stride);
    Require(a.sample==b.sample,label);
    Require(a.tables.quant==b.tables.quant && a.tables.scale==b.tables.scale,"DiagnosticOff changed quant or scale tables");
    Require(a.state.frame_index==b.state.frame_index && a.state.fs_rate_limiter==b.state.fs_rate_limiter && a.state.previous_sample_bytes==b.state.previous_sample_bytes,"DiagnosticOff changed the rate state used for this frame");
    Require(on.state().frame_index==off.state().frame_index && on.state().fs_rate_limiter==off.state().fs_rate_limiter && on.state().previous_sample_bytes==off.state().previous_sample_bytes,"DiagnosticOff changed committed rate state");
    Require(a.coefficient_hash==FilmScan3CoefficientHash::Full && b.coefficient_hash==FilmScan3CoefficientHash::DiagnosticOff,"DiagnosticOff did not stay a status-only change");
    const auto ah=ReadHeader(a.sample),bh=ReadHeader(b.sample);
    Require(ah.frame_number==bh.frame_number && ah.quality==bh.quality && ah.frame_hits==1 && ah.quality_hits==1,"DiagnosticOff changed frame or quality metadata");
    Require(RecordText(a.sample,"TIMC",11,'c')==RecordText(b.sample,"TIMC",11,'c'),"DiagnosticOff changed timecode");
    Require(RecordUfrm(a.sample)==RecordUfrm(b.sample),"DiagnosticOff changed UFRM");
}
// Sample bytes hold the quantized entropy payload and the metadata records.
// Matching bytes, tables and rate state across the known 1080p limiter step
// (4 then 0) are the executed proof. coefficient_hash_ms stays 0 when hashing
// is off. framing_ms is the residual of that skipped measurement and is not
// claimed stable. Nothing here proves an FNV word cannot collide with a
// tombstone pattern; the gate does not rely on that.
void CheckDiagnosticBounds(){
    AlignedVector<uint32_t> hd(1920*1080);
    auto* hddata=reinterpret_cast<uint8_t*>(hd.data());
    const auto hdbytes=hd.size()*4;
    FilmScan3Encoder on,off;off.SetCoefficientHashing(false);
    reference_fixture::GeneratePhase3AFrame(1920,1080,0,reference_fixture::Phase3APattern::FlatBlack,hddata,7680,0x1337beef);
    CompareDiagnosticFrame(on,off,hddata,hdbytes,7680,"DiagnosticOff changed 1080p frame 0");
    Require(on.state().fs_rate_limiter==4 && off.state().fs_rate_limiter==4,"1080p frame 0 limiter is not 4");
    reference_fixture::GeneratePhase3AFrame(1920,1080,1,reference_fixture::Phase3APattern::DeterministicNoise,hddata,7680,0x1337beef);
    CompareDiagnosticFrame(on,off,hddata,hdbytes,7680,"DiagnosticOff changed 1080p frame 1");
    Require(on.state().frame_index==2 && off.state().frame_index==2,"1080p pair did not both commit two frames");
    Require(on.state().fs_rate_limiter==0 && off.state().fs_rate_limiter==0,"1080p feedback did not reach limiter 0");
    const FilmScan3Geometry geometry{128,24};
    AlignedVector<uint8_t> tiny(128u*24u*4,128),noise(128u*24u*4);
    reference_fixture::GeneratePhase3AFrame(128,24,2,reference_fixture::Phase3APattern::DeterministicNoise,noise.data(),512,0x1337beef);
    FilmScan3Encoder son(geometry),soff(geometry);soff.SetCoefficientHashing(false);
    CompareDiagnosticFrame(son,soff,tiny.data(),tiny.size(),512,"DiagnosticOff changed 128x24 flat");
    CompareDiagnosticFrame(son,soff,noise.data(),noise.size(),512,"DiagnosticOff changed 128x24 noise");
    Require(son.state().fs_rate_limiter==soff.state().fs_rate_limiter && son.state().previous_sample_bytes==soff.state().previous_sample_bytes,"128x24 rate feedback diverged");
    FilmScan3Metadata meta;
    meta.timecode={{'0','1',':','0','2',':','0','3',':','0','4'}};
    meta.guid[0]=0x11;
    FilmScan3Encoder mon(geometry),moff(geometry);moff.SetCoefficientHashing(false);
    auto ma=mon.Encode(tiny.data(),tiny.size(),512,meta);
    auto mb=moff.Encode(tiny.data(),tiny.size(),512,meta);
    Require(ma.sample==mb.sample,"DiagnosticOff changed explicit metadata bytes");
    Require(ma.coefficient_hash==FilmScan3CoefficientHash::Full && mb.coefficient_hash==FilmScan3CoefficientHash::DiagnosticOff,"explicit metadata path changed hash status");
    Require(RecordText(mb.sample,"TIMC",11,'c')=="01:02:03:04","explicit timecode missing under DiagnosticOff");
    Require(ma.tables.quant==mb.tables.quant && ma.tables.scale==mb.tables.scale,"explicit metadata path changed tables");
    Require(ma.state.fs_rate_limiter==mb.state.fs_rate_limiter && ma.state.previous_sample_bytes==mb.state.previous_sample_bytes,"explicit metadata path changed rate state");
    auto probe=std::make_shared<TimingProbe>(detail::CpuFrontend(geometry));
    FilmScan3Encoder probed(geometry);probed.SetCoefficientHashing(false);
    detail::FrontendAccess::Use(probed,probe);
    FilmScan3Encoder plain(geometry);plain.SetCoefficientHashing(false);
    auto timed=probed.Encode(tiny.data(),tiny.size(),512);
    auto untimed=plain.Encode(tiny.data(),tiny.size(),512);
    Require(timed.sample==untimed.sample,"profiling DiagnosticOff changed CFHD bytes");
    Require(timed.tables.quant==untimed.tables.quant && timed.state.fs_rate_limiter==untimed.state.fs_rate_limiter,"profiling DiagnosticOff changed quant or rate");
    Require(probe->timing.coefficient_hash_ms==0.0,"DiagnosticOff still recorded coefficient hash time");
}
void RequireUntouchedIdentity(const FilmScan3Encoder& encoder,const char* label){
    Require(detail::FrontendAccess::DiagnosticSerial(encoder)==1 && detail::FrontendAccess::HashSession(encoder)==0,label);
}
void CheckDiagnosticFailure(){
    const FilmScan3Geometry geometry{128,24};
    AlignedVector<uint8_t> tiny(128u*24u*4,128);
    FilmScan3Encoder sized(geometry);
    const auto fit=sized.Encode(tiny.data(),tiny.size(),512).sample.size();
    FilmScan3Encoder tight(FilmScan3Geometry{128,24,fit-1});
    tight.SetCoefficientHashing(false);
    RequireUntouchedIdentity(tight,"fresh encoder does not start at serial 1");
    const auto before=tight.state();
    bool capacity=false;
    try{tight.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::length_error& e){capacity=std::string(e.what())=="CFHD capacity exceeded";}
    Require(capacity,"near-full budget did not fail closed");
    RequireSameState(before,tight.state(),"capacity failure committed rate state");
    RequireUntouchedIdentity(tight,"failed packet finalization consumed diagnostic identity");
    FilmScan3Encoder early(geometry);early.SetCoefficientHashing(false);
    detail::FrontendAccess::Use(early,std::make_shared<Failure>(false));
    bool oom=false;
    try{early.Encode(tiny.data(),tiny.size(),512);}catch(const std::bad_alloc&){oom=true;}
    Require(oom,"transform failure was not bad_alloc");
    RequireUntouchedIdentity(early,"tombstone created before transform");
    FilmScan3Encoder mid(geometry);mid.SetCoefficientHashing(false);
    detail::FrontendAccess::Use(mid,std::make_shared<Failure>(true));
    bool entropy=false;
    try{mid.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::invalid_argument& e){entropy=std::string(e.what())=="invalid entropy plane";}
    Require(entropy,"post-quant entropy failure missing");
    RequireSameState(before,mid.state(),"entropy failure committed rate state");
    RequireUntouchedIdentity(mid,"tombstone created before the packet was complete");
    auto probe=std::make_shared<TimingProbe>(detail::CpuFrontend(geometry));
    probe->FailOnce();
    FilmScan3Encoder timed(geometry);timed.SetCoefficientHashing(false);
    detail::FrontendAccess::Use(timed,probe);
    const auto timed_before=timed.state();
    bool timing_failed=false;
    try{timed.Encode(tiny.data(),tiny.size(),512);}
    catch(const std::runtime_error& e){timing_failed=std::string(e.what())=="host timing failed";}
    Require(timing_failed,"host timing failure did not surface");
    RequireSameState(timed_before,timed.state(),"timing failure committed rate state");
    RequireUntouchedIdentity(timed,"timing failure consumed diagnostic identity");
    auto recovered=timed.Encode(tiny.data(),tiny.size(),512);
    FilmScan3Encoder fresh(geometry);fresh.SetCoefficientHashing(false);
    auto first=fresh.Encode(tiny.data(),tiny.size(),512);
    Require(recovered.sample==first.sample,"recovery after timing failure changed CFHD bytes");
    Require(recovered.coefficient_hash==FilmScan3CoefficientHash::DiagnosticOff,"recovery lost DiagnosticOff");
    Require(detail::FrontendAccess::DiagnosticSerial(timed)==2,"recovery did not consume exactly one serial");
    Require((recovered.coefficient_fnv64[0]&0xffffffu)==1 && (first.coefficient_fnv64[0]&0xffffffu)==1,"failed finalization shifted the diagnostic serial");
    Require((recovered.coefficient_fnv64[0]>>56)==0xD1,"recovered tombstone lost its marker");
    Require(probe->timing.coefficient_hash_ms==0.0,"recovered DiagnosticOff recorded hash time");
}
void CheckDiagnosticSerialReuse(){
    const FilmScan3Geometry geometry{128,24};
    AlignedVector<uint8_t> tiny(128u*24u*4,128);
    FilmScan3Encoder enc(geometry);enc.SetCoefficientHashing(false);
    detail::FrontendAccess::SetHashSession(enc,0x2Au);
    detail::FrontendAccess::SetDiagnosticSerial(enc,0xFFFFFFFFu);
    auto hi=enc.Encode(tiny.data(),tiny.size(),512);
    Require(detail::FrontendAccess::DiagnosticSerial(enc)==0,"uint32 diagnostic serial did not wrap");
    auto wrapped=enc.Encode(tiny.data(),tiny.size(),512);
    Require((hi.coefficient_fnv64[0]&0xffffffu)==0xFFFFFFu,"high serial was truncated early");
    Require((wrapped.coefficient_fnv64[0]&0xffffffu)==0u,"wrapped serial was not zero in the low 24 bits");
    Require(wrapped.coefficient_fnv64[0]!=0,"wrapped tombstone collapsed to a zero hash");
    detail::FrontendAccess::SetDiagnosticSerial(enc,0x00FFFFFFu);
    auto again=enc.Encode(tiny.data(),tiny.size(),512);
    Require(hi.coefficient_fnv64==again.coefficient_fnv64,"24-bit serial window did not reuse the tombstone");
    Require(!FilmScan3FullCoefficientHashesEqual(hi,again),"reused diagnostic tombstones passed the hash gate");
    FilmScan3Encoder low(geometry),high(geometry);
    low.SetCoefficientHashing(false);high.SetCoefficientHashing(false);
    detail::FrontendAccess::SetHashSession(low,0x11u);
    detail::FrontendAccess::SetHashSession(high,0x1000011u);
    detail::FrontendAccess::SetDiagnosticSerial(low,7u);
    detail::FrontendAccess::SetDiagnosticSerial(high,7u);
    auto a=low.Encode(tiny.data(),tiny.size(),512);
    auto b=high.Encode(tiny.data(),tiny.size(),512);
    Require(a.coefficient_fnv64==b.coefficient_fnv64,"24-bit session window did not reuse the tombstone");
    Require(!FilmScan3FullCoefficientHashesEqual(a,b),"reused session ids passed the hash gate");
    for(unsigned i=0;i<30;++i){
        Require((a.coefficient_fnv64[i]>>56)==0xD1,"tombstone marker missing");
        Require(((a.coefficient_fnv64[i]>>48)&0xffu)==i,"tombstone slot missing");
        Require(a.coefficient_fnv64[i]!=0,"tombstone slot is a zero hash");
    }
}
}

int main(){try{
    CheckCoefficientGate();
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
    CheckDiagnosticBounds();
    CheckDiagnosticFailure();
    CheckDiagnosticSerialReuse();
    CheckRateState(first.sample,second.sample);
    std::cout<<"PASS: production FS3 API arithmetic, initial tables, feedback, invalid storage, reset, coefficient-hash gate, diagnostic-off isolation, tombstone rollback, 24-bit serial reuse, 30fps metadata split, budget/entropy rollback, parallel sessions, 65535-frame stop\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
