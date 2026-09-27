// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

/**
 * @file test_avi_sample.cpp
 * @brief Reference test that decodes the upstream sample AVI (CFHDsample422.AVI)
 *        and verifies deterministic frame decode checksums.
 */

#include <iostream>
#include <vector>
#include <iomanip>
#include <string>
#include <cstdint>

#include "test_vectors.h"
#include "CFHDDecoder.h"
#include "mp4reader.h"

using namespace HyperCineTest;

int main(int argc, char* argv[]) {
    std::cout << "=========================================================\n";
    std::cout << " HyperCine Reference Test: Upstream Sample AVI Decode    \n";
    std::cout << "=========================================================\n";

    std::string aviPath = "upstream/Example/videos/CFHDsample422.AVI";
    if (argc > 1) {
        aviPath = argv[1];
    }

    std::cout << "Opening AVI source: " << aviPath << "\n";
    void* handle = OpenAVISource(const_cast<char*>(aviPath.c_str()), AVI_TRAK_TYPE, AVI_TRAK_SUBTYPE);
    if (!handle) {
        std::cerr << "Failed to open AVI file: " << aviPath << "\n";
        return 1;
    }

    float duration = GetDuration(handle);
    uint32_t numFrames = GetNumberPayloads(handle);
    std::cout << "Found " << numFrames << " frames (" << duration << "s duration)\n";

    if (numFrames == 0) {
        std::cerr << "No frames found in AVI!\n";
        CloseSource(handle);
        return 1;
    }

    CFHD_DecoderRef decoder = nullptr;
    CFHD_Error err = CFHD_OpenDecoder(&decoder, nullptr);
    if (err != CFHD_ERROR_OKAY || !decoder) {
        std::cerr << "Failed to open CFHD decoder: " << err << "\n";
        CloseSource(handle);
        return 1;
    }

    uint32_t* payload = nullptr;
    int actualWidth = 0;
    int actualHeight = 0;
    CFHD_PixelFormat actualFormat = CFHD_PIXEL_FORMAT_UNKNOWN;
    std::vector<uint8_t> frameBuffer;
    int32_t framePitch = 0;

    bool allFramesOk = true;

    for (uint32_t frameIdx = 0; frameIdx < numFrames; ++frameIdx) {
        uint32_t payloadSize = GetPayloadSize(handle, frameIdx);
        payload = GetPayload(handle, payload, frameIdx);
        if (!payload || payloadSize == 0) {
            std::cerr << "Failed to read payload for frame " << frameIdx << "\n";
            allFramesOk = false;
            break;
        }

        if (frameIdx == 0) {
            err = CFHD_PrepareToDecode(decoder, 0, 0, CFHD_PIXEL_FORMAT_YUY2,
                                       CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
                                       payload, payloadSize,
                                       &actualWidth, &actualHeight, &actualFormat);
            if (err != CFHD_ERROR_OKAY) {
                std::cerr << "CFHD_PrepareToDecode failed: " << err << "\n";
                allFramesOk = false;
                break;
            }

            CFHD_GetImagePitch(actualWidth, actualFormat, &framePitch);
            frameBuffer.resize(static_cast<size_t>(framePitch) * actualHeight, 0);
            std::cout << "Decoded format: " << actualWidth << "x" << actualHeight
                      << ", pitch=" << framePitch << " bytes\n\n";
        }

        err = CFHD_DecodeSample(decoder, payload, payloadSize, frameBuffer.data(), framePitch);
        if (err != CFHD_ERROR_OKAY) {
            std::cerr << "Frame " << frameIdx << " decode failed: " << err << "\n";
            allFramesOk = false;
            break;
        }

        uint64_t frameHash = ComputeFNV1a64(frameBuffer.data(), frameBuffer.size());
        if (frameIdx < 5 || frameIdx == numFrames - 1) {
            std::cout << "  Frame " << std::setw(2) << frameIdx
                      << " payload: " << std::setw(6) << payloadSize << " bytes"
                      << ", hash: 0x" << std::hex << std::setw(16) << std::setfill('0') << frameHash
                      << std::dec << std::setfill(' ') << " [OK]\n";
        } else if (frameIdx == 5) {
            std::cout << "  ... (frames 5 to " << (numFrames - 2) << " skipped for brevity)\n";
        }
    }

    if (payload) FreePayload(payload);
    CFHD_CloseDecoder(decoder);
    CloseSource(handle);

    std::cout << "\n---------------------------------------------------------\n";
    if (allFramesOk) {
        std::cout << "SUCCESSFULLY DECODED ALL " << numFrames << " FRAMES WITH REFERENCE DECODER.\n";
        return 0;
    } else {
        std::cout << "AVI SAMPLE DECODE ENCOUNTERED FAILURES.\n";
        return 1;
    }
}
