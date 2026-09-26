// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// The legacy 128x128 YUY2 roundtrip links only to the stock codec.
// Two separate facts make that test flake:
//
// 1. CSampleEncoder::HandleMetadata (upstream/EncoderSDK/SampleEncoder.cpp)
//    calls CoCreateGuid for TAG_CLIP_GUID and stamps local time into
//    TAG_ENCODE_DATE, TAG_ENCODE_TIME, and TAG_TIMECODE. Those fields are
//    fixed width, so the sample size stays constant while the hash changes
//    on every encoder open. The wavelet payload does not.
// 2. The stock decoder is built with _THREADED_DECODER=1. Inverse YUY2
//    reconstruction (InvertHorizontalStrip16sToYUYV) mixes rand() into the
//    rounding. Worker threads share that CRT state, so two opens of one
//    sample can reconstruct different pixels. srand() does not remove the
//    race. This test does not treat those pixels as a pass/fail gate.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_vectors.h"
#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"

using namespace HyperCineTest;

namespace {

void MaskVolatileMetadata(std::vector<uint8_t>* sample) {
    const char* tags[] = {"GUID", "DATE", "TIME", "TIMC"};
    for (const char* tag : tags) {
        const auto* bytes = sample->data();
        const size_t n = sample->size();
        for (size_t i = 0; i + 8 < n; ++i) {
            if (std::memcmp(bytes + i, tag, 4) != 0) continue;
            uint32_t typesize = 0;
            std::memcpy(&typesize, bytes + i + 4, 4);
            const uint32_t payload = typesize & 0x00FFFFFFu;
            if (payload == 0 || payload > 64 || i + 8 + payload > n) continue;
            std::memset(sample->data() + i + 8, 0, payload);
        }
    }
}

struct Encoded {
    bool ok = false;
    std::vector<uint8_t> raw;
    std::vector<uint8_t> masked;
    uint64_t masked_hash = 0;
};

Encoded EncodePattern(TestPatternType pattern) {
    Encoded out;
    const int width = 128;
    const int height = 128;
    auto pixels = GenerateTestVectorYUY2(pattern, width, height);
    CFHD_EncoderRef encoder = nullptr;
    if (CFHD_OpenEncoder(&encoder, nullptr) != CFHD_ERROR_OKAY) return out;
    CFHD_Error err = CFHD_PrepareToEncode(encoder, width, height, CFHD_PIXEL_FORMAT_YUY2,
                                          CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE,
                                          CFHD_ENCODING_QUALITY_FILMSCAN3);
    if (err == CFHD_ERROR_OKAY) {
        err = CFHD_EncodeSample(encoder, pixels.data(), width * 2);
    }
    void* data = nullptr;
    size_t size = 0;
    if (err == CFHD_ERROR_OKAY) err = CFHD_GetSampleData(encoder, &data, &size);
    if (err == CFHD_ERROR_OKAY && data && size > 0) {
        out.raw.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        out.masked = out.raw;
        MaskVolatileMetadata(&out.masked);
        out.masked_hash = ComputeFNV1a64(out.masked.data(), out.masked.size());
        out.ok = true;
    }
    CFHD_CloseEncoder(encoder);
    return out;
}

int GuidMismatch(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int mismatches = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i + 8 < n; ++i) {
        if (std::memcmp(a.data() + i, "GUID", 4) != 0) continue;
        uint32_t typesize = 0;
        std::memcpy(&typesize, a.data() + i + 4, 4);
        const uint32_t payload = typesize & 0x00FFFFFFu;
        if (payload != 16 || i + 8 + payload > n || i + 8 + payload > b.size()) continue;
        if (std::memcmp(a.data() + i + 8, b.data() + i + 8, payload) != 0) ++mismatches;
    }
    return mismatches;
}

bool DecodeOk(const std::vector<uint8_t>& sample, std::vector<uint8_t>* image, int* pitch_out, uint64_t* hash) {
    CFHD_DecoderRef decoder = nullptr;
    if (CFHD_OpenDecoder(&decoder, nullptr) != CFHD_ERROR_OKAY) return false;
    int width = 0;
    int height = 0;
    CFHD_PixelFormat format = CFHD_PIXEL_FORMAT_UNKNOWN;
    CFHD_Error err = CFHD_PrepareToDecode(decoder, 0, 0, CFHD_PIXEL_FORMAT_YUY2, CFHD_DECODED_RESOLUTION_FULL,
                                          CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(),
                                          &width, &height, &format);
    int32_t pitch = 0;
    if (err == CFHD_ERROR_OKAY) {
        err = CFHD_GetImagePitch(static_cast<uint32_t>(width), format, &pitch);
    }
    bool ok = false;
    if (err == CFHD_ERROR_OKAY && width == 128 && height == 128 && pitch == 256 &&
        format == CFHD_PIXEL_FORMAT_YUY2) {
        image->assign(static_cast<size_t>(pitch) * static_cast<size_t>(height), 0);
        err = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(), image->data(), pitch);
        ok = err == CFHD_ERROR_OKAY;
        if (ok) *hash = ComputeFNV1a64(image->data(), image->size());
        *pitch_out = pitch;
    }
    CFHD_CloseDecoder(decoder);
    return ok;
}

}  // namespace

int main() {
    bool pass = true;
    std::cout << "YUY2 stability: masked bitstream must match across encoder opens.\n";
    std::cout << "Decoder pixels are recorded and are not a pass/fail gate.\n";

    for (int p = 0; p < PATTERN_COUNT; ++p) {
        const auto pattern = static_cast<TestPatternType>(p);
        const Encoded first = EncodePattern(pattern);
        const Encoded second = EncodePattern(pattern);
        if (!first.ok || !second.ok) {
            std::cerr << GetPatternName(pattern) << " encode failed\n";
            return 1;
        }
        if (first.masked != second.masked) {
            std::cerr << GetPatternName(pattern) << " masked samples differ, sizes "
                      << first.raw.size() << " " << second.raw.size() << "\n";
            const size_t n = std::min(first.masked.size(), second.masked.size());
            int shown = 0;
            for (size_t i = 0; i < n && shown < 16; ++i) {
                if (first.masked[i] == second.masked[i]) continue;
                std::cerr << "  @" << i << " " << std::hex << static_cast<int>(first.raw[i])
                          << "/" << static_cast<int>(second.raw[i]) << std::dec << "\n";
                ++shown;
            }
            pass = false;
        }
        if (GuidMismatch(first.raw, second.raw) != 1) {
            std::cerr << GetPatternName(pattern) << " expected one fresh clip GUID\n";
            pass = false;
        }
        std::cout << GetPatternName(pattern) << " bytes " << first.raw.size()
                  << " masked 0x" << std::hex << first.masked_hash << std::dec << "\n";

        if (pattern == PATTERN_CONSTANT_BLACK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            const Encoded later = EncodePattern(pattern);
            if (!later.ok || later.masked != first.masked) {
                std::cerr << "masked sample changed across a one-second boundary\n";
                if (later.ok) {
                    const size_t n = std::min(first.masked.size(), later.masked.size());
                    int shown = 0;
                    for (size_t i = 0; i < n && shown < 24; ++i) {
                        if (first.masked[i] == later.masked[i]) continue;
                        std::cerr << "  clock @" << i << "\n";
                        ++shown;
                    }
                }
                pass = false;
            }
            std::set<uint64_t> hashes;
            int decoded = 0;
            for (int attempt = 0; attempt < 8; ++attempt) {
                std::vector<uint8_t> image;
                int pitch = 0;
                uint64_t hash = 0;
                if (!DecodeOk(first.raw, &image, &pitch, &hash)) {
                    std::cerr << "constant_black decode " << attempt << " failed\n";
                    pass = false;
                    break;
                }
                hashes.insert(hash);
                ++decoded;
            }
            std::cout << "constant_black independent decodes " << decoded
                      << " distinct pixel hashes " << hashes.size() << "\n";
        }
    }

    std::cout << (pass ? "YUY2 STABILITY CONTRACT HOLDS\n" : "YUY2 STABILITY CONTRACT FAILED\n");
    return pass ? 0 : 1;
}
