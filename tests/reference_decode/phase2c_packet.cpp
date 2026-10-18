// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// CPU reference sequence from VerifyPhase2CIdentity; no GPU/library dispatch.
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include "test_vectors.h"
#include "hypercine/canonical.h"
#include "hypercine/entropy_parallel.h"
#include "CFHDEncoder.h"
extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "metadata.h"
}
#include "VideoBuffers.h"
#define private public
#include "SampleEncoder.h"
#undef private

struct CloseEncoder { void operator()(void* p) const { CFHD_CloseEncoder(p); } };
void Require(bool b) { if (!b) throw std::runtime_error("reference export failed"); }
void Save(const char* path,const uint8_t* bytes,size_t size) {
    std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes),size); Require(bool(out));
}
int main(int argc,char** argv) {
    try {
        Require(argc==3);
        const auto raw=hypercine::MakeFrame(1920,1080,hypercine::CanonicalPattern::Canonical);
        HyperCineTest::AlignedVector<uint8_t> frame(raw.begin(),raw.end());
        CFHD_EncoderRef ref=nullptr;
        auto err=CFHD_OpenEncoder(&ref,nullptr); std::unique_ptr<void,CloseEncoder> owner(ref); Require(err==0);
        Require(CFHD_PrepareToEncode(ref,1920,1080,CFHD_PIXEL_FORMAT_BGRA,CFHD_ENCODED_FORMAT_YUV_422,
            CFHD_ENCODING_FLAGS_NONE,CFHD_ENCODING_QUALITY_FILMSCAN3)==0);
        Require(CFHD_EncodeSample(ref,frame.data(),1920*4)==0);
        auto* sample=static_cast<CSampleEncoder*>(ref);
        auto* enc=reinterpret_cast<ENCODER*>(sample->m_encoder);
        auto** transforms=reinterpret_cast<TRANSFORM**>(sample->m_transformArray);
        Require(enc && transforms && sample->m_channelCount==3);
        uint32_t guid[4]={0x1337BEEF,0x20260420,0xCAFEBABE,0x43464844};
        char date[16]="2026-01-01",time[16]="00:00:00",tc[16]="00:00:00:00";
        Require(enc->metadata.global.block && enc->metadata.global.size);
        AddMetadata(&enc->metadata.global,TAG_CLIP_GUID,'G',16,guid);
        AddMetadata(&enc->metadata.global,TAG_ENCODE_DATE,'c',10,reinterpret_cast<uint32_t*>(date));
        AddMetadata(&enc->metadata.global,TAG_ENCODE_TIME,'c',8,reinterpret_cast<uint32_t*>(time));
        AddMetadata(&enc->metadata.global,TAG_TIMECODE,'c',11,reinterpret_cast<uint32_t*>(tc));
        Require(hypercine::RefreshUpstreamPyramid(enc,transforms,3,frame.data(),1920*4,
            sample->m_scratchBuffer,sample->m_scratchBufferSize));
        const auto refs=hypercine::EnumerateBands(transforms,3); Require(refs.size()==30);
        // Preserve the per-band external-encode state progression preceding the
        // final serial reference in VerifyPhase2CIdentity (not repeat encoding).
        for (const auto& band:refs) {
            auto* image=transforms[band.channel]->wavelet[band.wavelet_index];
            HyperCineTest::AlignedVector<int16_t> coeff(size_t(band.width)*band.height);
            for(int y=0;y<band.height;++y) std::memcpy(coeff.data()+size_t(y)*band.width,
                reinterpret_cast<const uint8_t*>(image->band[band.band])+size_t(y)*image->pitch,band.width*2);
            hypercine::ExternalCoeff ext{};
            ext.channel=band.channel; ext.level=band.level; ext.band=band.band; ext.subband=band.subband;
            ext.lowpass=band.lowpass; ext.data=coeff.data(); ext.elem_bytes=2; ext.width=band.width; ext.height=band.height;
            hypercine::EncodedPiece piece;
            Require(hypercine::EncodeOneExternalBand(enc,transforms,ext,&piece) && piece.ok);
        }
        const auto before=enc->frame_number;
        const auto packet=hypercine::EncodeEntropy(enc,transforms,3,1,hypercine::EntropySchedule::Serial,true);
        Require(packet.success && packet.byte_match && !packet.bitstream.empty());
        Save(argv[1],packet.bitstream.data(),packet.bitstream.size()); Save(argv[2],frame.data(),frame.size());
        std::cout << "{\"source\":\"VerifyPhase2CIdentity CPU reference sequence\",\"width\":1920,\"height\":1080,"
            << "\"frame_number_before\":" << before << ",\"frame_number_after\":" << enc->frame_number
            << ",\"gop_length\":" << static_cast<int>(enc->gop_length) << ",\"sample_bytes\":" << packet.bitstream.size()
            << ",\"serial_reference_byte_match\":true,\"exact_M4_sample\":\"UNAVAILABLE\"}\n";
        return 0;
    } catch (...) { std::cerr << "reference export failed\n"; return 2; }
}
