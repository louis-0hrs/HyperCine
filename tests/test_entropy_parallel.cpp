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
#include "hypercine/entropy_parallel.h"

#include <iostream>
#include <vector>

namespace {

bool DecodeOk(const std::vector<uint8_t>& sample, int width, int height) {
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
    int32_t out_pitch = 0;
    CFHD_GetImagePitch(aw > 0 ? aw : width, fmt, &out_pitch);
    if (out_pitch <= 0) out_pitch = width * 4;
    std::vector<uint8_t> image(static_cast<size_t>(out_pitch) * static_cast<size_t>(ah > 0 ? ah : height));
    err = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(), image.data(), out_pitch);
    CFHD_CloseDecoder(decoder);
    return err == CFHD_ERROR_OKAY;
}

}  // namespace

int main() {
    const int width = 128;
    const int height = 128;
    auto frame = hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::Canonical);

    CFHD_EncoderRef encoder_ref = nullptr;
    if (CFHD_OpenEncoder(&encoder_ref, nullptr) != CFHD_ERROR_OKAY) return 1;
    if (CFHD_PrepareToEncode(encoder_ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        return 1;
    }
    if (CFHD_EncodeSample(encoder_ref, frame.data(), width * 4) != CFHD_ERROR_OKAY) return 1;

    CSampleEncoder* sample = reinterpret_cast<CSampleEncoder*>(encoder_ref);
    if (!hypercine::RefreshUpstreamPyramid(reinterpret_cast<encoder*>(sample->m_encoder),
                                           reinterpret_cast<transform**>(sample->m_transformArray),
                                           sample->m_channelCount, frame.data(), width * 4,
                                           sample->m_scratchBuffer, sample->m_scratchBufferSize)) {
        std::cerr << "refresh failed\n";
        return 1;
    }

    auto described = hypercine::DescribeBands(reinterpret_cast<transform**>(sample->m_transformArray),
                                              sample->m_channelCount);
    if (described.size() != 30) {
        std::cerr << "expected 30 subbands, got " << described.size() << "\n";
        return 1;
    }

    const int workers[] = {1, 2, 4};
    for (int count : workers) {
        if (!hypercine::RefreshUpstreamPyramid(reinterpret_cast<encoder*>(sample->m_encoder),
                                               reinterpret_cast<transform**>(sample->m_transformArray),
                                               sample->m_channelCount, frame.data(), width * 4,
                                               sample->m_scratchBuffer, sample->m_scratchBufferSize)) {
            return 1;
        }
        auto output = hypercine::EncodeEntropy(reinterpret_cast<encoder*>(sample->m_encoder),
                                               reinterpret_cast<transform**>(sample->m_transformArray),
                                               sample->m_channelCount, count,
                                               hypercine::EntropySchedule::SubbandParallel, true);
        if (!output.success || !output.byte_match) {
            std::cerr << "subband workers=" << count << " failed match=" << output.byte_match
                      << " offset=" << output.mismatch_offset << " " << output.error << "\n";
            return 1;
        }
        if (!DecodeOk(output.bitstream, width, height)) {
            std::cerr << "upstream decoder rejected subband bitstream\n";
            return 1;
        }
        std::cout << "subband workers " << count << " byte-match, " << output.bitstream_bytes << " bytes\n";
    }

    if (!hypercine::RefreshUpstreamPyramid(reinterpret_cast<encoder*>(sample->m_encoder),
                                           reinterpret_cast<transform**>(sample->m_transformArray),
                                           sample->m_channelCount, frame.data(), width * 4,
                                           sample->m_scratchBuffer, sample->m_scratchBufferSize)) {
        return 1;
    }
    auto channel = hypercine::EncodeEntropy(reinterpret_cast<encoder*>(sample->m_encoder),
                                            reinterpret_cast<transform**>(sample->m_transformArray),
                                            sample->m_channelCount, 3,
                                            hypercine::EntropySchedule::ChannelParallel, true);
    if (!channel.success || !channel.byte_match) {
        std::cerr << "channel parallel mismatch offset=" << channel.mismatch_offset << " " << channel.error << "\n";
        return 1;
    }
    std::cout << "channel workers byte-match\n";
    CFHD_CloseEncoder(encoder_ref);

    {
        const int w = 1920, h = 1080;
        auto big = hypercine::MakeFrame(w, h, hypercine::CanonicalPattern::Canonical);
        CFHD_EncoderRef big_ref = nullptr;
        if (CFHD_OpenEncoder(&big_ref, nullptr) != CFHD_ERROR_OKAY) return 1;
        if (CFHD_PrepareToEncode(big_ref, w, h, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                                 CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) return 1;
        if (CFHD_EncodeSample(big_ref, big.data(), w * 4) != CFHD_ERROR_OKAY) return 1;
        CSampleEncoder* big_sample = reinterpret_cast<CSampleEncoder*>(big_ref);
        if (!hypercine::RefreshUpstreamPyramid(reinterpret_cast<encoder*>(big_sample->m_encoder),
                                               reinterpret_cast<transform**>(big_sample->m_transformArray),
                                               big_sample->m_channelCount, big.data(), w * 4,
                                               big_sample->m_scratchBuffer, big_sample->m_scratchBufferSize)) return 1;
        auto big_out = hypercine::EncodeEntropy(reinterpret_cast<encoder*>(big_sample->m_encoder),
                                                reinterpret_cast<transform**>(big_sample->m_transformArray),
                                                big_sample->m_channelCount, 8,
                                                hypercine::EntropySchedule::SubbandParallel, true);
        if (!big_out.success || !big_out.byte_match) {
            std::cerr << "1080p x8 mismatch offset=" << big_out.mismatch_offset << " " << big_out.error << "\n";
            return 1;
        }
        std::cout << "1080p subband x8 byte-match, " << big_out.bitstream_bytes << " bytes\n";
        CFHD_CloseEncoder(big_ref);
    }
    std::cout << "entropy parallel tests passed\n";
    return 0;
}
