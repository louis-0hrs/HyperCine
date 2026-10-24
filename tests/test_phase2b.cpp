// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#ifndef _ALLOCATOR
#define _ALLOCATOR 1
#endif

#include <cstring>

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
#include "hypercine/entropy_gpu.h"
#include "hypercine/entropy_parallel.h"
#include "hypercine/entropy_stage.h"
#include "hypercine/phase2b.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int Fail(const std::string& message) {
    std::cerr << message << "\n";
    return 1;
}

struct Opened {
    CFHD_EncoderRef ref = nullptr;
    CSampleEncoder* sample = nullptr;
};

Opened OpenEncoder(int width, int height) {
    Opened opened;
    if (CFHD_OpenEncoder(&opened.ref, nullptr) != CFHD_ERROR_OKAY) return opened;
    if (CFHD_PrepareToEncode(opened.ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(opened.ref);
        opened.ref = nullptr;
        return opened;
    }
    opened.sample = reinterpret_cast<CSampleEncoder*>(opened.ref);
    auto frame = hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::Canonical);
    if (CFHD_EncodeSample(opened.ref, frame.data(), width * 4) != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(opened.ref);
        opened.ref = nullptr;
        opened.sample = nullptr;
    }
    return opened;
}

hypercine::ExternalCoeff AsBand(const hypercine::BandRef& ref, const int16_t* data) {
    hypercine::ExternalCoeff coeff;
    coeff.channel = ref.channel;
    coeff.level = ref.level;
    coeff.band = ref.band;
    coeff.subband = ref.subband;
    coeff.lowpass = false;
    coeff.data = data;
    coeff.elem_bytes = 2;
    coeff.width = ref.width;
    coeff.height = ref.height;
    return coeff;
}

bool CompareSymbols(const hypercine::EntropySymbols& cpu, const hypercine::GpuBandResult& gpu, std::string* error) {
    if (!gpu.ok) {
        *error = gpu.error.empty() ? "gpu entropy failed" : gpu.error;
        return false;
    }
    if (gpu.tokens != cpu.tokens || gpu.total_bits != cpu.total_bits || gpu.coeff_payload != cpu.coeff_bytes) {
        *error = "gpu coefficient bytes differ from the CPU stage";
        return false;
    }
    if (gpu.tokens_packed != cpu.tokens_packed || gpu.codeword != cpu.codeword ||
        gpu.bit_length != cpu.bit_length || gpu.bit_offset != cpu.bit_offset) {
        *error = "gpu token, codeword, or bit offset sequence differs";
        return false;
    }
    if (!cpu.bit_offset.empty() && cpu.bit_offset[0] != 0) {
        *error = "first bit offset is not zero";
        return false;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < cpu.bit_length.size(); ++i) {
        if (cpu.bit_offset[i] != sum) {
            *error = "bit offset is not the exclusive prefix of bit lengths";
            return false;
        }
        sum += cpu.bit_length[i];
    }
    if (static_cast<int>(sum) != cpu.total_bits) {
        *error = "total bit count does not match the prefix sum";
        return false;
    }
    return true;
}

int CheckBuffer(Opened* opened, const hypercine::BandRef& ref, const std::vector<int16_t>& samples,
                hypercine::GpuEntropySession* gpu, const char* name) {
    auto coeff = AsBand(ref, samples.data());
    hypercine::EntropySymbols cpu;
    if (!hypercine::MatchUpstreamHighpass(reinterpret_cast<encoder*>(opened->sample->m_encoder),
                                          reinterpret_cast<transform**>(opened->sample->m_transformArray),
                                          coeff, &cpu)) {
        return Fail(std::string(name) + " CPU stage: " + cpu.error);
    }
    int16_t* device = nullptr;
    if (cudaMalloc(&device, samples.size() * sizeof(int16_t)) != cudaSuccess) return Fail("cudaMalloc failed");
    cudaMemcpy(device, samples.data(), samples.size() * sizeof(int16_t), cudaMemcpyHostToDevice);
    hypercine::EntropyTables tables;
    if (!hypercine::LoadEntropyTables(reinterpret_cast<encoder*>(opened->sample->m_encoder), ref.subband, &tables)) {
        cudaFree(device);
        return Fail(std::string(name) + " codebook load failed");
    }
    hypercine::GpuBandResult got;
    const bool encoded = hypercine::EncodeHighpassOnGpu(gpu, device, ref.width, ref.height, ref.width, tables, true, &got,
                                                        hypercine::GpuEntropyStop::Full);
    std::string error;
    if (!encoded || !CompareSymbols(cpu, got, &error)) {
        cudaFree(device);
        if (error.empty()) error = got.error;
        return Fail(std::string(name) + " " + error);
    }
    hypercine::GpuBandResult tokens;
    if (!hypercine::EncodeHighpassOnGpu(gpu, device, ref.width, ref.height, ref.width, tables, true, &tokens,
                                        hypercine::GpuEntropyStop::Tokens) ||
        tokens.coeff_payload != cpu.coeff_bytes || tokens.tokens_packed != cpu.tokens_packed) {
        cudaFree(device);
        return Fail(std::string(name) + " GPU tokens plus CPU pack differ");
    }
    hypercine::GpuBandResult words;
    if (!hypercine::EncodeHighpassOnGpu(gpu, device, ref.width, ref.height, ref.width, tables, true, &words,
                                        hypercine::GpuEntropyStop::Codewords) ||
        words.coeff_payload != cpu.coeff_bytes || words.codeword != cpu.codeword || words.bit_length != cpu.bit_length) {
        cudaFree(device);
        return Fail(std::string(name) + " GPU codewords plus CPU pack differ");
    }
    cudaFree(device);
    std::cout << name << " tokens " << cpu.tokens << " bits " << cpu.total_bits
              << " payload " << cpu.payload_bytes
              << " coeff 0x" << std::hex << cpu.coeff_hash
              << " hash 0x" << cpu.payload_hash << std::dec
              << " scan " << got.timing.scan_ms << " ms token " << got.timing.token_ms
              << " book " << got.timing.codebook_ms << " prefix " << got.timing.prefix_ms
              << " pack " << got.timing.pack_ms << "\n";
    return 0;
}

bool DecodeSample(const std::vector<uint8_t>& sample, int width, int height, std::vector<uint8_t>* image, int* pitch_out) {
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
    return err == CFHD_ERROR_OKAY && aw == width && ah == height;
}

double BestPsnr(const std::vector<uint8_t>& src, const std::vector<uint8_t>& dec, int width, int height, int pitch) {
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
        const double mse = count ? sse / count : 0;
        const double psnr = mse <= 0 ? 99 : 10.0 * std::log10(255.0 * 255.0 / mse);
        best = std::max(best, psnr);
    }
    return best;
}

std::vector<int16_t> Fill(int width, int height, int kind) {
    std::vector<int16_t> samples(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    if (kind == 1 && !samples.empty()) samples[0] = 1;
    if (kind == 2 && !samples.empty()) samples.back() = -1;
    if (kind == 3) {
        for (size_t i = 0; i < samples.size(); i += 2) samples[i] = 1;
    }
    if (kind == 4) {
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = (i % 5 == 0) ? static_cast<int16_t>(1 + (i % 7)) : 0;
    }
    if (kind == 5) {
        const int at = std::min(static_cast<int>(samples.size()) - 1, 256);
        samples[static_cast<size_t>(at)] = 3;
        if (at > 0) samples[static_cast<size_t>(at - 1)] = -2;
    }
    if (kind == 6 && !samples.empty()) {
        samples[0] = 1023;
        if (samples.size() > 1) samples[1] = 1024;
        if (samples.size() > 2) samples[2] = -1023;
        if (samples.size() > 3) samples[3] = -1024;
    }
    if (kind == 7) {
        uint32_t state = 0x1337BEEFu;
        for (int16_t& sample : samples) {
            state = state * 1664525u + 1013904223u;
            const int span = static_cast<int>(state % 41u) - 20;
            sample = static_cast<int16_t>((state & 3u) == 0u ? span : 0);
        }
    }
    return samples;
}

}  // namespace

int main() {
    Opened opened = OpenEncoder(128, 128);
    if (!opened.sample) return Fail("encoder open failed");
    const auto bands = hypercine::EnumerateBands(reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                                  opened.sample->m_channelCount);
    const hypercine::BandRef* ref = nullptr;
    for (const auto& band : bands) {
        if (!band.lowpass && band.channel == 0 && band.band == HL_BAND) {
            ref = &band;
            break;
        }
    }
    if (!ref) return Fail("Y HL band missing");
    auto gpu = hypercine::CreateGpuEntropySession();
    if (!gpu) return Fail("gpu session failed");

    const char* names[] = {"zero", "single_pos", "single_neg", "alternating", "short_runs",
                           "block_boundary", "extremes", "random"};
    for (int kind = 0; kind < 8; ++kind) {
        auto samples = Fill(ref->width, ref->height, kind);
        if (CheckBuffer(&opened, *ref, samples, gpu, names[kind]) != 0) {
            hypercine::DestroyGpuEntropySession(gpu);
            CFHD_CloseEncoder(opened.ref);
            return 1;
        }
    }

    int real_bands = 0;
    for (const auto& band : bands) {
        if (band.lowpass || band.channel != 0) continue;
        TRANSFORM* tf = reinterpret_cast<transform**>(opened.sample->m_transformArray)[band.channel];
        IMAGE* wavelet = nullptr;
        for (int k = 0; k < tf->num_wavelets; ++k) {
            if (tf->wavelet[k] && tf->wavelet[k]->level == band.level) wavelet = tf->wavelet[k];
        }
        if (!wavelet || !wavelet->band[band.band]) continue;
        std::vector<int16_t> samples(static_cast<size_t>(band.width) * static_cast<size_t>(band.height));
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        const PIXEL* src = wavelet->band[band.band];
        for (int y = 0; y < band.height; ++y) {
            std::memcpy(samples.data() + static_cast<size_t>(y) * band.width,
                        src + static_cast<size_t>(y) * stride, sizeof(int16_t) * band.width);
        }
        const std::string name = std::string("real_Y_L") + std::to_string(band.level) + "_" + std::to_string(band.band);
        if (CheckBuffer(&opened, band, samples, gpu, name.c_str()) != 0) {
            hypercine::DestroyGpuEntropySession(gpu);
            CFHD_CloseEncoder(opened.ref);
            return 1;
        }
        ++real_bands;
    }
    hypercine::DestroyGpuEntropySession(gpu);
    if (real_bands < 3) {
        CFHD_CloseEncoder(opened.ref);
        return Fail("expected the luma highpass bands");
    }

    CFHD_CloseEncoder(opened.ref);
    opened = OpenEncoder(1920, 1080);
    if (!opened.sample) return Fail("1080p encoder open failed");
    hypercine::Phase2BConfig config;
    config.frames = 1;
    config.warmup = 0;
    config.verify = true;
    auto frame = hypercine::MakeFrame(1920, 1080, hypercine::CanonicalPattern::Canonical);
    auto full = hypercine::RunPhase2B(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                      reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                      opened.sample->m_channelCount, frame.data(), 1920 * 4,
                                      opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, config);
    if (!full.success || !full.byte_match || full.coeff_mismatches != 0) {
        CFHD_CloseEncoder(opened.ref);
        return Fail("full-frame GPU entropy: " + (full.error.empty() ? std::string("byte mismatch") : full.error) +
                    " mismatches " + std::to_string(full.coeff_mismatches));
    }
    std::vector<uint8_t> decoded;
    int decoded_pitch = 0;
    if (!DecodeSample(full.bitstream, 1920, 1080, &decoded, &decoded_pitch)) {
        CFHD_CloseEncoder(opened.ref);
        return Fail("upstream decoder rejected the 1080p GPU sample");
    }
    const double psnr = BestPsnr(frame, decoded, 1920, 1080, decoded_pitch);
    CFHD_CloseEncoder(opened.ref);
    if (psnr < 35.0) return Fail("1080p decode PSNR is below 35 dB");
    std::cout << "1080p full sample bytes " << full.encoded_bytes
              << " compressed D2H " << full.d2h_compressed_bytes
              << " lowpass D2H " << full.d2h_lowpass_bytes
              << " entropy " << full.mean_entropy_ms << " ms"
              << " psnr " << psnr << "\n";

    opened = OpenEncoder(3840, 2160);
    if (!opened.sample) return Fail("4K encoder open failed");
    gpu = hypercine::CreateGpuEntropySession();
    if (!gpu) {
        CFHD_CloseEncoder(opened.ref);
        return Fail("4K gpu session failed");
    }
    const auto bands_4k = hypercine::EnumerateBands(reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                                     opened.sample->m_channelCount);
    int level1 = 0;
    for (const auto& band : bands_4k) {
        if (band.lowpass || band.channel != 0 || band.level != 1) continue;
        TRANSFORM* tf = reinterpret_cast<transform**>(opened.sample->m_transformArray)[band.channel];
        IMAGE* wavelet = nullptr;
        for (int k = 0; k < tf->num_wavelets; ++k) {
            if (tf->wavelet[k] && tf->wavelet[k]->level == band.level) wavelet = tf->wavelet[k];
        }
        if (!wavelet || !wavelet->band[band.band]) continue;
        std::vector<int16_t> samples(static_cast<size_t>(band.width) * static_cast<size_t>(band.height));
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        const PIXEL* src = wavelet->band[band.band];
        for (int y = 0; y < band.height; ++y) {
            std::memcpy(samples.data() + static_cast<size_t>(y) * band.width,
                        src + static_cast<size_t>(y) * stride, sizeof(int16_t) * band.width);
        }
        const std::string name = std::string("4k_Y_L1_") + std::to_string(band.band);
        if (CheckBuffer(&opened, band, samples, gpu, name.c_str()) != 0) {
            hypercine::DestroyGpuEntropySession(gpu);
            CFHD_CloseEncoder(opened.ref);
            return 1;
        }
        ++level1;
    }
    hypercine::DestroyGpuEntropySession(gpu);
    CFHD_CloseEncoder(opened.ref);
    if (level1 != 3) return Fail("expected three 4K luma level-1 highpass bands");
    std::cout << "PHASE 2B ENTROPY STAGES MATCH\n";
    return 0;
}
