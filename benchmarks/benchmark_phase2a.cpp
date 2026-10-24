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

#include <cstddef>
#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"
#include "CFHDTypes.h"

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
#include "hypercine/hybrid_encoder.h"
#include "hypercine/phase2a.h"
#include "hypercine/phase2a_gpu.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "bench_host.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto origin = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - origin).count();
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

struct Opened {
    CFHD_EncoderRef ref = nullptr;
    CSampleEncoder* sample = nullptr;
    bool ok = false;
};

Opened OpenAt(int width, int height, CFHD_EncodingQuality quality) {
    Opened opened;
    if (CFHD_OpenEncoder(&opened.ref, nullptr) != CFHD_ERROR_OKAY) return opened;
    if (CFHD_PrepareToEncode(opened.ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, quality) != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(opened.ref);
        opened.ref = nullptr;
        return opened;
    }
    opened.sample = reinterpret_cast<CSampleEncoder*>(opened.ref);
    opened.ok = true;
    return opened;
}

double TimeUpstream(CFHD_EncoderRef ref, const std::vector<uint8_t>& frame, int pitch, int warmup, int frames) {
    for (int i = 0; i < warmup; ++i) CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
    const double t0 = NowMs();
    for (int i = 0; i < frames; ++i) CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
    return (NowMs() - t0) / frames;
}

std::vector<uint8_t> MakePattern(int width, int height, const char* name) {
    if (std::strcmp(name, "flat0") == 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4u, 0);
        for (size_t i = 3; i < frame.size(); i += 4) frame[i] = 255;
        return frame;
    }
    if (std::strcmp(name, "flat255") == 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4u, 255);
        return frame;
    }
    if (std::strcmp(name, "flat128") == 0) return hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::Flat128);
    if (std::strcmp(name, "gradient") == 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4u);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
                frame[i] = static_cast<uint8_t>((x * 255) / width);
                frame[i + 1] = static_cast<uint8_t>((y * 255) / height);
                frame[i + 2] = static_cast<uint8_t>(((x + y) * 255) / (width + height));
                frame[i + 3] = 255;
            }
        }
        return frame;
    }
    if (std::strcmp(name, "checker") == 0) return hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::GradientChecker);
    if (std::strcmp(name, "hf") == 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4u);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
                const uint8_t v = ((x ^ y) & 1) ? 255 : 0;
                frame[i] = v;
                frame[i + 1] = static_cast<uint8_t>(255 - v);
                frame[i + 2] = ((x & 1) ? 255 : 0);
                frame[i + 3] = 255;
            }
        }
        return frame;
    }
    if (std::strcmp(name, "impulse") == 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4u, 0);
        for (size_t i = 3; i < frame.size(); i += 4) frame[i] = 255;
        const int xs[] = {width / 2, 8, width - 9};
        const int ys[] = {height / 2, 8, height - 9};
        for (int k = 0; k < 3; ++k) {
            const size_t i = (static_cast<size_t>(ys[k]) * width + xs[k]) * 4u;
            frame[i] = frame[i + 1] = frame[i + 2] = 255;
        }
        return frame;
    }
    if (std::strcmp(name, "noise") == 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4u);
        uint32_t s = hypercine::kCanonicalSeed;
        for (size_t i = 0; i < frame.size(); i += 4) {
            s = s * 1664525u + 1013904223u;
            frame[i] = static_cast<uint8_t>(s >> 24);
            frame[i + 1] = static_cast<uint8_t>(s >> 16);
            frame[i + 2] = static_cast<uint8_t>(s >> 8);
            frame[i + 3] = 255;
        }
        return frame;
    }
    return hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::Canonical);
}

struct Agg {
    int frames = 0;
    int frames_fit = 0;
    int64_t coeffs = 0;
    int64_t fit_coeffs = 0;
    int vmin = 0;
    int vmax = 0;
    bool have = false;
};

void Accumulate(Agg* agg, int vmin, int vmax, int64_t coeffs, int64_t fit) {
    if (!agg->have) {
        agg->vmin = vmin;
        agg->vmax = vmax;
        agg->have = true;
    } else {
        agg->vmin = std::min(agg->vmin, vmin);
        agg->vmax = std::max(agg->vmax, vmax);
    }
    agg->coeffs += coeffs;
    agg->fit_coeffs += fit;
    ++agg->frames;
    if (fit == coeffs) ++agg->frames_fit;
}

std::string BandKey(int channel, int level, int band) {
    const char* ch = channel == 0 ? "Y" : (channel == 1 ? "C1" : "C2");
    const char* name = band == 0 ? "LL" : (band == 1 ? "LH" : (band == 2 ? "HL" : "HH"));
    return std::string(ch) + "L" + std::to_string(level) + name;
}

void WriteRun(std::ostream& json, const hypercine::Phase2Run& run) {
    json << "\"success\": " << (run.success ? "true" : "false")
         << ", \"verified\": " << (run.verified ? "true" : "false")
         << ", \"byte_match\": " << (run.byte_match ? "true" : "false")
         << ", \"coeff_mismatches\": " << run.coeff_mismatches
         << ", \"coeff_compared\": " << run.coeff_compared
         << ", \"expand_mismatches\": " << run.expand_mismatches
         << ", \"frames\": " << run.frames
         << ", \"workers\": " << run.workers
         << ", \"pipeline_depth\": " << run.pipeline_depth
         << ", \"sustained_fps\": " << run.sustained_fps
         << ", \"wall_ms\": " << run.wall_ms
         << ", \"mean_latency_ms\": " << run.mean_latency_ms
         << ", \"mean_color_ms\": " << run.mean_color_ms
         << ", \"mean_h2d_ms\": " << run.mean_h2d_ms
         << ", \"mean_gpu_ms\": " << run.mean_gpu_ms
         << ", \"mean_range_ms\": " << run.mean_range_ms
         << ", \"mean_pack_ms\": " << run.mean_pack_ms
         << ", \"mean_d2h_ms\": " << run.mean_d2h_ms
         << ", \"mean_entropy_ms\": " << run.mean_entropy_ms
         << ", \"mean_assembly_ms\": " << run.mean_assembly_ms
         << ", \"mean_schedule_ms\": " << run.mean_schedule_ms
         << ", \"mean_overlap_gpu_d2h_ms\": " << run.mean_overlap_gpu_d2h_ms
         << ", \"mean_overlap_d2h_entropy_ms\": " << run.mean_overlap_d2h_entropy_ms
         << ", \"mean_assembly_delay_ms\": " << run.mean_assembly_delay_ms
         << ", \"mean_ready_bands\": " << run.mean_ready_bands
         << ", \"cpu_cores\": " << run.cpu_cores
         << ", \"h2d_bytes\": " << run.h2d_bytes
         << ", \"d2h_bytes\": " << run.d2h_bytes
         << ", \"encoded_bytes\": " << run.encoded_bytes
         << ", \"h2d_gbps\": " << (run.mean_h2d_ms > 0 ? (run.h2d_bytes / run.mean_h2d_ms / 1.0e6) : 0)
         << ", \"d2h_gbps\": " << (run.mean_d2h_ms > 0 ? (run.d2h_bytes / run.mean_d2h_ms / 1.0e6) : 0)
         << ", \"error\": \"" << JsonEscape(run.error) << "\"";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1)
        ? argv[1]
        : "benchmarks/results/phase2a.json";
    std::ostringstream json;
    HyperCineDeviceLink link{};
    HyperCine_QueryDeviceLink(&link);
    std::cout << "GPU " << link.name << " async_engines " << link.async_engines
              << " overlap " << link.gpu_overlap << " bus " << link.mem_bus_bits << "\n";

    json << "{\n  \"system\": {\"gpu\": \"" << JsonEscape(link.name)
         << "\", \"async_engines\": " << link.async_engines
         << ", \"can_map_host\": " << link.can_map_host
         << ", \"concurrent_kernels\": " << link.concurrent_kernels
         << ", \"gpu_overlap\": " << link.gpu_overlap
         << ", \"l2_bytes\": " << link.l2_bytes
         << ", \"mem_bus_bits\": " << link.mem_bus_bits
         << ", \"mem_clock_khz\": " << link.mem_clock_khz
         << ", \"pci_bus\": " << link.pci_bus
         << ", \"pci_device\": " << link.pci_device
         << ", \"unified_addressing\": " << link.unified_addressing
         << ", \"upstream_default_threaded_flag\": " << _THREADED_ENCODER << "},\n";

    json << "  \"pcie_transfers\": [\n";
    std::vector<HyperCineTransferSample> transfers(256);
    const int transfer_count = HyperCine_BenchmarkTransfers(transfers.data(), static_cast<int>(transfers.size()));
    if (transfer_count < 0) {
        std::cerr << "transfer benchmark failed\n";
        return 1;
    }
    for (int i = 0; i < transfer_count; ++i) {
        const auto& sample = transfers[static_cast<size_t>(i)];
        std::cout << sample.host_kind << " " << sample.direction << " " << (sample.bytes >> 20)
                  << " MB  " << sample.gbps << " GB/s\n";
        if (i) json << ",\n";
        json << "    {\"host\": \"" << sample.host_kind << "\", \"direction\": \"" << sample.direction
             << "\", \"bytes\": " << sample.bytes << ", \"ms\": " << sample.ms
             << ", \"gbps\": " << sample.gbps << "}";
    }
    json << "\n  ],\n";

    json << "  \"upstream_default\": [\n";
    for (size_t r = 0; r < 3; ++r) {
        const auto& res = hypercine::kResolutions[r];
        const int frames = res.height >= 4000 ? 2 : hypercine::kCanonicalTimedFrames;
        auto picture = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        Opened opened = OpenAt(res.width, res.height, CFHD_ENCODING_QUALITY_FILMSCAN3);
        if (!opened.ok) return 1;
        const double ms = TimeUpstream(opened.ref, picture, res.width * 4, hypercine::kCanonicalWarmupFrames, frames);
        std::cout << "UPSTREAM_DEFAULT " << res.name << " " << ms << " ms fps " << (1000.0 / ms) << "\n";
        if (r) json << ",\n";
        json << "    {\"resolution\": \"" << res.name << "\", \"ms_per_frame\": " << ms
             << ", \"latency_ms\": " << ms << ", \"sustained_fps\": " << (1000.0 / ms)
             << ", \"frames\": " << frames << ", \"threaded_encoder\": " << _THREADED_ENCODER << "}";
        CFHD_CloseEncoder(opened.ref);
    }
    json << "\n  ],\n";

    json << "  \"coefficient_ranges\": [\n";
    const char* patterns[] = {"flat0", "flat128", "flat255", "gradient", "checker", "noise", "impulse", "hf", "canonical"};
    const CFHD_EncodingQuality qualities[] = {
        CFHD_ENCODING_QUALITY_LOW, CFHD_ENCODING_QUALITY_MEDIUM, CFHD_ENCODING_QUALITY_HIGH,
        CFHD_ENCODING_QUALITY_FILMSCAN1, CFHD_ENCODING_QUALITY_FILMSCAN2, CFHD_ENCODING_QUALITY_FILMSCAN3
    };
    const char* quality_names[] = {"low", "medium", "high", "filmscan1", "filmscan2", "filmscan3"};
    std::map<std::string, Agg> aggs;
    bool first_range = true;
    auto scan_one = [&](int width, int height, const char* pattern, CFHD_EncodingQuality quality, const char* qname) {
        auto picture = MakePattern(width, height, pattern);
        Opened opened = OpenAt(width, height, quality);
        if (!opened.ok) return false;
        if (CFHD_EncodeSample(opened.ref, picture.data(), width * 4) != CFHD_ERROR_OKAY) {
            CFHD_CloseEncoder(opened.ref);
            return false;
        }
        auto* enc = reinterpret_cast<encoder*>(opened.sample->m_encoder);
        auto* tfs = reinterpret_cast<transform**>(opened.sample->m_transformArray);
        if (!hypercine::RefreshUpstreamPyramid(enc, tfs, opened.sample->m_channelCount, picture.data(), width * 4,
                                               opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize)) {
            CFHD_CloseEncoder(opened.ref);
            return false;
        }
        const auto refs = hypercine::EnumerateBands(tfs, opened.sample->m_channelCount);
        for (const auto& ref : refs) {
            IMAGE* wavelet = opened.sample->m_transformArray[ref.channel]->wavelet[ref.wavelet_index];
            const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
            const PIXEL* src = wavelet->band[ref.band];
            int vmin = src[0];
            int vmax = src[0];
            int64_t fit = 0;
            const int64_t count = static_cast<int64_t>(ref.width) * ref.height;
            for (int y = 0; y < ref.height; ++y) {
                const PIXEL* row = src + static_cast<size_t>(y) * stride;
                for (int x = 0; x < ref.width; ++x) {
                    const int value = row[x];
                    vmin = std::min(vmin, value);
                    vmax = std::max(vmax, value);
                    if (value >= -128 && value <= 127) ++fit;
                }
            }
            const std::string key = BandKey(ref.channel, ref.wavelet_index, ref.band);
            Accumulate(&aggs[key], vmin, vmax, count, fit);
            if (!first_range) json << ",\n";
            first_range = false;
            json << "    {\"resolution\": \"" << width << "x" << height << "\", \"pattern\": \"" << pattern
                 << "\", \"quality\": \"" << qname << "\", \"band\": \"" << key
                 << "\", \"min\": " << vmin << ", \"max\": " << vmax
                 << ", \"coeffs\": " << count << ", \"fit_int8\": " << fit
                 << ", \"entire_band_fits\": " << (fit == count ? "true" : "false") << "}";
        }
        CFHD_CloseEncoder(opened.ref);
        return true;
    };
    for (const char* pattern : patterns) {
        for (int q = 0; q < 6; ++q) {
            std::cout << "range 1080p " << pattern << " " << quality_names[q] << "\n";
            if (!scan_one(1920, 1080, pattern, qualities[q], quality_names[q])) return 1;
        }
    }
    for (const char* pattern : {"canonical", "noise", "hf", "flat128"}) {
        std::cout << "range 4K " << pattern << "\n";
        if (!scan_one(3840, 2160, pattern, CFHD_ENCODING_QUALITY_FILMSCAN3, "filmscan3")) return 1;
    }
    json << "\n  ],\n  \"range_summary\": [\n";
    first_range = true;
    for (const auto& item : aggs) {
        const Agg& agg = item.second;
        if (!first_range) json << ",\n";
        first_range = false;
        json << "    {\"band\": \"" << item.first << "\", \"min\": " << agg.vmin << ", \"max\": " << agg.vmax
             << ", \"frames\": " << agg.frames
             << ", \"frames_entire_band_fits\": " << agg.frames_fit
             << ", \"pct_frames_fit\": " << (agg.frames ? 100.0 * agg.frames_fit / agg.frames : 0)
             << ", \"pct_coeffs_fit\": " << (agg.coeffs ? 100.0 * agg.fit_coeffs / agg.coeffs : 0) << "}";
    }
    json << "\n  ],\n";

    json << "  \"encoders\": [\n";
    bool first_enc = true;
    auto emit = [&](const char* resolution, const char* path_name, const char* mode, const hypercine::Phase2Run& run) {
        if (!first_enc) json << ",\n";
        first_enc = false;
        json << "    {\"resolution\": \"" << resolution << "\", \"path\": \"" << path_name
             << "\", \"mode\": \"" << mode << "\", ";
        WriteRun(json, run);
        json << "}";
        std::cout << path_name << " " << resolution << " " << mode
                  << " fps " << run.sustained_fps << " latency " << run.mean_latency_ms
                  << " d2h " << run.d2h_bytes << " " << run.error << "\n";
    };
    for (size_t r = 0; r < 3; ++r) {
        const auto& res = hypercine::kResolutions[r];
        const int sustained_frames = res.height >= 4000 ? 2 : 4;
        auto picture = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        Opened opened = OpenAt(res.width, res.height, CFHD_ENCODING_QUALITY_FILMSCAN3);
        if (!opened.ok) return 1;
        if (CFHD_EncodeSample(opened.ref, picture.data(), res.width * 4) != CFHD_ERROR_OKAY) return 1;
        auto* enc = reinterpret_cast<encoder*>(opened.sample->m_encoder);
        auto* tfs = reinterpret_cast<transform**>(opened.sample->m_transformArray);
        auto pipe = hypercine::RunHybrid(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                         opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize,
                                         sustained_frames, 16, true);
        hypercine::Phase2Run wrapped;
        wrapped.success = pipe.success;
        wrapped.frames = pipe.frames;
        wrapped.sustained_fps = pipe.sustained_fps;
        wrapped.wall_ms = pipe.wall_ms;
        wrapped.mean_latency_ms = pipe.mean_latency_ms;
        wrapped.mean_color_ms = pipe.mean_color_ms;
        wrapped.mean_h2d_ms = pipe.mean_h2d_ms;
        wrapped.mean_gpu_ms = pipe.mean_gpu_ms;
        wrapped.mean_d2h_ms = pipe.mean_d2h_ms;
        wrapped.mean_entropy_ms = pipe.mean_entropy_ms;
        wrapped.mean_assembly_ms = pipe.mean_assembly_ms;
        wrapped.h2d_bytes = pipe.h2d_bytes;
        wrapped.d2h_bytes = pipe.d2h_bytes;
        wrapped.encoded_bytes = pipe.encoded_bytes;
        wrapped.byte_match = pipe.byte_match;
        wrapped.coeff_mismatches = pipe.coeff_mismatches;
        wrapped.coeff_compared = pipe.coeff_compared;
        wrapped.error = pipe.error;
        wrapped.workers = 16;
        wrapped.pipeline_depth = 3;
        emit(res.name, "HYPERCINE_PHASE1C_PIPE", "sustained", wrapped);

        const hypercine::Phase2Transport transports[] = {
            hypercine::Phase2Transport::FullInt16,
            hypercine::Phase2Transport::Int16Stream,
            hypercine::Phase2Transport::CompactStream
        };
        const char* names[] = {"HYPERCINE_GPU_COLOR", "HYPERCINE_BAND_STREAM", "HYPERCINE_COMPACT_STREAM"};
        for (int t = 0; t < 3; ++t) {
            hypercine::Phase2Config cfg;
            cfg.input = hypercine::Phase2Input::HostBgra;
            cfg.transport = transports[t];
            cfg.workers = 16;
            cfg.warmup = 1;
            cfg.frames = 1;
            cfg.pipeline_depth = 1;
            cfg.verify = false;
            auto latency = hypercine::RunPhase2(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                                opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, cfg);
            emit(res.name, names[t], "latency", latency);
            cfg.frames = sustained_frames;
            cfg.pipeline_depth = 2;
            cfg.verify = true;
            auto sustained = hypercine::RunPhase2(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                                  opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, cfg);
            emit(res.name, names[t], "sustained", sustained);
        }
        hypercine::Phase2Config device;
        device.input = hypercine::Phase2Input::DeviceBgra;
        device.transport = hypercine::Phase2Transport::FullInt16;
        device.workers = 16;
        device.warmup = 1;
        device.frames = 1;
        device.pipeline_depth = 1;
        device.verify = false;
        auto device_latency = hypercine::RunPhase2(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                                   opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, device);
        emit(res.name, "DEVICE_BGRA_GPU_FRONTEND", "latency", device_latency);
        device.transport = hypercine::Phase2Transport::CompactStream;
        device.frames = sustained_frames;
        device.pipeline_depth = 2;
        device.verify = true;
        auto resident = hypercine::RunPhase2(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                             opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, device);
        emit(res.name, "HYPERCINE_GPU_RESIDENT", "sustained", resident);
        CFHD_CloseEncoder(opened.ref);
    }
    json << "\n  ]\n}\n";
    const std::string body = json.str();
    std::cerr << "json bytes " << body.size() << "\n";
    if (!hypercine::bench::WriteCompleteFile(path, body)) {
        return 1;
    }
    std::cout << "Wrote " << path << " (" << body.size() << " bytes)\n";
    return 0;
}
