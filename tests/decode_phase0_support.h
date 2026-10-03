// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_vectors.h"
#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"
#include "decoder.h"
#include "ISampleDecoder.h"
#include "SampleDecoder.h"

namespace DecodePhase0 {
using Bytes = HyperCineTest::AlignedVector<uint8_t>;
constexpr unsigned Seed = 0x1337BEEF;
// Research-only access to the actual member declaration, without a guessed ABI
// prefix or redefining the class. Explicit instantiation permits naming private
// members ([temp.explicit]); the resulting typed member pointer preserves layout.
// This accessor is pinned to this upstream revision and is not a public API.
struct DecoderMemberTag {
    using type = DECODER* CSampleDecoder::*;
    friend type DecoderMember(DecoderMemberTag);
};
template<class Tag, typename Tag::type Member> struct ResearchMemberAccess {
    friend typename Tag::type DecoderMember(Tag) { return Member; }
};
template struct ResearchMemberAccess<DecoderMemberTag, &CSampleDecoder::m_decoder>;
inline DECODER* NativeDecoder(void* handle) {
    return static_cast<CSampleDecoder*>(handle)->*DecoderMember(DecoderMemberTag{});
}
inline void Check(CFHD_Error e, const char* operation) {
    if (e != CFHD_ERROR_OKAY)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(e));
}
struct EncoderDeleter { void operator()(void* p) const { if (p) CFHD_CloseEncoder(p); } };
struct DecoderDeleter { void operator()(void* p) const { if (p) CFHD_CloseDecoder(p); } };
struct MetadataDeleter { void operator()(void* p) const { if (p) CFHD_CloseMetadata(p); } };
using Encoder = std::unique_ptr<void, EncoderDeleter>;
using DecoderHandle = std::unique_ptr<void, DecoderDeleter>;
using Metadata = std::unique_ptr<void, MetadataDeleter>;
inline std::string Hash(const void* p, size_t n) {
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << HyperCineTest::ComputeFNV1a64(p, n);
    return s.str();
}
inline const char* PatternName(int p) {
    static const char* names[] = {"flat", "gradient", "noise", "texture", "edges"};
    return names[p];
}
inline Bytes MakeInput(int w, int h, int pattern) {
    Bytes out(size_t(w) * h * 2);
    uint32_t state = Seed + uint32_t(pattern);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        const size_t i = (size_t(y) * w + x) * 2;
        auto luma = [&](int px) -> uint8_t {
            state = state * 1664525u + 1013904223u;
            switch (pattern) {
            case 0: return 126;
            case 1: return uint8_t(16 + 219 * px / (w - 1));
            case 2: return uint8_t(16 + ((state >> 24) % 220));
            case 3: return ((px / 2 + y / 2) & 1) ? 235 : 16;
            default: return (((px / 64) ^ (y / 64)) & 1) ? 235 : 16;
            }
        };
        out[i] = luma(x); out[i + 2] = luma(x + 1);
        out[i + 1] = pattern == 0 ? 128 : uint8_t(96 + (y * 64 / h));
        out[i + 3] = pattern == 0 ? 128 : uint8_t(96 + (x * 64 / w));
    }
    return out;
}
inline uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
// Walk container tuples; do not search arbitrary VLC bytes for ASCII tags.
// Preserve every metadata tag, type, length and padding byte. Only zero the
// GUID/clock payloads in declared metadata chunks in our generated samples.
inline void NormalizeMetadata(Bytes& sample) {
    size_t i = 0;
    while (i + 4 <= sample.size()) {
        const int16_t raw = int16_t(BE16(sample.data() + i));
        const unsigned tag = raw < 0 ? unsigned(-int(raw)) : unsigned(raw);
        const unsigned value = BE16(sample.data() + i + 2);
        i += 4;
        if (tag == CODEC_TAG_INDEX) { i += size_t(value) * 4; continue; }
        if ((tag & 0xff00) == CODEC_TAG_SUBBAND_SIZE) {
            const size_t n = size_t(((tag & 255) << 16) | value) * 4;
            if (n > sample.size() - i) throw std::runtime_error("invalid subband size");
            i += n; continue;
        }
        if (tag >= CODEC_TAG_CHUNK && tag < CODEC_TAG_CUSTOM_CHUNK24BIT) {
            const size_t n = size_t(value) * 4;
            if (n > sample.size() - i) throw std::runtime_error("invalid chunk size");
            if (tag == CODEC_TAG_METADATA) {
                for (size_t m = i; m + 8 <= i + n;) {
                    uint32_t ts = 0; std::memcpy(&ts, sample.data() + m + 4, 4);
                    const size_t size = ts & 0xffffffu;
                    const size_t padded = (size + 3) & ~size_t(3);
                    if (padded > i + n - m - 8) throw std::runtime_error("invalid metadata tuple");
                    const char* tags[] = {"GUID", "DATE", "TIME", "TIMC"};
                    for (const char* name : tags)
                        if (std::memcmp(sample.data() + m, name, 4) == 0)
                            std::memset(sample.data() + m + 8, 0, size);
                    m += 8 + padded;
                }
            }
            i += n;
        }
    }
    if (i != sample.size()) throw std::runtime_error("unconsumed sample bytes");
}
struct Sample {
    int width, height, pattern;
    Bytes compressed;
    std::string input_hash;
};
inline std::vector<Sample> Generate(int w, int h, int patterns = 5) {
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr), "open encoder");
    Encoder encoder(raw);
    Check(CFHD_PrepareToEncode(raw, w, h, CFHD_PIXEL_FORMAT_YUY2,
          CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE,
          CFHD_ENCODING_QUALITY_FILMSCAN3), "prepare encoder");
    std::vector<Sample> result;
    for (int p = 0; p < patterns; ++p) {
        auto input = MakeInput(w, h, p);
        std::srand(Seed);
        Check(CFHD_EncodeSample(raw, input.data(), w * 2), "encode");
        void* data = nullptr; size_t size = 0;
        Check(CFHD_GetSampleData(raw, &data, &size), "get sample");
        if (!data || !size) throw std::runtime_error("empty encoded sample");
        Sample s{w, h, p, Bytes(size), Hash(input.data(), input.size())};
        std::memcpy(s.compressed.data(), data, size);
        NormalizeMetadata(s.compressed);
        result.push_back(std::move(s));
    }
    return result;
}
class ReferenceDecoder {
    DecoderHandle decoder_;
    Metadata metadata_;
public:
    Bytes output;
    int width = 0, height = 0;
    int32_t pitch = 0;
    CFHD_PixelFormat format;
    explicit ReferenceDecoder(Sample& s, CFHD_PixelFormat requested) : format(requested) {
        void* raw = nullptr; Check(CFHD_OpenDecoder(&raw, nullptr), "open decoder");
        decoder_.reset(raw);
        Check(CFHD_PrepareToDecode(raw, 0, 0, requested, CFHD_DECODED_RESOLUTION_FULL,
              CFHD_DECODING_FLAGS_NONE, s.compressed.data(), s.compressed.size(),
              &width, &height, &format), "prepare decoder");
        if (width != s.width || height != s.height || format != requested)
            throw std::runtime_error("unexpected decode geometry/format");
        Check(CFHD_GetImagePitch(width, format, &pitch), "get pitch");
        if (pitch <= 0 || (pitch & 15)) throw std::runtime_error("unaligned pitch");
        output.resize(size_t(pitch) * height);
        CFHD_MetadataRef md = nullptr;
        Check(CFHD_OpenMetadata(&md), "open metadata"); metadata_.reset(md);
        Check(CFHD_InitSampleMetadata(md, METADATATYPE_ORIGINAL,
              s.compressed.data(), uint32_t(s.compressed.size())), "init metadata");
        uint32_t process = PROCESSING_ALL_ON, cpu = 1, ignore = 1;
        Check(CFHD_SetActiveMetadata(raw, md, TAG_PROCESS_PATH, METADATATYPE_UINT32,
              &process, sizeof(process)), "process path");
        Check(CFHD_SetActiveMetadata(raw, md, TAG_CPU_MAX, METADATATYPE_HIDDEN,
              &cpu, sizeof(cpu)), "cpu max");
        Check(CFHD_SetActiveMetadata(raw, md, TAG_IGNORE_DATABASE, METADATATYPE_HIDDEN,
              &ignore, sizeof(ignore)), "ignore external look database");
        // _DELAY_THREAD_START=1: clamp before the first decode. Also make the
        // output-worker capability count deterministic before metadata dispatch.
        auto* d = NativeDecoder(raw);
        d->cfhddata.cpu_limit = 1;
        d->thread_cntrl.limit = 1;
        d->thread_cntrl.set_thread_params = 1;
        d->thread_cntrl.capabilities = (d->thread_cntrl.capabilities & 0xffff) | (1 << 16);
        VerifyThreads();
    }
    void VerifyThreads() const {
        auto* d = NativeDecoder(decoder_.get());
        if (d->entropy_worker_new.pool.thread_count != 0 || d->worker_thread.pool.thread_count > 1)
            throw std::runtime_error("reference decoder is not synchronous");
    }
    int CodecPrecision() const {
        return NativeDecoder(decoder_.get())->codec.precision;
    }
    DECODER* GetNative() const { return NativeDecoder(decoder_.get()); }
    void* GetHandle() const { return decoder_.get(); }
    // Seed outside the timed region. Only one decoder runs at a time: rand()
    // belongs to the process CRT, not the decoder instance.
    void SeedDither() const { std::srand(Seed); }
    void Decode(Sample& s) {
        Check(CFHD_DecodeSample(decoder_.get(), s.compressed.data(),
              s.compressed.size(), output.data(), pitch), "decode sample");
    }
};
} // namespace DecodePhase0
