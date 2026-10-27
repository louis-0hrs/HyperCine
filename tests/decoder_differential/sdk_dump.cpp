// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <cstdint>
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
struct DecoderDelete { void operator()(void* p) const { if (p) CFHD_CloseDecoder(p); } };
struct MetadataDelete { void operator()(void* p) const { if (p) CFHD_CloseMetadata(p); } };
void Check(CFHD_Error e) { if (e != CFHD_ERROR_OKAY) throw std::runtime_error("SDK status=" + std::to_string(int(e))); }
unsigned U16(const Bytes& b, size_t i) { return b.at(i) | (unsigned(b.at(i+1)) << 8); }
void Put16(Bytes& b, size_t i, unsigned v) { b.at(i)=uint8_t(v); b.at(i+1)=uint8_t(v>>8); }
Bytes Planar(const Bytes& yu64, int w, int h, int pitch) {
    const size_t pixels=size_t(w)*h; Bytes out(pixels*4);
    for (int y=0;y<h;++y) for (int x=0;x<w;x+=2) {
        const size_t src=size_t(y)*pitch+size_t(x)*4, yi=size_t(y)*w+x, ci=size_t(y)*(w/2)+x/2;
        Put16(out,yi*2,U16(yu64,src)>>6); Put16(out,(yi+1)*2,U16(yu64,src+4)>>6);
        Put16(out,(pixels+ci)*2,U16(yu64,src+6)>>6);
        Put16(out,(pixels+pixels/2+ci)*2,U16(yu64,src+2)>>6);
    }
    return out;
}
void SelfTest() {
    Bytes input(32,0xcd); // Two 2-pixel rows, 16-byte padded pitch.
    const unsigned values[]={0,65535,64,32768,65472,128,256,512};
    for (size_t row=0;row<2;++row) for (size_t i=0;i<4;++i) Put16(input,row*16+i*2,values[row*4+i]);
    const auto result=Planar(input,2,2,16);
    const unsigned expected[]={0,1,1023,4,512,8,1023,2};
    for (size_t i=0;i<8;++i) if (U16(result,i*2)!=expected[i]) throw std::runtime_error("YU64 packing test failed");
    for (size_t row=0;row<2;++row) for (size_t i=8;i<16;++i)
        if (input[row*16+i]!=0xcd) throw std::runtime_error("Input padding modified");
}
}
int main(int argc, char** argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--self-test") { SelfTest(); return 0; }
        if (argc != 5) throw std::runtime_error("usage: decoder_sdk_dump packet.cfhd output.p10 width height");
        const int width=std::stoi(argv[3]), height=std::stoi(argv[4]);
        if (width < 16 || width > 7680 || width%16 || height < 16 || height > 4320)
            throw std::runtime_error("Audit geometry outside bounded 8K limit");
        std::ifstream f(argv[1], std::ios::binary|std::ios::ate);
        const auto size=f.tellg();
        if (!f || size <= 0 || size > 128*1024*1024) throw std::runtime_error("Invalid packet length");
        Bytes packet(static_cast<size_t>(size)); f.seekg(0);
        f.read(reinterpret_cast<char*>(packet.data()), size);
        if (!f) throw std::runtime_error("Packet read failed");
        // Input and output survive decoder teardown on every exit path.
        Bytes yu64;
        CFHD_DecoderRef dr=nullptr; const auto opened=CFHD_OpenDecoder(&dr,nullptr);
        std::unique_ptr<void,DecoderDelete> decoder(dr); Check(opened);
        int w=0,h=0; CFHD_PixelFormat format=CFHD_PIXEL_FORMAT_UNKNOWN;
        Check(CFHD_PrepareToDecode(decoder.get(),0,0,CFHD_PIXEL_FORMAT_YU64,
            CFHD_DECODED_RESOLUTION_FULL,CFHD_DECODING_FLAGS_NONE,packet.data(),packet.size(),&w,&h,&format));
        if (w!=width || h!=height || format!=CFHD_PIXEL_FORMAT_YU64) throw std::runtime_error("Unexpected SDK geometry/format");
        CFHD_MetadataRef mr=nullptr; const auto mo=CFHD_OpenMetadata(&mr);
        std::unique_ptr<void,MetadataDelete> metadata(mr); Check(mo);
        Check(CFHD_InitSampleMetadata(metadata.get(),METADATATYPE_ORIGINAL,packet.data(),packet.size()));
        unsigned cpu=1,process=0xffff,yuv=2,rgb=1,filter=0;
        Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_CPU_MAX,METADATATYPE_HIDDEN,&cpu,sizeof(cpu)));
        Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_PROCESS_PATH,METADATATYPE_UINT32,&process,sizeof(process)));
        Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_YUV,METADATATYPE_UINT32,&yuv,sizeof(yuv)));
        Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_RGB,METADATATYPE_UINT32,&rgb,sizeof(rgb)));
        Check(CFHD_SetActiveMetadata(decoder.get(),metadata.get(),TAG_COLORSPACE_FTR,METADATATYPE_UINT32,&filter,sizeof(filter)));
        int32_t pitch=0; Check(CFHD_GetImagePitch(w,format,&pitch));
        if (pitch < w*4 || pitch%16) throw std::runtime_error("Invalid SDK pitch");
        yu64.assign(size_t(pitch)*h,0);
        Check(CFHD_DecodeSample(decoder.get(),packet.data(),packet.size(),yu64.data(),pitch));
        Check(CFHD_CloseMetadata(metadata.release()));
        Check(CFHD_CloseDecoder(decoder.release()));
        // Match #138: inspect/publish pixels only after the SDK is closed.
        // YU64 is top-down LE Y0/Cr/Y1/Cb, p16; p10 = unsigned value >> 6.
        const auto out=Planar(yu64,w,h,pitch);
        std::ofstream file(argv[2],std::ios::binary);
        file.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size())); file.close();
        if (!file) throw std::runtime_error("Output write failed");
        std::cout << "YU64 top-down -> yuv422p10le " << w << 'x' << h << " pitch=" << pitch << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
