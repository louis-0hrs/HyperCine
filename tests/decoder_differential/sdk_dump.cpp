// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_vectors.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"

namespace {
using Bytes = HyperCineTest::AlignedVector<uint8_t>;
using Packet = std::vector<uint8_t>;
struct DecoderDelete { void operator()(void* p) const { if (p) CFHD_CloseDecoder(p); } };
struct MetadataDelete { void operator()(void* p) const { if (p) CFHD_CloseMetadata(p); } };
void Check(CFHD_Error e) { if (e != CFHD_ERROR_OKAY) throw std::runtime_error("SDK status=" + std::to_string(int(e))); }
unsigned U16(const Bytes& b, size_t i) { return b.at(i) | (unsigned(b.at(i+1)) << 8); }
void Put16(Bytes& b, size_t i, unsigned v) { b.at(i)=uint8_t(v); b.at(i+1)=uint8_t(v>>8); }
bool IsBgra(const std::string& format) {
    if (format == "bgra") return true;
    if (format == "yuv422p10le") return false;
    throw std::runtime_error("Unsupported output format selection");
}
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
Bytes Tighten(const Bytes& source, int width, int height, int pitch, int pixel_bytes) {
    Bytes out(size_t(width)*height*pixel_bytes);
    for (int y=0;y<height;++y) {
        const size_t from=size_t(y)*pitch, to=size_t(y)*width*pixel_bytes;
        for (int i=0;i<width*pixel_bytes;++i) out.at(to+i)=source.at(from+i);
    }
    return out;
}
void SelfTest() {
    {
        Bytes input(32,0xcd); // Two 2-pixel rows, 16-byte padded pitch.
        const unsigned values[]={0,65535,64,32768,65472,128,256,512};
        for (size_t row=0;row<2;++row) for (size_t i=0;i<4;++i) Put16(input,row*16+i*2,values[row*4+i]);
        const auto result=Planar(input,2,2,16);
        const unsigned expected[]={0,1,1023,4,512,8,1023,2};
        for (size_t i=0;i<8;++i) if (U16(result,i*2)!=expected[i]) throw std::runtime_error("YU64 packing test failed");
        for (size_t row=0;row<2;++row) for (size_t i=8;i<16;++i)
            if (input[row*16+i]!=0xcd) throw std::runtime_error("Input padding modified");
    }
    {
        Bytes padded(2*32,0xee); // Two BGRA rows, eight bytes of trailing padding.
        for (size_t i=0;i<8;++i) padded.at(i)=uint8_t(i);
        for (size_t i=0;i<8;++i) padded.at(32+i)=uint8_t(16+i);
        for (size_t row=0;row<2;++row) for (size_t i=8;i<32;++i)
            if (padded[row*32+i]!=0xee) throw std::runtime_error("BGRA padding setup failed");
        const auto tight=Tighten(padded,2,2,32,4);
        if (tight.size()!=16) throw std::runtime_error("BGRA tighten length failed");
        for (size_t i=0;i<8;++i) if (tight.at(i)!=i) throw std::runtime_error("BGRA tighten row 0 failed");
        for (size_t i=0;i<8;++i) if (tight.at(8+i)!=16+i) throw std::runtime_error("BGRA tighten row 1 failed");
    }
}
void ValidateGeometry(int width, int height) {
    if (width < 16 || width > 7680 || width%16 || height < 16 || height > 4320)
        throw std::runtime_error("Audit geometry outside bounded 8K limit");
}
Packet ReadPacket(const std::string& path) {
    std::ifstream f(path, std::ios::binary|std::ios::ate);
    const auto size=f.tellg();
    if (!f || size <= 0 || size > 128*1024*1024) throw std::runtime_error("Invalid packet length");
    Packet packet(static_cast<size_t>(size)); f.seekg(0);
    f.read(reinterpret_cast<char*>(packet.data()), size);
    if (!f) throw std::runtime_error("Packet read failed");
    return packet;
}
void Configure(CFHD_DecoderRef decoder, CFHD_MetadataRef metadata, Packet& packet) {
    Check(CFHD_InitSampleMetadata(metadata,METADATATYPE_ORIGINAL,packet.data(),packet.size()));
    unsigned cpu=1,process=0xffff,yuv=2,rgb=1,filter=0;
    Check(CFHD_SetActiveMetadata(decoder,metadata,TAG_CPU_MAX,METADATATYPE_HIDDEN,&cpu,sizeof(cpu)));
    Check(CFHD_SetActiveMetadata(decoder,metadata,TAG_PROCESS_PATH,METADATATYPE_UINT32,&process,sizeof(process)));
    Check(CFHD_SetActiveMetadata(decoder,metadata,TAG_COLORSPACE_YUV,METADATATYPE_UINT32,&yuv,sizeof(yuv)));
    Check(CFHD_SetActiveMetadata(decoder,metadata,TAG_COLORSPACE_RGB,METADATATYPE_UINT32,&rgb,sizeof(rgb)));
    Check(CFHD_SetActiveMetadata(decoder,metadata,TAG_COLORSPACE_FTR,METADATATYPE_UINT32,&filter,sizeof(filter)));
}
std::vector<std::string> ReadList(const std::string& path) {
    std::ifstream list(path);
    if (!list) throw std::runtime_error("Packet list unreadable");
    std::vector<std::string> paths;
    std::string line;
    while (std::getline(list,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (!line.empty()) paths.push_back(line);
    }
    if (paths.empty()) throw std::runtime_error("Packet list empty");
    return paths;
}
std::set<int> ParseKeep(const std::string& spec, int count) {
    std::set<int> keep;
    if (spec.empty()) { for (int i=0;i<count;++i) keep.insert(i); return keep; }
    std::stringstream stream(spec);
    std::string item;
    while (std::getline(stream,item,',')) {
        if (item.empty()) continue;
        const int index=std::stoi(item);
        if (index < 0 || index >= count) throw std::runtime_error("Keep index outside sequence");
        keep.insert(index);
    }
    if (keep.empty()) throw std::runtime_error("Keep index set empty");
    return keep;
}
void WriteFrame(const std::string& directory, int index, const Bytes& bytes, const char* extension) {
    char name[32];
    std::snprintf(name,sizeof(name),"frame_%04d.%s",index,extension);
    std::ofstream file(directory + "/" + name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("Output write failed");
}
void Sequence(const std::string& list, const std::string& directory, int width, int height,
              const std::string& format, const std::string& keep_spec) {
    ValidateGeometry(width,height);
    const auto paths=ReadList(list);
    const auto keep=ParseKeep(keep_spec,int(paths.size()));
    const bool bgra=IsBgra(format);
    CFHD_DecoderRef dr=nullptr; const auto opened=CFHD_OpenDecoder(&dr,nullptr);
    std::unique_ptr<void,DecoderDelete> decoder(dr); Check(opened);
    CFHD_MetadataRef mr=nullptr; const auto mo=CFHD_OpenMetadata(&mr);
    std::unique_ptr<void,MetadataDelete> metadata(mr); Check(mo);
    Bytes buffer; int pitch=0;
    for (size_t index=0;index<paths.size();++index) {
        auto packet=ReadPacket(paths[index]);
        if (index==0) {
            int w=0,h=0; CFHD_PixelFormat decoded=CFHD_PIXEL_FORMAT_UNKNOWN;
            Check(CFHD_PrepareToDecode(decoder.get(),0,0,
                bgra ? CFHD_PIXEL_FORMAT_BGRA : CFHD_PIXEL_FORMAT_YU64,
                CFHD_DECODED_RESOLUTION_FULL,CFHD_DECODING_FLAGS_NONE,
                packet.data(),packet.size(),&w,&h,&decoded));
            if (w!=width || h!=height || decoded!=(bgra ? CFHD_PIXEL_FORMAT_BGRA : CFHD_PIXEL_FORMAT_YU64))
                throw std::runtime_error("Unexpected SDK sequence geometry/format");
            int32_t requested=0; Check(CFHD_GetImagePitch(w,decoded,&requested));
            pitch=requested;
            if (pitch < w*4 || pitch%16) throw std::runtime_error("Invalid SDK sequence pitch");
            buffer.assign(size_t(pitch)*h,0);
            Configure(decoder.get(),metadata.get(),packet);
        }
        Check(CFHD_DecodeSample(decoder.get(),packet.data(),packet.size(),buffer.data(),pitch));
        const bool kept=keep.count(int(index))!=0;
        if (kept) {
            if (bgra) WriteFrame(directory,int(index),Tighten(buffer,width,height,pitch,4),"bgra");
            else {
                const auto out=Planar(buffer,width,height,pitch);
                Bytes bytes(out.begin(),out.end());
                WriteFrame(directory,int(index),bytes,"p10");
            }
        }
        std::cout << "frame " << index << " bytes=" << packet.size()
                  << " kept=" << (kept ? 1 : 0) << '\n';
    }
    std::cout << "sequence frames=" << paths.size() << " kept=" << keep.size()
              << " format=" << (bgra ? "bgra" : "yuv422p10le") << '\n';
}
void Single(const char* packet_path, const char* output_path, int width, int height,
            const std::string& format) {
    ValidateGeometry(width,height);
    const bool bgra=IsBgra(format);
    auto packet=ReadPacket(packet_path);
    CFHD_DecoderRef dr=nullptr; const auto opened=CFHD_OpenDecoder(&dr,nullptr);
    std::unique_ptr<void,DecoderDelete> decoder(dr); Check(opened);
    int w=0,h=0; CFHD_PixelFormat decoded=CFHD_PIXEL_FORMAT_UNKNOWN;
    Check(CFHD_PrepareToDecode(decoder.get(),0,0,
        bgra ? CFHD_PIXEL_FORMAT_BGRA : CFHD_PIXEL_FORMAT_YU64,
        CFHD_DECODED_RESOLUTION_FULL,CFHD_DECODING_FLAGS_NONE,
        packet.data(),packet.size(),&w,&h,&decoded));
    if (w!=width || h!=height || decoded!=(bgra ? CFHD_PIXEL_FORMAT_BGRA : CFHD_PIXEL_FORMAT_YU64))
        throw std::runtime_error("Unexpected SDK geometry/format");
    CFHD_MetadataRef mr=nullptr; const auto mo=CFHD_OpenMetadata(&mr);
    std::unique_ptr<void,MetadataDelete> metadata(mr); Check(mo);
    Configure(decoder.get(),metadata.get(),packet);
    int32_t pitch=0; Check(CFHD_GetImagePitch(w,decoded,&pitch));
    if (pitch < w*4 || pitch%16) throw std::runtime_error("Invalid SDK pitch");
    Bytes buffer(size_t(pitch)*h,0);
    Check(CFHD_DecodeSample(decoder.get(),packet.data(),packet.size(),buffer.data(),pitch));
    Bytes out;
    if (bgra) out=Tighten(buffer,width,height,pitch,4);
    else {
        const auto planar=Planar(buffer,w,h,pitch);
        out.assign(planar.begin(),planar.end());
    }
    std::ofstream file(output_path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size())); file.close();
    if (!file) throw std::runtime_error("Output write failed");
    std::cout << (bgra ? "BGRA " : "YU64 top-down -> yuv422p10le ") << w << 'x' << h
              << " pitch=" << pitch << '\n';
}
}
int main(int argc, char** argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--self-test") { SelfTest(); return 0; }
        if (argc>=6 && std::string(argv[1])=="--sequence") {
            if (argc>8) throw std::runtime_error("usage: decoder_sdk_dump --sequence list outdir width height [format] [keep]");
            const std::string format=argc>6 ? argv[6] : "yuv422p10le";
            const std::string keep=argc>7 ? argv[7] : "";
            Sequence(argv[2],argv[3],std::stoi(argv[4]),std::stoi(argv[5]),format,keep);
            return 0;
        }
        if (argc != 5 && argc != 6)
            throw std::runtime_error("usage: decoder_sdk_dump packet.cfhd output width height [yuv422p10le|bgra]");
        const std::string format=argc==6 ? argv[5] : "yuv422p10le";
        Single(argv[1],argv[2],std::stoi(argv[3]),std::stoi(argv[4]),format);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
