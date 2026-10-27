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
#include "hypercine/phase2c.h"

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
#include "bench_host.h"

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

struct UpstreamBenchResult {
    double fps = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    size_t bitstream_bytes = 0;
};

UpstreamBenchResult BenchmarkUpstreamDefault(int width, int height, const std::vector<uint8_t>& frame, int warmup, int frames) {
    UpstreamBenchResult res{};
    CFHD_EncoderRef ref = nullptr;
    if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) return res;
    if (CFHD_PrepareToEncode(ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(ref);
        return res;
    }
    const int pitch = width * 4;
    for (int i = 0; i < warmup; ++i) {
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
    }
    std::vector<double> dts;
    const double t0 = NowMs();
    for (int i = 0; i < frames; ++i) {
        const double f0 = NowMs();
        CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pitch);
        dts.push_back(NowMs() - f0);
    }
    const double wall = NowMs() - t0;
    res.fps = wall > 0.0 ? 1000.0 * frames / wall : 0.0;
    Stats(dts, &res.mean_ms, &res.median_ms, &res.p95_ms);
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

}  // namespace

int main(int argc, char** argv) {
    const std::string json_path = (argc > 1 && argv[1] != nullptr && argv[1][0] != '-')
        ? std::string(argv[1])
        : std::string("benchmarks/results/phase2c.json");
    std::cout << std::fixed << std::setprecision(2);
    HyperCineDeviceLink link{};
    if (HyperCine_QueryDeviceLink(&link) != 0) {
        std::cerr << "Failed to query CUDA device link\n";
        return 1;
    }
    std::cout << "=== HyperCine Phase 2C Benchmark Suite ===\n";
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
    json << "  \"phase\": \"2C\",\n";
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

    // Section 7: Canonical standalone PCIe copy benchmark.
    std::cout << "--- Section 7: Canonical Standalone PCIe Transfer Benchmark (Pinned Host <-> Device) ---\n";
    std::vector<HyperCineTransferStats> xfer_stats(128);
    const int xfer_count = HyperCine_BenchmarkTransfersCanonical(xfer_stats.data(), static_cast<int>(xfer_stats.size()), 9);
    json << "  \"canonical_pcie_transfers\": [\n";
    for (int i = 0; i < xfer_count; ++i) {
        const auto& st = xfer_stats[static_cast<size_t>(i)];
        std::printf("  %-15s | %-19s | %6.1f MB | runs=%d | mean=%6.2f GB/s (med=%6.2f, p95=%6.2f) | mean=%6.3f ms (med=%6.3f, p95=%6.3f)\n",
                    st.host_kind, st.direction, st.bytes / (1024.0 * 1024.0), st.runs,
                    st.mean_gbps, st.median_gbps, st.p95_gbps,
                    st.mean_ms, st.median_ms, st.p95_ms);
        json << "    {\n";
        json << "      \"host_kind\": \"" << st.host_kind << "\",\n";
        json << "      \"direction\": \"" << st.direction << "\",\n";
        json << "      \"bytes\": " << st.bytes << ",\n";
        json << "      \"runs\": " << st.runs << ",\n";
        json << "      \"mean_ms\": " << st.mean_ms << ",\n";
        json << "      \"median_ms\": " << st.median_ms << ",\n";
        json << "      \"p95_ms\": " << st.p95_ms << ",\n";
        json << "      \"mean_gbps\": " << st.mean_gbps << ",\n";
        json << "      \"median_gbps\": " << st.median_gbps << ",\n";
        json << "      \"p95_gbps\": " << st.p95_gbps << "\n";
        json << "    }" << (i + 1 < xfer_count ? "," : "") << "\n";
    }
    json << "  ],\n";
    std::cout << "\n";

    // Per-resolution sections (1, 2, 4, 5, 6, 8, 9, 10, 11).
    json << "  \"resolutions\": [\n";
    for (size_t r_idx = 0; r_idx < std::size(hypercine::kResolutions); ++r_idx) {
        const auto& res = hypercine::kResolutions[r_idx];
        const int w = res.width;
        const int h = res.height;
        const int timed_frames = TimedFramesForHeight(h);
        std::cout << "================================================================\n";
        std::cout << "Resolution: " << res.name << " (" << w << "x" << h << ")\n";
        std::cout << "================================================================\n";

        // Section 1: Coverage Audit
        const auto audit = hypercine::AuditPhase2BCoverage(w, h);
        std::cout << "[Section 1 Audit] total_bands=" << audit.total_bands
                  << " gpu_highpass=" << audit.gpu_entropy_bands
                  << " cpu_lowpass=" << audit.cpu_lowpass_bands
                  << " | lowpass_raw_d2h=" << audit.lowpass_raw_d2h_bytes << " B"
                  << " | highpass_comp_d2h=" << audit.highpass_compressed_d2h_bytes << " B"
                  << " (vs " << audit.highpass_raw_equiv_bytes << " B raw int16)"
                  << " | lowpass_cpu_ms=" << audit.lowpass_cpu_ms << " ms (" << audit.lowpass_pct_of_frame << "%)\n";
        if (r_idx == 1) {
            // Print full 30-band table for 4K as canonical example
            std::cout << "  Idx | Band | Dim       | Coeffs  | GPU_Ent | CPU_Ent | Raw_D2H_B | Comp_D2H_B | Final_B | GPU_ms | CPU_ms\n";
            for (const auto& b : audit.bands) {
                std::printf("  %2d  | %-4s | %4dx%-4d | %7d | %-7s | %-7s | %9zu | %10zu | %7zu | %6.3f | %6.3f\n",
                            b.index, b.name.c_str(), b.width, b.height, b.coeff_count,
                            b.gpu_entropy ? "YES" : "NO", b.cpu_entropy ? "YES" : "NO(wrap)",
                            b.raw_coeff_d2h_bytes, b.compressed_payload_d2h_bytes, b.final_band_stream_bytes,
                            b.gpu_entropy_ms, b.cpu_encode_or_wrap_ms);
            }
        }

        // Section 2: Bitstream Identity & SHA-256
        const auto id = hypercine::VerifyPhase2CIdentity(w, h);
        std::cout << "[Section 2 Identity] ok=" << (id.ok ? "PASS" : "FAIL")
                  << " coeff_eq=" << id.pre_entropy_coeff_equal
                  << " band_len_eq=" << id.per_band_payload_len_equal
                  << " band_byte_eq=" << id.per_band_payload_byte_equal
                  << " sample_byte_eq=" << id.complete_sample_byte_equal
                  << " streamed_eq=" << id.streamed_d2h_sample_byte_equal
                  << " multistream_eq=" << id.multistream_sample_byte_equal
                  << " gpu_lp_eq=" << id.gpu_lowpass_sample_byte_equal
                  << " dec_pixel_eq=" << id.upstream_decoder_pixel_equal
                  << " (max_diff=" << id.decoded_max_abs_diff << ", PSNR=" << id.psnr_vs_source_db << " dB)\n";
        std::cout << "  CPU Sample SHA-256 : " << id.cpu_sample_sha256 << "\n";
        std::cout << "  GPU Sample SHA-256 : " << id.gpu_consolidated_sample_sha256 << "\n";
        std::cout << "  Decoded Px SHA-256 : " << id.gpu_decoded_pixels_sha256 << "\n";

        // Section 4: GPU Entropy Concurrency Study
        std::cout << "[Section 4 GPU Entropy Concurrency]\n";
        const auto conc = hypercine::BenchmarkPhase2CEntropyConcurrency(w, h, 8);
        for (const auto& pt : conc) {
            std::printf("  %-32s | streams=%d | wall=%6.2f ms (med=%6.2f, p95=%6.2f) | gpu_ent=%6.2f ms (cons=%5.2f, d2h=%5.2f) | speedup=%4.2fx | match=%s\n",
                        pt.label.c_str(), pt.streams, pt.mean_wall_ms, pt.median_wall_ms, pt.p95_wall_ms,
                        pt.gpu_entropy_ms, pt.gpu_consolidate_ms, pt.compressed_d2h_ms, pt.speedup_vs_1stream,
                        pt.byte_match ? "YES" : "NO");
        }

        // Section 5: Compressed D2H Mode Comparison (FRAME_COMPRESSED_COPY vs BAND_COMPRESSED_STREAM)
        std::cout << "[Section 5 Pipelined Compressed D2H Modes (slots=4, streams=4, DeviceInput)]\n";
        hypercine::Phase2CConfig d2h_cfg_a;
        d2h_cfg_a.width = w; d2h_cfg_a.height = h;
        d2h_cfg_a.warmup_frames = 2; d2h_cfg_a.frames = timed_frames;
        d2h_cfg_a.frame_slots = 4; d2h_cfg_a.entropy_streams = 4;
        d2h_cfg_a.d2h_mode = hypercine::CompressedD2HMode::FrameConsolidated;
        d2h_cfg_a.input_mode = hypercine::Phase2CInputMode::DeviceInput;
        d2h_cfg_a.verify_bitstream = true;
        const auto res_d2h_a = hypercine::RunPhase2C(d2h_cfg_a);

        hypercine::Phase2CConfig d2h_cfg_b = d2h_cfg_a;
        d2h_cfg_b.d2h_mode = hypercine::CompressedD2HMode::BandStreamed;
        const auto res_d2h_b = hypercine::RunPhase2C(d2h_cfg_b);

        std::printf("  A. FRAME_COMPRESSED_COPY  : %7.2f FPS | lat mean=%6.2f med=%6.2f p95=%6.2f ms | comp_d2h=%5.2f ms | cpu_asm=%5.2f ms\n",
                    res_d2h_a.sustained_fps, res_d2h_a.mean_latency_ms, res_d2h_a.median_latency_ms, res_d2h_a.p95_latency_ms,
                    res_d2h_a.compressed_d2h_ms, res_d2h_a.cpu_assembly_total_ms);
        std::printf("  B. BAND_COMPRESSED_STREAM : %7.2f FPS | lat mean=%6.2f med=%6.2f p95=%6.2f ms | comp_d2h=%5.2f ms | cpu_asm=%5.2f ms\n",
                    res_d2h_b.sustained_fps, res_d2h_b.mean_latency_ms, res_d2h_b.median_latency_ms, res_d2h_b.p95_latency_ms,
                    res_d2h_b.compressed_d2h_ms, res_d2h_b.cpu_assembly_total_ms);

        // Section 6: Frame-Slot Count Sweep (1, 2, 3, 4, 6 slots)
        std::cout << "[Section 6 Multi-Frame Slot Sweep (DeviceInput, streams=4, FrameConsolidated)]\n";
        std::vector<hypercine::Phase2CResult> slot_results;
        for (int slots : {1, 2, 3, 4, 6}) {
            hypercine::Phase2CConfig scfg = d2h_cfg_a;
            scfg.frame_slots = slots;
            scfg.verify_bitstream = (slots == 1 || slots == 4);
            const auto sr = hypercine::RunPhase2C(scfg);
            slot_results.push_back(sr);
            std::printf("  slots=%d | %7.2f FPS | period=%6.2f ms | lat mean=%6.2f med=%6.2f p95=%6.2f ms | gpu_util=%5.1f%% cpu_util=%5.1f%% | in_flight=%4.2f | slot_vram=%6.1f MB\n",
                        slots, sr.sustained_fps, sr.frame_period_ms,
                        sr.mean_latency_ms, sr.median_latency_ms, sr.p95_latency_ms,
                        sr.gpu_utilization_pct, sr.cpu_utilization_pct,
                        sr.avg_frames_in_flight, sr.slot_vram_bytes / (1024.0 * 1024.0));
        }

        // Section 11: Lowpass GPU vs CPU comparison
        std::cout << "[Section 11 Lowpass GPU Pack vs CPU Pack (slots=4, streams=4, DeviceInput)]\n";
        hypercine::Phase2CConfig lp_gpu_cfg = d2h_cfg_a;
        lp_gpu_cfg.gpu_pack_lowpass = true;
        const auto res_lp_gpu = hypercine::RunPhase2C(lp_gpu_cfg);
        std::printf("  CPU Lowpass Pack : %7.2f FPS | cpu_lp=%5.3f ms | gpu_wv=%6.2f ms | lp_d2h=%zu B | match=%s\n",
                    res_d2h_a.sustained_fps, res_d2h_a.cpu_lowpass_ms, res_d2h_a.wavelet_ms,
                    res_d2h_a.lowpass_d2h_bytes_per_frame, res_d2h_a.bitstream_byte_match ? "YES" : "NO");
        std::printf("  GPU Lowpass Pack : %7.2f FPS | cpu_lp=%5.3f ms | gpu_wv=%6.2f ms | lp_d2h=%zu B | match=%s\n",
                    res_lp_gpu.sustained_fps, res_lp_gpu.cpu_lowpass_ms, res_lp_gpu.wavelet_ms,
                    res_lp_gpu.lowpass_d2h_bytes_per_frame, res_lp_gpu.bitstream_byte_match ? "YES" : "NO");

        // Sections 8, 9, 10: Complete Encoder Comparison & Bottleneck Profile
        std::cout << "[Sections 8, 9, 10 Full Mode Comparison & Bottleneck Breakdown]\n";
        const auto canonical_bgra = hypercine::MakeFrame(w, h, hypercine::CanonicalPattern::Canonical);
        const auto upstream_res = BenchmarkUpstreamDefault(w, h, canonical_bgra, 1, std::min(timed_frames, 6));

        // Open encoder for Phase 2A and Phase 2B reference runs
        CFHD_EncoderRef enc_ref = nullptr;
        CFHD_OpenEncoder(&enc_ref, nullptr);
        CFHD_PrepareToEncode(enc_ref, w, h, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3);
        CFHD_EncodeSample(enc_ref, const_cast<uint8_t*>(canonical_bgra.data()), w * 4);
        auto* sample_enc = reinterpret_cast<CSampleEncoder*>(enc_ref);
        const size_t scratch_sz = std::max<size_t>(64u << 20, static_cast<size_t>(w) * static_cast<size_t>(h) * 8u);
        std::vector<uint8_t> scratch(scratch_sz, 0);

        hypercine::Phase2Config p2a_cfg;
        p2a_cfg.input = hypercine::Phase2Input::DeviceBgra;
        p2a_cfg.transport = hypercine::Phase2Transport::CompactStream;
        p2a_cfg.pipeline_depth = 3;
        p2a_cfg.workers = 8;
        p2a_cfg.warmup = 1;
        p2a_cfg.frames = std::min(timed_frames, 8);
        p2a_cfg.verify = false;
        const auto p2a_run = hypercine::RunPhase2(reinterpret_cast<encoder*>(sample_enc->m_encoder),
                                                  reinterpret_cast<transform**>(sample_enc->m_transformArray),
                                                  sample_enc->m_channelCount, canonical_bgra.data(), w * 4,
                                                  scratch.data(), scratch.size(), p2a_cfg);

        hypercine::Phase2BConfig p2b_cfg;
        p2b_cfg.input = hypercine::Phase2Input::DeviceBgra;
        p2b_cfg.dispatch = hypercine::GpuEntropyDispatch::OneStream;
        p2b_cfg.streams = 1;
        p2b_cfg.warmup = 1;
        p2b_cfg.frames = std::min(timed_frames, 8);
        p2b_cfg.verify = false;
        const auto p2b_run = hypercine::RunPhase2B(reinterpret_cast<encoder*>(sample_enc->m_encoder),
                                                   reinterpret_cast<transform**>(sample_enc->m_transformArray),
                                                   sample_enc->m_channelCount, canonical_bgra.data(), w * 4,
                                                   scratch.data(), scratch.size(), p2b_cfg);
        CFHD_CloseEncoder(enc_ref);

        hypercine::Phase2CConfig p2c_host_cfg = d2h_cfg_a;
        p2c_host_cfg.input_mode = hypercine::Phase2CInputMode::HostInput;
        p2c_host_cfg.frame_slots = 4;
        const auto p2c_host = hypercine::RunPhase2C(p2c_host_cfg);

        // Detailed bottleneck profiling run on PHASE2C_DEVICE_PIPELINED
        hypercine::Phase2CConfig p2c_prof_cfg = d2h_cfg_a;
        p2c_prof_cfg.record_substage_events = true;
        p2c_prof_cfg.frames = 6;
        const auto p2c_prof = hypercine::RunPhase2C(p2c_prof_cfg);
        const auto& p2c_dev = res_d2h_a;

        std::printf("  1. UPSTREAM_DEFAULT             : %7.2f FPS | lat=%6.2f ms (med=%6.2f, p95=%6.2f) | size=%zu B\n",
                    upstream_res.fps, upstream_res.mean_ms, upstream_res.median_ms, upstream_res.p95_ms, upstream_res.bitstream_bytes);
        std::printf("  2. PHASE2A_DEVICE_PIPELINED     : %7.2f FPS | lat=%6.2f ms | H2D=%5.2f MB D2H=%5.2f MB | GPU=%6.2f ms CPU=%6.2f ms | size=%zu B\n",
                    p2a_run.sustained_fps, p2a_run.mean_latency_ms,
                    p2a_run.h2d_bytes / 1e6, p2a_run.d2h_bytes / 1e6,
                    p2a_run.mean_color_ms + p2a_run.mean_gpu_ms + p2a_run.mean_range_ms + p2a_run.mean_pack_ms,
                    p2a_run.mean_entropy_ms + p2a_run.mean_assembly_ms, p2a_run.encoded_bytes);
        std::printf("  3. PHASE2B_DEVICE_SINGLE_STREAM : %7.2f FPS | lat=%6.2f ms | H2D=%5.2f MB D2H=%5.2f MB | GPU=%6.2f ms CPU=%6.2f ms | size=%zu B\n",
                    p2b_run.sustained_fps, p2b_run.mean_latency_ms,
                    p2b_run.h2d_bytes / 1e6, p2b_run.d2h_bytes / 1e6,
                    p2b_run.mean_color_ms + p2b_run.mean_gpu_ms + p2b_run.entropy.total_ms - p2b_run.entropy.header_ms - p2b_run.entropy.lowpass_ms,
                    p2b_run.entropy.header_ms + p2b_run.entropy.lowpass_ms + p2b_run.mean_assembly_ms, p2b_run.encoded_bytes);
        std::printf("  4. PHASE2C_HOST_PIPELINED       : %7.2f FPS | lat=%6.2f ms (med=%6.2f, p95=%6.2f) | H2D=%5.2f MB D2H=%5.2f MB | GPU=%6.2f ms CPU=%6.2f ms | PSNR=%5.2f dB\n",
                    p2c_host.sustained_fps, p2c_host.mean_latency_ms, p2c_host.median_latency_ms, p2c_host.p95_latency_ms,
                    p2c_host.h2d_bytes_per_frame / 1e6, p2c_host.total_d2h_bytes_per_frame / 1e6,
                    p2c_host.total_gpu_ms, p2c_host.cpu_assembly_total_ms, p2c_host.psnr_db);
        std::printf("  5. PHASE2C_DEVICE_PIPELINED     : %7.2f FPS | lat=%6.2f ms (med=%6.2f, p95=%6.2f) | H2D=%5.2f MB D2H=%5.2f MB | GPU=%6.2f ms CPU=%6.2f ms | PSNR=%5.2f dB\n",
                    p2c_dev.sustained_fps, p2c_dev.mean_latency_ms, p2c_dev.median_latency_ms, p2c_dev.p95_latency_ms,
                    p2c_dev.h2d_bytes_per_frame / 1e6, p2c_dev.total_d2h_bytes_per_frame / 1e6,
                    p2c_dev.total_gpu_ms, p2c_dev.cpu_assembly_total_ms, p2c_dev.psnr_db);

        std::printf("  [Section 10 Bottleneck Profile (DeviceInput, period=%6.2f ms)]:\n"
                    "    GPU Color+Wavelet+Quant : %6.2f ms (color=%5.2f, wavelet+quant=%5.2f)\n"
                    "    GPU Entropy Wall (4s)   : %6.2f ms [sum across 27 bands: block_scan=%5.2f, merge_carry=%5.2f, gather=%5.2f, token=%5.2f, codebook=%5.2f, prefix=%5.2f, pack=%5.2f, consolidate=%5.2f]\n"
                    "    PCIe D2H (Comp + LP)    : %6.2f ms (compressed=%5.2f, lowpass=%5.2f)\n"
                    "    CPU Assembly (Worker)   : %6.2f ms (lowpass=%5.3f, header_wrap=%5.3f, sample_concat=%5.3f)\n"
                    "    CPU Dispatch/Event Poll : %6.2f ms\n\n",
                    p2c_dev.frame_period_ms,
                    p2c_dev.color_ms + p2c_dev.wavelet_ms, p2c_dev.color_ms, p2c_dev.wavelet_ms,
                    p2c_dev.gpu_entropy_ms,
                    p2c_prof.entropy_substage_sum.block_scan_ms, p2c_prof.entropy_substage_sum.merge_carry_ms,
                    p2c_prof.entropy_substage_sum.gather_ms, p2c_prof.entropy_substage_sum.token_ms,
                    p2c_prof.entropy_substage_sum.codebook_ms, p2c_prof.entropy_substage_sum.prefix_ms,
                    p2c_prof.entropy_substage_sum.pack_ms, p2c_dev.gpu_consolidate_ms,
                    p2c_dev.total_d2h_ms, p2c_dev.compressed_d2h_ms, p2c_dev.lowpass_d2h_ms,
                    p2c_dev.cpu_assembly_total_ms, p2c_dev.cpu_lowpass_ms, p2c_dev.cpu_header_wrap_ms, p2c_dev.cpu_sample_concat_ms,
                    p2c_dev.cpu_dispatch_poll_ms);

        // Emit JSON for this resolution
        json << "    {\n";
        json << "      \"name\": \"" << res.name << "\",\n";
        json << "      \"width\": " << w << ",\n";
        json << "      \"height\": " << h << ",\n";
        json << "      \"audit\": {\n";
        json << "        \"total_bands\": " << audit.total_bands << ",\n";
        json << "        \"gpu_entropy_bands\": " << audit.gpu_entropy_bands << ",\n";
        json << "        \"cpu_lowpass_bands\": " << audit.cpu_lowpass_bands << ",\n";
        json << "        \"lowpass_raw_d2h_bytes\": " << audit.lowpass_raw_d2h_bytes << ",\n";
        json << "        \"lowpass_encoded_bytes\": " << audit.lowpass_encoded_bytes << ",\n";
        json << "        \"highpass_raw_equiv_bytes\": " << audit.highpass_raw_equiv_bytes << ",\n";
        json << "        \"highpass_compressed_d2h_bytes\": " << audit.highpass_compressed_d2h_bytes << ",\n";
        json << "        \"highpass_encoded_bytes\": " << audit.highpass_encoded_bytes << ",\n";
        json << "        \"total_d2h_bytes\": " << audit.total_d2h_bytes << ",\n";
        json << "        \"lowpass_cpu_ms\": " << audit.lowpass_cpu_ms << ",\n";
        json << "        \"highpass_header_cpu_ms\": " << audit.highpass_header_cpu_ms << ",\n";
        json << "        \"assembly_cpu_ms\": " << audit.assembly_cpu_ms << ",\n";
        json << "        \"total_frame_ms\": " << audit.total_frame_ms << ",\n";
        json << "        \"lowpass_pct_of_frame\": " << audit.lowpass_pct_of_frame << ",\n";
        json << "        \"bands\": [\n";
        for (size_t bi = 0; bi < audit.bands.size(); ++bi) {
            const auto& b = audit.bands[bi];
            const auto& ib = id.bands[bi];
            json << "          {\"index\": " << b.index << ", \"name\": \"" << b.name << "\", \"channel\": " << b.channel
                 << ", \"level\": " << b.level << ", \"band\": " << b.band << ", \"subband\": " << b.subband
                 << ", \"lowpass\": " << (b.lowpass ? "true" : "false")
                 << ", \"width\": " << b.width << ", \"height\": " << b.height << ", \"coeffs\": " << b.coeff_count
                 << ", \"gpu_entropy\": " << (b.gpu_entropy ? "true" : "false")
                 << ", \"cpu_entropy\": " << (b.cpu_entropy ? "true" : "false")
                 << ", \"raw_d2h_bytes\": " << b.raw_coeff_d2h_bytes
                 << ", \"compressed_d2h_bytes\": " << b.compressed_payload_d2h_bytes
                 << ", \"final_bytes\": " << b.final_band_stream_bytes
                 << ", \"gpu_ms\": " << b.gpu_entropy_ms << ", \"cpu_ms\": " << b.cpu_encode_or_wrap_ms
                 << ", \"coeff_sha256\": \"" << ib.coeff_sha256 << "\""
                 << ", \"cpu_payload_sha256\": \"" << ib.cpu_payload_sha256 << "\""
                 << ", \"gpu_payload_sha256\": \"" << ib.gpu_payload_sha256 << "\"}"
                 << (bi + 1 < audit.bands.size() ? "," : "") << "\n";
        }
        json << "        ]\n";
        json << "      },\n";
        json << "      \"identity\": {\n";
        json << "        \"ok\": " << (id.ok ? "true" : "false") << ",\n";
        json << "        \"pre_entropy_coeff_equal\": " << (id.pre_entropy_coeff_equal ? "true" : "false") << ",\n";
        json << "        \"per_band_payload_byte_equal\": " << (id.per_band_payload_byte_equal ? "true" : "false") << ",\n";
        json << "        \"per_band_payload_len_equal\": " << (id.per_band_payload_len_equal ? "true" : "false") << ",\n";
        json << "        \"complete_sample_byte_equal\": " << (id.complete_sample_byte_equal ? "true" : "false") << ",\n";
        json << "        \"streamed_d2h_sample_byte_equal\": " << (id.streamed_d2h_sample_byte_equal ? "true" : "false") << ",\n";
        json << "        \"multistream_sample_byte_equal\": " << (id.multistream_sample_byte_equal ? "true" : "false") << ",\n";
        json << "        \"gpu_lowpass_sample_byte_equal\": " << (id.gpu_lowpass_sample_byte_equal ? "true" : "false") << ",\n";
        json << "        \"upstream_decoder_pixel_equal\": " << (id.upstream_decoder_pixel_equal ? "true" : "false") << ",\n";
        json << "        \"sample_bytes\": " << id.sample_bytes << ",\n";
        json << "        \"psnr_vs_source_db\": " << id.psnr_vs_source_db << ",\n";
        json << "        \"cpu_sample_sha256\": \"" << id.cpu_sample_sha256 << "\",\n";
        json << "        \"gpu_consolidated_sample_sha256\": \"" << id.gpu_consolidated_sample_sha256 << "\",\n";
        json << "        \"gpu_streamed_sample_sha256\": \"" << id.gpu_streamed_sample_sha256 << "\",\n";
        json << "        \"gpu_lowpass_sample_sha256\": \"" << id.gpu_lowpass_sample_sha256 << "\",\n";
        json << "        \"decoded_pixels_sha256\": \"" << id.gpu_decoded_pixels_sha256 << "\"\n";
        json << "      },\n";
        json << "      \"entropy_concurrency\": [\n";
        for (size_t ci = 0; ci < conc.size(); ++ci) {
            const auto& pt = conc[ci];
            json << "        {\"label\": \"" << pt.label << "\", \"streams\": " << pt.streams
                 << ", \"mean_wall_ms\": " << pt.mean_wall_ms << ", \"median_wall_ms\": " << pt.median_wall_ms
                 << ", \"p95_wall_ms\": " << pt.p95_wall_ms << ", \"gpu_entropy_ms\": " << pt.gpu_entropy_ms
                 << ", \"gpu_consolidate_ms\": " << pt.gpu_consolidate_ms << ", \"compressed_d2h_ms\": " << pt.compressed_d2h_ms
                 << ", \"speedup_vs_1s\": " << pt.speedup_vs_1stream
                 << ", \"max_active_blocks_per_sm\": " << pt.max_active_blocks_per_sm
                 << ", \"effective_gbps\": " << pt.effective_gbps
                 << ", \"block_scan_ms\": " << pt.substage_sum.block_scan_ms
                 << ", \"merge_carry_ms\": " << pt.substage_sum.merge_carry_ms
                 << ", \"gather_ms\": " << pt.substage_sum.gather_ms
                 << ", \"token_ms\": " << pt.substage_sum.token_ms
                 << ", \"codebook_ms\": " << pt.substage_sum.codebook_ms
                 << ", \"prefix_ms\": " << pt.substage_sum.prefix_ms
                 << ", \"pack_ms\": " << pt.substage_sum.pack_ms
                 << ", \"byte_match\": " << (pt.byte_match ? "true" : "false") << "}"
                 << (ci + 1 < conc.size() ? "," : "") << "\n";
        }
        json << "      ],\n";
        json << "      \"d2h_modes\": {\n";
        json << "        \"FRAME_COMPRESSED_COPY\": {\"fps\": " << res_d2h_a.sustained_fps
             << ", \"mean_latency_ms\": " << res_d2h_a.mean_latency_ms
             << ", \"median_latency_ms\": " << res_d2h_a.median_latency_ms
             << ", \"p95_latency_ms\": " << res_d2h_a.p95_latency_ms
             << ", \"compressed_d2h_ms\": " << res_d2h_a.compressed_d2h_ms
             << ", \"cpu_assembly_ms\": " << res_d2h_a.cpu_assembly_total_ms << "},\n";
        json << "        \"BAND_COMPRESSED_STREAM\": {\"fps\": " << res_d2h_b.sustained_fps
             << ", \"mean_latency_ms\": " << res_d2h_b.mean_latency_ms
             << ", \"median_latency_ms\": " << res_d2h_b.median_latency_ms
             << ", \"p95_latency_ms\": " << res_d2h_b.p95_latency_ms
             << ", \"compressed_d2h_ms\": " << res_d2h_b.compressed_d2h_ms
             << ", \"cpu_assembly_ms\": " << res_d2h_b.cpu_assembly_total_ms << "}\n";
        json << "      },\n";
        json << "      \"slot_sweep\": [\n";
        for (size_t si = 0; si < slot_results.size(); ++si) {
            const auto& sr = slot_results[si];
            json << "        {\"slots\": " << sr.config.frame_slots
                 << ", \"sustained_fps\": " << sr.sustained_fps
                 << ", \"frame_period_ms\": " << sr.frame_period_ms
                 << ", \"mean_latency_ms\": " << sr.mean_latency_ms
                 << ", \"median_latency_ms\": " << sr.median_latency_ms
                 << ", \"p95_latency_ms\": " << sr.p95_latency_ms
                 << ", \"gpu_util_pct\": " << sr.gpu_utilization_pct
                 << ", \"cpu_util_pct\": " << sr.cpu_utilization_pct
                 << ", \"avg_frames_in_flight\": " << sr.avg_frames_in_flight
                 << ", \"slot_vram_mb\": " << (sr.slot_vram_bytes / (1024.0 * 1024.0))
                 << ", \"vram_used_mb\": " << (sr.vram_used_bytes / (1024.0 * 1024.0)) << "}"
                 << (si + 1 < slot_results.size() ? "," : "") << "\n";
        }
        json << "      ],\n";
        json << "      \"lowpass_comparison\": {\n";
        json << "        \"cpu_pack\": {\"fps\": " << res_d2h_a.sustained_fps << ", \"cpu_lowpass_ms\": " << res_d2h_a.cpu_lowpass_ms
             << ", \"gpu_wavelet_ms\": " << res_d2h_a.wavelet_ms << ", \"lowpass_d2h_bytes\": " << res_d2h_a.lowpass_d2h_bytes_per_frame << "},\n";
        json << "        \"gpu_pack\": {\"fps\": " << res_lp_gpu.sustained_fps << ", \"cpu_lowpass_ms\": " << res_lp_gpu.cpu_lowpass_ms
             << ", \"gpu_wavelet_ms\": " << res_lp_gpu.wavelet_ms << ", \"lowpass_d2h_bytes\": " << res_lp_gpu.lowpass_d2h_bytes_per_frame << "}\n";
        json << "      },\n";
        json << "      \"mode_comparison\": {\n";
        json << "        \"UPSTREAM_DEFAULT\": {\"fps\": " << upstream_res.fps << ", \"mean_latency_ms\": " << upstream_res.mean_ms
             << ", \"median_latency_ms\": " << upstream_res.median_ms << ", \"p95_latency_ms\": " << upstream_res.p95_ms
             << ", \"bitstream_bytes\": " << upstream_res.bitstream_bytes << "},\n";
        json << "        \"PHASE2A_DEVICE_PIPELINED\": {\"fps\": " << p2a_run.sustained_fps << ", \"mean_latency_ms\": " << p2a_run.mean_latency_ms
             << ", \"h2d_mb\": " << (p2a_run.h2d_bytes / 1e6) << ", \"d2h_mb\": " << (p2a_run.d2h_bytes / 1e6)
             << ", \"gpu_ms\": " << (p2a_run.mean_color_ms + p2a_run.mean_gpu_ms + p2a_run.mean_range_ms + p2a_run.mean_pack_ms)
             << ", \"cpu_ms\": " << (p2a_run.mean_entropy_ms + p2a_run.mean_assembly_ms)
             << ", \"bitstream_bytes\": " << p2a_run.encoded_bytes << "},\n";
        json << "        \"PHASE2B_DEVICE_SINGLE_STREAM\": {\"fps\": " << p2b_run.sustained_fps << ", \"mean_latency_ms\": " << p2b_run.mean_latency_ms
             << ", \"h2d_mb\": " << (p2b_run.h2d_bytes / 1e6) << ", \"d2h_mb\": " << (p2b_run.d2h_bytes / 1e6)
             << ", \"gpu_ms\": " << (p2b_run.mean_color_ms + p2b_run.mean_gpu_ms + p2b_run.entropy.total_ms - p2b_run.entropy.header_ms - p2b_run.entropy.lowpass_ms)
             << ", \"cpu_ms\": " << (p2b_run.entropy.header_ms + p2b_run.entropy.lowpass_ms + p2b_run.mean_assembly_ms)
             << ", \"bitstream_bytes\": " << p2b_run.encoded_bytes << "},\n";
        json << "        \"PHASE2C_HOST_PIPELINED\": {\"fps\": " << p2c_host.sustained_fps
             << ", \"mean_latency_ms\": " << p2c_host.mean_latency_ms << ", \"median_latency_ms\": " << p2c_host.median_latency_ms
             << ", \"p95_latency_ms\": " << p2c_host.p95_latency_ms
             << ", \"h2d_mb\": " << (p2c_host.h2d_bytes_per_frame / 1e6) << ", \"d2h_mb\": " << (p2c_host.total_d2h_bytes_per_frame / 1e6)
             << ", \"gpu_ms\": " << p2c_host.total_gpu_ms << ", \"cpu_ms\": " << p2c_host.cpu_assembly_total_ms
             << ", \"bitstream_bytes\": " << p2c_host.bitstream_bytes << ", \"psnr_db\": " << p2c_host.psnr_db
             << ", \"byte_match\": " << (p2c_host.bitstream_byte_match ? "true" : "false") << "},\n";
        json << "        \"PHASE2C_DEVICE_PIPELINED\": {\"fps\": " << p2c_dev.sustained_fps
             << ", \"mean_latency_ms\": " << p2c_dev.mean_latency_ms << ", \"median_latency_ms\": " << p2c_dev.median_latency_ms
             << ", \"p95_latency_ms\": " << p2c_dev.p95_latency_ms
             << ", \"h2d_mb\": " << (p2c_dev.h2d_bytes_per_frame / 1e6) << ", \"d2h_mb\": " << (p2c_dev.total_d2h_bytes_per_frame / 1e6)
             << ", \"gpu_ms\": " << p2c_dev.total_gpu_ms << ", \"cpu_ms\": " << p2c_dev.cpu_assembly_total_ms
             << ", \"bitstream_bytes\": " << p2c_dev.bitstream_bytes << ", \"psnr_db\": " << p2c_dev.psnr_db
             << ", \"byte_match\": " << (p2c_dev.bitstream_byte_match ? "true" : "false") << "}\n";
        json << "      },\n";
        json << "      \"bottleneck_breakdown\": {\n";
        json << "        \"frame_period_ms\": " << p2c_dev.frame_period_ms << ",\n";
        json << "        \"gpu_color_ms\": " << p2c_dev.color_ms << ",\n";
        json << "        \"gpu_wavelet_quant_ms\": " << p2c_dev.wavelet_ms << ",\n";
        json << "        \"gpu_entropy_wall_ms\": " << p2c_dev.gpu_entropy_ms << ",\n";
        json << "        \"gpu_consolidate_ms\": " << p2c_dev.gpu_consolidate_ms << ",\n";
        json << "        \"compressed_d2h_ms\": " << p2c_dev.compressed_d2h_ms << ",\n";
        json << "        \"lowpass_d2h_ms\": " << p2c_dev.lowpass_d2h_ms << ",\n";
        json << "        \"cpu_lowpass_ms\": " << p2c_dev.cpu_lowpass_ms << ",\n";
        json << "        \"cpu_header_wrap_ms\": " << p2c_dev.cpu_header_wrap_ms << ",\n";
        json << "        \"cpu_sample_concat_ms\": " << p2c_dev.cpu_sample_concat_ms << ",\n";
        json << "        \"cpu_dispatch_poll_ms\": " << p2c_dev.cpu_dispatch_poll_ms << ",\n";
        json << "        \"entropy_substages_sum\": {\n";
        json << "          \"block_scan_ms\": " << p2c_prof.entropy_substage_sum.block_scan_ms << ",\n";
        json << "          \"merge_carry_ms\": " << p2c_prof.entropy_substage_sum.merge_carry_ms << ",\n";
        json << "          \"gather_ms\": " << p2c_prof.entropy_substage_sum.gather_ms << ",\n";
        json << "          \"token_ms\": " << p2c_prof.entropy_substage_sum.token_ms << ",\n";
        json << "          \"codebook_ms\": " << p2c_prof.entropy_substage_sum.codebook_ms << ",\n";
        json << "          \"prefix_ms\": " << p2c_prof.entropy_substage_sum.prefix_ms << ",\n";
        json << "          \"pack_ms\": " << p2c_prof.entropy_substage_sum.pack_ms << "\n";
        json << "        }\n";
        json << "      }\n";
        json << "    }" << (r_idx + 1 < std::size(hypercine::kResolutions) ? "," : "") << "\n";
    }
    json << "  ]\n";
    json << "}\n";

    if (!hypercine::bench::WriteCompleteFile(json_path, json.str())) {
        return 1;
    }
    std::cout << "Wrote " << json_path << "\n";
    return 0;
}
