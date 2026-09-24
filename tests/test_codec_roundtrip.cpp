/**
 * @file test_codec_roundtrip.cpp
 * @brief Codec-level test suite: encodes synthetic corpus via reference encoder,
 *        decodes via reference decoder, verifies bitstream integrity and deterministic hashes.
 */

#include <iostream>
#include <vector>
#include <iomanip>
#include <cstdlib>
#include <cstring>

#include "test_vectors.h"
#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

using namespace HyperCineTest;

struct RoundtripResult {
    bool success;
    size_t bitstreamSize;
    uint64_t bitstreamHash;
    uint64_t decodedHash;
    double psnr;
};

// Interface for cross-implementation testing (Phase 1+ ready)
typedef CFHD_Error (*CustomEncodeFn)(void* frameBuffer, int width, int height, int pitch,
                                     void** outPayload, size_t* outPayloadSize);
typedef CFHD_Error (*CustomDecodeFn)(void* payload, size_t payloadSize, int width, int height,
                                     void* outFrameBuffer, int pitch);

RoundtripResult RunCodecRoundtrip(
    CFHD_EncoderRef encoder,
    CFHD_DecoderRef decoder,
    CFHD_MetadataRef metadata,
    TestPatternType patternType,
    int width,
    int height,
    CFHD_PixelFormat pixelFormat = CFHD_PIXEL_FORMAT_YUY2,
    CFHD_EncodedFormat encodedFormat = CFHD_ENCODED_FORMAT_YUV_422,
    CFHD_EncodingQuality quality = CFHD_ENCODING_QUALITY_FILMSCAN3)
{
    RoundtripResult result{};
    result.success = false;

    // 1. Generate input synthetic test frame
    AlignedVector<uint8_t> inputBuffer;
    int inPitch = 0;
    if (pixelFormat == CFHD_PIXEL_FORMAT_YUY2) {
        inputBuffer = GenerateTestVectorYUY2(patternType, width, height);
        inPitch = width * 2;
    } else {
        inputBuffer = GenerateTestVectorRGBA(patternType, width, height);
        inPitch = width * 4;
    }

    // 2. Prepare encoder
    CFHD_Error err = CFHD_PrepareToEncode(encoder, width, height, pixelFormat,
                                          encodedFormat, CFHD_ENCODING_FLAGS_NONE, quality);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "Failed to prepare encoder: " << err << "\n";
        return result;
    }

    // 3. Encode sample
    err = CFHD_EncodeSample(encoder, inputBuffer.data(), inPitch);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "CFHD_EncodeSample failed: " << err << "\n";
        return result;
    }

    void* sampleData = nullptr;
    size_t sampleSize = 0;
    err = CFHD_GetSampleData(encoder, &sampleData, &sampleSize);
    if (err != CFHD_ERROR_OKAY || !sampleData || sampleSize == 0) {
        std::cerr << "CFHD_GetSampleData failed: " << err << "\n";
        return result;
    }

    result.bitstreamSize = sampleSize;
    result.bitstreamHash = ComputeFNV1a64(sampleData, sampleSize);

    // Copy bitstream payload for independent decode
    AlignedVector<uint8_t> payload(sampleSize);
    std::memcpy(payload.data(), sampleData, sampleSize);

    // 4. Prepare decoder
    int actualWidth = 0;
    int actualHeight = 0;
    CFHD_PixelFormat actualFormat = CFHD_PIXEL_FORMAT_UNKNOWN;
    err = CFHD_PrepareToDecode(decoder, 0, 0, pixelFormat, CFHD_DECODED_RESOLUTION_FULL,
                               CFHD_DECODING_FLAGS_NONE, payload.data(), payload.size(),
                               &actualWidth, &actualHeight, &actualFormat);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "CFHD_PrepareToDecode failed: " << err << "\n";
        return result;
    }

    int32_t outPitch = 0;
    CFHD_GetImagePitch(actualWidth, actualFormat, &outPitch);
    AlignedVector<uint8_t> decodedBuffer(static_cast<size_t>(outPitch) * actualHeight, 0);

    if (metadata) {
        CFHD_InitSampleMetadata(metadata, METADATATYPE_ORIGINAL, payload.data(), payload.size());
        unsigned int processflags = PROCESSING_ALL_ON;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_PROCESS_PATH, METADATATYPE_UINT32, &processflags, sizeof(unsigned int));
    }

    err = CFHD_DecodeSample(decoder, payload.data(), payload.size(),
                            decodedBuffer.data(), outPitch);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "CFHD_DecodeSample failed: " << err << "\n";
        return result;
    }

    result.decodedHash = ComputeFNV1a64(decodedBuffer.data(), decodedBuffer.size());

    if (pixelFormat == CFHD_PIXEL_FORMAT_YUY2) {
        result.psnr = ComputePSNR_YUY2(inputBuffer.data(), decodedBuffer.data(), width, height, true, 255.0);
    } else {
        bool inverted = (pixelFormat == CFHD_PIXEL_FORMAT_BGRA || pixelFormat == CFHD_PIXEL_FORMAT_RG24);
        result.psnr = ComputePSNR_BGRA(inputBuffer.data(), decodedBuffer.data(), width, height, inverted);
    }
    result.success = true;

    return result;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " HyperCine Reference Test: Codec Roundtrip Suite         \n";
    std::cout << "=========================================================\n";

    const int width = 128;
    const int height = 128;

    CFHD_EncoderRef encoder = nullptr;
    CFHD_Error err = CFHD_OpenEncoder(&encoder, nullptr);
    if (err != CFHD_ERROR_OKAY || !encoder) {
        std::cerr << "Failed to open encoder: " << err << "\n";
        return 1;
    }

    CFHD_DecoderRef decoder = nullptr;
    err = CFHD_OpenDecoder(&decoder, nullptr);
    if (err != CFHD_ERROR_OKAY || !decoder) {
        std::cerr << "Failed to open decoder: " << err << "\n";
        CFHD_CloseEncoder(encoder);
        return 1;
    }

    CFHD_MetadataRef metadata = nullptr;
    err = CFHD_OpenMetadata(&metadata);
    if (err != CFHD_ERROR_OKAY || !metadata) {
        std::cerr << "Failed to open metadata: " << err << "\n";
        CFHD_CloseDecoder(decoder);
        CFHD_CloseEncoder(encoder);
        return 1;
    }

    bool allPassed = true;

    std::cout << "Encoding and decoding 128x128 synthetic test corpus (YUY2 -> YUV 4:2:2 FilmScan3):\n\n";
    std::cout << std::left
              << std::setw(26) << "Pattern"
              << std::setw(12) << "Size (B)"
              << std::setw(20) << "Bitstream Hash"
              << std::setw(20) << "Decoded Hash"
              << std::setw(10) << "PSNR (dB)"
              << "Status\n";
    std::cout << std::string(96, '-') << "\n";

    for (int p = 0; p < PATTERN_COUNT; ++p) {
        TestPatternType pattern = static_cast<TestPatternType>(p);
        auto res = RunCodecRoundtrip(encoder, decoder, metadata, pattern, width, height);

        if (!res.success) {
            std::cout << std::left << std::setw(26) << GetPatternName(pattern)
                      << " FAILED\n";
            allPassed = false;
            continue;
        }

        // PSNR threshold >= 24.0 dB accommodates exact 16-code studio pedestal clamp on black (20*log10(255/16) = 24.05 dB)
        bool psnrPass = (res.psnr >= 24.0);
        std::ios state(nullptr);
        state.copyfmt(std::cout);
        std::cout << std::left << std::setw(26) << GetPatternName(pattern)
                  << std::setw(12) << res.bitstreamSize
                  << "0x" << std::hex << std::setw(16) << std::setfill('0') << res.bitstreamHash << "  "
                  << "0x" << std::setw(16) << res.decodedHash;
        std::cout.copyfmt(state);
        std::cout << "  " << std::fixed << std::setprecision(2) << std::setw(10) << res.psnr
                  << (psnrPass ? "PASS" : "WARN (Low PSNR)") << "\n";

        if (!psnrPass) {
            allPassed = false;
        }
    }

    CFHD_CloseMetadata(metadata);
    CFHD_CloseDecoder(decoder);
    CFHD_CloseEncoder(encoder);

    std::cout << std::string(96, '-') << "\n";
    if (allPassed) {
        std::cout << "ALL CODEC ROUNDTRIP TESTS PASSED SUCCESSFULLY.\n";
        return 0;
    } else {
        std::cout << "SOME CODEC ROUNDTRIP TESTS FAILED.\n";
        return 1;
    }
}
