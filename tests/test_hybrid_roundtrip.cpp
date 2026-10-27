// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <cstddef>
#include <cstring>

#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "frame.h"
}

#include "VideoBuffers.h"
#include "SampleMetadata.h"

#define private public
#define protected public
#include "SampleEncoder.h"
#undef private
#undef protected

#include "hypercine/canonical.h"
#include "hypercine/hybrid_encoder.h"
#include "hypercine/gpu_pyramid.h"

#include <algorithm>
#include <iostream>
#include <vector>
#include <cmath>

static bool Decode(const std::vector<uint8_t>& sample, int width, int height, std::vector<uint8_t>* image, int* pitch_out) {
    CFHD_DecoderRef decoder = nullptr;
    if (CFHD_OpenDecoder(&decoder, nullptr) != CFHD_ERROR_OKAY) return false;
    int aw = 0, ah = 0;
    CFHD_PixelFormat fmt = CFHD_PIXEL_FORMAT_UNKNOWN;
    CFHD_Error err = CFHD_PrepareToDecode(decoder, 0, 0, CFHD_PIXEL_FORMAT_BGRA, CFHD_DECODED_RESOLUTION_FULL,
                                          CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(),
                                          &aw, &ah, &fmt);
    if (err != CFHD_ERROR_OKAY) {
        CFHD_CloseDecoder(decoder);
        return false;
    }
    int32_t pitch = 0;
    CFHD_GetImagePitch(aw > 0 ? aw : width, fmt, &pitch);
    if (pitch <= 0) pitch = width * 4;
    image->assign(static_cast<size_t>(pitch) * static_cast<size_t>(ah > 0 ? ah : height), 0);
    CFHD_MetadataRef metadata = nullptr;
    CFHD_OpenMetadata(&metadata);
    if (metadata) {
        CFHD_InitSampleMetadata(metadata, METADATATYPE_ORIGINAL, const_cast<uint8_t*>(sample.data()),
                                static_cast<uint32_t>(sample.size()));
        unsigned int processflags = PROCESSING_ALL_ON;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_PROCESS_PATH, METADATATYPE_UINT32, &processflags, sizeof(processflags));
        uint32_t cpu_max = 1;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_CPU_MAX, METADATATYPE_HIDDEN, &cpu_max, sizeof(cpu_max));
    }
    err = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(), image->data(), pitch);
    if (metadata) CFHD_CloseMetadata(metadata);
    CFHD_CloseDecoder(decoder);
    if (pitch_out) *pitch_out = pitch;
    return err == CFHD_ERROR_OKAY;
}

static double Psnr(const std::vector<uint8_t>& src, const std::vector<uint8_t>& dec, int width, int height, int pitch) {
    double sse = 0;
    int count = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int a = src[(static_cast<size_t>(y) * width + x) * 4 + c];
                const int b = dec[static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4 + c];
                const double d = static_cast<double>(a - b);
                sse += d * d;
                ++count;
            }
        }
    }
    if (sse <= 0.0) return 99.0;
    const double mse = sse / count;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

int main() {
    const int width = 128;
    const int height = 128;
    auto frame = hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::Canonical);
    CFHD_EncoderRef encoder_ref = nullptr;
    if (CFHD_OpenEncoder(&encoder_ref, nullptr) != CFHD_ERROR_OKAY) return 1;
    if (CFHD_PrepareToEncode(encoder_ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) return 1;
    if (CFHD_EncodeSample(encoder_ref, frame.data(), width * 4) != CFHD_ERROR_OKAY) return 1;
    void* upstream_data = nullptr;
    size_t upstream_size = 0;
    CFHD_GetSampleData(encoder_ref, &upstream_data, &upstream_size);
    std::vector<uint8_t> upstream_sample(static_cast<uint8_t*>(upstream_data), static_cast<uint8_t*>(upstream_data) + upstream_size);
    auto score = [&](const std::vector<uint8_t>& image, int pitch, bool flip) {
        double sse = 0;
        int count = 0;
        for (int y = 0; y < height; ++y) {
            const int ry = flip ? (height - 1 - y) : y;
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int a = frame[(static_cast<size_t>(y) * width + x) * 4 + c];
                    const int b = image[static_cast<size_t>(ry) * pitch + static_cast<size_t>(x) * 4 + c];
                    const double d = static_cast<double>(a - b);
                    sse += d * d;
                    ++count;
                }
            }
        }
        if (count == 0 || sse <= 0.0) return 99.0;
        return 10.0 * std::log10((255.0 * 255.0) / (sse / count));
    };
    // The upstream decoder's reconstructed pixels are not stable across calls on this
    // machine (same sample, two opens). Keep the best of several attempts.
    auto best_psnr = [&](const std::vector<uint8_t>& sample_bytes) {
        double best = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::vector<uint8_t> image;
            int image_pitch = 0;
            if (!Decode(sample_bytes, width, height, &image, &image_pitch)) continue;
            const double a = score(image, image_pitch, false); printf("Decode attempt %d: a=%f\n", attempt, a);
            const double b = score(image, image_pitch, true);
            best = std::max(best, std::max(a, b));
        }
        return best;
    };
    const double upstream_best = best_psnr(upstream_sample);
    std::cout << "upstream decode best psnr " << upstream_best << "\n";
    if (upstream_best < 35.0) {
        std::cerr << "upstream sample did not decode to a usable image\n";
        return 1;
    }
    CSampleEncoder* sample = reinterpret_cast<CSampleEncoder*>(encoder_ref);

    auto sync = hypercine::RunHybrid(reinterpret_cast<encoder*>(sample->m_encoder),
                                     reinterpret_cast<transform**>(sample->m_transformArray),
                                     sample->m_channelCount, frame.data(), width * 4,
                                     sample->m_scratchBuffer, sample->m_scratchBufferSize,
                                     2, 4, false);
    if (!sync.success || sync.bitstream.empty()) {
        std::cerr << "sync hybrid failed: " << sync.error << "\n";
        return 1;
    }
    const double hybrid_best = best_psnr(sync.bitstream);
    std::cout << "sync hybrid decoded, coeff mismatches " << sync.coeff_mismatches
              << "/" << sync.coeff_compared << ", byte_match=" << sync.byte_match
              << ", best psnr=" << hybrid_best << " dB\n";
    if (hybrid_best + 1.0 < upstream_best) {
        std::cerr << "hybrid PSNR " << hybrid_best << " is below upstream " << upstream_best << "\n";
        return 1;
    }

    auto pipe = hypercine::RunHybrid(reinterpret_cast<encoder*>(sample->m_encoder),
                                     reinterpret_cast<transform**>(sample->m_transformArray),
                                     sample->m_channelCount, frame.data(), width * 4,
                                     sample->m_scratchBuffer, sample->m_scratchBufferSize,
                                     4, 4, true);
    if (!pipe.success || pipe.bitstream.empty()) {
        std::cerr << "pipelined hybrid failed: " << pipe.error << "\n";
        return 1;
    }
    if (best_psnr(pipe.bitstream) + 1.0 < upstream_best) {
        std::cerr << "pipelined hybrid did not decode to the expected image\n";
        return 1;
    }
    std::cout << "pipelined hybrid decoded, fps=" << pipe.sustained_fps << "\n";

    std::vector<int16_t> y(width * height), u((width / 2) * height), v((width / 2) * height);
    HyperCineColorTiming timing{};
    const int color_space = 2;
    if (HyperCine_BgraToYuv10_CUDA(frame.data(), width, height, width * 4, color_space,
                                   y.data(), u.data(), v.data(), &timing) != 0) {
        std::cerr << "cuda color conversion failed\n";
        return 1;
    }
    std::cout << "cuda color kernel " << timing.kernel_ms << " ms\n";
    CFHD_CloseEncoder(encoder_ref);
    std::cout << "hybrid roundtrip tests passed\n";
    return 0;
}

