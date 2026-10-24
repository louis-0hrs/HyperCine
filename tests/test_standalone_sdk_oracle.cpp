// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_vectors.h"
#include "reference_oracle.h"
#include "hypercine/canonical.h"
#include "CFHDDecoder.h"
#include "CFHDEncoder.h"
#include "CFHDMetadata.h"

namespace {
namespace oracle = hypercine::test_oracle;
using Bytes = HyperCineTest::AlignedVector<uint8_t>;
using Path = std::filesystem::path;
constexpr int width = 128, height = 128;
struct DecoderDelete { void operator()(void* p) const { if(p) CFHD_CloseDecoder(p); } };
struct MetadataDelete { void operator()(void* p) const { if(p) CFHD_CloseMetadata(p); } };
struct EncoderDelete { void operator()(void* p) const { if(p) CFHD_CloseEncoder(p); } };
using Decoder = std::unique_ptr<void, DecoderDelete>;
using Metadata = std::unique_ptr<void, MetadataDelete>;
using Encoder = std::unique_ptr<void, EncoderDelete>;
void Require(bool ok, const std::string& message) { if(!ok) throw std::runtime_error(message); }
void Check(CFHD_Error code, const std::string& operation) {
    std::cout << operation << " status=" << int(code) << '\n';
    Require(code == CFHD_ERROR_OKAY, operation + " failed status=" + std::to_string(int(code)));
}
Bytes Read(const Path& path) {
    std::ifstream file(path, std::ios::binary|std::ios::ate);
    Require(bool(file), "Cannot open " + path.string());
    auto size = file.tellg();
    Require(size > 0 && size <= 1024*1024, "Invalid fixture size");
    Bytes out(static_cast<size_t>(size)); file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    Require(bool(file), "Cannot read " + path.string()); return out;
}
void Write(const Path& path, const Bytes& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    file.close(); Require(bool(file), "Cannot write " + path.string());
}
unsigned U16(const Bytes& data, size_t i) { return data.at(i) | (unsigned(data.at(i+1))<<8); }
void Put16(Bytes& data, size_t i, unsigned value) { data.at(i)=uint8_t(value);data.at(i+1)=uint8_t(value>>8); }

Bytes Decode(Bytes packet, CFHD_PixelFormat requested, const std::string& label) {
    CFHD_DecoderRef raw=nullptr; auto status=CFHD_OpenDecoder(&raw,nullptr);
    Decoder decoder(raw); Check(status,label+" OpenDecoder");
    int w=0,h=0; CFHD_PixelFormat actual=CFHD_PIXEL_FORMAT_UNKNOWN;
    Check(CFHD_PrepareToDecode(decoder.get(),0,0,requested,CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE,packet.data(),packet.size(),&w,&h,&actual),label+" PrepareToDecode");
    Require(w==width && h==height && actual==requested,"Unexpected decoded geometry/format");
    int32_t pitch=0;Check(CFHD_GetImagePitch(w,actual,&pitch),label+" GetImagePitch");
    Require(pitch==width*4 && pitch%16==0,"Unexpected output pitch");
    CFHD_MetadataRef mr=nullptr;status=CFHD_OpenMetadata(&mr);Metadata metadata(mr);
    Check(status,label+" OpenMetadata");
    Check(CFHD_InitSampleMetadata(metadata.get(),METADATATYPE_ORIGINAL,packet.data(),packet.size()),label+" InitMetadata");
    unsigned process=0xffff, yuv=2, rgb=1, filter=0, cpu=1;
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_PROCESS_PATH,METADATATYPE_UINT32,&process,sizeof(process)),label+" PRCS");
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_YUV,METADATATYPE_UINT32,&yuv,sizeof(yuv)),label+" BT709");
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_RGB,METADATATYPE_UINT32,&rgb,sizeof(rgb)),label+" fullRGB");
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_FTR,METADATATYPE_UINT32,&filter,sizeof(filter)),label+" chromaFilterOff");
    // Documented oracle mitigation for the frozen SDK's worker races. This is
    // an actual decoder setting; every returned pixel is still compared.
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_CPU_MAX,METADATATYPE_HIDDEN,&cpu,sizeof(cpu)),label+" CPU_MAX=1");
    Bytes output(size_t(pitch)*h,0);
    Check(CFHD_DecodeSample(decoder.get(),packet.data(),packet.size(),output.data(),pitch),label+" DecodeSample");
    Require(std::any_of(output.begin(),output.end(),[](uint8_t v){return v!=0;}),"All-zero decode");
    std::cout<<label<<" geometry="<<w<<'x'<<h<<" pitch="<<pitch<<" fnv1a64="<<std::hex
             <<HyperCineTest::ComputeFNV1a64(output.data(),output.size())<<std::dec<<'\n';
    return output;
}
// SDK YU64 = LE Y0/Cr/Y1/Cb, unsigned p16 (p10 left shifted six).
// No row inversion for YU64; BGRA is bottom-up per CFHDTypes.h.
Bytes Planar(const Bytes& yu64) {
    Require(yu64.size()==65536,"Invalid YU64 image");Bytes out(65536);
    for(int y=0;y<height;++y) for(int x=0;x<width;x+=2) {
        const auto src=size_t(y*width+x)*4;
        Put16(out,size_t(y*width+x)*2,U16(yu64,src)>>6);
        Put16(out,size_t(y*width+x+1)*2,U16(yu64,src+4)>>6);
        Put16(out,32768+size_t(y*64+x/2)*2,U16(yu64,src+6)>>6);
        Put16(out,49152+size_t(y*64+x/2)*2,U16(yu64,src+2)>>6);
    }
    return out;
}
struct Metric { double psnr; int maximum; size_t mismatches; };
Metric CompareP10(const Bytes& a,const Bytes& b,const std::string& label) {
    Require(a.size()==65536 && b.size()==a.size(),"Invalid p10 image");
    double total=0;int maximum=0;size_t count=0;
    const size_t starts[]={0,16384,24576,32768};
    for(int p=0;p<3;++p) {
        double square=0;int maxerr=0;size_t mismatch=0,first=32768;
        for(size_t i=starts[p];i<starts[p+1];++i) {
            const int error=int(U16(a,i*2))-int(U16(b,i*2));square+=double(error)*error;
            maxerr=std::max(maxerr,std::abs(error)); if(error) {++mismatch;first=std::min(first,i-starts[p]);}
        }
        const double psnr=square?10*std::log10(1023.0*1023*(starts[p+1]-starts[p])/square):999;
        std::cout<<label<<" plane="<<p<<" psnr="<<psnr<<" max="<<maxerr<<" mismatches="<<mismatch;
        if(mismatch) std::cout<<" first_x="<<first%(p?64:128)<<" first_y="<<first/(p?64:128)
                            <<" actual="<<U16(a,2*(first+starts[p]))<<" reference="<<U16(b,2*(first+starts[p]));
        std::cout<<'\n';total+=square;maximum=std::max(maximum,maxerr);count+=mismatch;
    }
    return {total?10*std::log10(1023.0*1023*32768/total):999,maximum,count};
}
void CompareRgb(const Bytes& sdk,const Bytes& ffmpeg,const std::string& label) {
    Require(sdk.size()==65536 && ffmpeg.size()==sdk.size(),"Invalid BGRA image");
    double mse=0;int maxerr=0;size_t mismatch=0,first=65536;
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) for(int c=0;c<3;++c) {
        size_t i=size_t(y*width+x)*4+c;
        const int diff=int(sdk[size_t((height-1-y)*width+x)*4+c])-int(ffmpeg[i]);
        mse+=double(diff)*diff;maxerr=std::max(maxerr,std::abs(diff));if(diff) {++mismatch;first=std::min(first,i);}
    }
    std::cout<<label<<" BGRA alphaExcluded=1 bottomUpSDK=1 topDownFFmpeg=1 psnr="
        <<(mse?10*std::log10(255.0*255*49152/mse):999)<<" max="<<maxerr<<" mismatches="<<mismatch;
    if(mismatch) std::cout<<" first_x="<<(first/4)%128<<" first_y="<<(first/4)/128<<" first_channel="<<first%4;
    std::cout<<'\n'; // Diagnostic: FFmpeg bicubic chroma vs SDK unfiltered conversion.
}
std::string Base64(const Bytes& bytes) {
    const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string out;
    for(size_t i=0;i<bytes.size();i+=3) {
        unsigned v=unsigned(bytes[i])<<16;
        if(i+1<bytes.size()) v|=unsigned(bytes[i+1])<<8;
        if(i+2<bytes.size()) v|=bytes[i+2];
        out+=alphabet[v>>18];out+=alphabet[(v>>12)&63];out+=i+1<bytes.size()?alphabet[(v>>6)&63]:'=';
        out+=i+2<bytes.size()?alphabet[v&63]:'=';
    }return out;
}
void Emit(const std::string& name,const Bytes& data) {
    const auto encoded=Base64(data);
    std::cout<<"ORACLE_DATA_BEGIN "<<name<<' '<<data.size()<<'\n';
    for(size_t i=0;i<encoded.size();i+=120) std::cout<<"ORACLE_DATA "<<encoded.substr(i,120)<<'\n';
    std::cout<<"ORACLE_DATA_END "<<name<<'\n';
}
// Encode the same declared YUV p10 source with GoPro (independent control).
Bytes Control(const Bytes& planar) {
    Bytes yu64(65536);
    for(int y=0;y<height;++y) for(int x=0;x<width;x+=2) {
        size_t i=size_t(y*width+x)*4;
        Put16(yu64,i,U16(planar,size_t(y*width+x)*2)<<6);
        Put16(yu64,i+4,U16(planar,size_t(y*width+x+1)*2)<<6);
        Put16(yu64,i+2,U16(planar,49152+size_t(y*64+x/2)*2)<<6);
        Put16(yu64,i+6,U16(planar,32768+size_t(y*64+x/2)*2)<<6);
    }
    CFHD_EncoderRef raw=nullptr;auto status=CFHD_OpenEncoder(&raw,nullptr);Encoder encoder(raw);
    Check(status,"control OpenEncoder");
    Check(CFHD_PrepareToEncode(encoder.get(),width,height,CFHD_PIXEL_FORMAT_YU64,CFHD_ENCODED_FORMAT_YUV_422,
        CFHD_ENCODING_FLAGS_NONE,CFHD_ENCODING_QUALITY_FILMSCAN3),"control PrepareToEncode");
    Check(CFHD_EncodeSample(encoder.get(),yu64.data(),512),"control EncodeSample");
    void* p=nullptr;size_t size=0;Check(CFHD_GetSampleData(encoder.get(),&p,&size),"control GetSampleData");
    Require(p && size>0 && size<1024*1024,"Invalid control packet");
    return Bytes(static_cast<uint8_t*>(p),static_cast<uint8_t*>(p)+size);
}
void InvalidHeader() {
    CFHD_DecoderRef raw=nullptr;auto status=CFHD_OpenDecoder(&raw,nullptr);Decoder decoder(raw);Check(status,"invalid OpenDecoder");
    Bytes bad(256,0);int w=0,h=0;CFHD_PixelFormat f=CFHD_PIXEL_FORMAT_UNKNOWN;
    status=CFHD_PrepareToDecode(decoder.get(),0,0,CFHD_PIXEL_FORMAT_YU64,CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE,bad.data(),bad.size(),&w,&h,&f);
    std::cout<<"invalid zero header PrepareToDecode status="<<int(status)<<'\n';
    Require(status!=CFHD_ERROR_OKAY,"Zero header accepted");
}
} // namespace
int main(int argc,char** argv) {
    const bool report=argc==4 && std::string(argv[3])=="--report";
    try {
        Require(argc==3 || report,"usage: test_standalone_sdk_oracle fixtures output [--report]");
        const Path fixtures=argv[1],output=argv[2];std::filesystem::create_directories(output);
        // Strict decoding/storage/API/geometry/pitch/non-zero signals stay fatal.
        bool strict_failure=false;
        std::size_t observed_decodes=0,distinct_per_pattern=0;
        bool quality_measured=false,quality_meets_threshold=true;
        std::cout<<std::setprecision(10)<<"Standalone SDK oracle: Windows x86, frozen GoPro SDK, YU64+BGRA, 5 fresh instances per format\n";
        for(const std::string pattern:{"flat","gradient","canonical"}) {
            try {
                const auto packet=Read(fixtures/(pattern+".cfhd"));
                const auto expected=Read(fixtures/(pattern+".p10"));const auto rgb=Read(fixtures/(pattern+".bgra"));
                std::set<uint64_t> yhash,rhash;
                for(int repeat=0;repeat<5;++repeat) {
                    auto yu64=Decode(packet,CFHD_PIXEL_FORMAT_YU64,pattern+" YU64 repeat="+std::to_string(repeat));
                    auto p10=Planar(yu64);auto metric=CompareP10(p10,expected,pattern+" SDK-vs-FFmpeg");
                    yhash.insert(HyperCineTest::ComputeFNV1a64(yu64.data(),yu64.size()));++observed_decodes;
                    auto bgra=Decode(packet,CFHD_PIXEL_FORMAT_BGRA,pattern+" BGRA repeat="+std::to_string(repeat));
                    CompareRgb(bgra,rgb,pattern+" SDK-vs-FFmpeg");rhash.insert(HyperCineTest::ComputeFNV1a64(bgra.data(),bgra.size()));++observed_decodes;
                    if(repeat==0) {Write(output/(pattern+".sdk-p10"),p10);Write(output/(pattern+".sdk-bgra"),bgra);
                        if(report) {Emit(pattern+".sdk-p10",p10);Emit(pattern+".sdk-bgra",bgra);}}
                    // The pinned .p10 decode is not an independently qualified oracle
                    // (#224), so the comparison is recorded, never gated. RGB is diagnostic.
                    quality_measured=true;
                    if(metric.psnr<=35 || (pattern=="flat" && metric.maximum!=0)) quality_meets_threshold=false;
                }
                distinct_per_pattern=std::max(distinct_per_pattern,std::max(yhash.size(),rhash.size()));
                std::cout<<pattern<<" distinctYU64="<<yhash.size()<<" distinctBGRA="<<rhash.size()<<'\n';
            }catch(const std::exception& e) {strict_failure=true;std::cerr<<pattern<<" STRICT_FAILURE "<<e.what()<<'\n';}
        }
        try {
            auto control=Control(Read(fixtures/"canonical.input-p10"));Write(output/"control.cfhd",control);
            auto sdk=Planar(Decode(control,CFHD_PIXEL_FORMAT_YU64,"GoPro control"));Write(output/"control.sdk-p10",sdk);
            auto metric=CompareP10(sdk,Read(fixtures/"canonical.input-p10"),"GoPro control vs source");
            quality_measured=true;
            if(metric.psnr<=35) quality_meets_threshold=false;
            if(report) {Emit("control.cfhd",control);Emit("control.sdk-p10",sdk);}
            InvalidHeader();
        }catch(const std::exception& e){strict_failure=true;std::cerr<<"control STRICT_FAILURE "<<e.what()<<'\n';}
        // Only the decoded-pixel half is quarantined: an unqualified pinned reference
        // can be BLOCKED_ORACLE at best, and disagreeing repeats are the unstable
        // reference state. Neither is a pixel-accuracy pass or a stable failure.
        const oracle::GateDecision decision=strict_failure
            ?oracle::GateDecision::ReferenceDecodeError
            :oracle::ClassifyDecodeOnlyReference(observed_decodes,distinct_per_pattern);
        std::cout<<"standalone_sdk_decode_3_packets quality_measured="<<(quality_measured?1:0)
                 <<" quality_meets_threshold="<<(quality_meets_threshold?1:0)
                 <<" distinctOutputsMax="<<distinct_per_pattern
                 <<' '<<oracle::GateDecisionName(decision)
                 <<" exit="<<oracle::GateExitCode(decision)<<'\n';
        return report ? 0 : oracle::GateExitCode(decision);
    }catch(const std::exception& e) {std::cerr<<"Oracle harness error: "<<e.what()<<'\n';return 1;}
}
