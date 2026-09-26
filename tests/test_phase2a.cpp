// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
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
#include "hypercine/phase2a.h"

#include <algorithm>
#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>

namespace {

bool Decode(const std::vector<uint8_t>& sample, int width, int height, std::vector<uint8_t>* image, int* pitch_out) {
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
    }
    err = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(), image->data(), pitch);
    if (metadata) CFHD_CloseMetadata(metadata);
    CFHD_CloseDecoder(decoder);
    if (pitch_out) *pitch_out = pitch;
    return err == CFHD_ERROR_OKAY;
}

double Psnr(const std::vector<uint8_t>& src, const std::vector<uint8_t>& dec, int width, int height, int pitch) {
    double best = 0;
    for (int flip = 0; flip < 2; ++flip) {
        double sse = 0;
        int count = 0;
        for (int y = 0; y < height; ++y) {
            const int ry = flip ? (height - 1 - y) : y;
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int a = src[(static_cast<size_t>(y) * width + x) * 4 + c];
                    const int b = dec[static_cast<size_t>(ry) * pitch + static_cast<size_t>(x) * 4 + c];
                    const double d = static_cast<double>(a - b);
                    sse += d * d;
                    ++count;
                }
            }
        }
        const double score = sse <= 0 ? 99.0 : 10.0 * std::log10((255.0 * 255.0) / (sse / count));
        if (score > best) best = score;
    }
    return best;
}

bool Expect(const hypercine::Phase2Run& run, const char* name) {
    if (!run.success || !run.byte_match || run.coeff_mismatches != 0 || run.expand_mismatches != 0 || run.bitstream.empty()) {
        std::cerr << name << " failed: " << run.error
                  << " byte_match=" << run.byte_match
                  << " coeff=" << run.coeff_mismatches << "/" << run.coeff_compared
                  << " expand=" << run.expand_mismatches << "\n";
        return false;
    }
    std::cout << name << " ok fps=" << run.sustained_fps
              << " latency=" << run.mean_latency_ms
              << " d2h_bytes=" << run.d2h_bytes
              << " int8_bands=" << (run.frame_times.empty() ? 0 : run.frame_times.back().int8_bands) << "\n";
    return true;
}

}  // namespace

int main() {
    const int width = 1920;
    const int height = 1080;
    const int pitch = width * 4;
    auto frame = hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::Canonical);
    CFHD_EncoderRef encoder_ref = nullptr;
    if (CFHD_OpenEncoder(&encoder_ref, nullptr) != CFHD_ERROR_OKAY) return 1;
    if (CFHD_PrepareToEncode(encoder_ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) return 1;
    if (CFHD_EncodeSample(encoder_ref, frame.data(), pitch) != CFHD_ERROR_OKAY) return 1;
    CSampleEncoder* sample = reinterpret_cast<CSampleEncoder*>(encoder_ref);
    auto* enc = reinterpret_cast<encoder*>(sample->m_encoder);
    auto* transforms = reinterpret_cast<transform**>(sample->m_transformArray);

    if (!hypercine::RefreshUpstreamPyramid(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                           sample->m_scratchBuffer, sample->m_scratchBufferSize)) {
        std::cerr << "pyramid refresh failed\n";
        return 1;
    }
    const auto refs = hypercine::EnumerateBands(transforms, sample->m_channelCount);
    ENCODER* raw = sample->m_encoder;
    TRANSFORM** raw_tf = sample->m_transformArray;
    std::vector<std::vector<uint8_t>> storage(refs.size());
    std::vector<hypercine::EncodedPiece> pieces(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto& ref = refs[i];
        IMAGE* wavelet = nullptr;
        for (int k = 0; k < raw_tf[ref.channel]->num_wavelets; ++k) {
            if (raw_tf[ref.channel]->wavelet[k] && raw_tf[ref.channel]->wavelet[k]->level == ref.level) {
                wavelet = raw_tf[ref.channel]->wavelet[k];
            }
        }
        if (!wavelet) return 1;
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        const PIXEL* src = wavelet->band[ref.band];
        int vmin = src[0];
        int vmax = src[0];
        for (int y = 0; y < ref.height; ++y) {
            const PIXEL* row = src + static_cast<size_t>(y) * stride;
            for (int x = 0; x < ref.width; ++x) {
                vmin = std::min(vmin, static_cast<int>(row[x]));
                vmax = std::max(vmax, static_cast<int>(row[x]));
            }
        }
        hypercine::ExternalCoeff coeff;
        coeff.channel = ref.channel;
        coeff.level = ref.level;
        coeff.band = ref.band;
        coeff.subband = ref.subband;
        coeff.lowpass = ref.lowpass;
        coeff.width = ref.width;
        coeff.height = ref.height;
        coeff.value_min = vmin;
        coeff.value_max = vmax;
        const bool as_int8 = !ref.lowpass && vmin >= -128 && vmax <= 127;
        if (as_int8) {
            storage[i].resize(static_cast<size_t>(ref.width) * ref.height);
            for (int y = 0; y < ref.height; ++y) {
                const PIXEL* row = src + static_cast<size_t>(y) * stride;
                for (int x = 0; x < ref.width; ++x) {
                    const int value = row[x];
                    if (value < -128 || value > 127) {
                        std::cerr << "int8 pack would saturate\n";
                        return 1;
                    }
                    storage[i][static_cast<size_t>(y) * ref.width + x] = static_cast<uint8_t>(static_cast<int8_t>(value));
                }
            }
            coeff.elem_bytes = 1;
        } else {
            storage[i].resize(static_cast<size_t>(ref.width) * ref.height * 2u);
            auto* dst = reinterpret_cast<int16_t*>(storage[i].data());
            for (int y = 0; y < ref.height; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * ref.width,
                            src + static_cast<size_t>(y) * stride,
                            static_cast<size_t>(ref.width) * 2u);
            }
            coeff.elem_bytes = 2;
        }
        coeff.data = storage[i].data();
        if (!hypercine::EncodeOneExternalBand(enc, transforms, coeff, &pieces[i])) {
            std::cerr << "external band encode failed channel " << ref.channel << " level " << ref.level << "\n";
            return 1;
        }
    }
    auto entropy = hypercine::AssembleEncodedPieces(enc, transforms, sample->m_channelCount, pieces, true);
    if (!entropy.success || !entropy.byte_match) {
        std::cerr << "external entropy mismatch at " << entropy.mismatch_offset << " " << entropy.error << "\n";
        return 1;
    }
    std::cout << "external int8/int16 entropy matches EncodeQuantizedGroup\n";

    hypercine::Phase2Config cfg;
    cfg.frames = 1;
    cfg.warmup = 1;
    cfg.workers = 8;
    cfg.pipeline_depth = 1;
    cfg.verify = true;
    cfg.input = hypercine::Phase2Input::HostBgra;
    cfg.transport = hypercine::Phase2Transport::FullInt16;
    auto full = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                     sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(full, "host full int16")) return 1;

    cfg.transport = hypercine::Phase2Transport::Int16Stream;
    cfg.pipeline_depth = 2;
    auto streamed = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                         sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(streamed, "host int16 stream")) return 1;

    cfg.transport = hypercine::Phase2Transport::CompactStream;
    auto compact = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                        sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(compact, "host compact stream")) return 1;
    if (!compact.frame_times.empty() && compact.frame_times.back().int8_bands <= 0) {
        std::cerr << "canonical frame produced no int8 bands\n";
        return 1;
    }

    cfg.input = hypercine::Phase2Input::DeviceBgra;
    auto resident = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                         sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(resident, "device compact stream")) return 1;
    if (resident.h2d_bytes != 0) {
        std::cerr << "device-resident path copied " << resident.h2d_bytes << " bytes to the device\n";
        return 1;
    }
    if (resident.bitstream.size() != compact.bitstream.size()) {
        std::cerr << "device-resident sample size " << resident.bitstream.size()
                  << " differs from host compact " << compact.bitstream.size() << "\n";
        return 1;
    }
    size_t bitstream_diffs = 0;
    for (size_t i = 0; i < resident.bitstream.size(); ++i) {
        if (resident.bitstream[i] != compact.bitstream[i]) ++bitstream_diffs;
    }
    std::vector<uint8_t> host_image, device_image;
    int host_pitch = 0, device_pitch = 0;
    if (!Decode(compact.bitstream, width, height, &host_image, &host_pitch) ||
        !Decode(resident.bitstream, width, height, &device_image, &device_pitch) ||
        host_pitch != device_pitch || host_image != device_image) {
        std::cerr << "device-resident decode differs from host compact (" << bitstream_diffs << " bitstream bytes)\n";
        return 1;
    }
    std::cout << "device and host compact decode identically (" << bitstream_diffs
              << " header bytes differ, sample size " << compact.bitstream.size() << ")\n";

    std::vector<uint8_t> image;
    int decoded_pitch = 0;
    if (!Decode(compact.bitstream, width, height, &image, &decoded_pitch)) {
        std::cerr << "upstream decoder rejected compact bitstream\n";
        return 1;
    }
    const double psnr = Psnr(frame, image, width, height, decoded_pitch);
    std::cout << "decoded psnr " << psnr << " dB\n";
    if (psnr < 35.0) {
        std::cerr << "psnr below 35 dB\n";
        return 1;
    }
    CFHD_CloseEncoder(encoder_ref);
    std::cout << "phase 2a tests passed\n";
    return 0;
}
