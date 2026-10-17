// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <vector>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include "test_vectors.h"
#include "CFHDEncoder.h"
#include "CFHDDecoder.h"

using Buffer = HyperCineTest::AlignedVector<uint8_t>;
struct EncoderDelete { void operator()(void* p) const { CFHD_CloseEncoder(p); } };
struct DecoderDelete { void operator()(void* p) const { CFHD_CloseDecoder(p); } };
using Encoder = std::unique_ptr<void, EncoderDelete>;
using Decoder = std::unique_ptr<void, DecoderDelete>;

void Write(const std::string& path, const Buffer& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!out) throw std::runtime_error("output error");
}
Buffer Read(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto size = in.tellg();
    if (!in || size <= 0 || size > 16*1024*1024) throw std::runtime_error("input error");
    Buffer bytes(static_cast<size_t>(size));
    in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!in) throw std::runtime_error("input error");
    return bytes;
}
void Check(CFHD_Error status) { if (status != CFHD_ERROR_OKAY) throw std::runtime_error("encoder API error"); }

int main(int argc, char** argv) {
    try {
        // Only two fixed geometries; no unbounded allocations or GPU linkage.
        if (argc != 5) throw std::runtime_error("arguments");
        const int width = std::string(argv[2]) == "128" ? 128 : std::string(argv[2]) == "1080" ? 1920 : 0;
        const int height = width == 128 ? 128 : 1080;
        if (!width) throw std::runtime_error("geometry");
        const std::string mode = argv[1], packet_path = argv[3], output = argv[4];
        if (mode == "generate") {
            Buffer source(static_cast<size_t>(width)*height*2);
            uint32_t seed = 0x1337BEEF;
            for (int y=0; y<height; ++y) for (int x=0; x<width; x+=2) {
                seed = seed*1664525u+1013904223u;
                const size_t i = (static_cast<size_t>(y)*width+x)*2;
                source[i] = static_cast<uint8_t>(16+(x*219/width));
                source[i+1] = static_cast<uint8_t>(112+((seed>>24)&31));
                source[i+2] = static_cast<uint8_t>(16+(y*219/height));
                source[i+3] = static_cast<uint8_t>(112+((seed>>16)&31));
            }
            CFHD_EncoderRef raw=nullptr; Check(CFHD_OpenEncoder(&raw,nullptr)); Encoder encoder(raw);
            Check(CFHD_PrepareToEncode(raw,width,height,CFHD_PIXEL_FORMAT_YUY2,CFHD_ENCODED_FORMAT_YUV_422,
                                      CFHD_ENCODING_FLAGS_NONE,CFHD_ENCODING_QUALITY_FILMSCAN3));
            Check(CFHD_EncodeSample(raw,source.data(),width*2));
            void* data=nullptr; size_t size=0; Check(CFHD_GetSampleData(raw,&data,&size));
            if (!data || !size || size>16*1024*1024) throw std::runtime_error("sample");
            Buffer packet(static_cast<uint8_t*>(data),static_cast<uint8_t*>(data)+size);
            Write(packet_path,packet); Write(output,source);
            return 0;
        }
        if (mode != "decode") throw std::runtime_error("mode");
        const Buffer immutable = Read(packet_path);
        // Reopen the original decoder each call, retaining original CRT/process state.
        for (int ordinal=1; ordinal<=5; ++ordinal) {
            Buffer sample=immutable; // protect immutable input from API writes
            Buffer pixels; // Both sample and output outlive decoder teardown.
            CFHD_DecoderRef raw=nullptr;
            CFHD_Error status=CFHD_OpenDecoder(&raw,nullptr); Decoder decoder(raw);
            int w=0,h=0; int32_t pitch=0; CFHD_PixelFormat format=CFHD_PIXEL_FORMAT_UNKNOWN;
            if (status==CFHD_ERROR_OKAY) status=CFHD_PrepareToDecode(raw,0,0,CFHD_PIXEL_FORMAT_YUY2,
                CFHD_DECODED_RESOLUTION_FULL,CFHD_DECODING_FLAGS_NONE,sample.data(),sample.size(),&w,&h,&format);
            if (status==CFHD_ERROR_OKAY) status=CFHD_GetImagePitch(w,format,&pitch);
            bool valid=status==CFHD_ERROR_OKAY && w==width && h==height && format==CFHD_PIXEL_FORMAT_YUY2 && pitch==width*2 && pitch%16==0;
            if (valid) {
                pixels.resize(static_cast<size_t>(pitch)*h);
                status=CFHD_DecodeSample(raw,sample.data(),sample.size(),pixels.data(),pitch);
                decoder.reset(); // Match existing tests: close before inspecting pixels.
                valid=status==CFHD_ERROR_OKAY && sample==immutable;
                if (valid) Write(output+"-"+std::to_string(ordinal)+".yuy2",pixels);
            }
            std::cout << "{\"ordinal\":" << ordinal << ",\"decoder_status\":" << status
                << ",\"output_valid\":" << (valid?"true":"false") << ",\"width\":" << w
                << ",\"height\":" << h << ",\"pitch\":" << pitch << ",\"buffer_size\":" << pixels.size()
                << ",\"actual_format_code\":" << static_cast<uint32_t>(format)
                << ",\"requested_format\":\"YUY2\"}\n";
        }
        return 0;
    } catch (...) { std::cerr << "bounded reference worker error\n"; return 2; }
}
