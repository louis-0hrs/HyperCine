// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// A single fresh-process observation of the unchanged original SDK.
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_vectors.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"

namespace {
using Bytes = HyperCineTest::AlignedVector<uint8_t>;
struct DecoderDelete { void operator()(void* p) const { if(p) CFHD_CloseDecoder(p); } };
struct MetadataDelete { void operator()(void* p) const { if(p) CFHD_CloseMetadata(p); } };
struct FileDelete { void operator()(void* p) const { if(p && p!=INVALID_HANDLE_VALUE) CloseHandle(p); } };
int sdk_error=0;
void Check(CFHD_Error status) { sdk_error=static_cast<int>(status); if(status != CFHD_ERROR_OKAY) throw std::runtime_error("sdk_call"); }
unsigned long long Number(const char* text, int base=10) {
    std::string s(text); std::size_t used=0;
    if(s.empty() || s.front()=='-') throw std::invalid_argument("number");
    auto value=std::stoull(s,&used,base);
    if(used!=s.size()) throw std::invalid_argument("number");
    return value;
}
void Inventory() {
    DWORD_PTR available=0, system=0;
    if(!GetProcessAffinityMask(GetCurrentProcess(),&available,&system)) throw std::runtime_error("affinity_query");
    int regs[4]{}; __cpuid(regs,0); const int maximum=regs[0];
    __cpuidex(regs,1,0); const bool sse2=(regs[3]&(1<<26))!=0;
    const bool osxsave=(regs[2]&(1<<27))!=0, avx=(regs[2]&(1<<28))!=0;
    const auto xcr=osxsave ? _xgetbv(0) : 0;
    bool avx2=false,avx512=false;
    if(maximum>=7) { __cpuidex(regs,7,0); avx2=avx && (xcr&6)==6 && (regs[1]&(1<<5));
        avx512=avx && (xcr&0xe6)==0xe6 && (regs[1]&(1<<16)); }
    std::cout << "{\"status\":\"PASS\",\"process_affinity\":\"" << std::hex << available << std::dec
        << "\",\"processor_groups\":" << GetActiveProcessorGroupCount()
        << ",\"cpu_isa\":{\"sse2\":" << (sse2?"true":"false")
        << ",\"avx2_os_enabled\":" << (avx2?"true":"false")
        << ",\"avx512f_os_enabled\":" << (avx512?"true":"false") << "},\"physical_cores\":[";
    DWORD length=0; GetLogicalProcessorInformation(nullptr,&length);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> topology(length/sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    bool success=!topology.empty() && GetLogicalProcessorInformation(topology.data(),&length);
    bool first=true;
    if(success) for(const auto& core:topology) if(core.Relationship==RelationProcessorCore) {
        if(!first) std::cout << ','; first=false;
        std::cout << "{\"logical_mask\":\"" << std::hex << core.ProcessorMask << std::dec
            << "\",\"smt\":" << (core.ProcessorCore.Flags?"true":"false") << '}';
    }
    std::cout << "],\"topology_status\":\"" << (success && GetActiveProcessorGroupCount()==1?"PASS":"INCONCLUSIVE") << "\"}\n";
}
}
int main(int argc,char** argv) {
    const char* stage="arguments";
    try {
        if(argc==2 && std::string(argv[1])=="--inventory") { Inventory(); return 0; }
        if(argc!=5) throw std::invalid_argument("arguments");
        auto requested=Number(argv[3]); auto mask=Number(argv[4],16);
        if(requested<1 || requested>8 || !mask) throw std::invalid_argument("arguments");
        stage="affinity";
        if(!SetProcessAffinityMask(GetCurrentProcess(),static_cast<DWORD_PTR>(mask))) throw std::runtime_error("set_affinity");
        DWORD_PTR actual=0,system=0;
        if(!GetProcessAffinityMask(GetCurrentProcess(),&actual,&system) || actual!=mask) throw std::runtime_error("verify_affinity");
        stage="input";
        std::ifstream input(argv[1],std::ios::binary|std::ios::ate); auto size=input.tellg();
        if(!input || size<=0 || size>67108864) throw std::runtime_error("input_size");
        Bytes packet(static_cast<std::size_t>(size)); input.seekg(0);
        input.read(reinterpret_cast<char*>(packet.data()),size); if(!input) throw std::runtime_error("input_read");
        stage="open";
        CFHD_DecoderRef raw=nullptr; auto code=CFHD_OpenDecoder(&raw,nullptr);
        std::unique_ptr<void,DecoderDelete> decoder(raw); Check(code);
        // This tag returns linked decoder SDK constants, not the sample's encoder.
        // See frozen SampleDecoder.cpp::GetSampleInfo case CFHD_SAMPLE_SDK_VERSION.
        int32_t sample_sdk_version=0;
        const auto sample_version_status=CFHD_GetSampleInfo(raw,packet.data(),packet.size(),
            CFHD_SAMPLE_SDK_VERSION,&sample_sdk_version,sizeof(sample_sdk_version));
        int width=0,height=0; CFHD_PixelFormat format=CFHD_PIXEL_FORMAT_UNKNOWN;
        stage="prepare";
        Check(CFHD_PrepareToDecode(raw,0,0,CFHD_PIXEL_FORMAT_YU64,CFHD_DECODED_RESOLUTION_FULL,
            CFHD_DECODING_FLAGS_NONE,packet.data(),packet.size(),&width,&height,&format));
        if(width!=1920 || height!=1080 || format!=CFHD_PIXEL_FORMAT_YU64) throw std::runtime_error("geometry_format");
        CFHD_MetadataRef mr=nullptr; code=CFHD_OpenMetadata(&mr);
        std::unique_ptr<void,MetadataDelete> metadata(mr); Check(code);
        Check(CFHD_InitSampleMetadata(mr,METADATATYPE_ORIGINAL,packet.data(),packet.size()));
        unsigned threads=static_cast<unsigned>(requested),process=0xffff,yuv=2,rgb=1,filter=0;
        stage="metadata";
        Check(CFHD_SetActiveMetadata(raw,mr,TAG_CPU_MAX,METADATATYPE_HIDDEN,&threads,sizeof(threads)));
        Check(CFHD_SetActiveMetadata(raw,mr,TAG_PROCESS_PATH,METADATATYPE_UINT32,&process,sizeof(process)));
        Check(CFHD_SetActiveMetadata(raw,mr,TAG_COLORSPACE_YUV,METADATATYPE_UINT32,&yuv,sizeof(yuv)));
        Check(CFHD_SetActiveMetadata(raw,mr,TAG_COLORSPACE_RGB,METADATATYPE_UINT32,&rgb,sizeof(rgb)));
        Check(CFHD_SetActiveMetadata(raw,mr,TAG_COLORSPACE_FTR,METADATATYPE_UINT32,&filter,sizeof(filter)));
        int32_t pitch=0; Check(CFHD_GetImagePitch(width,format,&pitch));
        if(pitch!=7680 || pitch%16) throw std::runtime_error("pitch");
        Bytes output(static_cast<std::size_t>(pitch)*height,0);
        stage="decode"; Check(CFHD_DecodeSample(raw,packet.data(),packet.size(),output.data(),pitch));
        if(!std::any_of(output.begin(),output.end(),[](uint8_t v){return v!=0;})) throw std::runtime_error("zero_output");
        // CREATE_NEW protects an existing sample/output even when the probe is
        // invoked directly rather than through the temporary-directory runner.
        stage="save";
        HANDLE file=CreateFileA(argv[2],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("output_exists_or_unwritable");
        std::unique_ptr<void,FileDelete> out(file);
        DWORD written=0;
        if(!WriteFile(file,output.data(),static_cast<DWORD>(output.size()),&written,nullptr)
           || written!=output.size()) throw std::runtime_error("output_write");
        std::cout << "{\"status\":\"PASS\",\"requested_threads\":" << requested
            << ",\"observed_decoder_threads\":null,\"thread_observation\":\"INCONCLUSIVE: public metadata request is not worker-count telemetry\""
            << ",\"process_affinity\":\"" << std::hex << actual << std::dec
            << "\",\"pixel_format\":\"YU64\",\"width\":1920,\"height\":1080,\"pitch\":7680"
            << ",\"sdk_api_header_version\":\"CFHDDecoder.h @version 1.0.0 (header label only)\""
            << ",\"decoder_sdk_version_packed\":" << sample_sdk_version
            << ",\"decoder_sdk_version_query_status\":" << static_cast<int>(sample_version_status)
            << ",\"sdk_source_baseline\":\"11574d0295771edccadd17af14af74a539f924c7\"}\n";
        return 0;
    } catch(...) { std::cout << "{\"status\":\"FAIL\",\"error_stage\":\"" << stage << "\",\"sdk_error\":" << sdk_error << "}\n"; return 1; }
}
