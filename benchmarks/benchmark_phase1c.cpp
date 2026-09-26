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

#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "frame.h"
void EncodeQuantizedGroup(ENCODER* encoder, TRANSFORM* transform[], int num_transforms, BITSTREAM* output);
}

#include "VideoBuffers.h"
#include "SampleMetadata.h"

#define private public
#define protected public
#include "SampleEncoder.h"
#undef private
#undef protected

#include "hypercine/wavelet.h"
#include "hypercine/canonical.h"
#include "hypercine/entropy_parallel.h"
#include "hypercine/hybrid_encoder.h"
#include "hypercine/gpu_pyramid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif

namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto origin = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - origin).count();
}

std::string CpuBrand() {
#ifdef _WIN32
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004) return "unknown";
    char brand[49] = {};
    __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
    __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
    __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
    std::string text(brand);
    const size_t start = text.find_first_not_of(' ');
    return start == std::string::npos ? text : text.substr(start);
#else
    return "unknown";
#endif
}

unsigned AffinityMask() {
#ifdef _WIN32
    DWORD_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    return static_cast<unsigned>(process_mask);
#else
    return 0;
#endif
}

struct Opened {
    CFHD_EncoderRef ref = nullptr;
    CSampleEncoder* sample = nullptr;
    bool ok = false;
};

Opened OpenCanon(int width, int height) {
    Opened opened;
    if (CFHD_OpenEncoder(&opened.ref, nullptr) != CFHD_ERROR_OKAY) return opened;
    if (CFHD_PrepareToEncode(opened.ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        return opened;
    }
    opened.sample = reinterpret_cast<CSampleEncoder*>(opened.ref);
    opened.ok = true;
    return opened;
}

double TimeEncode(CFHD_EncoderRef ref, const std::vector<uint8_t>& frame, int pitch, int frames) {
    for (int i = 0; i < hypercine::kCanonicalWarmupFrames; ++i) {
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
    }
    const double t0 = NowMs();
    for (int i = 0; i < frames; ++i) {
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
    }
    return (NowMs() - t0) / frames;
}

struct Stage {
    const char* name;
    double ms;
    const char* kind;
};

struct Replay {
    double wall_ms = 0;
    double color_ms = 0;
    double level1_y_ms = 0;
    double level1_u_ms = 0;
    double level1_v_ms = 0;
    double higher_ms = 0;
    double entropy_ms = 0;
    double timer_overhead_ms = 0;
};

Replay ExclusiveReplay(CSampleEncoder* sample, const std::vector<uint8_t>& frame, int pitch) {
    ENCODER* encoder = sample->m_encoder;
    TRANSFORM** transforms = sample->m_transformArray;
    const int channels = sample->m_channelCount;
    Replay replay;
    const double wall0 = NowMs();

    const double c0 = NowMs();
    ConvertRGB32to10bitYUVFrame(const_cast<uint8_t*>(frame.data()), pitch, encoder->frame,
                                static_cast<uint8_t*>(sample->m_scratchBuffer),
                                static_cast<int>(sample->m_scratchBufferSize),
                                encoder->input.color_space, encoder->codec.precision, 1, 0);
    replay.color_ms = NowMs() - c0;

    auto level1 = [&](int channel) {
        const double t0 = NowMs();
        IMAGE* image = encoder->frame->channel[channel];
        IMAGE* wavelet = transforms[channel]->wavelet[0];
        TransformForwardSpatial(encoder->allocator, image, 0, wavelet, 1,
                                static_cast<PIXEL*>(sample->m_scratchBuffer), sample->m_scratchBufferSize,
                                0, wavelet->quant, 0);
        return NowMs() - t0;
    };
    replay.level1_y_ms = level1(0);
    replay.level1_u_ms = level1(1);
    replay.level1_v_ms = level1(2);

    const double h0 = NowMs();
    ComputeGroupTransformQuant(encoder, transforms, channels);
    replay.higher_ms = NowMs() - h0;

    BITSTREAM stream;
    std::vector<uint8_t> buffer(std::max<size_t>(sample->m_sampleBuffer->BufferSize(), 1u << 20));
    std::memset(&stream, 0, sizeof(stream));
    InitBitstreamBuffer(&stream, buffer.data(), buffer.size(), BITSTREAM_ACCESS_WRITE);
    const double e0 = NowMs();
    EncodeQuantizedGroup(encoder, transforms, channels, &stream);
    replay.entropy_ms = NowMs() - e0;

    replay.wall_ms = NowMs() - wall0;
    const double sum = replay.color_ms + replay.level1_y_ms + replay.level1_u_ms + replay.level1_v_ms
                     + replay.higher_ms + replay.entropy_ms;
    replay.timer_overhead_ms = std::max(0.0, replay.wall_ms - sum);
    return replay;
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
    return err == CFHD_ERROR_OKAY;
}

double Psnr(const std::vector<uint8_t>& src, const std::vector<uint8_t>& dec, int width, int height, int pitch) {
    double sse = 0;
    int count = 0;
    const int rows = std::min(height, pitch > 0 ? static_cast<int>(dec.size() / pitch) : 0);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int a = src[(static_cast<size_t>(y) * width + x) * 4u + c];
                const int b = dec[static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4u + c];
                const double d = static_cast<double>(a - b);
                sse += d * d;
                ++count;
            }
        }
    }
    if (count == 0 || sse <= 0.0) return 99.0;
    return 10.0 * std::log10((255.0 * 255.0) / (sse / count));
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::fprintf(stderr, "phase1c start\n");
    std::fflush(stderr);
    const std::string json_path = (argc > 1)
        ? argv[1]
        : "C:/Users/User/Documents/GitHub/HyperCine/benchmarks/results/hybrid_phase1c.json";
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    std::fprintf(stderr, "threads %d\n", hw);
    const std::string cpu = CpuBrand();
    std::fprintf(stderr, "cpu %s\n", cpu.c_str());
    HyperCineCUDADeviceInfo device{};
    HyperCine_GetCUDADeviceInfo(&device);
    std::fprintf(stderr, "gpu ok\n");
    std::cout << "Phase 1C canonical benchmark\n";
    std::cout << "CPU " << cpu << " threads " << hw << " affinity 0x" << std::hex << AffinityMask() << std::dec << "\n";
    std::cout << "GPU " << device.device_name << " sm " << device.major << "." << device.minor << "\n";
    std::cout << "threaded encoder compile flag " << _THREADED_ENCODER << "\n";

    std::ostringstream json;
    json << "{\n  \"classification_legend\": {\n";
    json << "    \"measured\": \"timed on this machine in this process\",\n";
    json << "    \"derived\": \"arithmetic on measured values\",\n";
    json << "    \"estimated\": \"model that is not a direct timer\",\n";
    json << "    \"hypothetical\": \"not implemented and not timed\"\n  },\n";
    json << "  \"canonical_configuration\": {\n";
    json << "    \"pixel_format\": \"CFHD_PIXEL_FORMAT_BGRA\",\n";
    json << "    \"encoded_format\": \"CFHD_ENCODED_FORMAT_YUV_422\",\n";
    json << "    \"quality\": \"CFHD_ENCODING_QUALITY_FILMSCAN3\",\n";
    json << "    \"flags\": \"CFHD_ENCODING_FLAGS_NONE\",\n";
    json << "    \"gop_length\": 1,\n";
    json << "    \"progressive\": true,\n";
    json << "    \"pattern\": \"canonical_lcg_0x1337BEEF\",\n";
    json << "    \"seed\": " << hypercine::kCanonicalSeed << ",\n";
    json << "    \"warmup_frames\": " << hypercine::kCanonicalWarmupFrames << ",\n";
    json << "    \"timed_frames\": " << hypercine::kCanonicalTimedFrames << ",\n";
    json << "    \"timer\": \"CFHD_EncodeSample wall clock, GetSampleData excluded\",\n";
    json << "    \"upstream_threaded_encoder\": " << _THREADED_ENCODER << ",\n";
    json << "    \"cpu_affinity_mask\": " << AffinityMask() << ",\n";
    json << "    \"compiler\": \"MSVC " << _MSC_VER << "\",\n";
    json << "    \"build_type\": \"Release\"\n  },\n";
    json << "  \"system\": {\n";
    json << "    \"cpu_model\": \"" << JsonEscape(cpu) << "\",\n";
    json << "    \"thread_count\": " << hw << ",\n";
    json << "    \"gpu_model\": \"" << JsonEscape(device.device_name) << "\",\n";
    json << "    \"compute_capability\": \"" << device.major << "." << device.minor << "\"\n  },\n";

    json << "  \"reconciliation\": [\n";
    const hypercine::CanonicalPattern patterns[] = {
        hypercine::CanonicalPattern::Flat128,
        hypercine::CanonicalPattern::GradientChecker,
        hypercine::CanonicalPattern::Canonical
    };
    for (size_t p = 0; p < 3; ++p) {
        const auto pattern = patterns[p];
        std::fprintf(stderr, "reconcile %s\n", hypercine::PatternName(pattern));
        std::fflush(stderr);
        const int width = 3840, height = 2160, pitch = width * 4;
        auto frame = hypercine::MakeFrame(width, height, pattern);
        Opened opened = OpenCanon(width, height);
        std::fprintf(stderr, " opened %d\n", opened.ok ? 1 : 0);
        std::fflush(stderr);
        if (!opened.ok) return 1;
        std::fprintf(stderr, " timing %s\n", hypercine::PatternName(pattern));
        std::fflush(stderr);
        const double with_get = [&]() {
            for (int i = 0; i < 2; ++i) CFHD_EncodeSample(opened.ref, frame.data(), pitch);
            const double t0 = NowMs();
            for (int i = 0; i < 4; ++i) {
                CFHD_EncodeSample(opened.ref, frame.data(), pitch);
                void* data = nullptr; size_t size = 0;
                CFHD_GetSampleData(opened.ref, &data, &size);
            }
            return (NowMs() - t0) / 4.0;
        }();
        const double bare = TimeEncode(opened.ref, frame, pitch, 4);
        Replay replay = ExclusiveReplay(opened.sample, frame, pitch);
        const double level1 = replay.level1_y_ms + replay.level1_u_ms + replay.level1_v_ms;
        const double exclusive_sum = replay.color_ms + level1 + replay.higher_ms + replay.entropy_ms;
        const double nested_parent_and_child = exclusive_sum + level1;
        std::cout << hypercine::PatternName(pattern) << " bare " << bare << " ms, with GetSampleData " << with_get
                  << " ms, replay " << replay.wall_ms << " ms\n";
        json << "    {\n";
        json << "      \"pattern\": \"" << hypercine::PatternName(pattern) << "\",\n";
        json << "      \"resolution\": \"4K_UHD\", \"width\": 3840, \"height\": 2160,\n";
        json << "      \"uninstrumented_full_encoder_ms\": " << bare << ",\n";
        json << "      \"uninstrumented_plus_get_sample_data_ms\": " << with_get << ",\n";
        json << "      \"instrumented_replay_wall_ms\": " << replay.wall_ms << ",\n";
        json << "      \"instrumentation_overhead_ms\": " << replay.timer_overhead_ms << ",\n";
        json << "      \"slowdown_replay_over_uninstrumented\": " << (bare > 0 ? replay.wall_ms / bare : 0) << ",\n";
        json << "      \"exclusive_stages_ms\": {\n";
        json << "        \"color\": " << replay.color_ms << ",\n";
        json << "        \"wavelet_level1_y\": " << replay.level1_y_ms << ",\n";
        json << "        \"wavelet_level1_u\": " << replay.level1_u_ms << ",\n";
        json << "        \"wavelet_level1_v\": " << replay.level1_v_ms << ",\n";
        json << "        \"wavelet_levels_2_and_3\": " << replay.higher_ms << ",\n";
        json << "        \"entropy_and_framing\": " << replay.entropy_ms << "\n      },\n";
        json << "      \"exclusive_sum_ms\": " << exclusive_sum << ",\n";
        json << "      \"double_counted_if_level1_added_again_ms\": " << nested_parent_and_child << ",\n";
        json << "      \"exclusive_percent_of_instrumented_wall\": {\n";
        json << "        \"color\": " << (100.0 * replay.color_ms / replay.wall_ms) << ",\n";
        json << "        \"wavelet_level1\": " << (100.0 * level1 / replay.wall_ms) << ",\n";
        json << "        \"wavelet_levels_2_and_3\": " << (100.0 * replay.higher_ms / replay.wall_ms) << ",\n";
        json << "        \"entropy_and_framing\": " << (100.0 * replay.entropy_ms / replay.wall_ms) << ",\n";
        json << "        \"timer_overhead\": " << (100.0 * replay.timer_overhead_ms / replay.wall_ms) << "\n      },\n";
        json << "      \"class\": \"measured\"\n    }" << (p + 1 < 3 ? ",\n" : "\n");
        CFHD_CloseEncoder(opened.ref);
    }
    json << "  ],\n";

    json << "  \"resolutions\": [\n";
    const int worker_list[] = {1, 2, 4, 8, 16, hw};
    for (size_t r = 0; r < 3; ++r) {
        const auto res = hypercine::kResolutions[r];
        const int pitch = res.width * 4;
        const int frames = hypercine::kCanonicalTimedFrames;
        auto frame = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        Opened opened = OpenCanon(res.width, res.height);
        if (!opened.ok) return 1;
        std::cout << "\n== " << res.name << " ==\n";
        const double bare = TimeEncode(opened.ref, frame, pitch, frames);
        Replay replay = ExclusiveReplay(opened.sample, frame, pitch);
        std::cout << "UNINSTRUMENTED " << bare << " ms  INSTRUMENTED " << replay.wall_ms << " ms\n";

        if (!hypercine::RefreshUpstreamPyramid(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                               reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                               opened.sample->m_channelCount, frame.data(), pitch,
                                               opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize)) {
            std::cerr << "refresh failed\n";
            return 1;
        }
        auto bands = hypercine::DescribeBands(reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                              opened.sample->m_channelCount);
        size_t allocated = 0, entropy_bytes = 0, tight_8 = 0, meaningful = 0;
        for (const auto& band : bands) {
            allocated += static_cast<size_t>(band.allocated_bytes);
            entropy_bytes += static_cast<size_t>(band.entropy_input_bytes);
            meaningful += static_cast<size_t>(band.coeff_count) * static_cast<size_t>(band.element_bytes);
            if (band.fits_in_8bit) tight_8 += static_cast<size_t>(band.coeff_count);
            else tight_8 += static_cast<size_t>(band.entropy_input_bytes);
        }
        auto runs = hypercine::ScanZeroRuns(reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                            opened.sample->m_channelCount, 0);

        json << "    {\n      \"name\": \"" << res.name << "\", \"width\": " << res.width << ", \"height\": " << res.height << ",\n";
        json << "      \"uninstrumented_full_encoder_ms\": " << bare << ",\n";
        json << "      \"instrumented_full_encoder_ms\": " << replay.wall_ms << ",\n";
        json << "      \"instrumentation_overhead_ms\": " << replay.timer_overhead_ms << ",\n";
        json << "      \"instrumentation_slowdown\": " << (bare > 0 ? replay.wall_ms / bare : 0) << ",\n";
        json << "      \"class\": \"measured\",\n";
        json << "      \"bands\": [\n";
        for (size_t b = 0; b < bands.size(); ++b) {
            const auto& band = bands[b];
            json << "        {\"channel\": \"" << (band.channel == 0 ? "Y" : (band.channel == 1 ? "C1" : "C2"))
                 << "\", \"level\": " << band.level << ", \"band\": \"" << (band.lowpass ? "LL" : (band.band == 1 ? "LH" : (band.band == 2 ? "HL" : "HH")))
                 << "\", \"width\": " << band.width << ", \"height\": " << band.height
                 << ", \"storage_type\": \"" << band.storage_type << "\", \"pixel_type\": " << band.pixel_type
                 << ", \"pitch_bytes\": " << band.pitch_bytes
                 << ", \"allocated_bytes\": " << band.allocated_bytes
                 << ", \"entropy_input_bytes\": " << band.entropy_input_bytes
                 << ", \"value_min\": " << band.value_min << ", \"value_max\": " << band.value_max
                 << ", \"fits_in_int8\": " << (band.fits_in_8bit ? "true" : "false") << "}"
                 << (b + 1 < bands.size() ? ",\n" : "\n");
        }
        json << "      ],\n";
        json << "      \"transfer\": {\n";
        json << "        \"allocated_subband_bytes\": " << allocated << ",\n";
        json << "        \"entropy_input_bytes\": " << entropy_bytes << ",\n";
        json << "        \"minimum_d2h_bytes_for_encodequantizedband\": " << meaningful << ",\n";
        json << "        \"packed_bytes_if_int8_where_range_fits\": " << tight_8 << ",\n";
        json << "        \"class\": \"measured\"\n      },\n";

        json << "      \"entropy_scaling\": [\n";
        double best_ms = 1e100;
        int best_workers = 1;
        std::string best_schedule = "subband";
        double serial_ms = 0;
        for (int windex = 0; windex < 6; ++windex) {
            const int workers = worker_list[windex];
            if (workers <= 0) continue;
            if (windex > 0 && workers == worker_list[windex - 1]) continue;
            hypercine::RefreshUpstreamPyramid(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                              reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                              opened.sample->m_channelCount, frame.data(), pitch,
                                              opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize);
            const bool compare = (res.width <= 3840 && (workers == 1 || workers == 8));
            auto output = hypercine::EncodeEntropy(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                                   reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                                   opened.sample->m_channelCount, workers,
                                                   hypercine::EntropySchedule::SubbandParallel, compare);
            const double ms = output.entropy_wall_ms + output.assembly_ms;
            if (workers == 1) serial_ms = output.entropy_wall_ms;
            if (ms < best_ms) { best_ms = ms; best_workers = workers; best_schedule = "subband"; }
            std::cout << "  subband x" << workers << " entropy " << output.entropy_wall_ms
                      << " asm " << output.assembly_ms << " match " << output.byte_match << "\n";
            json << "        {\"schedule\": \"subband\", \"workers\": " << workers
                 << ", \"entropy_ms\": " << output.entropy_wall_ms
                 << ", \"assembly_ms\": " << output.assembly_ms
                 << ", \"sum_band_ms\": " << output.sum_band_ms
                 << ", \"max_worker_ms\": " << output.max_worker_ms
                 << ", \"min_worker_ms\": " << output.min_worker_ms
                 << ", \"scheduling_overhead_ms\": " << output.scheduling_overhead_ms
                 << ", \"byte_match\": " << (compare ? (output.byte_match ? "true" : "false") : "\"not_compared\"")
                 << ", \"encoded_bytes\": " << output.bitstream_bytes
                 << ", \"class\": \"measured\"},\n";
        }
        hypercine::RefreshUpstreamPyramid(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                          reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                          opened.sample->m_channelCount, frame.data(), pitch,
                                          opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize);
        const bool compare_channel = res.width <= 1920;
        auto channel = hypercine::EncodeEntropy(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                                reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                                opened.sample->m_channelCount, 3,
                                                hypercine::EntropySchedule::ChannelParallel, compare_channel);
        const double channel_total = channel.entropy_wall_ms + channel.assembly_ms;
        if (channel_total < best_ms) { best_ms = channel_total; best_workers = 3; best_schedule = "channel"; }
        std::cout << "  channel x3 entropy " << channel.entropy_wall_ms << " match " << channel.byte_match << "\n";
        json << "        {\"schedule\": \"channel\", \"workers\": 3, \"entropy_ms\": " << channel.entropy_wall_ms
             << ", \"assembly_ms\": " << channel.assembly_ms
             << ", \"sum_band_ms\": " << channel.sum_band_ms
             << ", \"max_worker_ms\": " << channel.max_worker_ms
             << ", \"min_worker_ms\": " << channel.min_worker_ms
             << ", \"scheduling_overhead_ms\": " << channel.scheduling_overhead_ms
             << ", \"byte_match\": " << (compare_channel ? (channel.byte_match ? "true" : "false") : "\"not_compared\"")
             << ", \"encoded_bytes\": " << channel.bitstream_bytes
             << ", \"note\": \"upstream _THREADED_ENCODER is 0; this is the external equivalent of EncodeQuantizedGroupThreaded\", \"class\": \"measured\"}\n      ],\n";

        std::vector<hypercine::BandInfo> ranked = channel.bands;
        std::sort(ranked.begin(), ranked.end(), [](const hypercine::BandInfo& a, const hypercine::BandInfo& b) {
            return a.encode_ms > b.encode_ms;
        });
        json << "      \"dominant_bands\": [\n";
        const size_t show = std::min<size_t>(8, ranked.size());
        for (size_t i = 0; i < show; ++i) {
            json << "        {\"name\": \"" << ranked[i].name << "\", \"encode_ms\": " << ranked[i].encode_ms
                 << ", \"coeffs\": " << ranked[i].coeff_count << ", \"encoded_bytes\": " << ranked[i].encoded_bytes << "}"
                 << (i + 1 < show ? ",\n" : "\n");
        }
        json << "      ],\n";
        json << "      \"fastest_entropy\": {\"schedule\": \"" << best_schedule << "\", \"workers\": " << best_workers
             << ", \"ms\": " << best_ms << ", \"class\": \"measured\"},\n";
        json << "      \"scaling_efficiency_1_to_best\": " << (best_ms > 0 && serial_ms > 0 ? (serial_ms / best_ms) / best_workers : 0)
             << ",\n";
        const size_t encoded_for_stats = channel.bitstream_bytes;
        auto zero = hypercine::ScanZeroRuns(reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                            opened.sample->m_channelCount, encoded_for_stats);
        json << "      \"coefficient_stats\": {\"coeffs\": " << zero.coeff_count
             << ", \"zeros\": " << zero.zero_count << ", \"nonzeros\": " << zero.nonzero_count
             << ", \"runs\": " << zero.run_count << ", \"mean_zero_run\": " << zero.mean_run
             << ", \"encoded_bits_per_coeff_including_headers\": " << zero.encoded_bits_per_coeff
             << ", \"run_hist_1_2_4_8_16_32_128_more\": [";
        for (int i = 0; i < 8; ++i) json << zero.run_hist[i] << (i < 7 ? ", " : "");
        json << "], \"class\": \"measured\"},\n";
        (void)runs;

        const int hybrid_frames = (res.width >= 7680) ? 2 : hypercine::kCanonicalTimedFrames;
        auto sync = hypercine::RunHybrid(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                         reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                         opened.sample->m_channelCount, frame.data(), pitch,
                                         opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize,
                                         hybrid_frames, best_workers, false);
        auto pipe = hypercine::RunHybrid(reinterpret_cast<encoder*>(opened.sample->m_encoder),
                                         reinterpret_cast<transform**>(opened.sample->m_transformArray),
                                         opened.sample->m_channelCount, frame.data(), pitch,
                                         opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize,
                                         hybrid_frames, best_workers, true);
        std::vector<uint8_t> decoded;
        int decoded_pitch = 0;
        const bool dec_ok = !sync.bitstream.empty() && DecodeSample(sync.bitstream, res.width, res.height, &decoded, &decoded_pitch);
        const double psnr = dec_ok ? Psnr(frame, decoded, res.width, res.height, decoded_pitch) : 0;
        std::cout << "  hybrid sync " << sync.sustained_fps << " fps latency " << sync.mean_latency_ms
                  << " pipe " << pipe.sustained_fps << " fps latency " << pipe.mean_latency_ms
                  << " psnr " << psnr << "\n";
        auto write_hybrid = [&](const char* name, const hypercine::HybridRun& hybrid) {
            json << "      \"" << name << "\": {\"success\": " << (hybrid.success ? "true" : "false")
                 << ", \"frames\": " << hybrid.frames
                 << ", \"sustained_fps\": " << hybrid.sustained_fps
                 << ", \"wall_ms\": " << hybrid.wall_ms
                 << ", \"startup_ms\": " << hybrid.startup_ms
                 << ", \"mean_latency_ms\": " << hybrid.mean_latency_ms
                 << ", \"mean_color_ms\": " << hybrid.mean_color_ms
                 << ", \"mean_pack_ms\": " << hybrid.mean_pack_ms
                 << ", \"mean_h2d_ms\": " << hybrid.mean_h2d_ms
                 << ", \"mean_gpu_ms\": " << hybrid.mean_gpu_ms
                 << ", \"mean_d2h_ms\": " << hybrid.mean_d2h_ms
                 << ", \"mean_entropy_ms\": " << hybrid.mean_entropy_ms
                 << ", \"mean_assembly_ms\": " << hybrid.mean_assembly_ms
                 << ", \"h2d_bytes\": " << hybrid.h2d_bytes
                 << ", \"d2h_bytes\": " << hybrid.d2h_bytes
                 << ", \"encoded_bytes\": " << hybrid.encoded_bytes
                 << ", \"gpu_compute_duty\": " << hybrid.gpu_compute_duty
                 << ", \"process_cpu_seconds\": " << hybrid.process_cpu_seconds
                 << ", \"byte_match\": " << (hybrid.byte_match ? "true" : "false")
                 << ", \"coeff_mismatches\": " << hybrid.coeff_mismatches
                 << ", \"coeff_compared\": " << hybrid.coeff_compared
                 << ", \"error\": \"" << JsonEscape(hybrid.error) << "\", \"class\": \"measured\"}";
        };
        write_hybrid("hybrid_synchronous", sync);
        json << ",\n";
        write_hybrid("hybrid_pipelined", pipe);
        json << ",\n      \"decode\": {\"upstream_decoder\": " << (dec_ok ? "true" : "false")
             << ", \"psnr_db\": " << psnr << ", \"class\": \"measured\"}\n";

        json << "    }" << (r + 1 < 3 ? ",\n" : "\n");
        CFHD_CloseEncoder(opened.ref);
    }
    json << "  ],\n";

    json << "  \"color_conversion\": [\n";
    for (int which = 0; which < 2; ++which) {
        const auto res = hypercine::kResolutions[which];
        auto frame = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        Opened opened = OpenCanon(res.width, res.height);
        if (!opened.ok) return 1;
        CFHD_EncodeSample(opened.ref, frame.data(), res.width * 4);
        ENCODER* encoder = opened.sample->m_encoder;
        const double c0 = NowMs();
        ConvertRGB32to10bitYUVFrame(frame.data(), res.width * 4, encoder->frame,
                                    static_cast<uint8_t*>(opened.sample->m_scratchBuffer),
                                    static_cast<int>(opened.sample->m_scratchBufferSize),
                                    encoder->input.color_space, encoder->codec.precision, 1, 0);
        const double cpu_ms = NowMs() - c0;
        std::vector<int16_t> y(static_cast<size_t>(res.width) * res.height);
        std::vector<int16_t> u(static_cast<size_t>(res.width / 2) * res.height);
        std::vector<int16_t> v(static_cast<size_t>(res.width / 2) * res.height);
        HyperCineColorTiming timing{};
        const int rc = HyperCine_BgraToYuv10_CUDA(frame.data(), res.width, res.height, res.width * 4,
                                                  encoder->input.color_space, y.data(), u.data(), v.data(), &timing);
        int mismatch = 0;
        if (rc == 0) {
            auto count_plane = [&](int channel, const std::vector<int16_t>& got, int width) {
                IMAGE* image = encoder->frame->channel[channel];
                const int stride = image->pitch / static_cast<int>(sizeof(PIXEL));
                for (int row = 0; row < image->height; ++row) {
                    const PIXEL* src = image->band[0] + static_cast<size_t>(row) * stride;
                    const int16_t* dst = got.data() + static_cast<size_t>(row) * width;
                    for (int col = 0; col < width; ++col) if (src[col] != dst[col]) ++mismatch;
                }
            };
            count_plane(0, y, res.width);
            int u_mismatch = 0, v_mismatch = 0;
            auto count_against = [&](int channel, const std::vector<int16_t>& got) {
                IMAGE* image = encoder->frame->channel[channel];
                const int stride = image->pitch / static_cast<int>(sizeof(PIXEL));
                int local = 0;
                for (int row = 0; row < res.height; ++row) {
                    const PIXEL* src = image->band[0] + static_cast<size_t>(row) * stride;
                    const int16_t* dst = got.data() + static_cast<size_t>(row) * (res.width / 2);
                    for (int col = 0; col < res.width / 2; ++col) if (src[col] != dst[col]) ++local;
                }
                return local;
            };
            u_mismatch = count_against(2, u);
            v_mismatch = count_against(1, v);
            mismatch += u_mismatch + v_mismatch;
        }
        std::cout << "color " << res.name << " cpu " << cpu_ms << " kernel " << timing.kernel_ms
                  << " mismatches " << mismatch << "\n";
        json << "    {\"resolution\": \"" << res.name << "\", \"cpu_ms\": " << cpu_ms
             << ", \"cuda_kernel_ms\": " << timing.kernel_ms
             << ", \"h2d_ms\": " << timing.h2d_ms
             << ", \"d2h_ms\": " << timing.d2h_ms
             << ", \"resident_kernel_ms\": " << timing.resident_kernel_ms
             << ", \"h2d_plus_kernel_ms\": " << (timing.h2d_ms + timing.kernel_ms)
             << ", \"mismatches\": " << mismatch
             << ", \"color_space\": " << encoder->input.color_space
             << ", \"class\": \"measured\"}" << (which == 0 ? ",\n" : "\n");
        CFHD_CloseEncoder(opened.ref);
    }
    json << "  ],\n";

    json << "  \"gpu_entropy_decision\": {\n";
    json << "    \"implemented\": false,\n";
    json << "    \"class\": \"hypothetical\",\n";
    json << "    \"note\": \"Phase 1C does not implement GPU zero-run, symbol generation, prefix sums, or VLC packing. The recommendation is in the completion report and uses the measured CPU entropy times above.\"\n";
    json << "  }\n}\n";

    std::ofstream out(json_path, std::ios::binary);
    if (!out) {
        std::cerr << "cannot write " << json_path << "\n";
        const std::string fallback = "C:/Users/User/AppData/Local/Temp/hybrid_phase1c.json";
        std::ofstream alt(fallback, std::ios::binary);
        if (!alt) return 1;
        alt << json.str();
        std::cout << "\nWrote " << fallback << "\n";
        return 0;
    }
    out << json.str();
    std::cout << "\nWrote " << json_path << "\n";
    return 0;
}
