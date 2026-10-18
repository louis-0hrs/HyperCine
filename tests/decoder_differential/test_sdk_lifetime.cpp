// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Exercise the real dump entry point with a synthetic SDK, never the codec.
#define main SdkDumpMain
#include "sdk_dump.cpp"
#undef main
#include <filesystem>

namespace {
std::filesystem::path output_path;
void* pending_pixels = nullptr;
bool inspected_before_close = false;
bool fail_decode = false;
bool fail_close = false;
int closes = 0;
}

extern "C" {
CFHD_Error CFHD_OpenDecoder(CFHD_DecoderRef* ref, CFHD_ALLOCATOR*) {
    *ref = &closes; return CFHD_ERROR_OKAY;
}
CFHD_Error CFHD_PrepareToDecode(CFHD_DecoderRef, int, int, CFHD_PixelFormat,
    CFHD_DecodedResolution, CFHD_DecodingFlags, void*, size_t,
    int* w, int* h, CFHD_PixelFormat* format) {
    *w=16; *h=16; *format=CFHD_PIXEL_FORMAT_YU64; return CFHD_ERROR_OKAY;
}
CFHD_Error CFHD_GetImagePitch(uint32_t, CFHD_PixelFormat, int32_t* pitch) {
    *pitch=64; return CFHD_ERROR_OKAY;
}
CFHD_Error CFHD_OpenMetadata(CFHD_MetadataRef* ref) {
    *ref=&closes; return CFHD_ERROR_OKAY;
}
CFHD_Error CFHD_InitSampleMetadata(CFHD_MetadataRef, CFHD_MetadataTrack, void*, size_t) {
    return CFHD_ERROR_OKAY;
}
CFHD_Error CFHD_SetActiveMetadata(CFHD_DecoderRef, CFHD_MetadataRef, unsigned int,
    CFHD_MetadataType, void*, unsigned int) { return CFHD_ERROR_OKAY; }
CFHD_Error CFHD_CloseMetadata(CFHD_MetadataRef) { return CFHD_ERROR_OKAY; }
CFHD_Error CFHD_DecodeSample(CFHD_DecoderRef, void*, size_t, void* pixels, int32_t) {
    pending_pixels=pixels;
    return fail_decode ? CFHD_ERROR_INTERNAL : CFHD_ERROR_OKAY;
}
CFHD_Error CFHD_CloseDecoder(CFHD_DecoderRef) {
    ++closes;
    inspected_before_close=std::filesystem::exists(output_path);
    // No dereference on the old, prematurely published path (it may be freed).
    // A final synthetic write makes pre-close conversion observable.
    if (!inspected_before_close && !fail_decode)
        std::memset(pending_pixels, 0x40, 16*16*4);
    return fail_close ? CFHD_ERROR_THREAD_WAIT_FAILED : CFHD_ERROR_OKAY;
}
}

int main(int argc, char** argv) {
    if (argc!=2) return 2;
    const auto directory=std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    const auto input=directory/"synthetic.cfhd";
    output_path=directory/"synthetic.p10";
    std::ofstream(input, std::ios::binary).put('x');
    std::string program="sdk-dump-test", in=input.string(), out=output_path.string(), size="16";
    char* arguments[]={program.data(),in.data(),out.data(),size.data(),size.data()};
    for (int mode : {0,1,2}) {
        std::filesystem::remove(output_path);
        fail_decode=mode==1; fail_close=mode==2;
        const bool failure=mode!=0;
        inspected_before_close=false; closes=0;
        const auto status=SdkDumpMain(5,arguments);
        if (closes!=1 || inspected_before_close || (status!=0)!=failure) return 1;
        if (failure) {
            if (std::filesystem::exists(output_path)) return 1;
        } else {
            std::ifstream file(output_path,std::ios::binary);
            Bytes bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            if (bytes.size()!=16*16*4) return 1;
            for (size_t i=0;i<bytes.size();i+=2) if (U16(bytes,i)!=257) return 1;
        }
    }
    std::filesystem::remove(input);
    return 0;
}
