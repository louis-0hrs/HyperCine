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
#include "hypercine/phase2a_gpu.h"
#include "hypercine/phase2c.h"
#include "hypercine/phase2d.h"

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

const char* SlotPolicyName(hypercine::Phase2DSlotPolicy p) {
    switch (p) {
        case hypercine::Phase2DSlotPolicy::DefaultAuto: return "DEFAULT_AUTO";
        case hypercine::Phase2DSlotPolicy::LowLatency: return "LOW_LATENCY";
        case hypercine::Phase2DSlotPolicy::MaxThroughput: return "MAX_THROUGHPUT";
        case hypercine::Phase2DSlotPolicy::UserOverride: return "USER_OVERRIDE";
    }
    return "UNKNOWN";
}

const char* QuantStateName(hypercine::Phase2DQuantStateMode m) {
    return (m == hypercine::Phase2DQuantStateMode::Frame1Initial)
        ? "Frame1Initial(FSratelimiter=4)"
        : "SteadyState(FSratelimiter=0)";
}

struct UpstreamBenchResult {
    double fps = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    size_t bitstream_bytes = 0;
    int fs_ratelimiter = 0;
};

// Benchmark pristine upstream CFHD_EncodeSample in either:
// - Frame1Initial (resetting lastgopbitcount = 0 before each frame so FSratelimiter stays 4, matching single-frame state)
// - SteadyState (normal multi-frame warmup >= 3 frames so FSratelimiter converges to 1)
UpstreamBenchResult BenchmarkUpstream(int width, int height, const std::vector<uint8_t>& frame,
                                      int warmup, int frames,
                                      hypercine::Phase2DQuantStateMode qmode) {
    UpstreamBenchResult res{};
    CFHD_EncoderRef ref = nullptr;
    if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) return res;
    if (CFHD_PrepareToEncode(ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(ref);
        return res;
    }
    CSampleEncoder* sample = reinterpret_cast<CSampleEncoder*>(ref);
    ENCODER* enc = reinterpret_cast<ENCODER*>(sample->m_encoder);
    const int pitch = width * 4;
    const int eff_warmup = (qmode == hypercine::Phase2DQuantStateMode::SteadyState)
        ? std::max(warmup, 3)
        : warmup;

    for (int i = 0; i < eff_warmup; ++i) {
        if (qmode == hypercine::Phase2DQuantStateMode::Frame1Initial && enc) {
            enc->lastgopbitcount = 0;
            enc->q.FSratelimiter = 4;
        }
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
    }
    std::vector<double> dts;
    const double t0 = NowMs();
    for (int i = 0; i < frames; ++i) {
        if (qmode == hypercine::Phase2DQuantStateMode::Frame1Initial && enc) {
            enc->lastgopbitcount = 0;
            enc->q.FSratelimiter = 4;
        }
        const double f0 = NowMs();
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
        dts.push_back(NowMs() - f0);
    }
    const double wall = NowMs() - t0;
    res.fps = wall > 0.0 ? 1000.0 * frames / wall : 0.0;
    Stats(dts, &res.mean_ms, &res.median_ms, &res.p95_ms);
    if (enc) {
        res.fs_ratelimiter = enc->q.FSratelimiter;
    }
    void* sample_ptr = nullptr;
    size_t sample_size = 0;
    if (CFHD_GetSampleData(ref, &sample_ptr, &sample_size) == CFHD_ERROR_OKAY) {
        res.bitstream_bytes = sample_size;
    }
    CFHD_CloseEncoder(ref);
    return res;
}

int TimedFramesForHeight(int height) {
    if (height >= 4000) return 12;
    if (height >= 2000) return 18;
    return 24;
}

void PrintRunRow(const char* label, const hypercine::Phase2DResult& r) {
    std::printf("  %-34s | slots=%d | fps=%6.2f | lat=%6.2f/%6.2f/%6.2f ms | wav=%5.2f | ent=%5.2f | cpu_asm=%5.2f | norm_cpu=%5.1f%% (1core=%5.1f%%, spin=%4.2fms, blk=%4.2fms, core_s/60s=%5.3f) | bytes=%zu | vram=%.1fMB\n",
                label, r.slot_selection.selected_slots, r.sustained_fps,
                r.mean_latency_ms, r.median_latency_ms, r.p95_latency_ms,
                r.wavelet_ms, r.gpu_entropy_ms, r.cpu_assembly_total_ms,
                r.cpu.normalized_process_cpu_pct,
                r.cpu.single_core_equiv_pct,
                r.cpu.main_thread_spin_ms,
                r.cpu.main_thread_blocked_ms,
                r.cpu.cpu_core_sec_per_60fps_video_sec,
                r.bitstream_bytes,
                r.vram_used_bytes / (1024.0 * 1024.0));
}

void WriteRunJson(std::ostringstream& json, const char* label, const hypercine::Phase2DResult& r, const std::string& indent) {
    json << indent << "{\n";
    json << indent << "  \"label\": \"" << JsonEscape(label) << "\",\n";
    json << indent << "  \"width\": " << r.config.width << ",\n";
    json << indent << "  \"height\": " << r.config.height << ",\n";
    json << indent << "  \"frames\": " << r.encoded_frames << ",\n";
    json << indent << "  \"frame_slots\": " << r.slot_selection.selected_slots << ",\n";
    json << indent << "  \"entropy_streams\": " << r.config.entropy_streams << ",\n";
    json << indent << "  \"slot_policy\": \"" << SlotPolicyName(r.slot_selection.policy) << "\",\n";
    json << indent << "  \"d2h_mode\": \"" << (r.slot_selection.selected_d2h_mode == hypercine::CompressedD2HMode::FrameConsolidated ? "THROUGHPUT_FRAME_CONSOLIDATED" : "LOW_LATENCY_BAND_STREAMED") << "\",\n";
    json << indent << "  \"sync_mode\": \"" << (r.config.sync_mode == hypercine::Phase2DSyncMode::Blocking ? "BLOCKING" : "POLLING") << "\",\n";
    json << indent << "  \"input_mode\": \"" << (r.config.input_mode == hypercine::Phase2CInputMode::DeviceInput ? "DEVICE" : "HOST") << "\",\n";
    json << indent << "  \"use_cuda_graph\": " << (r.graph_used ? "true" : "false") << ",\n";
    json << indent << "  \"kernel_launches_per_band\": " << r.kernel_launches_per_band << ",\n";
    json << indent << "  \"kernel_launches_per_frame\": " << r.kernel_launches_per_frame << ",\n";
    json << indent << "  \"host_launch_calls_per_frame\": " << r.host_launch_calls_per_frame << ",\n";
    json << indent << "  \"graph_kernel_nodes\": " << r.graph_kernel_nodes << ",\n";
    json << indent << "  \"graph_total_nodes\": " << r.graph_total_nodes << ",\n";
    json << indent << "  \"sustained_fps\": " << r.sustained_fps << ",\n";
    json << indent << "  \"mean_latency_ms\": " << r.mean_latency_ms << ",\n";
    json << indent << "  \"median_latency_ms\": " << r.median_latency_ms << ",\n";
    json << indent << "  \"p95_latency_ms\": " << r.p95_latency_ms << ",\n";
    json << indent << "  \"h2d_ms\": " << r.h2d_ms << ",\n";
    json << indent << "  \"gpu_color_ms\": " << r.color_ms << ",\n";
    json << indent << "  \"gpu_wavelet_ms\": " << r.wavelet_ms << ",\n";
    json << indent << "  \"gpu_entropy_ms\": " << r.gpu_entropy_ms << ",\n";
    json << indent << "  \"gpu_consolidate_ms\": " << r.gpu_consolidate_ms << ",\n";
    json << indent << "  \"d2h_compressed_ms\": " << r.compressed_d2h_ms << ",\n";
    json << indent << "  \"host_enqueue_ms\": " << r.host_enqueue_ms << ",\n";
    json << indent << "  \"graph_capture_ms\": " << r.graph_capture_ms << ",\n";
    json << indent << "  \"cpu_lowpass_ms\": " << r.cpu_lowpass_ms << ",\n";
    json << indent << "  \"cpu_header_wrap_ms\": " << r.cpu_header_wrap_ms << ",\n";
    json << indent << "  \"cpu_sample_concat_ms\": " << r.cpu_sample_concat_ms << ",\n";
    json << indent << "  \"cpu_assembly_total_ms\": " << r.cpu_assembly_total_ms << ",\n";
    json << indent << "  \"normalized_process_cpu_pct\": " << r.cpu.normalized_process_cpu_pct << ",\n";
    json << indent << "  \"single_core_equiv_pct\": " << r.cpu.single_core_equiv_pct << ",\n";
    json << indent << "  \"main_thread_single_core_pct\": " << r.cpu.main_thread_single_core_pct << ",\n";
    json << indent << "  \"worker_thread_single_core_pct\": " << r.cpu.worker_thread_single_core_pct << ",\n";
    json << indent << "  \"logical_processors\": " << r.cpu.logical_processors << ",\n";
    json << indent << "  \"active_host_threads\": " << r.cpu.active_host_threads << ",\n";
    json << indent << "  \"main_thread_running_ms\": " << r.cpu.main_thread_running_ms << ",\n";
    json << indent << "  \"main_thread_blocked_ms\": " << r.cpu.main_thread_blocked_ms << ",\n";
    json << indent << "  \"main_thread_spin_ms\": " << r.cpu.main_thread_spin_ms << ",\n";
    json << indent << "  \"poll_iterations\": " << r.cpu.poll_iterations << ",\n";
    json << indent << "  \"blocking_waits\": " << r.cpu.blocking_waits << ",\n";
    json << indent << "  \"cpu_core_sec_per_wall_sec\": " << r.cpu.cpu_core_sec_per_wall_sec << ",\n";
    json << indent << "  \"cpu_core_ms_per_frame\": " << r.cpu.cpu_core_ms_per_frame << ",\n";
    json << indent << "  \"cpu_core_sec_per_60fps_video_sec\": " << r.cpu.cpu_core_sec_per_60fps_video_sec << ",\n";
    json << indent << "  \"total_d2h_bytes_per_frame\": " << r.total_d2h_bytes_per_frame << ",\n";
    json << indent << "  \"bitstream_bytes\": " << r.bitstream_bytes << ",\n";
    json << indent << "  \"vram_used_mb\": " << (r.vram_used_bytes / (1024.0 * 1024.0)) << ",\n";
    json << indent << "  \"slot_vram_mb\": " << (r.slot_vram_bytes / (1024.0 * 1024.0)) << ",\n";
    json << indent << "  \"psnr_db\": " << r.psnr_db << ",\n";
    json << indent << "  \"sample_sha256\": \"" << r.sample_sha256 << "\"\n";
    json << indent << "}";
}

}  // namespace

int main() {
    std::cout << std::fixed << std::setprecision(2);
    HyperCineDeviceLink link{};
    if (HyperCine_QueryDeviceLink(&link) != 0) {
        std::cerr << "Failed to query CUDA device link\n";
        return 1;
    }
    std::cout << "=== HyperCine Phase 2D Benchmark & Verification Suite ===\n";
    std::cout << "GPU: " << link.name
              << " | SM " << link.compute_major << "." << link.compute_minor
              << " (" << link.sm_count << " SMs)"
              << " | Driver " << link.driver_version
              << " | CUDA Runtime " << link.runtime_version
              << " | AsyncEngines=" << link.async_engines
              << " | VRAM=" << (link.total_vram_bytes >> 20) << " MB\n\n";

    std::ostringstream json;
    json << std::fixed << std::setprecision(3);
    json << "{\n";
    json << "  \"phase\": \"2D\",\n";
    json << "  \"device\": {\n";
    json << "    \"name\": \"" << JsonEscape(link.name) << "\",\n";
    json << "    \"compute_capability\": \"" << link.compute_major << "." << link.compute_minor << "\",\n";
    json << "    \"sm_count\": " << link.sm_count << ",\n";
    json << "    \"driver_version\": " << link.driver_version << ",\n";
    json << "    \"runtime_version\": " << link.runtime_version << ",\n";
    json << "    \"pci_bus_id\": " << link.pci_bus << ",\n";
    json << "    \"pci_device_id\": " << link.pci_device << ",\n";
    json << "    \"async_engines\": " << link.async_engines << ",\n";
    json << "    \"total_vram_mb\": " << (link.total_vram_bytes / (1024.0 * 1024.0)) << "\n";
    json << "  },\n";

    // Part 2: Metadata Safety Verification
    std::cout << "--- Part 2: Production Metadata Safety Verification ---\n";
    const auto meta_safety = hypercine::VerifyPhase2DMetadataSafety(1920, 1080);
    std::cout << "  ok=" << (meta_safety.ok ? "PASS" : "FAIL")
              << " | clip1_guid=" << meta_safety.clip1_guid_hex
              << " | clip2_guid=" << meta_safety.clip2_guid_hex
              << " | guids_distinct=" << meta_safety.normal_mode_guids_distinct
              << " | normal_unmasked_distinct=" << meta_safety.normal_mode_unmasked_distinct
              << " | normal_masked_identical=" << meta_safety.normal_mode_masked_identical
              << " | det_unmasked_identical=" << meta_safety.deterministic_mode_unmasked_identical << "\n\n";

    json << "  \"part2_metadata_safety\": {\n";
    json << "    \"ok\": " << (meta_safety.ok ? "true" : "false") << ",\n";
    json << "    \"clip1_guid\": \"" << meta_safety.clip1_guid_hex << "\",\n";
    json << "    \"clip2_guid\": \"" << meta_safety.clip2_guid_hex << "\",\n";
    json << "    \"clip1_date\": \"" << JsonEscape(meta_safety.clip1_date) << "\",\n";
    json << "    \"clip1_time\": \"" << JsonEscape(meta_safety.clip1_time) << "\",\n";
    json << "    \"normal_mode_guids_distinct\": " << (meta_safety.normal_mode_guids_distinct ? "true" : "false") << ",\n";
    json << "    \"normal_mode_unmasked_distinct\": " << (meta_safety.normal_mode_unmasked_distinct ? "true" : "false") << ",\n";
    json << "    \"normal_mode_masked_identical\": " << (meta_safety.normal_mode_masked_identical ? "true" : "false") << ",\n";
    json << "    \"deterministic_mode_unmasked_identical\": " << (meta_safety.deterministic_mode_unmasked_identical ? "true" : "false") << ",\n";
    json << "    \"clip1_sha256\": \"" << meta_safety.clip1_sha256 << "\",\n";
    json << "    \"clip2_sha256\": \"" << meta_safety.clip2_sha256 << "\",\n";
    json << "    \"clip1_masked_sha256\": \"" << meta_safety.clip1_masked_sha256 << "\",\n";
    json << "    \"clip2_masked_sha256\": \"" << meta_safety.clip2_masked_sha256 << "\",\n";
    json << "    \"det1_sha256\": \"" << meta_safety.det1_sha256 << "\",\n";
    json << "    \"det2_sha256\": \"" << meta_safety.det2_sha256 << "\"\n";
    json << "  },\n";

    // Part 11: Canonical PCIe Gen4 x8 Transfer Baseline
    std::cout << "--- Part 11: Canonical Standalone PCIe Gen4 x8 Transfer Baseline (Pinned Host <-> Device) ---\n";
    std::vector<HyperCineTransferStats> xfer_stats(128);
    const int xfer_count = HyperCine_BenchmarkTransfersCanonical(xfer_stats.data(), static_cast<int>(xfer_stats.size()), 9);
    json << "  \"part11_canonical_pcie_baseline\": {\n";
    json << "    \"canonical_note\": \"Direct cudaHostAlloc pinned-memory H2D/D2H (~13.3 GB/s on PCIe Gen4 x8) is the canonical hardware transfer baseline. Earlier ~3.3 GB/s and ~6.6 GB/s figures from Phase 2A/2B mixed pageable/staged paths are preserved as superseded historical measurements.\",\n";
    json << "    \"measurements\": [\n";
    for (int i = 0; i < xfer_count; ++i) {
        const auto& st = xfer_stats[static_cast<size_t>(i)];
        std::printf("  %-15s | %-19s | %6.1f MB | mean=%6.2f GB/s (med=%6.2f, p95=%6.2f) | mean=%6.3f ms\n",
                    st.host_kind, st.direction, st.bytes / (1024.0 * 1024.0),
                    st.mean_gbps, st.median_gbps, st.p95_gbps, st.mean_ms);
        json << "      {\n";
        json << "        \"host_kind\": \"" << st.host_kind << "\",\n";
        json << "        \"direction\": \"" << st.direction << "\",\n";
        json << "        \"bytes\": " << st.bytes << ",\n";
        json << "        \"mean_ms\": " << st.mean_ms << ",\n";
        json << "        \"median_ms\": " << st.median_ms << ",\n";
        json << "        \"p95_ms\": " << st.p95_ms << ",\n";
        json << "        \"mean_gbps\": " << st.mean_gbps << ",\n";
        json << "        \"median_gbps\": " << st.median_gbps << ",\n";
        json << "        \"p95_gbps\": " << st.p95_gbps << "\n";
        json << "      }" << (i + 1 < xfer_count ? "," : "") << "\n";
    }
    json << "    ]\n";
    json << "  },\n";
    std::cout << "\n";

    // Part 4: Slot Selection Policy Table
    std::cout << "--- Part 4: Resolution-Aware & VRAM-Bounded Slot Selection Policy ---\n";
    json << "  \"part4_slot_policies\": [\n";
    bool first_slot_json = true;
    for (const auto& res : hypercine::kResolutions) {
        for (auto pol : {hypercine::Phase2DSlotPolicy::DefaultAuto,
                         hypercine::Phase2DSlotPolicy::LowLatency,
                         hypercine::Phase2DSlotPolicy::MaxThroughput}) {
            const auto sel = hypercine::SelectPhase2DFrameSlots(
                res.width, res.height, pol, 0, hypercine::Phase2DD2HPolicy::Throughput, 0);
            const bool clamped = sel.selected_slots < sel.policy_target_slots;
            std::printf("  %-5s | %-14s | target=%d vram_max=%d sel=%d | per_slot=%.1f MB | est_total=%.1f MB | avail_vram=%.1f MB | clamped=%s\n",
                        res.name, SlotPolicyName(sel.policy), sel.policy_target_slots, sel.vram_max_slots, sel.selected_slots,
                        sel.per_slot_vram_bytes / (1024.0 * 1024.0),
                        sel.total_estimated_vram_bytes / (1024.0 * 1024.0),
                        sel.available_vram_bytes / (1024.0 * 1024.0),
                        clamped ? "YES" : "NO");
            if (!first_slot_json) json << ",\n";
            first_slot_json = false;
            json << "    {\n";
            json << "      \"resolution\": \"" << res.name << "\",\n";
            json << "      \"policy\": \"" << SlotPolicyName(sel.policy) << "\",\n";
            json << "      \"policy_target_slots\": " << sel.policy_target_slots << ",\n";
            json << "      \"vram_max_slots\": " << sel.vram_max_slots << ",\n";
            json << "      \"selected_slots\": " << sel.selected_slots << ",\n";
            json << "      \"per_slot_vram_mb\": " << (sel.per_slot_vram_bytes / (1024.0 * 1024.0)) << ",\n";
            json << "      \"session_workspace_vram_mb\": " << (sel.session_workspace_vram_bytes / (1024.0 * 1024.0)) << ",\n";
            json << "      \"estimated_total_vram_mb\": " << (sel.total_estimated_vram_bytes / (1024.0 * 1024.0)) << ",\n";
            json << "      \"available_vram_mb\": " << (sel.available_vram_bytes / (1024.0 * 1024.0)) << ",\n";
            json << "      \"clamped_by_vram\": " << (clamped ? "true" : "false") << "\n";
            json << "    }";
        }
    }
    json << "\n  ],\n";
    std::cout << "\n";

    // Per-resolution comprehensive benchmarks
    json << "  \"resolutions\": [\n";
    for (size_t r_idx = 0; r_idx < std::size(hypercine::kResolutions); ++r_idx) {
        const auto& res = hypercine::kResolutions[r_idx];
        const int w = res.width;
        const int h = res.height;
        const int timed_frames = TimedFramesForHeight(h);
        const std::vector<uint8_t> canonical_frame = hypercine::MakeFrame(w, h, hypercine::CanonicalPattern::Canonical);

        std::cout << "====================================================================================================\n";
        std::cout << "Resolution: " << res.name << " (" << w << "x" << h << ")\n";
        std::cout << "====================================================================================================\n";

        // Part 1: A vs B vs C comparison at both Frame1Initial and SteadyState
        const auto cmp_f1 = hypercine::VerifyPhase2DUpstreamComparison(
            w, h, hypercine::Phase2DQuantStateMode::Frame1Initial);
        const auto cmp_ss = hypercine::VerifyPhase2DUpstreamComparison(
            w, h, hypercine::Phase2DQuantStateMode::SteadyState);

        std::cout << "[Part 1 Upstream (A) vs HyperCine CPU/Ref (B) vs HyperCine GPU (C)]\n";
        for (const auto* cmp : {&cmp_f1, &cmp_ss}) {
            std::printf("  State=%-30s | FSratelimiter=%d | lastgopbytes=%8d | A=%8zu B | B=%8zu B | C=%8zu B | B==C(unmasked)=%s | A==B(same_h)=%s | A==C(masked)=%s | 30_subbands_eq=%s | px_diff=%d | PSNR=%.2f dB\n",
                        QuantStateName(cmp->quant_state_mode), cmp->fs_rate_limiter, cmp->last_gop_bytes,
                        cmp->a_upstream_sample_bytes, cmp->b_cpu_ref_sample_bytes, cmp->c_gpu_ref_sample_bytes,
                        cmp->b_eq_c_unmasked ? "YES" : "NO",
                        cmp->a_eq_b_same_handle_unmasked ? "YES" : "NO",
                        cmp->a_eq_c_independent_handle_masked ? "YES" : "NO",
                        cmp->all_30_subbands_a_eq_b_eq_c ? "YES" : "NO",
                        cmp->max_abs_pixel_diff, cmp->psnr_db);
            std::cout << "    A (Upstream fresh handle) SHA-256 : " << cmp->a_upstream_sha256
                      << " (masked: " << cmp->a_masked_sha256 << ")\n";
            std::cout << "    B (HyperCine CPU/Ref)     SHA-256 : " << cmp->b_cpu_ref_sha256
                      << " (masked: " << cmp->b_masked_sha256 << ")\n";
            std::cout << "    C (HyperCine GPU all 4)   SHA-256 : " << cmp->c_gpu_ref_sha256
                      << " (masked: " << cmp->c_masked_sha256 << ")\n";
            std::cout << "    Decoded BGRA Pixels       SHA-256 : " << cmp->decoded_pixels_sha256 << "\n";
        }
        if (r_idx == 1) {
            std::cout << "  --- 4K Subband-by-Subband Comparison: Frame1Initial (FSratelimiter=4, 3,563,684 B) vs SteadyState (FSratelimiter=0, 4,668,864 B) ---\n";
            std::cout << "  Idx | Band | Dim       | F1_Q | F1_S | F1_Bytes | SS_Q | SS_S | SS_Bytes | Delta_B   | F1_A==B==C | SS_A==B==C | SS_Payload_SHA256\n";
            for (size_t bi = 0; bi < cmp_f1.bands.size() && bi < cmp_ss.bands.size(); ++bi) {
                const auto& b1 = cmp_f1.bands[bi];
                const auto& bs = cmp_ss.bands[bi];
                const long long delta = static_cast<long long>(bs.c_gpu_bytes) - static_cast<long long>(b1.c_gpu_bytes);
                std::printf("  %2d  | %-4s | %4dx%-4d | %4d | %4d | %8zu | %4d | %4d | %8zu | %+9lld | %-10s | %-10s | %.16s...\n",
                            b1.index, b1.name.c_str(), b1.width, b1.height,
                            b1.quant, b1.scale, b1.c_gpu_bytes,
                            bs.quant, bs.scale, bs.c_gpu_bytes,
                            delta,
                            (b1.a_eq_b && b1.b_eq_c) ? "YES" : "NO",
                            (bs.a_eq_b && bs.b_eq_c) ? "YES" : "NO",
                            bs.c_sha256.c_str());
            }
        }

        // Parts 6, 7, 8, 9: Kernel Evolution & Substage Profiling
        std::cout << "\n[Parts 6-9 GPU Entropy Kernel Evolution & Substage Breakdown (SteadyState)]\n";
        const auto evo = hypercine::BenchmarkPhase2DKernelEvolution(
            w, h, 8, hypercine::Phase2DQuantStateMode::SteadyState);
        for (const auto& pt : evo) {
            const double pfx_scan_sum = pt.substage_sum.merge_carry_ms + pt.substage_sum.prefix_ms;
            const double all_scan_sum = pt.substage_sum.scan_ms + pt.substage_sum.prefix_ms;
            const double pfx_scan_pct = pt.substage_sum.total_ms > 0.0
                ? (100.0 * pfx_scan_sum / pt.substage_sum.total_ms)
                : 0.0;
            const double all_scan_pct = pt.substage_sum.total_ms > 0.0
                ? (100.0 * all_scan_sum / pt.substage_sum.total_ms)
                : 0.0;
            std::printf("  %-32s | band_k=%2d frame_k=%3d calls=%3d (graph_k=%3d/%3d) | cap=%5.2fms enq=%5.3fms | wall=%6.3f ms (med=%6.3f, p95=%6.3f) | gpu_ent=%6.3f ms (%4.2fx) | sub_tot=%6.3f [blk=%.3f mrg=%.3f gth=%.3f tsc=%.3f tok=%.3f cbk=%.3f bsc=%.3f pck=%.3f | pfx_scan=%.1f%% all_scan=%.1f%%] | match=%s\n",
                        pt.label.c_str(), pt.kernel_launches_per_band, pt.kernel_launches_per_frame,
                        pt.host_launch_calls_per_frame, pt.graph_kernel_nodes, pt.graph_total_nodes,
                        pt.graph_capture_ms, pt.host_enqueue_ms,
                        pt.mean_wall_ms, pt.median_wall_ms, pt.p95_wall_ms,
                        pt.gpu_entropy_ms, pt.speedup_vs_ref_no_graph,
                        pt.substage_sum.total_ms,
                        pt.substage_sum.block_scan_ms, pt.substage_sum.merge_carry_ms,
                        pt.substage_sum.gather_ms, pt.substage_sum.scan_ms,
                        pt.substage_sum.token_ms, pt.substage_sum.codebook_ms,
                        pt.substage_sum.prefix_ms, pt.substage_sum.pack_ms,
                        pfx_scan_pct, all_scan_pct, pt.byte_match_reference ? "YES" : "NO");
        }

        // Part 3: Polling vs Blocking Sync Comparison (with optimal slots)
        std::cout << "\n[Part 3 Synchronization & CPU Utilization: Polling vs Blocking]\n";
        hypercine::Phase2DConfig sync_cfg_poll{};
        sync_cfg_poll.width = w;
        sync_cfg_poll.height = h;
        sync_cfg_poll.warmup_frames = 2;
        sync_cfg_poll.frames = timed_frames;
        sync_cfg_poll.slot_policy = hypercine::Phase2DSlotPolicy::DefaultAuto;
        sync_cfg_poll.d2h_policy = hypercine::Phase2DD2HPolicy::Throughput;
        sync_cfg_poll.sync_mode = hypercine::Phase2DSyncMode::Polling;
        sync_cfg_poll.input_mode = hypercine::Phase2CInputMode::DeviceInput;
        sync_cfg_poll.kernel_mode = hypercine::GpuEntropyKernelMode::FusedScanOptimized;
        sync_cfg_poll.use_cuda_graph = true;
        sync_cfg_poll.quant_state_mode = hypercine::Phase2DQuantStateMode::SteadyState;
        sync_cfg_poll.deterministic_test_metadata = true;

        hypercine::Phase2DConfig sync_cfg_block = sync_cfg_poll;
        sync_cfg_block.sync_mode = hypercine::Phase2DSyncMode::Blocking;

        const auto run_poll = hypercine::RunPhase2D(sync_cfg_poll);
        const auto run_block = hypercine::RunPhase2D(sync_cfg_block);
        PrintRunRow("PHASE2D_POLLING_SPIN", run_poll);
        PrintRunRow("PHASE2D_BLOCKING_EVENT", run_block);

        // Part 5: Compressed D2H Strategy Comparison (THROUGHPUT vs LOW_LATENCY, slots=1 and optimal slots)
        std::cout << "\n[Part 5 Compressed D2H Mode Comparison: THROUGHPUT vs LOW_LATENCY]\n";
        hypercine::Phase2DConfig d2h_tput_1s = sync_cfg_block;
        d2h_tput_1s.slot_policy = hypercine::Phase2DSlotPolicy::LowLatency;
        d2h_tput_1s.d2h_policy = hypercine::Phase2DD2HPolicy::Throughput;

        hypercine::Phase2DConfig d2h_lowlat_1s = d2h_tput_1s;
        d2h_lowlat_1s.d2h_policy = hypercine::Phase2DD2HPolicy::LowLatency;

        hypercine::Phase2DConfig d2h_tput_opt = sync_cfg_block;
        d2h_tput_opt.slot_policy = hypercine::Phase2DSlotPolicy::DefaultAuto;
        d2h_tput_opt.d2h_policy = hypercine::Phase2DD2HPolicy::Throughput;

        hypercine::Phase2DConfig d2h_lowlat_opt = d2h_tput_opt;
        d2h_lowlat_opt.d2h_policy = hypercine::Phase2DD2HPolicy::LowLatency;

        const auto r_d2h_tput_1s = hypercine::RunPhase2D(d2h_tput_1s);
        const auto r_d2h_lowlat_1s = hypercine::RunPhase2D(d2h_lowlat_1s);
        const auto r_d2h_tput_opt = hypercine::RunPhase2D(d2h_tput_opt);
        const auto r_d2h_lowlat_opt = hypercine::RunPhase2D(d2h_lowlat_opt);
        PrintRunRow("D2H_THROUGHPUT_SLOTS1", r_d2h_tput_1s);
        PrintRunRow("D2H_LOW_LATENCY_SLOTS1", r_d2h_lowlat_1s);
        PrintRunRow("D2H_THROUGHPUT_AUTO_SLOTS", r_d2h_tput_opt);
        PrintRunRow("D2H_LOW_LATENCY_AUTO_SLOTS", r_d2h_lowlat_opt);

        // Part 10: End-to-End Progression at Resolution-Optimal Slot Count (SteadyState & Frame1Initial)
        std::cout << "\n[Part 10 End-to-End Benchmark Progression (Apples-to-Apples SteadyState & Frame1Initial)]\n";
        const auto up_ss = BenchmarkUpstream(w, h, canonical_frame, 3, std::min(timed_frames, 8),
                                             hypercine::Phase2DQuantStateMode::SteadyState);
        const auto up_f1 = BenchmarkUpstream(w, h, canonical_frame, 1, std::min(timed_frames, 8),
                                             hypercine::Phase2DQuantStateMode::Frame1Initial);
        std::printf("  %-34s | slots=1 | fps=%6.2f | lat=%6.2f/%6.2f/%6.2f ms | FSratelimiter=%d | bytes=%zu\n",
                    "UPSTREAM_DEFAULT (SteadyState)", up_ss.fps, up_ss.mean_ms, up_ss.median_ms, up_ss.p95_ms,
                    up_ss.fs_ratelimiter, up_ss.bitstream_bytes);
        std::printf("  %-34s | slots=1 | fps=%6.2f | lat=%6.2f/%6.2f/%6.2f ms | FSratelimiter=%d | bytes=%zu\n",
                    "UPSTREAM_DEFAULT (Frame1Initial)", up_f1.fps, up_f1.mean_ms, up_f1.median_ms, up_f1.p95_ms,
                    up_f1.fs_ratelimiter, up_f1.bitstream_bytes);

        hypercine::Phase2DConfig c_p2c_best = sync_cfg_poll;
        c_p2c_best.kernel_mode = hypercine::GpuEntropyKernelMode::ReferencePhase2C;
        c_p2c_best.use_cuda_graph = false;
        c_p2c_best.sync_mode = hypercine::Phase2DSyncMode::Polling;

        hypercine::Phase2DConfig c_p2d_graph = sync_cfg_block;
        c_p2d_graph.kernel_mode = hypercine::GpuEntropyKernelMode::ReferencePhase2C;
        c_p2d_graph.use_cuda_graph = true;

        hypercine::Phase2DConfig c_p2d_fused = sync_cfg_block;
        c_p2d_fused.kernel_mode = hypercine::GpuEntropyKernelMode::Fused;
        c_p2d_fused.use_cuda_graph = true;

        hypercine::Phase2DConfig c_p2d_final_dev = sync_cfg_block;
        c_p2d_final_dev.kernel_mode = hypercine::GpuEntropyKernelMode::FusedScanOptimized;
        c_p2d_final_dev.use_cuda_graph = true;

        hypercine::Phase2DConfig c_p2d_final_host = c_p2d_final_dev;
        c_p2d_final_host.input_mode = hypercine::Phase2CInputMode::HostInput;

        hypercine::Phase2DConfig c_p2d_final_f1_dev = c_p2d_final_dev;
        c_p2d_final_f1_dev.quant_state_mode = hypercine::Phase2DQuantStateMode::Frame1Initial;

        hypercine::Phase2DConfig c_p2d_final_f1_host = c_p2d_final_host;
        c_p2d_final_f1_host.quant_state_mode = hypercine::Phase2DQuantStateMode::Frame1Initial;

        const auto r_p2c_best = hypercine::RunPhase2D(c_p2c_best);
        const auto r_p2d_graph = hypercine::RunPhase2D(c_p2d_graph);
        const auto r_p2d_fused = hypercine::RunPhase2D(c_p2d_fused);
        const auto r_p2d_final_dev = hypercine::RunPhase2D(c_p2d_final_dev);
        const auto r_p2d_final_host = hypercine::RunPhase2D(c_p2d_final_host);
        const auto r_p2d_final_f1_dev = hypercine::RunPhase2D(c_p2d_final_f1_dev);
        const auto r_p2d_final_f1_host = hypercine::RunPhase2D(c_p2d_final_f1_host);

        PrintRunRow("PHASE2C_BEST (Dev,SS)", r_p2c_best);
        PrintRunRow("PHASE2D_GRAPH (Dev,SS)", r_p2d_graph);
        PrintRunRow("PHASE2D_FUSED_GRAPH (Dev,SS)", r_p2d_fused);
        PrintRunRow("PHASE2D_FINAL (Dev,SS)", r_p2d_final_dev);
        PrintRunRow("PHASE2D_FINAL (Host,SS)", r_p2d_final_host);
        PrintRunRow("PHASE2D_FINAL (Dev,Frame1Init)", r_p2d_final_f1_dev);
        PrintRunRow("PHASE2D_FINAL (Host,Frame1Init)", r_p2d_final_f1_host);

        // At 8K, also benchmark MAX_THROUGHPUT (slots=3) alongside DEFAULT_AUTO (slots=2)
        hypercine::Phase2DResult r_p2d_max_tput_dev{};
        hypercine::Phase2DResult r_p2d_max_tput_host{};
        if (w == 7680) {
            hypercine::Phase2DConfig c_max_dev = c_p2d_final_dev;
            c_max_dev.slot_policy = hypercine::Phase2DSlotPolicy::MaxThroughput;
            hypercine::Phase2DConfig c_max_host = c_p2d_final_host;
            c_max_host.slot_policy = hypercine::Phase2DSlotPolicy::MaxThroughput;
            r_p2d_max_tput_dev = hypercine::RunPhase2D(c_max_dev);
            r_p2d_max_tput_host = hypercine::RunPhase2D(c_max_host);
            PrintRunRow("PHASE2D_FINAL_MAX_TPUT (Dev,SS)", r_p2d_max_tput_dev);
            PrintRunRow("PHASE2D_FINAL_MAX_TPUT (Host,SS)", r_p2d_max_tput_host);
        }
        std::cout << "\n";

        // Emit JSON for this resolution
        json << "    {\n";
        json << "      \"name\": \"" << res.name << "\",\n";
        json << "      \"width\": " << w << ",\n";
        json << "      \"height\": " << h << ",\n";
        json << "      \"part1_comparison\": {\n";
        json << "        \"frame1_initial\": {\n";
        json << "          \"fs_ratelimiter\": " << cmp_f1.fs_rate_limiter << ",\n";
        json << "          \"lastgopbytes\": " << cmp_f1.last_gop_bytes << ",\n";
        json << "          \"a_upstream_bytes\": " << cmp_f1.a_upstream_sample_bytes << ",\n";
        json << "          \"b_cpu_ref_bytes\": " << cmp_f1.b_cpu_ref_sample_bytes << ",\n";
        json << "          \"c_gpu_bytes\": " << cmp_f1.c_gpu_ref_sample_bytes << ",\n";
        json << "          \"b_eq_c_unmasked\": " << (cmp_f1.b_eq_c_unmasked ? "true" : "false") << ",\n";
        json << "          \"a_eq_b_same_handle_unmasked\": " << (cmp_f1.a_eq_b_same_handle_unmasked ? "true" : "false") << ",\n";
        json << "          \"a_eq_c_independent_handle_masked\": " << (cmp_f1.a_eq_c_independent_handle_masked ? "true" : "false") << ",\n";
        json << "          \"all_30_subbands_a_eq_b_eq_c\": " << (cmp_f1.all_30_subbands_a_eq_b_eq_c ? "true" : "false") << ",\n";
        json << "          \"a_sha256\": \"" << cmp_f1.a_upstream_sha256 << "\",\n";
        json << "          \"b_sha256\": \"" << cmp_f1.b_cpu_ref_sha256 << "\",\n";
        json << "          \"c_sha256\": \"" << cmp_f1.c_gpu_ref_sha256 << "\",\n";
        json << "          \"masked_sha256\": \"" << cmp_f1.c_masked_sha256 << "\",\n";
        json << "          \"decoded_pixels_sha256\": \"" << cmp_f1.decoded_pixels_sha256 << "\",\n";
        json << "          \"psnr_db\": " << cmp_f1.psnr_db << "\n";
        json << "        },\n";
        json << "        \"steady_state\": {\n";
        json << "          \"fs_ratelimiter\": " << cmp_ss.fs_rate_limiter << ",\n";
        json << "          \"lastgopbytes\": " << cmp_ss.last_gop_bytes << ",\n";
        json << "          \"a_upstream_bytes\": " << cmp_ss.a_upstream_sample_bytes << ",\n";
        json << "          \"b_cpu_ref_bytes\": " << cmp_ss.b_cpu_ref_sample_bytes << ",\n";
        json << "          \"c_gpu_bytes\": " << cmp_ss.c_gpu_ref_sample_bytes << ",\n";
        json << "          \"b_eq_c_unmasked\": " << (cmp_ss.b_eq_c_unmasked ? "true" : "false") << ",\n";
        json << "          \"a_eq_b_same_handle_unmasked\": " << (cmp_ss.a_eq_b_same_handle_unmasked ? "true" : "false") << ",\n";
        json << "          \"a_eq_c_independent_handle_masked\": " << (cmp_ss.a_eq_c_independent_handle_masked ? "true" : "false") << ",\n";
        json << "          \"all_30_subbands_a_eq_b_eq_c\": " << (cmp_ss.all_30_subbands_a_eq_b_eq_c ? "true" : "false") << ",\n";
        json << "          \"a_sha256\": \"" << cmp_ss.a_upstream_sha256 << "\",\n";
        json << "          \"b_sha256\": \"" << cmp_ss.b_cpu_ref_sha256 << "\",\n";
        json << "          \"c_sha256\": \"" << cmp_ss.c_gpu_ref_sha256 << "\",\n";
        json << "          \"masked_sha256\": \"" << cmp_ss.c_masked_sha256 << "\",\n";
        json << "          \"decoded_pixels_sha256\": \"" << cmp_ss.decoded_pixels_sha256 << "\",\n";
        json << "          \"psnr_db\": " << cmp_ss.psnr_db << "\n";
        json << "        }\n";
        json << "      },\n";

        json << "      \"kernel_evolution\": [\n";
        for (size_t ei = 0; ei < evo.size(); ++ei) {
            const auto& pt = evo[ei];
            const double pfx_scan_sum = pt.substage_sum.merge_carry_ms + pt.substage_sum.prefix_ms;
            const double all_scan_sum = pt.substage_sum.scan_ms + pt.substage_sum.prefix_ms;
            const double pfx_scan_pct = pt.substage_sum.total_ms > 0.0
                ? (100.0 * pfx_scan_sum / pt.substage_sum.total_ms)
                : 0.0;
            const double all_scan_pct = pt.substage_sum.total_ms > 0.0
                ? (100.0 * all_scan_sum / pt.substage_sum.total_ms)
                : 0.0;
            json << "        {\n";
            json << "          \"label\": \"" << pt.label << "\",\n";
            json << "          \"use_cuda_graph\": " << (pt.use_cuda_graph ? "true" : "false") << ",\n";
            json << "          \"kernel_launches_per_band\": " << pt.kernel_launches_per_band << ",\n";
            json << "          \"kernel_launches_per_frame\": " << pt.kernel_launches_per_frame << ",\n";
            json << "          \"host_launch_calls_per_frame\": " << pt.host_launch_calls_per_frame << ",\n";
            json << "          \"graph_kernel_nodes\": " << pt.graph_kernel_nodes << ",\n";
            json << "          \"graph_total_nodes\": " << pt.graph_total_nodes << ",\n";
            json << "          \"graph_capture_ms\": " << pt.graph_capture_ms << ",\n";
            json << "          \"host_enqueue_ms\": " << pt.host_enqueue_ms << ",\n";
            json << "          \"mean_wall_ms\": " << pt.mean_wall_ms << ",\n";
            json << "          \"median_wall_ms\": " << pt.median_wall_ms << ",\n";
            json << "          \"p95_wall_ms\": " << pt.p95_wall_ms << ",\n";
            json << "          \"gpu_entropy_ms\": " << pt.gpu_entropy_ms << ",\n";
            json << "          \"gpu_consolidate_ms\": " << pt.gpu_consolidate_ms << ",\n";
            json << "          \"compressed_d2h_ms\": " << pt.compressed_d2h_ms << ",\n";
            json << "          \"speedup_vs_ref_no_graph\": " << pt.speedup_vs_ref_no_graph << ",\n";
            json << "          \"substage_block_scan_ms\": " << pt.substage_sum.block_scan_ms << ",\n";
            json << "          \"substage_merge_carry_ms\": " << pt.substage_sum.merge_carry_ms << ",\n";
            json << "          \"substage_gather_ms\": " << pt.substage_sum.gather_ms << ",\n";
            json << "          \"substage_token_scan_ms\": " << pt.substage_sum.scan_ms << ",\n";
            json << "          \"substage_tokenize_ms\": " << pt.substage_sum.token_ms << ",\n";
            json << "          \"substage_codebook_ms\": " << pt.substage_sum.codebook_ms << ",\n";
            json << "          \"substage_bit_scan_ms\": " << pt.substage_sum.prefix_ms << ",\n";
            json << "          \"substage_pack_ms\": " << pt.substage_sum.pack_ms << ",\n";
            json << "          \"substage_total_ms\": " << pt.substage_sum.total_ms << ",\n";
            json << "          \"prefix_scan_share_pct\": " << pfx_scan_pct << ",\n";
            json << "          \"all_scan_share_pct\": " << all_scan_pct << ",\n";
            json << "          \"sample_bytes\": " << pt.sample_bytes << ",\n";
            json << "          \"sample_sha256\": \"" << pt.sample_sha256 << "\",\n";
            json << "          \"byte_match_reference\": " << (pt.byte_match_reference ? "true" : "false") << "\n";
            json << "        }" << (ei + 1 < evo.size() ? "," : "") << "\n";
        }
        json << "      ],\n";

        json << "      \"upstream_steady_state\": {\n";
        json << "        \"fps\": " << up_ss.fps << ",\n";
        json << "        \"mean_ms\": " << up_ss.mean_ms << ",\n";
        json << "        \"median_ms\": " << up_ss.median_ms << ",\n";
        json << "        \"p95_ms\": " << up_ss.p95_ms << ",\n";
        json << "        \"fs_ratelimiter\": " << up_ss.fs_ratelimiter << ",\n";
        json << "        \"bitstream_bytes\": " << up_ss.bitstream_bytes << "\n";
        json << "      },\n";
        json << "      \"upstream_frame1_initial\": {\n";
        json << "        \"fps\": " << up_f1.fps << ",\n";
        json << "        \"mean_ms\": " << up_f1.mean_ms << ",\n";
        json << "        \"median_ms\": " << up_f1.median_ms << ",\n";
        json << "        \"p95_ms\": " << up_f1.p95_ms << ",\n";
        json << "        \"fs_ratelimiter\": " << up_f1.fs_ratelimiter << ",\n";
        json << "        \"bitstream_bytes\": " << up_f1.bitstream_bytes << "\n";
        json << "      },\n";

        json << "      \"runs\": [\n";
        WriteRunJson(json, "PHASE2D_POLLING_SPIN", run_poll, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2D_BLOCKING_EVENT", run_block, "        "); json << ",\n";
        WriteRunJson(json, "D2H_THROUGHPUT_SLOTS1", r_d2h_tput_1s, "        "); json << ",\n";
        WriteRunJson(json, "D2H_LOW_LATENCY_SLOTS1", r_d2h_lowlat_1s, "        "); json << ",\n";
        WriteRunJson(json, "D2H_THROUGHPUT_AUTO_SLOTS", r_d2h_tput_opt, "        "); json << ",\n";
        WriteRunJson(json, "D2H_LOW_LATENCY_AUTO_SLOTS", r_d2h_lowlat_opt, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2C_BEST (Dev,SS)", r_p2c_best, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2D_GRAPH (Dev,SS)", r_p2d_graph, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2D_FUSED_GRAPH (Dev,SS)", r_p2d_fused, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2D_FINAL (Dev,SS)", r_p2d_final_dev, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2D_FINAL (Host,SS)", r_p2d_final_host, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2D_FINAL (Dev,Frame1Init)", r_p2d_final_f1_dev, "        "); json << ",\n";
        WriteRunJson(json, "PHASE2D_FINAL (Host,Frame1Init)", r_p2d_final_f1_host, "        ");
        if (w == 7680) {
            json << ",\n";
            WriteRunJson(json, "PHASE2D_FINAL_MAX_TPUT (Dev,SS)", r_p2d_max_tput_dev, "        "); json << ",\n";
            WriteRunJson(json, "PHASE2D_FINAL_MAX_TPUT (Host,SS)", r_p2d_max_tput_host, "        ");
        }
        json << "\n      ]\n";
        json << "    }" << (r_idx + 1 < std::size(hypercine::kResolutions) ? "," : "") << "\n";
    }
    json << "  ]\n";
    json << "}\n";

    std::filesystem::path out_path = std::filesystem::path("benchmarks") / "results" / "phase2d.json";
    std::error_code ec;
    std::filesystem::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path, std::ios::binary);
    if (out) {
        out << json.str();
        std::cout << "Wrote JSON results to " << out_path.string() << "\n";
    }
    return 0;
}
