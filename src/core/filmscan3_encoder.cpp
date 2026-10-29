// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/filmscan3.h"
#include "filmscan3_backend.h"
#include "quality_profile_bridge.h"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>
#include <chrono>
namespace hypercine { namespace standalone {
FilmScan3Tables ComputeFilmScan3Tables(int limiter) {
    if(limiter<0 || limiter>20) throw std::invalid_argument("invalid FilmScan3 limiter");
    // Frozen quantize.h high tables; quantize.c FS3, progressive, 10-bit, GOP1.
    // FS3 disables the separate FixedQuality broadcast-rate limiter (vbr=256).
    const unsigned scale=(16u+static_cast<unsigned>(std::min(limiter,16))*2u)/2u;
    FilmScan3Tables out;
    for(unsigned c=0;c<3;++c) for(unsigned lv=0;lv<3;++lv) for(unsigned b=0;b<4;++b) {
        const auto at=c*12+lv*4+b;
        out.scale[at]=(b==0?4u:(b==3?1u:2u))<<(2*lv);
        if(!b) out.quant[at]=1;
        else if(lv) out.quant[at]=2*out.scale[at]/4;
        else {
            const unsigned base=b==3 ? (c?16u:12u) : 8u;
            out.quant[at]=std::max(2u,base*scale/16u);
        }
    }
    return out;
}
namespace {
class Writer {
    std::size_t capacity_;
public:
    explicit Writer(std::size_t capacity = 16384) : capacity_(capacity) {}
    std::vector<uint8_t> bytes;
    void Check(std::size_t extra) const {
        if(bytes.size()>capacity_ || extra>capacity_-bytes.size())throw std::length_error("CFHD capacity exceeded");
    }
    void U16(unsigned v) { Check(2); bytes.push_back(static_cast<uint8_t>(v>>8));bytes.push_back(static_cast<uint8_t>(v)); }
    void Tag(int key,unsigned v) {
        if(v>65535) throw std::length_error("CFHD tag value overflow");
        U16(static_cast<uint16_t>(key));U16(v);
    }
    void Patch(std::size_t at,uint32_t v) {
        if(at>bytes.size() || bytes.size()-at<4)throw std::out_of_range("CFHD patch");
        for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<uint8_t>(v>>(24-i*8));
    }
    void Append(const std::vector<uint8_t>& v) {
        Check(v.size());
        bytes.insert(bytes.end(),v.begin(),v.end());
    }
    std::size_t Begin(unsigned key) {auto n=bytes.size();Tag(key,0);return n;}
    void End(std::size_t at,unsigned key) {
        if(bytes.size()%4 || at>bytes.size() || bytes.size()-at<4)throw std::logic_error("unaligned CFHD chunk");
        const auto words=(bytes.size()-at-4)/4;
        if(words>0xffffff)throw std::length_error("CFHD chunk overflow");
        Patch(at,(static_cast<uint32_t>(-static_cast<int>(key|(words>>16)))<<16)|(words&65535));
    }
    void Record(const char* key,char type,const void* data,unsigned size) {
        if(size>0xffffff)throw std::length_error("CFHD metadata record overflow");
        Check(8+((static_cast<std::size_t>(size)+3)&~std::size_t{3}));
        const auto* p=static_cast<const uint8_t*>(data);
        for(int i=0;i<4;++i)bytes.push_back(static_cast<uint8_t>(key[i]));
        for(unsigned shift=0;shift<24;shift+=8)bytes.push_back(static_cast<uint8_t>(size>>shift));
        bytes.push_back(static_cast<uint8_t>(type));
        bytes.insert(bytes.end(),p,p+size);while(bytes.size()%4)bytes.push_back(0);
    }
};
void Metadata(Writer& s,const FilmScan3Metadata& m,unsigned frame) {
    Writer values;
    values.Record("GUID",'G',m.guid.data(),16);values.Record("DATE",'c',m.date.data(),10);
    values.Record("TIME",'c',m.time.data(),8);values.Record("TIMC",'c',m.timecode.data(),11);
    const uint8_t number[]={static_cast<uint8_t>(frame),static_cast<uint8_t>(frame>>8),static_cast<uint8_t>(frame>>16),static_cast<uint8_t>(frame>>24)};
    values.Record("UFRM",'L',number,4);
    s.Tag(-0x4002,static_cast<unsigned>(values.bytes.size()/4));s.Append(values.bytes);
    s.Tag(-0x4002,128);
    std::vector<uint8_t> free(504);Writer padding;padding.Record("FREE",0,free.data(),504);s.Append(padding.bytes);
}
uint64_t Hash(const Plane& p) {
    uint64_t h=14695981039346656037ull;
    for(int y=0;y<p.height;++y)for(int x=0;x<p.width;++x) {
        const auto v=static_cast<uint16_t>(p.pixels[y*p.stride+x]);
        h^=static_cast<uint8_t>(v);h*=1099511628211ull;h^=static_cast<uint8_t>(v>>8);h*=1099511628211ull;
    }
    return h;
}
int NextLimiter(const FilmScan3State& state, std::size_t pixels) {
    int limiter=state.fs_rate_limiter;
    if(state.previous_sample_bytes) {
        float compression=static_cast<float>(pixels*3*10/8)/static_cast<float>(state.previous_sample_bytes);
        compression=static_cast<float>(compression/1.5);
        if(compression>4.5f) {--limiter;if(compression>5.5f)--limiter;if(compression>6.5f)limiter-=2;}
        else if(compression<3.0f) {++limiter;if(compression<2.5f)++limiter;if(compression<2.0f)++limiter;if(compression<1.5f)limiter+=2;}
    }
    return std::clamp(limiter,0,20);
}
void Lowpass(Writer& s,const Plane& p) {
    s.Tag(4,0x1a4a);s.Tag(25,0);s.Tag(26,3);s.Tag(27,p.width);s.Tag(28,p.height);
    for(int tag:{31,29,32,30,33})s.Tag(tag,0);
    s.Tag(34,1);s.Tag(35,16);auto start=s.Begin(0x2000);s.Tag(4,0x0f0f);
    for(int y=0;y<p.height;++y)for(int x=0;x<p.width;++x)s.U16(static_cast<uint16_t>(p.pixels[y*p.stride+x]));
    s.Tag(4,0x1b4b);s.End(start,0x2000);
}
}
namespace {
uint32_t MintHashSession() {
    static std::atomic<uint32_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
}
FilmScan3Encoder::FilmScan3Encoder(FilmScan3Backend backend) : frontend_(detail::SelectFrontend(backend)) {}
FilmScan3Encoder::FilmScan3Encoder(FilmScan3Geometry geometry) : frontend_(detail::CpuFrontend(geometry)) {}
FilmScan3Encoder::FilmScan3Encoder(FilmScan3Geometry geometry, WaveletIsa isa)
    : frontend_(detail::CpuFrontend(geometry, isa)) {}
FilmScan3Encoder::FilmScan3Encoder(FilmScan3Backend backend, FilmScan3Geometry geometry)
    : frontend_(detail::SelectFrontend(backend, geometry)) {}
WaveletIsa FilmScan3Encoder::wavelet_isa() const noexcept {
    // The default-constructed encoder lazily takes the shared CPU frontend, which is
    // Auto-resolved at first use; report what that frontend will actually run.
    return frontend_ ? frontend_->Isa() : WaveletIsa::Auto;
}
void FilmScan3Encoder::SetCoefficientHashing(bool enabled) noexcept { coefficient_hashing_=enabled; }
void FilmScan3Encoder::SetSessionQuality(EncodingQuality quality) {
    const char* refusal=SessionProfileRefusalReason(quality);
    if(*refusal)throw std::invalid_argument(refusal);
    // The profile owns the rate limiter, the hysteresis token and the previous
    // sample size. Letting it change after a frame exists would hand one
    // profile's rate state to another profile's tables, so the session profile
    // is settled by the first successfully encoded frame. Re-asserting the same
    // profile is a no-op and stays allowed.
    if(quality_locked_ && quality!=quality_)
        throw std::logic_error("session quality profile is already fixed by an encoded frame; "
                               "construct a new encoder for another profile");
    quality_=quality;
}
void FilmScan3Encoder::StampDiagnosticOff(FilmScan3Frame& result) {
    // 0xD1 in the top byte, slot in the next, then session and serial. A skipped
    // frame is never all zeros, and two skipped frames do not share a fingerprint
    // inside the 24-bit session/serial window. These values are not FNV-64 hashes.
    if(!hash_session_) hash_session_=MintHashSession();
    const uint32_t serial=diagnostic_serial_++;
    for(unsigned i=0;i<30;++i) {
        result.coefficient_fnv64[i]=(uint64_t{0xD1}<<56) | (uint64_t{i}<<48)
            | ((uint64_t{hash_session_}&0xffffffu)<<24)
            | (uint64_t{serial}&0xffffffu);
    }
    result.coefficient_hash=FilmScan3CoefficientHash::DiagnosticOff;
}
FilmScan3Geometry FilmScan3Encoder::geometry() const noexcept {
    return frontend_ ? frontend_->Geometry() : FilmScan3Geometry{};
}
FilmScan3Frame FilmScan3Encoder::Encode(const uint8_t* data,std::size_t bytes,std::size_t stride) {
    FilmScan3Metadata metadata;
    const unsigned f=state_.frame_index,parts[]={f/(30*60*60)%24,f/(30*60)%60,f/30%60,f%30};
    for(unsigned p=0;p<4;++p){metadata.timecode[3*p]=static_cast<char>('0'+parts[p]/10);metadata.timecode[3*p+1]=static_cast<char>('0'+parts[p]%10);}
    return Encode(data,bytes,stride,metadata);
}
FilmScan3Frame FilmScan3Encoder::Encode(const uint8_t* data,std::size_t bytes,std::size_t stride,const FilmScan3Metadata& metadata) {
    if(state_.frame_index>=65535)throw std::length_error("FilmScan3 frame counter exhausted");
    auto& frontend=*(frontend_ ? frontend_ : detail::CpuFrontend());
    const auto g=geometry();
    ValidateFilmScan3Geometry(g);
    const auto pixels=static_cast<std::size_t>(g.width)*g.height;
    auto pyramid=frontend.Transform(data,bytes,g.width,g.height,stride);
    using Clock=std::chrono::steady_clock;
    const bool profile=frontend.ProfileHost();
    auto stamp=[&]{return profile ? Clock::now() : Clock::time_point{};};
    auto elapsed=[&](Clock::time_point start){return profile ? std::chrono::duration<double,std::milli>(Clock::now()-start).count() : 0.0;};
    detail::HostTiming timing;
    const auto host_start=stamp();
    FilmScan3Frame result;result.state=state_;
    // The frame limiter and its 36 quant/scale entries come from the single
    // shared SDK-free calculator (#29), not from a second local table. The
    // frozen FilmScan3 rate formula stays as an independent cross-check so a
    // drift between the two fails loudly instead of changing bytes silently.
    ProfileTableRequest request;
    request.quality=quality_;
    request.frame_index=state_.frame_index;
    request.fs_rate_limiter=state_.fs_rate_limiter;
    request.previous_completed_sample_bytes=state_.previous_sample_bytes;
    request.previous_overbitrate=state_.fs_overbitrate;
    request.width=g.width;request.height=g.height;
    const auto session=ComputeProfileTables(request);
    // The frozen rate formula is the FilmScan3 law. The other profiles run the
    // shared calculator's own feedback, so comparing them against this formula
    // would fail on correct input rather than catch a defect.
    if(quality_==EncodingQuality::FilmScan3 && session.fs_rate_limiter!=NextLimiter(state_,pixels))
        throw std::logic_error("shared quality state and frozen FilmScan3 rate feedback disagree");
    result.state.fs_rate_limiter=session.fs_rate_limiter;
    result.state.fs_overbitrate=session.overbitrate;
    result.tables=session.tables;
    Writer s(g.sample_capacity_bytes ? g.sample_capacity_bytes : 8*pixels+16384);
    s.Tag(1,9);s.Tag(2,3);const auto index=s.bytes.size();for(int c=0;c<3;++c)s.Tag(0,0);
    s.Tag(10,0);s.Tag(11,1);s.Tag(12,3);s.Tag(-71,32);s.Tag(84,1);s.Tag(-91,2);
    s.Tag(13,3);s.Tag(14,10);s.Tag(15,2);s.Tag(16,3);s.Tag(20,g.width);s.Tag(21,g.height);
    s.Tag(-69,state_.frame_index+1);s.Tag(70,10);s.Tag(-85,g.height);s.Tag(-79,0xa100);
    // Tag -80 carries the session quality enum. The SDK writes 1..6 here for
    // low..filmscan3; hard-coding 6 would relabel every other profile while
    // leaving the rest of the packet untouched.
    s.Tag(-80,static_cast<unsigned>(static_cast<int>(quality_)));s.Tag(-81,0x1a0);s.Tag(-83,0x2000);
    const auto sample=s.Begin(0x2200);Metadata(s,metadata,state_.frame_index);
    s.Tag(-63,0);s.Tag(-64,0);s.Tag(-65,16);s.Tag(-66,9);s.Tag(68,1);
    for(int c=0;c<3;++c) {
        if(c){s.Tag(1,3);s.Tag(62,c);}const auto begin=s.bytes.size();
        auto& channel=pyramid.levels[c];
        WaveletLevel* levels[]={&channel[0],&channel[1],&channel[2]};int sub=0;
        auto start=stamp();
        if(coefficient_hashing_){
            result.coefficient_fnv64[c*10+sub]=Hash(channel[2].bands[0]);
            timing.coefficient_hash_ms+=elapsed(start);
        }
        ++sub;Lowpass(s,channel[2].bands[0]);
        for(int lv=2;lv>=0;--lv) {
            auto& bands=levels[lv]->bands;const auto& p=bands[0];
            s.Tag(4,0x0d0d);s.Tag(37,lv?3:5);s.Tag(38,lv+1);s.Tag(39,lv+1);s.Tag(40,4);
            s.Tag(41,p.width);s.Tag(42,p.height);s.Tag(43,0);s.Tag(44,0);s.Tag(45,result.tables.scale[c*12+lv*4]);s.Tag(46,0);
            const auto level=s.Begin(0x2100);
            for(int b=1;b<4;++b) {
                auto& p=bands[b];const auto at=c*12+lv*4+b;const auto q=result.tables.quant[at];
                start=stamp();QuantizeFilmScan3Block(p.pixels.data(),p.pixels.size(),q);
                timing.quant_ms+=elapsed(start);
                if(coefficient_hashing_){
                    start=stamp();
                    result.coefficient_fnv64[c*10+sub]=Hash(p);
                    timing.coefficient_hash_ms+=elapsed(start);
                }
                start=stamp();
                const auto entropy=PackHighpass(p,4*(static_cast<std::size_t>(p.width)*p.height+1));
                timing.entropy_ms+=elapsed(start);
                s.Tag(4,0x0e0e);s.Tag(48,b);s.Tag(72,1);s.Tag(49,p.width);s.Tag(50,p.height);s.Tag(51,sub++);
                s.Tag(52,3);s.Tag(53,q);s.Tag(54,result.tables.scale[at]);
                const auto band=s.Begin(0x2000);s.Tag(55,0);s.Append(entropy);s.Tag(56,0);s.End(band,0x2000);
            }
            s.Tag(4,0x0c0c);s.End(level,0x2100);
        }
        const auto channel_bytes=s.bytes.size()-begin;
        if(sub!=10)throw std::logic_error("FilmScan3 coefficient slot count");
        if(channel_bytes>std::numeric_limits<uint32_t>::max())throw std::length_error("CFHD channel index overflow");
        s.Patch(index+4*c,static_cast<uint32_t>(channel_bytes));
    }
    s.Tag(24,0);s.End(sample,0x2200);
    result.sample=std::move(s.bytes);
    timing.framing_ms=elapsed(host_start)-timing.quant_ms-timing.coefficient_hash_ms-timing.entropy_ms;
    if(profile)frontend.RecordHostTiming(timing);
    // Stamp only after the sample is closed and host timing has returned.
    // Both of those can throw. StampDiagnosticOff cannot, and it is the first
    // write to the diagnostic serial and hash session. A failed Encode therefore
    // leaves that identity untouched. Rate state is committed immediately after.
    if(coefficient_hashing_) result.coefficient_hash=FilmScan3CoefficientHash::Full;
    else StampDiagnosticOff(result);
    state_.fs_rate_limiter=result.state.fs_rate_limiter;
    state_.fs_overbitrate=result.state.fs_overbitrate;
    state_.previous_sample_bytes=result.sample.size();++state_.frame_index;
    // First successful frame settles the profile. Only reached after every
    // throwing step above, so a rejected Encode does not lock the session.
    quality_locked_=true;
    return result;
}
}}