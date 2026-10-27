// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/standalone.h"
#include "standalone_codebook.h"
#include "entropy_fast.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hypercine { namespace standalone {
namespace {
// Wire tags from frozen Codec/codec.h; no SDK declarations enter this target.
enum WireTag {
    kSampleType = 1,
    kIndex = 2,
    kIndexEntry = 3,
    kMarker = 4,
    kTransformType = 10,
    kFrameCount = 11,
    kChannelCount = 12,
    kWaveletCount = 13,
    kSubbandCount = 14,
    kSpatialCount = 15,
    kFirstWavelet = 16,
    kFrameWidth = 20,
    kFrameHeight = 21,
    kFrameTrailer = 24,
    kLowpassSubband = 25,
    kLevelCount = 26,
    kLowpassWidth = 27,
    kLowpassHeight = 28,
    kMarginTop = 29,
    kMarginBottom = 30,
    kMarginLeft = 31,
    kMarginRight = 32,
    kPixelOffset = 33,
    kLowpassQuantization = 34,
    kPixelDepth = 35,
    kWaveletType = 37,
    kWaveletNumber = 38,
    kWaveletLevelTag = 39,
    kBandCount = 40,
    kHighpassWidth = 41,
    kHighpassHeight = 42,
    kLowpassBorder = 43,
    kHighpassBorder = 44,
    kLowpassScale = 45,
    kLowpassDivisor = 46,
    kBandNumber = 48,
    kBandWidth = 49,
    kBandHeight = 50,
    kBandSubband = 51,
    kBandEncoding = 52,
    kBandQuantization = 53,
    kBandScale = 54,
    kBandHeader = 55,
    kBandTrailer = 56,
    kChannel = 62,
    kSampleFlags = 68,
    kFrameNumber = 69,
    kPrecision = 70,
    kCodingFlags = 72,
    kQualityLow = 80,
    kQualityHigh = 81,
    kPrescale = 83,
    kEncodedFormat = 84,
    kDisplayHeight = 85,
    kColorSpace = 91,
    kSubbandSize = 0x2000, kLevelSize = 0x2100, kSampleSize = 0x2200
};
// Inverse of table17's integer cubic decompanding, identical tie policy to
// FillVleTable: largest mapped index <= magnitude, clamped to 1023.
constexpr std::array<unsigned,1024> InverseCubic() {
    std::array<unsigned,1024> cubic{};
    for (unsigned i=1;i<256;++i) {
        const unsigned m=std::min(1023u,i+static_cast<unsigned>((uint64_t{768}*i*i*i)/(256u*256u*256u)));
        cubic[m]=i;
    }
    unsigned previous=0;
    for(auto& x:cubic) { if (x) previous=x; else x=previous; }
    return cubic;
}
inline constexpr auto kInverseCubic=InverseCubic();
class Sample {
    // 27 HP bands contain <2*frame_pixels coefficients, <=4 bytes each.
    // Final LL payload and all headers fit in the remaining conservative bound.
    static constexpr std::size_t kCapacity = 8*kMaxPixels + 16384;
    void Check(std::size_t extra) const {
        if (extra > kCapacity-bytes.size()) throw std::length_error("sample capacity exceeded");
    }
public:
    std::vector<uint8_t> bytes;
    void U16(unsigned x) { Check(2); bytes.push_back(static_cast<uint8_t>(x>>8)); bytes.push_back(static_cast<uint8_t>(x)); }
    void Tag(int tag, unsigned value) {
        if (value>65535) throw std::length_error("wire tag value exceeds 16 bits");
        U16(static_cast<uint16_t>(tag)); U16(value); }
    void Append(const std::vector<uint8_t>& v) { Check(v.size()); bytes.insert(bytes.end(),v.begin(),v.end()); }
    void Patch32(std::size_t at, uint32_t value) {
        if (at > bytes.size() || bytes.size()-at < 4) throw std::out_of_range("sample patch");
        for (unsigned i=0;i<4;++i) bytes[at+i] = static_cast<uint8_t>(value >> (24-i*8));
    }
    std::size_t Begin(unsigned tag) { const auto n=bytes.size(); Tag(tag,0); return n; }
    void End(std::size_t at, unsigned tag) {
        if (bytes.size()%4 || bytes.size()<at+4) throw std::logic_error("unaligned chunk");
        const auto words=(bytes.size()-at-4)/4;
        if (words>0xFFFFFF) throw std::length_error("sample chunk too large");
        Patch32(at,(static_cast<uint32_t>(-static_cast<int>(tag | (words>>16)))<<16) | (words&65535));
    }
};
void Lowpass(Sample& s, const Plane& p) {
    s.Tag(kMarker,0x1A4A); s.Tag(kLowpassSubband,0); s.Tag(kLevelCount,3); s.Tag(kLowpassWidth,p.width); s.Tag(kLowpassHeight,p.height);
    for (int tag : {kMarginLeft,kMarginTop,kMarginRight,kMarginBottom,kPixelOffset}) s.Tag(tag,0);
    s.Tag(kLowpassQuantization,1); s.Tag(kPixelDepth,16);
    const auto start=s.Begin(kSubbandSize);
    s.Tag(kMarker,0x0F0F);
    for(int y=0;y<p.height;++y) for(int x=0;x<p.width;++x) s.U16(static_cast<uint16_t>(p.pixels[y*p.stride+x]));
    s.Tag(kMarker,0x1B4B); s.End(start,kSubbandSize);
}
}
std::vector<uint8_t> PackHighpass(const Plane& p, std::size_t capacity) {
    if (p.width<1 || p.width>kMaxDimension || p.width%8 || p.height<1 || p.height>kMaxDimension || p.stride<p.width || p.stride%8 ||
        !PlaneStorageFits(p.stride,p.height) ||
        p.pixels.size()<static_cast<std::size_t>(p.stride)*p.height)
        throw std::invalid_argument("invalid entropy plane");
    // <=26 bits per signed coefficient plus the 26-bit end marker and word
    // padding fit in 4*(N+1) bytes: one bounded allocation, no vector growth.
    detail::EntropyBits bits(capacity,4*(static_cast<std::size_t>(p.width)*p.height+1));
    unsigned run=0;
    auto flush=[&] {
        for(const auto& r:detail::runs17) while(run>=r.count) {
            bits.Put(r.code.bits,r.code.size); run-=r.count;
        }
    };
    for(int y=0;y<p.height;++y) for(int x=0;x<p.width;++x) {
        const int v=p.pixels[y*p.stride+x];
        if (!v) { ++run; continue; }
        if(run) flush();
        const auto magnitude=static_cast<unsigned>(v<0 ? -v:v);
        const auto code=detail::magnitude17[kInverseCubic[std::min(1023u,magnitude)]];
        bits.Put((code.bits<<1) | (v<0 ? 1u:0u),code.size+1);
    }
    flush(); bits.Put(detail::band_end17.bits,detail::band_end17.size); bits.Finish();
    return std::move(bits.bytes);
}
std::vector<uint8_t> EncodeBgra(const uint8_t* data, std::size_t bytes, int width, int height, std::size_t stride) {
    auto planes=ConvertBgra(data,bytes,width,height,stride);
    Sample s;
    s.Tag(kSampleType,9); s.Tag(kIndex,3);
    const auto index=s.bytes.size(); for(int i=0;i<3;++i) s.Tag(kIndexEntry,i);
    s.Tag(kTransformType,0); s.Tag(kFrameCount,1); s.Tag(kChannelCount,3); s.Tag(kEncodedFormat,1); s.Tag(-kColorSpace,2);
    s.Tag(kWaveletCount,3); s.Tag(kSubbandCount,10); s.Tag(kSpatialCount,2); s.Tag(kFirstWavelet,3);
    s.Tag(kFrameWidth,width); s.Tag(kFrameHeight,height); s.Tag(-kFrameNumber,0); s.Tag(kPrecision,10);
    s.Tag(-kDisplayHeight,height); s.Tag(-kQualityLow,6); s.Tag(-kQualityHigh,0); s.Tag(kPrescale,kSubbandSize); // 10-bit prescale 0,2,0
    const auto sample=s.Begin(kSampleSize);
    s.Tag(kSampleFlags,1); // progressive
    for(int c=0;c<3;++c) {
        if(c) { s.Tag(kSampleType,3); s.Tag(kChannel,c); }
        const auto begin=s.bytes.size();
        auto l1=ForwardLevel(planes[c],0);
        auto l2=ForwardLevel(l1.bands[0],2);
        auto l3=ForwardLevel(l2.bands[0],0);
        Lowpass(s,l3.bands[0]);
        WaveletLevel* levels[]={&l1,&l2,&l3};
        int subband=1;
        for(int lv=2;lv>=0;--lv) {
            auto& bands=levels[lv]->bands;
            const auto& p=bands[0];
            s.Tag(kMarker,0x0D0D); s.Tag(kWaveletType,lv==0?5:3); s.Tag(kWaveletNumber,lv+1); s.Tag(kWaveletLevelTag,lv+1);
            s.Tag(kBandCount,4); s.Tag(kHighpassWidth,p.width); s.Tag(kHighpassHeight,p.height);
            s.Tag(kLowpassBorder,0); s.Tag(kHighpassBorder,0); s.Tag(kLowpassScale,4u<<(2*lv)); s.Tag(kLowpassDivisor,0);
            const auto level=s.Begin(kLevelSize);
            for(int b=1;b<4;++b) {
                auto& band=bands[b];
                // Fixed FS3 initial low-frequency quantization, no rate control.
                constexpr unsigned quant=2;
                for(auto& v:band.pixels) v=Quantize(v,quant);
                const auto coeff=PackHighpass(band,4*(static_cast<std::size_t>(band.width)*band.height+1));
                s.Tag(kMarker,0x0E0E); s.Tag(kBandNumber,b); s.Tag(kCodingFlags,1);
                s.Tag(kBandWidth,band.width); s.Tag(kBandHeight,band.height); s.Tag(kBandSubband,subband++);
                s.Tag(kBandEncoding,3); s.Tag(kBandQuantization,quant); s.Tag(kBandScale,(b==3?1u:2u)<<(2*lv));
                const auto chunk=s.Begin(kSubbandSize); s.Tag(kBandHeader,0);
                s.Append(coeff);
                s.Tag(kBandTrailer,0); s.End(chunk,kSubbandSize);
            }
            s.Tag(kMarker,0x0C0C); s.End(level,kLevelSize);
        }
        s.Patch32(index+4*c,static_cast<uint32_t>(s.bytes.size()-begin));
    }
    s.Tag(kFrameTrailer,0); s.End(sample,kSampleSize);
    return std::move(s.bytes);
}
std::vector<uint8_t> EncodeBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride) {
    return EncodeBgra(data,bytes,128,128,stride);
}
}}
