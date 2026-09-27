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
#include "hypercine/phase2a.h"
#include "hypercine/phase2a_gpu.h"
#include "hypercine/phase2b.h"

#include <cuda_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

Opened OpenAt(int width, int height) {
    Opened opened;
    if (CFHD_OpenEncoder(&opened.ref, nullptr) != CFHD_ERROR_OKAY) return opened;
    if (CFHD_PrepareToEncode(opened.ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
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

int FramesFor(int height) {
    if (height >= 4000) return 2;
    if (height >= 2000) return 3;
    return 4;
}

struct UtilProbe {
    std::atomic<int> peak_util{0};
    std::atomic<bool> active{false};
    std::thread worker;

    void Start() {
        peak_util = 0;
        active = true;
        worker = std::thread([this] {
            while (active.load()) {
                FILE* pipe = _popen("nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits", "r");
                if (!pipe) break;
                int value = -1;
                if (std::fscanf(pipe, "%d", &value) == 1 && value > peak_util.load()) peak_util = value;
                _pclose(pipe);
#ifdef _WIN32
                Sleep(120);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
#endif
            }
        });
    }

    int Stop() {
        active = false;
        if (worker.joinable()) worker.join();
        return peak_util.load();
    }
};

struct StageReport {
    std::string name;
    bool match = false;
    double wall_ms = 0;
    hypercine::EntropyTiming timing;
    std::string error;
};

bool TightBand(IMAGE* wavelet, int band, int width, int height, std::vector<int16_t>* samples) {
    if (!wavelet || !wavelet->band[band] || !samples) return false;
    samples->resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
    const PIXEL* src = wavelet->band[band];
    for (int y = 0; y < height; ++y) {
        std::memcpy(samples->data() + static_cast<size_t>(y) * width, src + static_cast<size_t>(y) * stride,
                    sizeof(int16_t) * static_cast<size_t>(width));
    }
    return true;
}

StageReport MeasureStages(encoder* enc, transform** tfs, int channels, const uint8_t* bgra, int pitch, void* scratch,
                          size_t scratch_bytes, const char* mode) {
    StageReport report;
    report.name = mode;
    if (!hypercine::RefreshUpstreamPyramid(enc, tfs, channels, bgra, pitch, scratch, scratch_bytes)) {
        report.error = "pyramid refresh failed";
        return report;
    }
    const auto refs = hypercine::EnumerateBands(tfs, channels);
    struct Item {
        hypercine::BandRef ref;
        hypercine::EntropyTables tables;
        std::vector<int16_t> samples;
        hypercine::EntropySymbols cpu;
        int quant = 1;
        int scale = 0;
    };
    std::vector<Item> items;
    for (const auto& ref : refs) {
        if (ref.lowpass) continue;
        Item item;
        item.ref = ref;
        if (!hypercine::LoadEntropyTables(enc, ref.subband, &item.tables)) {
            report.error = "codebook load failed";
            return report;
        }
        if (item.tables.peaks) continue;
        TRANSFORM* tf = reinterpret_cast<TRANSFORM*>(tfs[ref.channel]);
        IMAGE* wavelet = tf ? tf->wavelet[ref.wavelet_index] : nullptr;
        if (!TightBand(wavelet, ref.band, ref.width, ref.height, &item.samples)) {
            report.error = "band extract failed";
            return report;
        }
        item.quant = wavelet->quantization[ref.band];
        if (item.quant <= 0) item.quant = wavelet->quant[ref.band];
        if (item.quant <= 0) item.quant = 1;
        item.scale = wavelet->scale[ref.band];
        items.push_back(std::move(item));
    }
    if (items.empty()) {
        report.error = "no highpass bands";
        return report;
    }

    auto sum_cpu = [&](hypercine::EntropyTiming* timing) {
        timing->scan_ms = timing->token_ms = timing->codebook_ms = 0;
        timing->prefix_ms = timing->pack_ms = timing->header_ms = 0;
        for (auto& item : items) {
            item.cpu = hypercine::EncodeHighpassStages(item.samples.data(), item.ref.width, item.ref.height, item.ref.width,
                                                       item.tables, false);
            if (!item.cpu.ok) return false;
            timing->scan_ms += item.cpu.timing.scan_ms;
            timing->token_ms += item.cpu.timing.token_ms;
            timing->codebook_ms += item.cpu.timing.codebook_ms;
            timing->prefix_ms += item.cpu.timing.prefix_ms;
            timing->pack_ms += item.cpu.timing.pack_ms;
            std::vector<uint8_t> payload;
            int bits_free = 0;
            const double t0 = NowMs();
            const bool built = hypercine::BuildHighpassPayload(enc, item.ref.band, item.ref.width, item.ref.height, item.ref.subband,
                                                               item.quant, item.scale, item.tables, item.cpu.coeff_bytes, &payload, &bits_free);
            timing->header_ms += NowMs() - t0;
            if (!built) return false;
        }
        timing->total_ms = timing->scan_ms + timing->token_ms + timing->codebook_ms + timing->prefix_ms + timing->pack_ms +
                           timing->header_ms;
        return true;
    };

    if (std::strcmp(mode, "CPU_ENTROPY") == 0) {
        const double t0 = NowMs();
        report.match = sum_cpu(&report.timing);
        report.wall_ms = NowMs() - t0;
        if (!report.match) report.error = "cpu stage failed";
        return report;
    }
    if (!sum_cpu(&report.timing)) {
        report.error = "cpu reference failed";
        return report;
    }
    report.timing = {};

    size_t coeff_bytes = 0;
    for (const auto& item : items) coeff_bytes += item.samples.size() * sizeof(int16_t);
    int16_t* device = nullptr;
    if (cudaMalloc(&device, coeff_bytes) != cudaSuccess) {
        report.error = "device coefficient buffer failed";
        return report;
    }
    std::vector<hypercine::GpuBandRequest> requests(items.size());
    std::vector<hypercine::EntropyTables> tables(items.size());
    size_t cursor = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        int16_t* dest = device + cursor;
        cudaMemcpy(dest, items[i].samples.data(), items[i].samples.size() * sizeof(int16_t), cudaMemcpyHostToDevice);
        requests[i] = {dest, items[i].ref.width, items[i].ref.height, items[i].ref.width,
                       items[i].ref.channel, items[i].ref.level, items[i].ref.band, items[i].ref.subband};
        tables[i] = items[i].tables;
        cursor += items[i].samples.size();
    }
    hypercine::GpuEntropyStop stop = hypercine::GpuEntropyStop::Full;
    if (std::strcmp(mode, "GPU_TOKENIZE_CPU_PACK") == 0) stop = hypercine::GpuEntropyStop::Tokens;
    if (std::strcmp(mode, "GPU_TOKENIZE_CODEBOOK_CPU_PACK") == 0) stop = hypercine::GpuEntropyStop::Codewords;
    auto* session = hypercine::CreateGpuEntropySession();
    std::vector<hypercine::GpuBandResult> results(items.size());
    hypercine::EntropyTiming wall;
    const bool batch = session && hypercine::EncodeHighpassBatch(session, requests.data(), results.data(), static_cast<int>(items.size()),
                                                                 tables.data(), hypercine::GpuEntropyDispatch::OneStream, 1, &wall, stop);
    hypercine::DestroyGpuEntropySession(session);
    cudaFree(device);
    if (!batch) {
        report.error = "gpu batch failed";
        return report;
    }
    report.match = true;
    double header = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!results[i].ok || results[i].coeff_payload != items[i].cpu.coeff_bytes) {
            report.match = false;
            report.error = "band payload mismatch";
        }
        std::vector<uint8_t> payload;
        int bits_free = 0;
        const double t0 = NowMs();
        hypercine::BuildHighpassPayload(enc, items[i].ref.band, items[i].ref.width, items[i].ref.height, items[i].ref.subband,
                                        items[i].quant, items[i].scale, tables[i], results[i].coeff_payload, &payload, &bits_free);
        header += NowMs() - t0;
    }
    report.timing.scan_ms = wall.scan_ms;
    report.timing.token_ms = wall.token_ms;
    report.timing.codebook_ms = wall.codebook_ms;
    report.timing.prefix_ms = wall.prefix_ms;
    report.timing.pack_ms = wall.pack_ms;
    report.timing.header_ms = header;
    report.wall_ms = wall.total_ms + header;
    report.timing.total_ms = report.wall_ms;
    return report;
}

void WriteTiming(std::ostream& json, const hypercine::EntropyTiming& timing) {
    json << "\"scan_ms\": " << timing.scan_ms
         << ", \"token_ms\": " << timing.token_ms
         << ", \"codebook_ms\": " << timing.codebook_ms
         << ", \"prefix_ms\": " << timing.prefix_ms
         << ", \"pack_ms\": " << timing.pack_ms
         << ", \"header_ms\": " << timing.header_ms
         << ", \"lowpass_ms\": " << timing.lowpass_ms
         << ", \"total_ms\": " << timing.total_ms;
}

void WritePhase2B(std::ostream& json, const hypercine::Phase2BRun& run, int gpu_util) {
    json << "\"success\": " << (run.success ? "true" : "false")
         << ", \"byte_match\": " << (run.byte_match ? "true" : "false")
         << ", \"verified\": " << (run.verified ? "true" : "false")
         << ", \"coeff_mismatches\": " << run.coeff_mismatches
         << ", \"coeff_compared\": " << run.coeff_compared
         << ", \"frames\": " << run.frames
         << ", \"streams\": " << run.streams
         << ", \"dispatch\": \"" << run.dispatch_name << "\""
         << ", \"input\": \"" << run.input_name << "\""
         << ", \"sustained_fps\": " << run.sustained_fps
         << ", \"mean_latency_ms\": " << run.mean_latency_ms
         << ", \"mean_color_ms\": " << run.mean_color_ms
         << ", \"mean_h2d_ms\": " << run.mean_h2d_ms
         << ", \"mean_gpu_ms\": " << run.mean_gpu_ms
         << ", \"mean_entropy_ms\": " << run.mean_entropy_ms
         << ", \"mean_assembly_ms\": " << run.mean_assembly_ms
         << ", \"cpu_cores\": " << run.cpu_cores
         << ", \"gpu_util_peak\": " << gpu_util
         << ", \"scan_occupancy_blocks\": " << run.scan_occupancy_blocks
         << ", \"h2d_bytes\": " << run.h2d_bytes
         << ", \"d2h_compressed_bytes\": " << run.d2h_compressed_bytes
         << ", \"d2h_lowpass_bytes\": " << run.d2h_lowpass_bytes
         << ", \"d2h_bytes\": " << run.d2h_bytes
         << ", \"encoded_bytes\": " << run.encoded_bytes
         << ", \"bits_per_coeff\": " << run.bits_per_coeff
         << ", ";
    WriteTiming(json, run.entropy);
    json << ", \"error\": \"" << JsonEscape(run.error) << "\"";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1]
                                        : "benchmarks/results/phase2b.json";
    std::ostringstream json;
    HyperCineDeviceLink link{};
    HyperCine_QueryDeviceLink(&link);
    int sm_count = 0;
    int threads_per_sm = 0;
    cudaDeviceGetAttribute(&sm_count, cudaDevAttrMultiProcessorCount, 0);
    cudaDeviceGetAttribute(&threads_per_sm, cudaDevAttrMaxThreadsPerMultiProcessor, 0);
    const int occupancy_blocks = hypercine::GpuEntropyScanOccupancy();
    const double occupancy = threads_per_sm > 0 ? (100.0 * occupancy_blocks * 256.0) / threads_per_sm : 0;
    std::cout << "GPU " << link.name << " sms " << sm_count << " scan occupancy blocks/sm " << occupancy_blocks
              << " (" << occupancy << "%)\n";
    json << "{\n  \"system\": {\"gpu\": \"" << JsonEscape(link.name) << "\", \"sms\": " << sm_count
         << ", \"scan_occupancy_blocks_per_sm\": " << occupancy_blocks
         << ", \"scan_occupancy_pct\": " << occupancy
         << ", \"async_engines\": " << link.async_engines
         << ", \"upstream_threaded\": " << _THREADED_ENCODER << "},\n";

    json << "  \"upstream_default\": [\n";
    bool first = true;
    for (const auto& res : hypercine::kResolutions) {
        const int frames = res.height >= 4000 ? 2 : hypercine::kCanonicalTimedFrames;
        auto picture = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        Opened opened = OpenAt(res.width, res.height);
        if (!opened.ok) return 1;
        const double ms = TimeUpstream(opened.ref, picture, res.width * 4, hypercine::kCanonicalWarmupFrames, frames);
        std::cout << "UPSTREAM_DEFAULT " << res.name << " " << ms << " ms fps " << (1000.0 / ms) << "\n";
        if (!first) json << ",\n";
        first = false;
        json << "    {\"resolution\": \"" << res.name << "\", \"ms_per_frame\": " << ms
             << ", \"sustained_fps\": " << (1000.0 / ms) << ", \"frames\": " << frames << "}";
        CFHD_CloseEncoder(opened.ref);
    }
    json << "\n  ],\n  \"stages\": [\n";
    first = true;
    bool stages_ok = true;
    const char* modes[] = {"CPU_ENTROPY", "GPU_TOKENIZE_CPU_PACK", "GPU_TOKENIZE_CODEBOOK_CPU_PACK", "GPU_FULL_BAND_ENTROPY"};
    for (const auto& res : hypercine::kResolutions) {
        auto picture = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        Opened opened = OpenAt(res.width, res.height);
        if (!opened.ok) return 1;
        if (CFHD_EncodeSample(opened.ref, picture.data(), res.width * 4) != CFHD_ERROR_OKAY) return 1;
        auto* enc = reinterpret_cast<encoder*>(opened.sample->m_encoder);
        auto* tfs = reinterpret_cast<transform**>(opened.sample->m_transformArray);
        for (const char* mode : modes) {
            auto report = MeasureStages(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                        opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, mode);
            std::cout << mode << " " << res.name << " wall " << report.wall_ms
                      << " scan " << report.timing.scan_ms << " token " << report.timing.token_ms
                      << " book " << report.timing.codebook_ms << " prefix " << report.timing.prefix_ms
                      << " pack " << report.timing.pack_ms << " header " << report.timing.header_ms
                      << " match " << report.match << " " << report.error << "\n";
            if (!report.match) stages_ok = false;
            if (!first) json << ",\n";
            first = false;
            json << "    {\"resolution\": \"" << res.name << "\", \"path\": \"" << mode
                 << "\", \"match\": " << (report.match ? "true" : "false")
                 << ", \"wall_ms\": " << report.wall_ms << ", ";
            WriteTiming(json, report.timing);
            json << ", \"error\": \"" << JsonEscape(report.error) << "\"}";
        }
        CFHD_CloseEncoder(opened.ref);
    }
    json << "\n  ],\n  \"encoders\": [\n";
    first = true;
    bool encoders_ok = true;
    for (const auto& res : hypercine::kResolutions) {
        const int frames = FramesFor(res.height);
        auto picture = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        Opened opened = OpenAt(res.width, res.height);
        if (!opened.ok) return 1;
        if (CFHD_EncodeSample(opened.ref, picture.data(), res.width * 4) != CFHD_ERROR_OKAY) return 1;
        auto* enc = reinterpret_cast<encoder*>(opened.sample->m_encoder);
        auto* tfs = reinterpret_cast<transform**>(opened.sample->m_transformArray);
        auto emit = [&](const char* path_name, const hypercine::Phase2Run* phase2a, const hypercine::Phase2BRun* phase2b, int gpu_util) {
            if (!first) json << ",\n";
            first = false;
            json << "    {\"resolution\": \"" << res.name << "\", \"path\": \"" << path_name << "\", ";
            if (phase2a) {
                json << "\"success\": " << (phase2a->success ? "true" : "false")
                     << ", \"byte_match\": " << (phase2a->byte_match ? "true" : "false")
                     << ", \"sustained_fps\": " << phase2a->sustained_fps
                     << ", \"mean_latency_ms\": " << phase2a->mean_latency_ms
                     << ", \"mean_entropy_ms\": " << phase2a->mean_entropy_ms
                     << ", \"mean_assembly_ms\": " << phase2a->mean_assembly_ms
                     << ", \"mean_gpu_ms\": " << phase2a->mean_gpu_ms
                     << ", \"mean_h2d_ms\": " << phase2a->mean_h2d_ms
                     << ", \"cpu_cores\": " << phase2a->cpu_cores
                     << ", \"h2d_bytes\": " << phase2a->h2d_bytes
                     << ", \"d2h_bytes\": " << phase2a->d2h_bytes
                     << ", \"encoded_bytes\": " << phase2a->encoded_bytes
                     << ", \"error\": \"" << JsonEscape(phase2a->error) << "\"}";
                std::cout << path_name << " " << res.name << " fps " << phase2a->sustained_fps
                          << " latency " << phase2a->mean_latency_ms << " entropy " << phase2a->mean_entropy_ms
                          << " asm " << phase2a->mean_assembly_ms << " d2h " << phase2a->d2h_bytes
                          << " " << phase2a->error << "\n";
                if (!phase2a->success || (phase2a->verified && !phase2a->byte_match)) encoders_ok = false;
            } else if (phase2b) {
                WritePhase2B(json, *phase2b, gpu_util);
                json << "}";
                std::cout << path_name << " " << res.name << " fps " << phase2b->sustained_fps
                          << " latency " << phase2b->mean_latency_ms
                          << " color " << phase2b->mean_color_ms
                          << " h2d_ms " << phase2b->mean_h2d_ms
                          << " gpu " << phase2b->mean_gpu_ms
                          << " entropy " << phase2b->mean_entropy_ms
                          << " asm " << phase2b->mean_assembly_ms
                          << " h2d " << phase2b->h2d_bytes
                          << " d2h_compressed " << phase2b->d2h_compressed_bytes
                          << " d2h_lowpass " << phase2b->d2h_lowpass_bytes
                          << " encoded " << phase2b->encoded_bytes
                          << " bits " << phase2b->bits_per_coeff
                          << " cpu " << phase2b->cpu_cores
                          << " scan " << phase2b->entropy.scan_ms
                          << " token " << phase2b->entropy.token_ms
                          << " book " << phase2b->entropy.codebook_ms
                          << " prefix " << phase2b->entropy.prefix_ms
                          << " pack " << phase2b->entropy.pack_ms
                          << " header " << phase2b->entropy.header_ms
                          << " lowpass " << phase2b->entropy.lowpass_ms
                          << " gpu_util " << gpu_util
                          << " match " << phase2b->byte_match << " " << phase2b->error << "\n";
                if (!phase2b->success || !phase2b->byte_match) encoders_ok = false;
            }
        };

        hypercine::Phase2Config a;
        a.input = hypercine::Phase2Input::HostBgra;
        a.transport = hypercine::Phase2Transport::Int16Stream;
        a.workers = 16;
        a.warmup = 1;
        a.frames = frames;
        a.pipeline_depth = 2;
        a.verify = true;
        auto int16 = hypercine::RunPhase2(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                          opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, a);
        emit("PHASE2A_INT16_STREAM", &int16, nullptr, -1);

        a.input = hypercine::Phase2Input::DeviceBgra;
        a.transport = hypercine::Phase2Transport::CompactStream;
        auto resident = hypercine::RunPhase2(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                             opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, a);
        emit("PHASE2A_DEVICE_RESIDENT", &resident, nullptr, -1);

        const hypercine::GpuEntropyDispatch dispatches[] = {
            hypercine::GpuEntropyDispatch::OneStream,
            hypercine::GpuEntropyDispatch::MultiStream,
            hypercine::GpuEntropyDispatch::MultiStream,
            hypercine::GpuEntropyDispatch::MultiStream,
            hypercine::GpuEntropyDispatch::Batched
        };
        const int stream_counts[] = {1, 2, 4, 8, 4};
        const char* names[] = {
            "PHASE2B_HOST_GPU_ENTROPY",
            "PHASE2B_HOST_STREAMS_2",
            "PHASE2B_HOST_STREAMS_4",
            "PHASE2B_HOST_STREAMS_8",
            "PHASE2B_HOST_BATCHED_4"
        };
        for (int i = 0; i < 5; ++i) {
            hypercine::Phase2BConfig cfg;
            cfg.input = hypercine::Phase2Input::HostBgra;
            cfg.frames = frames;
            cfg.warmup = 1;
            cfg.verify = true;
            cfg.dispatch = dispatches[i];
            cfg.streams = stream_counts[i];
            UtilProbe probe;
            probe.Start();
            auto run = hypercine::RunPhase2B(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                             opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, cfg);
            emit(names[i], nullptr, &run, probe.Stop());
        }
        hypercine::Phase2BConfig device;
        device.input = hypercine::Phase2Input::DeviceBgra;
        device.frames = frames;
        device.warmup = 1;
        device.verify = true;
        device.dispatch = hypercine::GpuEntropyDispatch::OneStream;
        device.streams = 1;
        UtilProbe probe;
        probe.Start();
        auto device_run = hypercine::RunPhase2B(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                                opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, device);
        const int device_util = probe.Stop();
        emit("PHASE2B_DEVICE_GPU_ENTROPY", nullptr, &device_run, device_util);
        device.dispatch = hypercine::GpuEntropyDispatch::MultiStream;
        device.streams = 4;
        auto device_streams = hypercine::RunPhase2B(enc, tfs, opened.sample->m_channelCount, picture.data(), res.width * 4,
                                                    opened.sample->m_scratchBuffer, opened.sample->m_scratchBufferSize, device);
        emit("PHASE2B_DEVICE_STREAMS_4", nullptr, &device_streams, -1);
        CFHD_CloseEncoder(opened.ref);
    }
    json << "\n  ]\n}\n";
    const std::filesystem::path out_path(path);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "failed to write " << out_path.string() << "\n";
        return 1;
    }
    const std::string body = json.str();
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.close();
    std::cout << "wrote " << std::filesystem::absolute(out_path).string()
              << " bytes " << body.size()
              << " stages_ok " << stages_ok << " encoders_ok " << encoders_ok << "\n";
    return (stages_ok && encoders_ok) ? 0 : 1;
}
