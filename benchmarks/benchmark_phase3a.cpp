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
#include "hypercine/phase3a.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

void Stats(std::vector<double> vals, double* mean, double* median, double* p95) {
    if (vals.empty()) return;
    double sum = 0.0;
    for (double v : vals) sum += v;
    *mean = sum / static_cast<double>(vals.size());
    std::sort(vals.begin(), vals.end());
    const size_t n = vals.size();
    *median = (n & 1u) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
    size_t idx = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(n))) - 1;
    if (idx >= n) idx = n - 1;
    *p95 = vals[idx];
}

struct UpstreamSequenceBenchResult {
    double fps = 0.0;
    double mpix_per_s = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    size_t last_bitstream_bytes = 0;
    int last_fs_ratelimiter = 0;
};

// Benchmark pristine upstream CFHD_EncodeSample on the same changing-complexity multi-frame sequence.
UpstreamSequenceBenchResult BenchmarkUpstreamSequence(int width, int height, int warmup, int frames) {
    UpstreamSequenceBenchResult res{};
    const int pitch = width * 4;
    const size_t bytes = static_cast<size_t>(pitch) * static_cast<size_t>(height);
    const int pool_len = std::min(std::max(frames, 8), 16);
    std::vector<std::vector<uint8_t>> pool(static_cast<size_t>(pool_len));
    for (int i = 0; i < pool_len; ++i) {
        pool[static_cast<size_t>(i)].resize(bytes);
        const auto pat = hypercine::SelectSequencePattern(i);
        hypercine::GeneratePhase3AFrame(width, height, i, pat, pool[static_cast<size_t>(i)].data(), pitch);
    }

    CFHD_EncoderRef ref = nullptr;
    if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) return res;
    if (CFHD_PrepareToEncode(ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(ref);
        return res;
    }
    CSampleEncoder* sample = reinterpret_cast<CSampleEncoder*>(ref);
    ENCODER* enc = reinterpret_cast<ENCODER*>(sample->m_encoder);

    for (int i = 0; i < warmup; ++i) {
        const auto& f = pool[static_cast<size_t>(i % pool_len)];
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(f.data()), pitch);
    }

    if (enc) {
        enc->lastgopbitcount = 0;
        enc->gop_length = 1;
        enc->q.FSratelimiter = 4;
    }

    std::vector<double> times;
    times.reserve(static_cast<size_t>(frames));
    const double t_start = NowMs();
    for (int i = 0; i < frames; ++i) {
        const auto& f = pool[static_cast<size_t>(i % pool_len)];
        const double t0 = NowMs();
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(f.data()), pitch);
        void* sptr = nullptr;
        size_t ssize = 0;
        CFHD_GetSampleData(ref, &sptr, &ssize);
        const double t1 = NowMs();
        times.push_back(t1 - t0);
        res.last_bitstream_bytes = ssize;
        if (enc) res.last_fs_ratelimiter = enc->q.FSratelimiter;
    }
    const double wall_ms = NowMs() - t_start;
    Stats(times, &res.mean_ms, &res.median_ms, &res.p95_ms);
    if (wall_ms > 0.0) {
        res.fps = static_cast<double>(frames) * 1000.0 / wall_ms;
        res.mpix_per_s = (static_cast<double>(width) * static_cast<double>(height) / 1e6) * res.fps;
    }
    CFHD_CloseEncoder(ref);
    return res;
}

void WritePhase3AResultJson(std::ostringstream& json, const hypercine::Phase3AResult& r,
                            const std::string& indent) {
    const double mpix_s = (static_cast<double>(r.config.width) * static_cast<double>(r.config.height) / 1e6) * r.sustained_fps;
    json << indent << "{\n"
         << indent << "  \"input_mode\": \"" << hypercine::Phase3AInputModeName(r.config.input_mode) << "\",\n"
         << indent << "  \"quant_mode\": \"" << hypercine::Phase3AQuantModeName(r.config.quant_mode) << "\",\n"
         << indent << "  \"slot_count\": " << r.slot_selection.selected_slots << ",\n"
         << indent << "  \"use_cuda_graph\": " << (r.graph_used ? "true" : "false") << ",\n"
         << indent << "  \"submitted_frames\": " << r.submitted_frames << ",\n"
         << indent << "  \"completed_frames\": " << r.completed_frames << ",\n"
         << indent << "  \"sustained_fps\": " << r.sustained_fps << ",\n"
         << indent << "  \"mpix_per_s\": " << mpix_s << ",\n"
         << indent << "  \"frame_period_ms\": " << r.frame_period_ms << ",\n"
         << indent << "  \"first_quarter_fps\": " << r.first_quarter_fps << ",\n"
         << indent << "  \"last_quarter_fps\": " << r.last_quarter_fps << ",\n"
         << indent << "  \"mean_latency_ms\": " << r.mean_latency_ms << ",\n"
         << indent << "  \"median_latency_ms\": " << r.median_latency_ms << ",\n"
         << indent << "  \"p95_latency_ms\": " << r.p95_latency_ms << ",\n"
         << indent << "  \"min_latency_ms\": " << r.min_latency_ms << ",\n"
         << indent << "  \"max_latency_ms\": " << r.max_latency_ms << ",\n"
         << indent << "  \"mean_period_ms\": " << r.mean_period_ms << ",\n"
         << indent << "  \"median_period_ms\": " << r.median_period_ms << ",\n"
         << indent << "  \"p95_period_ms\": " << r.p95_period_ms << ",\n"
         << indent << "  \"stage_breakdown_ms\": {\n"
         << indent << "    \"host_pageable_stage_ms\": " << r.host_pageable_stage_ms << ",\n"
         << indent << "    \"h2d_ms\": " << r.h2d_ms << ",\n"
         << indent << "    \"color_ms\": " << r.color_ms << ",\n"
         << indent << "    \"wavelet_ms\": " << r.wavelet_ms << ",\n"
         << indent << "    \"gpu_frontend_wavelet_ms\": " << r.gpu_frontend_wavelet_ms << ",\n"
         << indent << "    \"gpu_entropy_ms\": " << r.gpu_entropy_ms << ",\n"
         << indent << "    \"gpu_consolidate_ms\": " << r.gpu_consolidate_ms << ",\n"
         << indent << "    \"compressed_d2h_ms\": " << r.compressed_d2h_ms << ",\n"
         << indent << "    \"lowpass_d2h_ms\": " << r.lowpass_d2h_ms << ",\n"
         << indent << "    \"total_d2h_ms\": " << r.total_d2h_ms << ",\n"
         << indent << "    \"total_gpu_ms\": " << r.total_gpu_ms << ",\n"
         << indent << "    \"host_enqueue_ms\": " << r.host_enqueue_ms << ",\n"
         << indent << "    \"cpu_lowpass_ms\": " << r.cpu_lowpass_ms << ",\n"
         << indent << "    \"cpu_header_wrap_ms\": " << r.cpu_header_wrap_ms << ",\n"
         << indent << "    \"cpu_sample_concat_ms\": " << r.cpu_sample_concat_ms << ",\n"
         << indent << "    \"cpu_assembly_total_ms\": " << r.cpu_assembly_total_ms << ",\n"
         << indent << "    \"cpu_dispatch_poll_ms\": " << r.cpu_dispatch_poll_ms << ",\n"
         << indent << "    \"total_cpu_ms\": " << r.total_cpu_ms << "\n"
         << indent << "  },\n"
         << indent << "  \"pipeline_gpu_stage_occupancy_estimate_pct\": " << r.pipeline_gpu_stage_occupancy_estimate_pct << ",\n"
         << indent << "  \"nvml_telemetry\": {\n"
         << indent << "    \"available\": " << (r.nvml.nvml_available ? "true" : "false") << ",\n"
         << indent << "    \"sample_count\": " << r.nvml.sample_count << ",\n"
         << indent << "    \"hardware_gpu_utilization_pct_mean\": " << r.nvml.hardware_gpu_utilization_pct_mean << ",\n"
         << indent << "    \"hardware_gpu_utilization_pct_max\": " << r.nvml.hardware_gpu_utilization_pct_max << ",\n"
         << indent << "    \"hardware_mem_utilization_pct_mean\": " << r.nvml.hardware_mem_utilization_pct_mean << ",\n"
         << indent << "    \"hardware_mem_utilization_pct_max\": " << r.nvml.hardware_mem_utilization_pct_max << ",\n"
         << indent << "    \"graphics_clock_mhz_mean\": " << r.nvml.graphics_clock_mhz_mean << ",\n"
         << indent << "    \"graphics_clock_mhz_min\": " << r.nvml.graphics_clock_mhz_min << ",\n"
         << indent << "    \"graphics_clock_mhz_max\": " << r.nvml.graphics_clock_mhz_max << ",\n"
         << indent << "    \"mem_clock_mhz_mean\": " << r.nvml.mem_clock_mhz_mean << ",\n"
         << indent << "    \"temperature_c_start\": " << r.nvml.temperature_c_start << ",\n"
         << indent << "    \"temperature_c_end\": " << r.nvml.temperature_c_end << ",\n"
         << indent << "    \"temperature_c_max\": " << r.nvml.temperature_c_max << ",\n"
         << indent << "    \"power_w_mean\": " << r.nvml.power_w_mean << ",\n"
         << indent << "    \"power_w_max\": " << r.nvml.power_w_max << ",\n"
         << indent << "    \"throttle_reasons_bitmask\": " << r.nvml.throttle_reasons_bitmask << ",\n"
         << indent << "    \"throttle_reasons_str\": \"" << JsonEscape(r.nvml.throttle_reasons_str) << "\"\n"
         << indent << "  },\n"
         << indent << "  \"cpu_metrics\": {\n"
         << indent << "    \"main_thread_cpu_ms\": " << r.cpu.main_thread_cpu_ms << ",\n"
         << indent << "    \"worker_thread_cpu_ms\": " << r.cpu.worker_thread_cpu_ms << ",\n"
         << indent << "    \"process_cpu_ms\": " << r.cpu.process_cpu_ms << ",\n"
         << indent << "    \"single_core_equiv_pct\": " << r.cpu.single_core_equiv_pct << ",\n"
         << indent << "    \"normalized_process_cpu_pct\": " << r.cpu.normalized_process_cpu_pct << ",\n"
         << indent << "    \"cpu_core_sec_per_wall_sec\": " << r.cpu.cpu_core_sec_per_wall_sec << "\n"
         << indent << "  },\n"
         << indent << "  \"memory_bytes\": {\n"
         << indent << "    \"vram_before_bytes\": " << r.vram_before_bytes << ",\n"
         << indent << "    \"vram_after_init_bytes\": " << r.vram_after_init_bytes << ",\n"
         << indent << "    \"vram_mid_run_bytes\": " << r.vram_mid_run_bytes << ",\n"
         << indent << "    \"vram_end_run_bytes\": " << r.vram_end_run_bytes << ",\n"
         << indent << "    \"vram_after_destroy_bytes\": " << r.vram_after_destroy_bytes << ",\n"
         << indent << "    \"vram_used_bytes\": " << r.vram_used_bytes << ",\n"
         << indent << "    \"slot_vram_bytes\": " << r.slot_vram_bytes << ",\n"
         << indent << "    \"slot_pinned_host_bytes\": " << r.slot_pinned_host_bytes << ",\n"
         << indent << "    \"host_rss_before_bytes\": " << r.host_rss_before_bytes << ",\n"
         << indent << "    \"host_rss_mid_bytes\": " << r.host_rss_mid_bytes << ",\n"
         << indent << "    \"host_rss_end_bytes\": " << r.host_rss_end_bytes << ",\n"
         << indent << "    \"host_rss_after_destroy_bytes\": " << r.host_rss_after_destroy_bytes << "\n"
         << indent << "  },\n"
         << indent << "  \"sequence_verification\": {\n"
         << indent << "    \"sequence_order_ok\": " << (r.sequence_order_ok ? "true" : "false") << ",\n"
         << indent << "    \"decode_ok\": " << (r.decode_ok ? "true" : "false") << ",\n"
         << indent << "    \"stamp_verify_ok\": " << (r.stamp_verify_ok ? "true" : "false") << ",\n"
         << indent << "    \"min_psnr_db\": " << r.min_psnr_db << ",\n"
         << indent << "    \"mean_psnr_db\": " << r.mean_psnr_db << ",\n"
         << indent << "    \"distinct_fs_rate_limiters\": " << r.distinct_fs_rate_limiters << ",\n"
         << indent << "    \"mean_bitstream_bytes\": " << r.mean_bitstream_bytes << ",\n"
         << indent << "    \"min_bitstream_bytes\": " << r.min_bitstream_bytes << ",\n"
         << indent << "    \"max_bitstream_bytes\": " << r.max_bitstream_bytes << "\n"
         << indent << "  }\n"
         << indent << "}";
}

}  // namespace

int main() {
    std::cout << "================================================================================\n";
    std::cout << "HyperCine Phase 3A: Sequence Correctness, Reproducibility & Benchmark Hardening\n";
    std::cout << "================================================================================\n";

    // 1. 120-Frame Sequence Equivalence at 1080p (plus 24-frame at 4K and 8-frame at 8K)
    std::cout << "\n[1/7] Running 120-Frame Changing-Complexity Sequence Equivalence (A vs B vs C)..." << std::endl;
    const auto seq1080 = hypercine::VerifyPhase3ASequence120(1920, 1080, 120);
    const auto seq4k = hypercine::VerifyPhase3ASequence120(3840, 2160, 24);
    const auto seq8k = hypercine::VerifyPhase3ASequence120(7680, 4320, 8);
    std::cout << "  1080p (120f): ok=" << seq1080.ok
              << " matched=" << seq1080.matched_frames << "/" << seq1080.total_frames
              << " distinct_FSratelimiters=" << seq1080.distinct_fs_rate_limiters_observed
              << " quant_transitions=" << seq1080.quant_transitions_observed << "\n";
    std::cout << "  4K    (24f):  ok=" << seq4k.ok
              << " matched=" << seq4k.matched_frames << "/" << seq4k.total_frames
              << " distinct_FSratelimiters=" << seq4k.distinct_fs_rate_limiters_observed
              << " quant_transitions=" << seq4k.quant_transitions_observed << "\n";
    std::cout << "  8K    (8f):   ok=" << seq8k.ok
              << " matched=" << seq8k.matched_frames << "/" << seq8k.total_frames
              << " distinct_FSratelimiters=" << seq8k.distinct_fs_rate_limiters_observed
              << " quant_transitions=" << seq8k.quant_transitions_observed << std::endl;

    // 2. 300-Frame Distinct-Frame Multi-Slot Verification
    std::cout << "\n[2/7] Running 300-Frame Distinct-Frame Multi-Slot Verification..." << std::endl;
    const auto dist1080 = hypercine::VerifyPhase3ADistinctFrames300(
        1920, 1080, 300, hypercine::Phase3AInputMode::HostPageableEndToEnd,
        hypercine::Phase2DSlotPolicy::DefaultAuto);
    const auto dist4k = hypercine::VerifyPhase3ADistinctFrames300(
        3840, 2160, 48, hypercine::Phase3AInputMode::HostPageableEndToEnd,
        hypercine::Phase2DSlotPolicy::DefaultAuto);
    const auto dist8k = hypercine::VerifyPhase3ADistinctFrames300(
        7680, 4320, 16, hypercine::Phase3AInputMode::HostPageableEndToEnd,
        hypercine::Phase2DSlotPolicy::DefaultAuto);
    std::cout << "  1080p (300f): ok=" << dist1080.ok
              << " slots=" << dist1080.num_slots
              << " stamps_matched=" << dist1080.decoded_stamp_matched_count << "/" << dist1080.decoded_checked_count
              << " min_psnr=" << dist1080.min_psnr_db << " dB\n";
    std::cout << "  4K    (48f):  ok=" << dist4k.ok
              << " slots=" << dist4k.num_slots
              << " stamps_matched=" << dist4k.decoded_stamp_matched_count << "/" << dist4k.decoded_checked_count
              << " min_psnr=" << dist4k.min_psnr_db << " dB\n";
    std::cout << "  8K    (16f):  ok=" << dist8k.ok
              << " slots=" << dist8k.num_slots
              << " stamps_matched=" << dist8k.decoded_stamp_matched_count << "/" << dist8k.decoded_checked_count
              << " min_psnr=" << dist8k.min_psnr_db << " dB" << std::endl;

    // 3. Adversarial Entropy Suite (17 pathological cases)
    std::cout << "\n[3/7] Running 17-Case Adversarial Entropy Verification..." << std::endl;
    const auto adv = hypercine::VerifyPhase3AAdversarialEntropy();
    std::cout << "  Adversarial cases passed: " << adv.total_cases_passed << "/" << adv.total_cases_run
              << " (ok=" << adv.ok << ")" << std::endl;

    // 4. Multi-Frame Production Metadata Verification
    std::cout << "\n[4/7] Running 16-Frame Production Metadata Verification..." << std::endl;
    const auto meta = hypercine::VerifyPhase3AMetadataSequence(1920, 1080, 16);
    std::cout << "  Metadata ok=" << meta.ok
              << " clip1_guid=" << meta.clip1_guid_hex
              << " clip2_guid=" << meta.clip2_guid_hex
              << " date=" << (meta.clip1_frames.empty() ? "" : meta.clip1_frames.front().encode_date)
              << " " << (meta.clip1_frames.empty() ? "" : meta.clip1_frames.front().encode_time)
              << " tc=" << (meta.clip1_frames.empty() ? "" : meta.clip1_frames.front().timecode)
              << ".." << (meta.clip1_frames.empty() ? "" : meta.clip1_frames.back().timecode) << std::endl;

    // 5. Input-Mode Benchmark Matrix (HostPageableEndToEnd, HostPinnedApi, DeviceInput) + Upstream Sequence
    std::cout << "\n[5/7] Running Input-Mode Benchmark Matrix (1080p, 4K, 8K) with NVML Telemetry..." << std::endl;
    struct ResBenchBundle {
        hypercine::Resolution res;
        int warmup = 0;
        int frames = 0;
        UpstreamSequenceBenchResult upstream;
        hypercine::Phase3AResult pageable;
        hypercine::Phase3AResult pinned;
        hypercine::Phase3AResult device_in;
        hypercine::Phase3ACudaGraphABReport graph_ab;
    };
    std::vector<ResBenchBundle> res_bundles;
    for (const auto& res : hypercine::kResolutions) {
        ResBenchBundle b{};
        b.res = res;
        b.warmup = (res.width == 7680) ? 4 : 6;
        b.frames = (res.width == 1920) ? 60 : ((res.width == 3840) ? 40 : 24);
        const int up_frames = (res.width == 1920) ? 24 : ((res.width == 3840) ? 12 : 6);
        b.upstream = BenchmarkUpstreamSequence(res.width, res.height, 2, up_frames);

        hypercine::Phase3AConfig cfg{};
        cfg.width = res.width;
        cfg.height = res.height;
        cfg.warmup_frames = b.warmup;
        cfg.frames = b.frames;
        cfg.quant_mode = hypercine::Phase3AQuantMode::DynamicUpstreamState;
        cfg.slot_policy = hypercine::Phase2DSlotPolicy::DefaultAuto;
        cfg.use_cuda_graph = true;
        cfg.verify_sequence = false;
        cfg.retain_frame_records = false;

        cfg.input_mode = hypercine::Phase3AInputMode::HostPageableEndToEnd;
        b.pageable = hypercine::RunPhase3A(cfg);

        cfg.input_mode = hypercine::Phase3AInputMode::HostPinnedApi;
        b.pinned = hypercine::RunPhase3A(cfg);

        cfg.input_mode = hypercine::Phase3AInputMode::DeviceInput;
        b.device_in = hypercine::RunPhase3A(cfg);

        b.graph_ab = hypercine::RunPhase3ACudaGraphABStudy(res.width, res.height, b.frames, 5);

        std::cout << "  [" << res.name << " (" << res.width << "x" << res.height << ")]\n"
                  << "    Upstream CPU (Sequence):   " << std::fixed << std::setprecision(2)
                  << b.upstream.fps << " FPS (" << b.upstream.mean_ms << " ms)\n"
                  << "    Mode 1 HostPageableEndToEnd: " << b.pageable.sustained_fps << " FPS ("
                  << b.pageable.frame_period_ms << " ms/frame, lat=" << b.pageable.mean_latency_ms
                  << " ms, p95_lat=" << b.pageable.p95_latency_ms
                  << " ms, staging=" << b.pageable.host_pageable_stage_ms
                  << " ms, h2d=" << b.pageable.h2d_ms << " ms)\n"
                  << "    Mode 2 HostPinnedApi:        " << b.pinned.sustained_fps << " FPS ("
                  << b.pinned.frame_period_ms << " ms/frame, lat=" << b.pinned.mean_latency_ms
                  << " ms, p95_lat=" << b.pinned.p95_latency_ms
                  << " ms, h2d=" << b.pinned.h2d_ms << " ms)\n"
                  << "    Mode 3 DeviceInput:          " << b.device_in.sustained_fps << " FPS ("
                  << b.device_in.frame_period_ms << " ms/frame, lat=" << b.device_in.mean_latency_ms
                  << " ms, p95_lat=" << b.device_in.p95_latency_ms << " ms)\n"
                  << "    NVML Telemetry (Pageable):   gpu_util_avg=" << b.pageable.nvml.hardware_gpu_utilization_pct_mean
                  << "% clks=" << b.pageable.nvml.graphics_clock_mhz_mean
                  << " MHz [" << b.pageable.nvml.graphics_clock_mhz_min << ".."
                  << b.pageable.nvml.graphics_clock_mhz_max << "] temp_max="
                  << b.pageable.nvml.temperature_c_max << " C power_avg="
                  << b.pageable.nvml.power_w_mean << " W throttle="
                  << b.pageable.nvml.throttle_reasons_str << "\n"
                  << "    CUDA Graph A/B (5 trials):   stream=" << b.graph_ab.graph_off_mean_fps
                  << " FPS vs graph=" << b.graph_ab.graph_on_mean_fps
                  << " FPS (delta=" << b.graph_ab.fps_delta_pct
                  << "%, byte_identical=" << b.graph_ab.all_frames_byte_identical << ")" << std::endl;
        res_bundles.push_back(b);
    }

    // 6. 5-Trial 8K Variance & Thermal/Clock Study
    std::cout << "\n[6/7] Running 5-Trial 8K Variance & Thermal/Clock Study..." << std::endl;
    const auto var8k_pageable = hypercine::RunPhase3AVarianceStudy(
        7680, 4320, hypercine::Phase3AInputMode::HostPageableEndToEnd, 5, 24);
    const auto var8k_pinned = hypercine::RunPhase3AVarianceStudy(
        7680, 4320, hypercine::Phase3AInputMode::HostPinnedApi, 5, 24);
    const auto var8k_device = hypercine::RunPhase3AVarianceStudy(
        7680, 4320, hypercine::Phase3AInputMode::DeviceInput, 5, 24);
    std::cout << "  8K HostPageableEndToEnd (5 trials x 24f): mean=" << var8k_pageable.mean_fps
              << " FPS, median=" << var8k_pageable.median_fps
              << " FPS, stddev=" << var8k_pageable.stddev_fps
              << " FPS [" << var8k_pageable.min_fps << ".." << var8k_pageable.max_fps << "]\n";
    std::cout << "  8K HostPinnedApi        (5 trials x 24f): mean=" << var8k_pinned.mean_fps
              << " FPS, median=" << var8k_pinned.median_fps
              << " FPS, stddev=" << var8k_pinned.stddev_fps
              << " FPS [" << var8k_pinned.min_fps << ".." << var8k_pinned.max_fps << "]\n";
    std::cout << "  8K DeviceInput          (5 trials x 24f): mean=" << var8k_device.mean_fps
              << " FPS, median=" << var8k_device.median_fps
              << " FPS, stddev=" << var8k_device.stddev_fps
              << " FPS [" << var8k_device.min_fps << ".." << var8k_device.max_fps << "]" << std::endl;

    // 7. Sustained Long-Run Stability & Memory Leak Gate
    std::cout << "\n[7/7] Running Sustained Long-Run Stability & Memory Leak Gates (1000f@1080p, 300f@4K, 100f@8K)..." << std::endl;
    auto run_stability = [](int w, int h, int frames) {
        hypercine::Phase3AConfig cfg{};
        cfg.width = w;
        cfg.height = h;
        cfg.warmup_frames = 4;
        cfg.frames = frames;
        cfg.input_mode = hypercine::Phase3AInputMode::HostPageableEndToEnd;
        cfg.quant_mode = hypercine::Phase3AQuantMode::DynamicUpstreamState;
        cfg.slot_policy = hypercine::Phase2DSlotPolicy::DefaultAuto;
        cfg.use_cuda_graph = true;
        cfg.verify_sequence = true;
        cfg.retain_frame_records = false;
        return hypercine::RunPhase3A(cfg);
    };
    const auto stab1080 = run_stability(1920, 1080, 1000);
    const auto stab4k = run_stability(3840, 2160, 300);
    const auto stab8k = run_stability(7680, 4320, 100);
    std::cout << "  1080p (1000 frames): ok=" << stab1080.ok
              << " fps=" << stab1080.sustained_fps
              << " first_q_fps=" << stab1080.first_quarter_fps
              << " last_q_fps=" << stab1080.last_quarter_fps
              << " vram_leak_after_destroy="
              << (static_cast<int64_t>(stab1080.vram_after_destroy_bytes) - static_cast<int64_t>(stab1080.vram_before_bytes))
              << " bytes\n";
    std::cout << "  4K    (300 frames):  ok=" << stab4k.ok
              << " fps=" << stab4k.sustained_fps
              << " first_q_fps=" << stab4k.first_quarter_fps
              << " last_q_fps=" << stab4k.last_quarter_fps
              << " vram_leak_after_destroy="
              << (static_cast<int64_t>(stab4k.vram_after_destroy_bytes) - static_cast<int64_t>(stab4k.vram_before_bytes))
              << " bytes\n";
    std::cout << "  8K    (100 frames):  ok=" << stab8k.ok
              << " fps=" << stab8k.sustained_fps
              << " first_q_fps=" << stab8k.first_quarter_fps
              << " last_q_fps=" << stab8k.last_quarter_fps
              << " vram_leak_after_destroy="
              << (static_cast<int64_t>(stab8k.vram_after_destroy_bytes) - static_cast<int64_t>(stab8k.vram_before_bytes))
              << " bytes" << std::endl;

    // Write JSON report
    std::ostringstream json;
    json << std::fixed << std::setprecision(4);
    json << "{\n"
         << "  \"phase\": \"Phase 3A: Sequence Correctness, Reproducibility, and Benchmark Hardening\",\n"
         << "  \"sequence_equivalence\": {\n"
         << "    \"1080p_120f\": {\n"
         << "      \"ok\": " << (seq1080.ok ? "true" : "false") << ",\n"
         << "      \"total_frames\": " << seq1080.total_frames << ",\n"
         << "      \"matched_frames\": " << seq1080.matched_frames << ",\n"
         << "      \"distinct_fs_rate_limiters_observed\": " << seq1080.distinct_fs_rate_limiters_observed << ",\n"
         << "      \"quant_transitions_observed\": " << seq1080.quant_transitions_observed << ",\n"
         << "      \"fs_rate_limiter_trajectory\": [";
    for (size_t i = 0; i < seq1080.fs_rate_limiter_trajectory.size(); ++i) {
        json << seq1080.fs_rate_limiter_trajectory[i]
             << (i + 1 < seq1080.fs_rate_limiter_trajectory.size() ? ", " : "");
    }
    json << "]\n"
         << "    },\n"
         << "    \"4k_24f\": {\n"
         << "      \"ok\": " << (seq4k.ok ? "true" : "false") << ",\n"
         << "      \"total_frames\": " << seq4k.total_frames << ",\n"
         << "      \"matched_frames\": " << seq4k.matched_frames << ",\n"
         << "      \"distinct_fs_rate_limiters_observed\": " << seq4k.distinct_fs_rate_limiters_observed << ",\n"
         << "      \"quant_transitions_observed\": " << seq4k.quant_transitions_observed << "\n"
         << "    },\n"
         << "    \"8k_8f\": {\n"
         << "      \"ok\": " << (seq8k.ok ? "true" : "false") << ",\n"
         << "      \"total_frames\": " << seq8k.total_frames << ",\n"
         << "      \"matched_frames\": " << seq8k.matched_frames << ",\n"
         << "      \"distinct_fs_rate_limiters_observed\": " << seq8k.distinct_fs_rate_limiters_observed << ",\n"
         << "      \"quant_transitions_observed\": " << seq8k.quant_transitions_observed << "\n"
         << "    }\n"
         << "  },\n"
         << "  \"distinct_frames_verification\": {\n"
         << "    \"1080p_300f\": {\n"
         << "      \"ok\": " << (dist1080.ok ? "true" : "false") << ",\n"
         << "      \"num_slots\": " << dist1080.num_slots << ",\n"
         << "      \"submitted_frames\": " << dist1080.submitted_frames << ",\n"
         << "      \"completed_frames\": " << dist1080.completed_frames << ",\n"
         << "      \"strict_monotonic_indices\": " << (dist1080.strict_monotonic_indices ? "true" : "false") << ",\n"
         << "      \"no_duplicates_or_missing\": " << (dist1080.no_duplicates_or_missing ? "true" : "false") << ",\n"
         << "      \"no_stale_slot_reuse\": " << (dist1080.no_stale_slot_reuse ? "true" : "false") << ",\n"
         << "      \"decoded_checked_count\": " << dist1080.decoded_checked_count << ",\n"
         << "      \"decoded_stamp_matched_count\": " << dist1080.decoded_stamp_matched_count << ",\n"
         << "      \"min_psnr_db\": " << dist1080.min_psnr_db << ",\n"
         << "      \"mean_psnr_db\": " << dist1080.mean_psnr_db << "\n"
         << "    },\n"
         << "    \"4k_48f\": {\n"
         << "      \"ok\": " << (dist4k.ok ? "true" : "false") << ",\n"
         << "      \"num_slots\": " << dist4k.num_slots << ",\n"
         << "      \"completed_frames\": " << dist4k.completed_frames << ",\n"
         << "      \"decoded_checked_count\": " << dist4k.decoded_checked_count << ",\n"
         << "      \"decoded_stamp_matched_count\": " << dist4k.decoded_stamp_matched_count << ",\n"
         << "      \"min_psnr_db\": " << dist4k.min_psnr_db << "\n"
         << "    },\n"
         << "    \"8k_16f\": {\n"
         << "      \"ok\": " << (dist8k.ok ? "true" : "false") << ",\n"
         << "      \"num_slots\": " << dist8k.num_slots << ",\n"
         << "      \"completed_frames\": " << dist8k.completed_frames << ",\n"
         << "      \"decoded_checked_count\": " << dist8k.decoded_checked_count << ",\n"
         << "      \"decoded_stamp_matched_count\": " << dist8k.decoded_stamp_matched_count << ",\n"
         << "      \"min_psnr_db\": " << dist8k.min_psnr_db << "\n"
         << "    }\n"
         << "  },\n"
         << "  \"adversarial_entropy\": {\n"
         << "    \"ok\": " << (adv.ok ? "true" : "false") << ",\n"
         << "    \"total_cases_run\": " << adv.total_cases_run << ",\n"
         << "    \"total_cases_passed\": " << adv.total_cases_passed << ",\n"
         << "    \"cases\": [\n";
    for (size_t i = 0; i < adv.results.size(); ++i) {
        const auto& c = adv.results[i];
        json << "      {\n"
             << "        \"name\": \"" << JsonEscape(c.case_name) << "\",\n"
             << "        \"subband\": " << c.subband << ",\n"
             << "        \"width\": " << c.width << ",\n"
             << "        \"height\": " << c.height << ",\n"
             << "        \"pitch_px\": " << c.pitch_px << ",\n"
             << "        \"total_tokens\": " << c.total_tokens << ",\n"
             << "        \"total_bits\": " << c.total_bits << ",\n"
             << "        \"payload_bytes\": " << c.upstream_payload_bytes << ",\n"
             << "        \"sha256\": \"" << c.upstream_sha256 << "\",\n"
             << "        \"all_modes_equal\": "
             << ((c.cpu_stages_match && c.gpu_ref_match && c.gpu_fused_match && c.gpu_scan_opt_match && c.gpu_graph_match) ? "true" : "false") << "\n"
             << "      }" << (i + 1 < adv.results.size() ? "," : "") << "\n";
    }
    json << "    ]\n"
         << "  },\n"
         << "  \"metadata_sequence\": {\n"
         << "    \"ok\": " << (meta.ok ? "true" : "false") << ",\n"
         << "    \"num_frames\": " << meta.num_frames << ",\n"
         << "    \"clip1_guid_non_zero\": " << (meta.clip1_guid_non_zero ? "true" : "false") << ",\n"
         << "    \"clip1_guid_constant_across_frames\": " << (meta.clip1_guid_constant_across_frames ? "true" : "false") << ",\n"
         << "    \"clip2_guid_distinct_from_clip1\": " << (meta.clip2_guid_distinct_from_clip1 ? "true" : "false") << ",\n"
         << "    \"dates_and_times_valid\": " << (meta.dates_and_times_valid ? "true" : "false") << ",\n"
         << "    \"unique_framenum_monotonic_0_to_n\": " << (meta.unique_framenum_monotonic_0_to_n ? "true" : "false") << ",\n"
         << "    \"timecode_progresses\": " << (meta.timecode_progresses ? "true" : "false") << ",\n"
         << "    \"all_frames_match_upstream_masked\": " << (meta.all_frames_match_upstream_masked ? "true" : "false") << ",\n"
         << "    \"clip1_guid\": \"" << meta.clip1_guid_hex << "\",\n"
         << "    \"clip2_guid\": \"" << meta.clip2_guid_hex << "\",\n"
         << "    \"first_encode_date\": \"" << (meta.clip1_frames.empty() ? "" : JsonEscape(meta.clip1_frames.front().encode_date)) << "\",\n"
         << "    \"first_encode_time\": \"" << (meta.clip1_frames.empty() ? "" : JsonEscape(meta.clip1_frames.front().encode_time)) << "\",\n"
         << "    \"first_timecode\": \"" << (meta.clip1_frames.empty() ? "" : JsonEscape(meta.clip1_frames.front().timecode)) << "\",\n"
         << "    \"last_timecode\": \"" << (meta.clip1_frames.empty() ? "" : JsonEscape(meta.clip1_frames.back().timecode)) << "\"\n"
         << "  },\n"
         << "  \"resolutions\": {\n";

    for (size_t i = 0; i < res_bundles.size(); ++i) {
        const auto& b = res_bundles[i];
        json << "    \"" << b.res.name << "\": {\n"
             << "      \"width\": " << b.res.width << ",\n"
             << "      \"height\": " << b.res.height << ",\n"
             << "      \"upstream_cpu_sequence\": {\n"
             << "        \"fps\": " << b.upstream.fps << ",\n"
             << "        \"mpix_per_s\": " << b.upstream.mpix_per_s << ",\n"
             << "        \"mean_frame_ms\": " << b.upstream.mean_ms << ",\n"
             << "        \"median_frame_ms\": " << b.upstream.median_ms << ",\n"
             << "        \"p95_frame_ms\": " << b.upstream.p95_ms << ",\n"
             << "        \"last_bitstream_bytes\": " << b.upstream.last_bitstream_bytes << ",\n"
             << "        \"last_fs_ratelimiter\": " << b.upstream.last_fs_ratelimiter << "\n"
             << "      },\n"
             << "      \"mode1_host_pageable_end_to_end\": \n";
        WritePhase3AResultJson(json, b.pageable, "      ");
        json << ",\n      \"mode2_host_pinned_api\": \n";
        WritePhase3AResultJson(json, b.pinned, "      ");
        json << ",\n      \"mode3_device_input\": \n";
        WritePhase3AResultJson(json, b.device_in, "      ");
        json << ",\n      \"cuda_graph_ab_study\": {\n"
             << "        \"trials\": " << b.graph_ab.trials << ",\n"
             << "        \"frames_per_trial\": " << b.graph_ab.frames_per_trial << ",\n"
             << "        \"distinct_quant_states_exercised\": " << b.graph_ab.distinct_quant_states_exercised << ",\n"
             << "        \"graph_captures_per_run\": " << b.graph_ab.graph_captures_per_run << ",\n"
             << "        \"graph_off_mean_fps\": " << b.graph_ab.graph_off_mean_fps << ",\n"
             << "        \"graph_off_median_fps\": " << b.graph_ab.graph_off_median_fps << ",\n"
             << "        \"graph_off_stddev_fps\": " << b.graph_ab.graph_off_stddev_fps << ",\n"
             << "        \"graph_off_enqueue_ms\": " << b.graph_ab.graph_off_enqueue_ms << ",\n"
             << "        \"graph_off_entropy_ms\": " << b.graph_ab.graph_off_entropy_ms << ",\n"
             << "        \"graph_off_cpu_ms_per_frame\": " << b.graph_ab.graph_off_cpu_ms_per_frame << ",\n"
             << "        \"graph_on_mean_fps\": " << b.graph_ab.graph_on_mean_fps << ",\n"
             << "        \"graph_on_median_fps\": " << b.graph_ab.graph_on_median_fps << ",\n"
             << "        \"graph_on_stddev_fps\": " << b.graph_ab.graph_on_stddev_fps << ",\n"
             << "        \"graph_on_enqueue_ms\": " << b.graph_ab.graph_on_enqueue_ms << ",\n"
             << "        \"graph_on_entropy_ms\": " << b.graph_ab.graph_on_entropy_ms << ",\n"
             << "        \"graph_on_cpu_ms_per_frame\": " << b.graph_ab.graph_on_cpu_ms_per_frame << ",\n"
             << "        \"fps_delta_pct\": " << b.graph_ab.fps_delta_pct << ",\n"
             << "        \"enqueue_reduction_pct\": " << b.graph_ab.enqueue_reduction_pct << ",\n"
             << "        \"all_frames_byte_identical\": " << (b.graph_ab.all_frames_byte_identical ? "true" : "false") << "\n"
             << "      }\n"
             << "    }" << (i + 1 < res_bundles.size() ? "," : "") << "\n";
    }
    json << "  },\n";

    auto write_variance = [&](const char* key, const hypercine::Phase3AVarianceStudySummary& v, bool comma) {
        json << "    \"" << key << "\": {\n"
             << "      \"input_mode\": \"" << hypercine::Phase3AInputModeName(v.input_mode) << "\",\n"
             << "      \"trials\": " << v.trials << ",\n"
             << "      \"mean_fps\": " << v.mean_fps << ",\n"
             << "      \"median_fps\": " << v.median_fps << ",\n"
             << "      \"stddev_fps\": " << v.stddev_fps << ",\n"
             << "      \"min_fps\": " << v.min_fps << ",\n"
             << "      \"max_fps\": " << v.max_fps << ",\n"
             << "      \"mean_clock_mhz\": " << v.mean_clock_mhz << ",\n"
             << "      \"min_clock_mhz\": " << v.min_clock_mhz << ",\n"
             << "      \"max_clock_mhz\": " << v.max_clock_mhz << ",\n"
             << "      \"mean_power_w\": " << v.mean_power_w << ",\n"
             << "      \"max_power_w\": " << v.max_power_w << ",\n"
             << "      \"max_temp_c\": " << v.max_temp_c << ",\n"
             << "      \"combined_throttle_reasons\": \"" << JsonEscape(v.combined_throttle_reasons) << "\",\n"
             << "      \"trial_results\": [\n";
        for (size_t t = 0; t < v.trial_results.size(); ++t) {
            const auto& tr = v.trial_results[t];
            json << "        {\n"
                 << "          \"trial_index\": " << t << ",\n"
                 << "          \"sustained_fps\": " << tr.sustained_fps << ",\n"
                 << "          \"frame_period_ms\": " << tr.frame_period_ms << ",\n"
                 << "          \"mean_latency_ms\": " << tr.mean_latency_ms << ",\n"
                 << "          \"p95_latency_ms\": " << tr.p95_latency_ms << ",\n"
                 << "          \"nvml_gpu_util_avg_pct\": " << tr.nvml.hardware_gpu_utilization_pct_mean << ",\n"
                 << "          \"nvml_graphics_clock_avg_mhz\": " << tr.nvml.graphics_clock_mhz_mean << ",\n"
                 << "          \"nvml_graphics_clock_min_mhz\": " << tr.nvml.graphics_clock_mhz_min << ",\n"
                 << "          \"nvml_graphics_clock_max_mhz\": " << tr.nvml.graphics_clock_mhz_max << ",\n"
                 << "          \"nvml_temperature_max_c\": " << tr.nvml.temperature_c_max << ",\n"
                 << "          \"nvml_power_avg_w\": " << tr.nvml.power_w_mean << ",\n"
                 << "          \"nvml_throttle_reasons_str\": \"" << JsonEscape(tr.nvml.throttle_reasons_str) << "\"\n"
                 << "        }" << (t + 1 < v.trial_results.size() ? "," : "") << "\n";
        }
        json << "      ]\n"
             << "    }" << (comma ? "," : "") << "\n";
    };

    json << "  \"variance_study_8k\": {\n";
    write_variance("host_pageable_end_to_end", var8k_pageable, true);
    write_variance("host_pinned_api", var8k_pinned, true);
    write_variance("device_input", var8k_device, false);
    json << "  },\n";

    json << "  \"stability_and_leak_gates\": {\n"
         << "    \"1080p_1000f\": \n";
    WritePhase3AResultJson(json, stab1080, "    ");
    json << ",\n    \"4k_300f\": \n";
    WritePhase3AResultJson(json, stab4k, "    ");
    json << ",\n    \"8k_100f\": \n";
    WritePhase3AResultJson(json, stab8k, "    ");
    json << "\n  }\n"
         << "}\n";

    std::error_code ec;
    std::filesystem::path out_path = std::filesystem::path("benchmarks") / "results" / "phase3a_sequence.json";
    std::filesystem::create_directories(out_path.parent_path(), ec);
    const std::string payload = json.str();
    bool wrote_ok = false;
    {
        FILE* fp = std::fopen(out_path.string().c_str(), "wb");
        if (fp) {
            const size_t nw = std::fwrite(payload.data(), 1, payload.size(), fp);
            std::fclose(fp);
            wrote_ok = (nw == payload.size());
        }
    }
    if (!wrote_ok) {
        std::cerr << "WARNING: direct fopen(" << out_path.string() << ") failed (errno=" << errno
                  << "); emitting JSON_BEGIN..JSON_END on stdout\n";
        std::cout << "JSON_BEGIN\n" << payload << "JSON_END\n";
    } else {
        std::cout << "\nWrote Phase 3A sequence benchmark & verification report to "
                  << std::filesystem::absolute(out_path).string() << " (" << payload.size() << " bytes)\n";
    }
    return 0;
}
