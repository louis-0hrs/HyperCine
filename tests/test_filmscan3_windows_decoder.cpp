// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "test_vectors.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
namespace {
using Bytes=HyperCineTest::AlignedVector<uint8_t>;
struct DecoderDelete {void operator()(void* p)const{if(p)CFHD_CloseDecoder(p);}};
struct MetadataDelete {void operator()(void* p)const{if(p)CFHD_CloseMetadata(p);}};
void Check(CFHD_Error code,const char* action){if(code!=CFHD_ERROR_OKAY)throw std::runtime_error(std::string(action)+": "+std::to_string(code));}
Bytes Decode(Bytes& packet) {
    CFHD_DecoderRef dr=nullptr;auto status=CFHD_OpenDecoder(&dr,nullptr);std::unique_ptr<void,DecoderDelete> decoder(dr);Check(status,"OpenDecoder");
    int width=0,height=0;CFHD_PixelFormat format=CFHD_PIXEL_FORMAT_UNKNOWN;
    Check(CFHD_PrepareToDecode(decoder.get(),0,0,CFHD_PIXEL_FORMAT_YU64,CFHD_DECODED_RESOLUTION_FULL,CFHD_DECODING_FLAGS_NONE,packet.data(),packet.size(),&width,&height,&format),"PrepareToDecode");
    if(width!=1920 || height!=1080 || format!=CFHD_PIXEL_FORMAT_YU64)throw std::runtime_error("unexpected decoded geometry/format");
    CFHD_MetadataRef mr=nullptr;status=CFHD_OpenMetadata(&mr);std::unique_ptr<void,MetadataDelete> metadata(mr);Check(status,"OpenMetadata");
    Check(CFHD_InitSampleMetadata(metadata.get(),METADATATYPE_ORIGINAL,packet.data(),packet.size()),"InitMetadata");
    unsigned cpu=1,process=0xffff,yuv=2,rgb=1,filter=0;
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_CPU_MAX,METADATATYPE_HIDDEN,&cpu,sizeof(cpu)),"CPU_MAX=1");
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_PROCESS_PATH,METADATATYPE_UINT32,&process,sizeof(process)),"PRCS");
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_YUV,METADATATYPE_UINT32,&yuv,sizeof(yuv)),"YUV709");
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_RGB,METADATATYPE_UINT32,&rgb,sizeof(rgb)),"fullRGB");
    Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_FTR,METADATATYPE_UINT32,&filter,sizeof(filter)),"chromaFilterOff");
    int32_t pitch=0;Check(CFHD_GetImagePitch(width,format,&pitch),"GetImagePitch");
    if(pitch!=7680 || pitch%16)throw std::runtime_error("unexpected aligned decoder pitch");
    Bytes output(static_cast<std::size_t>(pitch)*height,0);
    Check(CFHD_DecodeSample(decoder.get(),packet.data(),packet.size(),output.data(),pitch),"DecodeSample");
    if(!std::any_of(output.begin(),output.end(),[](uint8_t v){return v!=0;}))throw std::runtime_error("all-zero decoder output");
    return output;
}
}
int main(int argc,char** argv){try{
    if(argc!=3)throw std::invalid_argument("expected two original Mac-produced 1080p CFHD samples");
    for(int i=1;i<argc;++i){
        std::ifstream f(argv[i],std::ios::binary|std::ios::ate);const auto size=f.tellg();
        if(!f || size<=0 || size>16793600)throw std::runtime_error("invalid fixture storage");
        Bytes packet(static_cast<std::size_t>(size));f.seekg(0);f.read(reinterpret_cast<char*>(packet.data()),size);if(!f)throw std::runtime_error("fixture read failed");
        const auto first=Decode(packet),second=Decode(packet);if(first!=second)throw std::runtime_error("non-deterministic GoPro decoder output");
        std::cout<<argv[i]<<" GoPro Windows YU64 1920x1080 CPU_MAX=1 repeats=2 FNV="<<std::hex<<HyperCineTest::ComputeFNV1a64(first.data(),first.size())<<std::dec<<" PASS\n";
    }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
