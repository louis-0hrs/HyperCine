// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Investigation aid for the test_decode_phase3b0 Release crash (issue #13/#38).
// Drives one input pixel-format -> encoded-format conversion through the unchanged
// upstream encoder and reports where it fails. Not a CTest target and not a
// production tool; it exists so the crash can be reproduced without the full probe.
//
// Usage: repro_phase3b0_convert <input-format> <encoded-format> [quality] [width] [height]
//   Formats are the CFHD_* enum values (e.g. CFHD_PIXEL_FORMAT_RG24,
//   CFHD_ENCODED_FORMAT_RGBA_4444). Defaults: YUY2 -> YUV_422, quality Low, 64x64.
// Exit codes: 0 ok, 10 open, 11 prepare, 12 encode, 13 sample, 139 SIGSEGV (crash).
//
// Build (examples):
//   g++ -std=c++17 -O1 -g -Iupstream/Common -Iinclude tools/repro_phase3b0_convert.cpp \
//       <build>/upstream/libCFHDCodec.a -o repro_phase3b0_convert -lpthread -luuid
//   # ASan/UBSan: add -fsanitize=address,undefined -fno-omit-frame-pointer, and link
//   # the ASan/UBSan build of the same static library.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "CFHDEncoder.h"
#include "test_vectors.h"

using HyperCineTest::AlignedVector;

namespace {

int BytesPerPixel(int format) {
    switch (format) {
        case CFHD_PIXEL_FORMAT_RG24:  return 3;
        case CFHD_PIXEL_FORMAT_BGRA:  return 4;
        case CFHD_PIXEL_FORMAT_RG48:  return 6;
        case CFHD_PIXEL_FORMAT_B64A:  return 8;
        default:                      return 2;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int input_format = argc > 1 ? std::atoi(argv[1]) : CFHD_PIXEL_FORMAT_YUY2;
    const int encoded_format = argc > 2 ? std::atoi(argv[2]) : CFHD_ENCODED_FORMAT_YUV_422;
    const int quality = argc > 3 ? std::atoi(argv[3]) : CFHD_ENCODING_QUALITY_LOW;
    const int w = argc > 4 ? std::atoi(argv[4]) : 64;
    const int h = argc > 5 ? std::atoi(argv[5]) : 64;
    const int bytes_per_pixel = BytesPerPixel(input_format);
    const int pitch = w * bytes_per_pixel;

    // Same shape as the probe's frame buffer: pixel bytes plus a generous tail.
    AlignedVector<uint8_t> frame(size_t(pitch) * size_t(h) + 65536u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = frame.data() + size_t(y) * size_t(pitch) + size_t(x) * bytes_per_pixel;
            for (int c = 0; c < bytes_per_pixel; ++c) {
                p[c] = uint8_t((x * (7 + c) + y * (3 + 2 * c)) & 0xFF);
            }
        }
    }

    void* raw = nullptr;
    int error = int(CFHD_OpenEncoder(&raw, nullptr));
    std::printf("open=%d\n", error);
    std::fflush(stdout);
    if (error != CFHD_ERROR_OKAY || !raw) return 10;

    error = int(CFHD_PrepareToEncode(raw, w, h,
        CFHD_PixelFormat(input_format), CFHD_EncodedFormat(encoded_format),
        CFHD_ENCODING_FLAGS_NONE, CFHD_EncodingQuality(quality)));
    std::printf("prepare=%d\n", error);
    std::fflush(stdout);
    if (error != CFHD_ERROR_OKAY) return 11;

    error = int(CFHD_EncodeSample(raw, frame.data(), pitch));
    std::printf("encode=%d\n", error);
    std::fflush(stdout);
    if (error != CFHD_ERROR_OKAY) return 12;

    void* data = nullptr;
    size_t size = 0;
    error = int(CFHD_GetSampleData(raw, &data, &size));
    std::printf("sample=%d size=%zu\n", error, size);
    std::fflush(stdout);
    CFHD_CloseEncoder(raw);
    return error == CFHD_ERROR_OKAY ? 0 : 13;
}
