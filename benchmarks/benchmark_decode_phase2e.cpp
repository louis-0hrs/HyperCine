// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <thread>
#include <cuda_runtime.h>

#include "../tests/decode_phase0_support.h"
#include "../tests/decode_phase1_support.h"
#include "hypercine/decode_phase2d.h"
#include "hypercine/decode_phase2e.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;
using Clock = std::chrono::steady_clock;

size_t PeakMemory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pm{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm))) return pm.PeakWorkingSetSize;
#endif
    return 0;
}

size_t PeakGpuMemory() {
    size_t free_bytes = 0, total_bytes = 0;
    if (cudaMemGetInfo(&free_bytes, &total_bytes) == cudaSuccess) {
        return total_bytes - free_bytes;
    }
    return 0;
}

struct PatternBenchmarkResult {
    std::string pattern_name;
    double cpu_total_ms;
    double p2d_host_index_ms;
    double p2d_gpu_replay_ms;
    double p2d_total_ms;
    double p2e_a_total_ms;
    double p2e_b_host_index_wall_ms;
    double p2e_b_gpu_replay_wall_ms;
    double p2e_b_inv_wall_ms;
    double p2e_b_total_ms;
    double overlap_saved_ms;
    double overlap_efficiency;
    double speedup_vs_2d;
    double ratio_vs_cpu;
};

struct StreamScalingResult {
    int streams;
    double gpu_replay_wall_ms;
    double total_decode_ms;
    double overlap_efficiency;
    double fps;
};

struct WorkerScalingResult {
    int workers;
    double host_index_wall_ms;
    double total_decode_ms;
    double fps;
};

struct PolicyComparisonResult {
    std::string test_name;
    std::string policy_name;
    double host_index_wall_ms;
    double first_gpu_ms;
    double first_inv_ms;
    double total_decode_ms;
};

struct SlotScalingResult {
    int slots;
    double fps;
    double megapixels_per_sec;
    double latency_ms;
};

struct Mandatory4kNoiseBreakdown {
    double p2d_index_wall_ms = 0;
    double p2d_replay_wall_ms = 0;
    double p2d_total_ms = 0;
    double p2e_overlap_saved_ms = 0;
    double p2e_overlap_efficiency = 0;
    double first_gpu_launch_ms = 0;
    double final_host_index_ms = 0;
    double first_inverse_launch_ms = 0;
    double final_entropy_done_ms = 0;
    double p2e_final_latency_ms = 0;
};

} // namespace

int main(int argc, char** argv) {
    try {
        std::cout << "=== HyperCine Decode Phase 2E End-to-End Benchmark ===" << std::endl;

        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, 0);
        std::cout << "GPU Device: " << prop.name << " (CC " << prop.major << "." << prop.minor
                  << ", " << (prop.totalGlobalMem / (1024 * 1024)) << " MB VRAM)" << std::endl;
        unsigned int hardware_concurrency = std::thread::hardware_concurrency();
        std::cout << "CPU Hardware Concurrency: " << hardware_concurrency << " threads\n" << std::endl;

        std::string output = "benchmarks/results/decode_phase2e_windows_msvc.json";
        std::string resolution = "all";

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (arg == "--output") output = value;
            else if (arg == "--resolution") resolution = value;
            else throw std::runtime_error("unknown option: " + arg);
        }

        std::filesystem::path out_path = std::filesystem::absolute(output);
        if (out_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }

        std::ofstream json(out_path, std::ios::out | std::ios::trunc);
        if (!json.is_open()) {
            std::cerr << "Failed to open output file: " << out_path.string() << " (parent exists: "
                      << std::filesystem::exists(out_path.parent_path()) << ", errno: " << errno << ")" << std::endl;
            return 1;
        }

        json << "{\n"
             << "  \"benchmark\": \"HyperCine Decode Phase 2E\",\n"
             << "  \"gpu\": \"" << prop.name << "\",\n"
             << "  \"compute_capability\": \"" << prop.major << "." << prop.minor << "\",\n"
             << "  \"total_vram_mb\": " << (prop.totalGlobalMem / (1024 * 1024)) << ",\n"
             << "  \"cpu_hardware_concurrency\": " << hardware_concurrency << ",\n"
             << "  \"resolutions\": [\n";

        struct ResConfig {
            const char* name;
            int width;
            int height;
            int patterns;
        };

        std::vector<ResConfig> configs;
        if (resolution == "all" || resolution == "1080p") configs.push_back({ "1080p", 1920, 1080, 5 });
        if (resolution == "all" || resolution == "4k")    configs.push_back({ "4k", 3840, 2160, 5 });
        if (resolution == "all" || resolution == "8k")    configs.push_back({ "8k", 7680, 4320, 1 });

        std::vector<StreamScalingResult> stream_scaling_1080p;
        std::vector<StreamScalingResult> stream_scaling_4k;
        std::vector<WorkerScalingResult> worker_scaling_1080p;
        std::vector<WorkerScalingResult> worker_scaling_4k;
        std::vector<PolicyComparisonResult> policy_results;
        std::vector<SlotScalingResult> slot_scaling_1080p;
        std::vector<SlotScalingResult> slot_scaling_4k;
        Mandatory4kNoiseBreakdown noise_4k_breakdown{};

        bool first_res = true;

        for (const auto& cfg : configs) {
            int w = cfg.width;
            int h = cfg.height;
            double megapixels = (w * h) / 1000000.0;

            std::cout << "\n========================================================" << std::endl;
            std::cout << "Benchmarking Resolution: " << cfg.name << " (" << w << "x" << h << ")" << std::endl;
            std::cout << "========================================================" << std::endl;

            auto samples = Generate(w, h, cfg.patterns);

            CudaDecoderPipelinePhase2D pipeline_2d;
            pipeline_2d.Initialize(w, h, 3, 10, 0);

            CudaDecoderPipelinePhase2E pipeline_2e;
            pipeline_2e.Initialize(w, h, 3, 10, 0, 4);

            std::vector<PatternBenchmarkResult> pattern_results;

            double p2d_fps_sum = 0, p2e_a_fps_sum = 0, p2e_b_fps_sum = 0;
            double p2d_lat_sum = 0, p2e_a_lat_sum = 0, p2e_b_lat_sum = 0;

            for (int p = 0; p < cfg.patterns; ++p) {
                auto& s = samples[p];
                std::string pat_name = PatternName(p);

                // 1. CPU Reference
                ReferenceDecoder ref_bgra(s, CFHD_PIXEL_FORMAT_BGRA);
                int cpu_runs = (w == 7680) ? 3 : ((w == 3840 && p == 2) ? 3 : 5);
                double cpu_sum_ms = 0;
                for (int r = 0; r < cpu_runs; ++r) {
                    auto t0 = Clock::now();
                    ref_bgra.Decode(s);
                    auto t1 = Clock::now();
                    cpu_sum_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                }
                double cpu_total_ms = cpu_sum_ms / cpu_runs;

                // 2. Warmup pipelines
                uint64_t tw_2d = pipeline_2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2d.WaitForDecode(tw_2d);
                pipeline_2d.ReleaseOutput(tw_2d);

                uint64_t tw_2e = pipeline_2e.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2e.WaitForDecode(tw_2e);
                pipeline_2e.ReleaseOutput(tw_2e);

                // 3. Phase 2D Baseline
                int runs = (w == 7680) ? 5 : ((w == 3840 && p == 2) ? 10 : 15);
                double p2d_tot_sum = 0, p2d_idx_sum = 0, p2d_vlc_sum = 0;
                for (int r = 0; r < runs; ++r) {
                    auto t0 = Clock::now();
                    uint64_t t = pipeline_2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    pipeline_2d.WaitForDecode(t);
                    auto t1 = Clock::now();
                    p2d_tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();

                    Phase2dSlotStageTimings timings{};
                    pipeline_2d.GetStageTimings(t, &timings);
                    p2d_idx_sum += timings.host_index_ms;
                    p2d_vlc_sum += timings.gpu_vlc_ms;
                    pipeline_2d.ReleaseOutput(t);
                }
                double p2d_total_ms = p2d_tot_sum / runs;
                double p2d_idx_ms = p2d_idx_sum / runs;
                double p2d_vlc_ms = p2d_vlc_sum / runs;

                // 4. Phase 2E Variant A (Entropy Overlap, inverse barrier retained)
                pipeline_2e.SetPipelineMode(Phase2EPipelineMode::Phase2EEntropyOverlap);
                double p2e_a_tot_sum = 0;
                for (int r = 0; r < runs; ++r) {
                    auto t0 = Clock::now();
                    uint64_t t = pipeline_2e.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    pipeline_2e.WaitForDecode(t);
                    auto t1 = Clock::now();
                    p2e_a_tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();
                    pipeline_2e.ReleaseOutput(t);
                }
                double p2e_a_total_ms = p2e_a_tot_sum / runs;

                // 5. Phase 2E Variant B (Full Overlap: Index + GPU Replay + Inverse DAG)
                pipeline_2e.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
                double p2e_b_tot_sum = 0, p2e_b_idx_sum = 0, p2e_b_rep_sum = 0, p2e_b_inv_sum = 0;
                double p2e_b_saved_sum = 0, p2e_b_eff_sum = 0;
                Phase2eSlotStageTimings last_b_tm{};

                for (int r = 0; r < runs; ++r) {
                    auto t0 = Clock::now();
                    uint64_t t = pipeline_2e.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    pipeline_2e.WaitForDecode(t);
                    auto t1 = Clock::now();
                    p2e_b_tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();

                    Phase2eSlotStageTimings timings{};
                    pipeline_2e.GetStageTimings(t, &timings);
                    p2e_b_idx_sum += timings.host_index_wall_ms;
                    p2e_b_rep_sum += timings.gpu_replay_wall_ms;
                    p2e_b_inv_sum += timings.inverse_wavelet_wall_ms;
                    p2e_b_saved_sum += timings.overlap_saved_ms;
                    p2e_b_eff_sum += timings.overlap_efficiency;
                    last_b_tm = timings;

                    // Export timeline JSON for 1080p noise, 4K noise, 4K texture on first iteration
                    if (r == 0) {
                        if (w == 1920 && p == 2) {
                            std::ofstream tjson(std::filesystem::absolute("benchmarks/results/timeline_1080p_noise.json"));
                            tjson << pipeline_2e.ExportTimelineJson(t);
                        } else if (w == 3840 && p == 2) {
                            std::ofstream tjson(std::filesystem::absolute("benchmarks/results/timeline_4k_noise.json"));
                            tjson << pipeline_2e.ExportTimelineJson(t);
                        } else if (w == 3840 && p == 3) {
                            std::ofstream tjson(std::filesystem::absolute("benchmarks/results/timeline_4k_texture.json"));
                            tjson << pipeline_2e.ExportTimelineJson(t);
                        }
                    }

                    pipeline_2e.ReleaseOutput(t);
                }

                double p2e_b_total_ms = p2e_b_tot_sum / runs;
                double p2e_b_idx_ms = p2e_b_idx_sum / runs;
                double p2e_b_rep_ms = p2e_b_rep_sum / runs;
                double p2e_b_inv_ms = p2e_b_inv_sum / runs;
                double p2e_b_saved_ms = p2e_b_saved_sum / runs;
                double p2e_b_eff = p2e_b_eff_sum / runs;

                if (w == 3840 && p == 2) {
                    noise_4k_breakdown.p2d_index_wall_ms = p2d_idx_ms;
                    noise_4k_breakdown.p2d_replay_wall_ms = p2d_vlc_ms;
                    noise_4k_breakdown.p2d_total_ms = p2d_total_ms;
                    noise_4k_breakdown.p2e_overlap_saved_ms = p2e_b_saved_ms;
                    noise_4k_breakdown.p2e_overlap_efficiency = p2e_b_eff;
                    noise_4k_breakdown.first_gpu_launch_ms = last_b_tm.t_first_gpu_launch_ms;
                    noise_4k_breakdown.final_host_index_ms = last_b_tm.t_final_index_end_ms;
                    noise_4k_breakdown.first_inverse_launch_ms = last_b_tm.t_first_inverse_start_ms;
                    noise_4k_breakdown.final_entropy_done_ms = last_b_tm.t_final_replay_end_ms;
                    noise_4k_breakdown.p2e_final_latency_ms = p2e_b_total_ms;
                }

                double speedup_vs_2d = (p2e_b_total_ms > 0) ? (p2d_total_ms / p2e_b_total_ms) : 1.0;
                double ratio_vs_cpu = (p2e_b_total_ms > 0) ? (cpu_total_ms / p2e_b_total_ms) : 1.0;

                PatternBenchmarkResult res{
                    pat_name,
                    cpu_total_ms,
                    p2d_idx_ms,
                    p2d_vlc_ms,
                    p2d_total_ms,
                    p2e_a_total_ms,
                    p2e_b_idx_ms,
                    p2e_b_rep_ms,
                    p2e_b_inv_ms,
                    p2e_b_total_ms,
                    p2e_b_saved_ms,
                    p2e_b_eff,
                    speedup_vs_2d,
                    ratio_vs_cpu
                };
                pattern_results.push_back(res);

                p2d_fps_sum += (1000.0 / p2d_total_ms);
                p2e_a_fps_sum += (1000.0 / p2e_a_total_ms);
                p2e_b_fps_sum += (1000.0 / p2e_b_total_ms);
                p2d_lat_sum += p2d_total_ms;
                p2e_a_lat_sum += p2e_a_total_ms;
                p2e_b_lat_sum += p2e_b_total_ms;

                std::cout << "  Pattern: " << std::left << std::setw(8) << pat_name
                          << " | CPU: " << std::right << std::fixed << std::setprecision(2) << std::setw(6) << cpu_total_ms << " ms"
                          << " | 2D: " << std::setw(6) << p2d_total_ms << " ms"
                          << " | 2E-A: " << std::setw(6) << p2e_a_total_ms << " ms"
                          << " | 2E-B: " << std::setw(6) << p2e_b_total_ms << " ms"
                          << " | Saved: " << std::setw(5) << p2e_b_saved_ms << " ms"
                          << " | Eff: " << std::setw(5) << (p2e_b_eff * 100.0) << "%"
                          << " | Speedup: " << std::setw(4) << speedup_vs_2d << "x"
                          << std::endl;
            }

            double mean_p2d_fps = p2d_fps_sum / cfg.patterns;
            double mean_p2e_a_fps = p2e_a_fps_sum / cfg.patterns;
            double mean_p2e_b_fps = p2e_b_fps_sum / cfg.patterns;
            double mean_p2d_lat = p2d_lat_sum / cfg.patterns;
            double mean_p2e_a_lat = p2e_a_lat_sum / cfg.patterns;
            double mean_p2e_b_lat = p2e_b_lat_sum / cfg.patterns;

            std::cout << "\n--> " << cfg.name << " Summary: Phase 2D = " << mean_p2d_fps << " FPS (" << mean_p2d_lat << " ms)"
                      << " | Phase 2E-A = " << mean_p2e_a_fps << " FPS (" << mean_p2e_a_lat << " ms)"
                      << " | Phase 2E-B = " << mean_p2e_b_fps << " FPS (" << mean_p2e_b_lat << " ms)"
                      << " | Throughput = " << (mean_p2e_b_fps * megapixels) << " MP/s"
                      << std::endl;

            // ----------------------------------------------------
            // Stream Pool Size Sweep (Section 32)
            // ----------------------------------------------------
            if (cfg.patterns >= 3) {
                std::cout << "\nStream Pool Size Sweep for " << cfg.name << " (Noise):" << std::endl;
                auto& noise_sample = samples[2];
                std::vector<int> stream_counts = {1, 2, 4, 8};

                for (int sc : stream_counts) {
                    pipeline_2e.SetReplayStreamCount(sc);
                    double tot_sum = 0, rep_sum = 0, eff_sum = 0;
                    int runs_sc = 10;
                    for (int r = 0; r < runs_sc; ++r) {
                        auto t0 = Clock::now();
                        uint64_t t = pipeline_2e.QueueDecode(noise_sample.compressed.data(), noise_sample.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2e.WaitForDecode(t);
                        auto t1 = Clock::now();
                        tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();

                        Phase2eSlotStageTimings tm{};
                        pipeline_2e.GetStageTimings(t, &tm);
                        rep_sum += tm.gpu_replay_wall_ms;
                        eff_sum += tm.overlap_efficiency;
                        pipeline_2e.ReleaseOutput(t);
                    }
                    double avg_tot = tot_sum / runs_sc;
                    double avg_rep = rep_sum / runs_sc;
                    double avg_eff = eff_sum / runs_sc;
                    double fps = 1000.0 / avg_tot;

                    StreamScalingResult ssr{ sc, avg_rep, avg_tot, avg_eff, fps };
                    if (w == 1920) stream_scaling_1080p.push_back(ssr);
                    else if (w == 3840) stream_scaling_4k.push_back(ssr);

                    std::cout << "  Streams=" << sc
                              << " | Replay Wall: " << std::fixed << std::setprecision(2) << std::setw(5) << avg_rep << " ms"
                              << " | Total: " << std::setw(5) << avg_tot << " ms"
                              << " | Overlap Eff: " << std::setw(5) << (avg_eff * 100.0) << "%"
                              << " | FPS: " << std::setw(5) << fps
                              << std::endl;
                }
                pipeline_2e.SetReplayStreamCount(4); // restore default
            }

            // ----------------------------------------------------
            // Worker Count Sweep (Section 33)
            // ----------------------------------------------------
            if (cfg.patterns >= 3) {
                std::cout << "\nWorker Count Sweep for " << cfg.name << " (Noise):" << std::endl;
                auto& noise_sample = samples[2];
                std::vector<int> worker_counts = {2, 4, 8};

                for (int wc : worker_counts) {
                    pipeline_2e.SetWorkerCount(wc);
                    double tot_sum = 0, idx_sum = 0;
                    int runs_wc = 10;
                    for (int r = 0; r < runs_wc; ++r) {
                        auto t0 = Clock::now();
                        uint64_t t = pipeline_2e.QueueDecode(noise_sample.compressed.data(), noise_sample.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2e.WaitForDecode(t);
                        auto t1 = Clock::now();
                        tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();

                        Phase2eSlotStageTimings tm{};
                        pipeline_2e.GetStageTimings(t, &tm);
                        idx_sum += tm.host_index_wall_ms;
                        pipeline_2e.ReleaseOutput(t);
                    }
                    double avg_tot = tot_sum / runs_wc;
                    double avg_idx = idx_sum / runs_wc;
                    double fps = 1000.0 / avg_tot;

                    WorkerScalingResult wsr{ wc, avg_idx, avg_tot, fps };
                    if (w == 1920) worker_scaling_1080p.push_back(wsr);
                    else if (w == 3840) worker_scaling_4k.push_back(wsr);

                    std::cout << "  Workers=" << wc
                              << " | Index Wall: " << std::fixed << std::setprecision(2) << std::setw(5) << avg_idx << " ms"
                              << " | Total: " << std::setw(5) << avg_tot << " ms"
                              << " | FPS: " << std::setw(5) << fps
                              << std::endl;
                }
                pipeline_2e.SetWorkerCount(0); // restore default
            }

            // ----------------------------------------------------
            // Scheduling Policy Comparison (Section 13)
            // ----------------------------------------------------
            if (cfg.patterns >= 4) {
                std::vector<int> test_patterns = { 2, 3 }; // Noise, Texture
                std::vector<std::pair<std::string, Phase2ESchedulingPolicy>> policies = {
                    {"Policy A (LargestFirst)", Phase2ESchedulingPolicy::PolicyA_LargestFirst},
                    {"Policy B (InverseCritical)", Phase2ESchedulingPolicy::PolicyB_InverseCritical},
                    {"Policy C (Hybrid)", Phase2ESchedulingPolicy::PolicyC_Hybrid}
                };

                for (int pi : test_patterns) {
                    auto& sample = samples[pi];
                    std::string test_lbl = std::string(cfg.name) + " " + PatternName(pi);
                    std::cout << "\nScheduling Policies Comparison for " << test_lbl << ":" << std::endl;

                    for (const auto& pol : policies) {
                        pipeline_2e.SetSchedulingPolicy(pol.second);
                        double tot_sum = 0, idx_sum = 0, g_sum = 0, inv_sum = 0;
                        int runs_pol = 10;
                        for (int r = 0; r < runs_pol; ++r) {
                            auto t0 = Clock::now();
                            uint64_t t = pipeline_2e.QueueDecode(sample.compressed.data(), sample.compressed.size(), DeviceSurfaceType::BGRA8);
                            pipeline_2e.WaitForDecode(t);
                            auto t1 = Clock::now();
                            tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();

                            Phase2eSlotStageTimings tm{};
                            pipeline_2e.GetStageTimings(t, &tm);
                            idx_sum += tm.host_index_wall_ms;
                            g_sum += tm.t_first_gpu_launch_ms;
                            inv_sum += tm.t_first_inverse_start_ms;
                            pipeline_2e.ReleaseOutput(t);
                        }

                        PolicyComparisonResult pcr{
                            test_lbl,
                            pol.first,
                            idx_sum / runs_pol,
                            g_sum / runs_pol,
                            inv_sum / runs_pol,
                            tot_sum / runs_pol
                        };
                        policy_results.push_back(pcr);

                        std::cout << "  " << std::left << std::setw(26) << pol.first
                                  << " | Index: " << std::right << std::fixed << std::setprecision(2) << std::setw(5) << pcr.host_index_wall_ms << " ms"
                                  << " | 1st GPU: " << std::setw(5) << pcr.first_gpu_ms << " ms"
                                  << " | 1st Inv: " << std::setw(5) << pcr.first_inv_ms << " ms"
                                  << " | Total: " << std::setw(5) << pcr.total_decode_ms << " ms"
                                  << std::endl;
                    }
                    pipeline_2e.SetSchedulingPolicy(Phase2ESchedulingPolicy::PolicyA_LargestFirst);
                }
            }

            // ----------------------------------------------------
            // Multi-Slot Concurrency Scaling (Section 40)
            // ----------------------------------------------------
            std::cout << "\nMulti-Slot Concurrency Scaling (" << cfg.name << "):" << std::endl;
            std::vector<int> slot_counts = {1, 2, 3, 4};
            for (int sc : slot_counts) {
                CudaDecoderPipelinePhase2E pipe_multi;
                pipe_multi.Initialize(w, h, sc, 10, 0, 4);

                auto t_start = Clock::now();
                int total_frames = 20;
                int dispatched = 0, completed = 0;
                std::vector<uint64_t> active_tickets;

                while (completed < total_frames) {
                    while (dispatched < total_frames && active_tickets.size() < static_cast<size_t>(sc)) {
                        auto& s = samples[dispatched % cfg.patterns];
                        uint64_t t = pipe_multi.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                        if (t != 0) {
                            active_tickets.push_back(t);
                            dispatched++;
                        } else {
                            break;
                        }
                    }

                    for (auto it = active_tickets.begin(); it != active_tickets.end(); ) {
                        if (pipe_multi.PollDecode(*it) == DecodeTicketStatus::Complete) {
                            pipe_multi.ReleaseOutput(*it);
                            it = active_tickets.erase(it);
                            completed++;
                        } else {
                            ++it;
                        }
                    }
                }
                auto t_end = Clock::now();
                double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
                double fps = total_frames * 1000.0 / total_ms;
                double mpps = fps * megapixels;
                double lat = total_ms / total_frames;

                SlotScalingResult ssr{ sc, fps, mpps, lat };
                if (w == 1920) slot_scaling_1080p.push_back(ssr);
                else if (w == 3840) slot_scaling_4k.push_back(ssr);

                std::cout << "  Slots=" << sc << ": " << std::fixed << std::setprecision(2) << fps << " FPS ("
                          << mpps << " MP/s, Latency: " << lat << " ms/frame)" << std::endl;
            }

            // Write JSON results
            if (!first_res) json << ",\n";
            first_res = false;

            json << "    {\n"
                 << "      \"resolution\": \"" << cfg.name << "\",\n"
                 << "      \"width\": " << w << ",\n"
                 << "      \"height\": " << h << ",\n"
                 << "      \"megapixels\": " << megapixels << ",\n"
                 << "      \"phase2d_fps\": " << mean_p2d_fps << ",\n"
                 << "      \"phase2d_latency_ms\": " << mean_p2d_lat << ",\n"
                 << "      \"phase2e_entropy_overlap_fps\": " << mean_p2e_a_fps << ",\n"
                 << "      \"phase2e_entropy_overlap_latency_ms\": " << mean_p2e_a_lat << ",\n"
                 << "      \"phase2e_full_overlap_fps\": " << mean_p2e_b_fps << ",\n"
                 << "      \"phase2e_full_overlap_latency_ms\": " << mean_p2e_b_lat << ",\n"
                 << "      \"phase2e_throughput_mpps\": " << (mean_p2e_b_fps * megapixels) << ",\n"
                 << "      \"speedup_vs_phase2d\": " << (mean_p2e_b_fps / mean_p2d_fps) << ",\n"
                 << "      \"patterns\": [\n";

            for (size_t pi = 0; pi < pattern_results.size(); ++pi) {
                const auto& pr = pattern_results[pi];
                json << "        {\n"
                     << "          \"pattern\": \"" << pr.pattern_name << "\",\n"
                     << "          \"cpu_total_ms\": " << pr.cpu_total_ms << ",\n"
                     << "          \"phase2d_host_index_ms\": " << pr.p2d_host_index_ms << ",\n"
                     << "          \"phase2d_gpu_replay_ms\": " << pr.p2d_gpu_replay_ms << ",\n"
                     << "          \"phase2d_total_ms\": " << pr.p2d_total_ms << ",\n"
                     << "          \"phase2e_a_total_ms\": " << pr.p2e_a_total_ms << ",\n"
                     << "          \"phase2e_b_host_index_wall_ms\": " << pr.p2e_b_host_index_wall_ms << ",\n"
                     << "          \"phase2e_b_gpu_replay_wall_ms\": " << pr.p2e_b_gpu_replay_wall_ms << ",\n"
                     << "          \"phase2e_b_inv_wall_ms\": " << pr.p2e_b_inv_wall_ms << ",\n"
                     << "          \"phase2e_b_total_ms\": " << pr.p2e_b_total_ms << ",\n"
                     << "          \"overlap_saved_ms\": " << pr.overlap_saved_ms << ",\n"
                     << "          \"overlap_efficiency\": " << pr.overlap_efficiency << ",\n"
                     << "          \"speedup_vs_2d\": " << pr.speedup_vs_2d << ",\n"
                     << "          \"ratio_vs_cpu\": " << pr.ratio_vs_cpu << "\n"
                     << "        }" << (pi + 1 < pattern_results.size() ? "," : "") << "\n";
            }
            json << "      ]\n"
                 << "    }";
        }

        json << "\n  ],\n";

        // Write stream scaling
        auto write_stream_scaling = [&](const char* key, const std::vector<StreamScalingResult>& v) {
            json << "  \"" << key << "\": [\n";
            for (size_t i = 0; i < v.size(); ++i) {
                const auto& r = v[i];
                json << "    { \"streams\": " << r.streams
                     << ", \"replay_wall_ms\": " << r.gpu_replay_wall_ms
                     << ", \"total_decode_ms\": " << r.total_decode_ms
                     << ", \"overlap_efficiency\": " << r.overlap_efficiency
                     << ", \"fps\": " << r.fps << " }"
                     << (i + 1 < v.size() ? "," : "") << "\n";
            }
            json << "  ],\n";
        };

        // Write worker scaling
        auto write_worker_scaling = [&](const char* key, const std::vector<WorkerScalingResult>& v) {
            json << "  \"" << key << "\": [\n";
            for (size_t i = 0; i < v.size(); ++i) {
                const auto& r = v[i];
                json << "    { \"workers\": " << r.workers
                     << ", \"host_index_wall_ms\": " << r.host_index_wall_ms
                     << ", \"total_decode_ms\": " << r.total_decode_ms
                     << ", \"fps\": " << r.fps << " }"
                     << (i + 1 < v.size() ? "," : "") << "\n";
            }
            json << "  ],\n";
        };

        // Write policy comparisons
        json << "  \"scheduling_policy_comparison\": [\n";
        for (size_t i = 0; i < policy_results.size(); ++i) {
            const auto& r = policy_results[i];
            json << "    { \"test\": \"" << r.test_name << "\""
                 << ", \"policy\": \"" << r.policy_name << "\""
                 << ", \"host_index_wall_ms\": " << r.host_index_wall_ms
                 << ", \"first_gpu_ms\": " << r.first_gpu_ms
                 << ", \"first_inv_ms\": " << r.first_inv_ms
                 << ", \"total_decode_ms\": " << r.total_decode_ms << " }"
                 << (i + 1 < policy_results.size() ? "," : "") << "\n";
        }
        json << "  ],\n";

        write_stream_scaling("stream_scaling_1080p_noise", stream_scaling_1080p);
        write_stream_scaling("stream_scaling_4k_noise", stream_scaling_4k);
        write_worker_scaling("worker_scaling_1080p_noise", worker_scaling_1080p);
        write_worker_scaling("worker_scaling_4k_noise", worker_scaling_4k);

        // Write 4K noise breakdown (Section 28)
        json << "  \"mandatory_4k_noise_breakdown\": {\n"
             << "    \"phase2d_index_wall_ms\": " << noise_4k_breakdown.p2d_index_wall_ms << ",\n"
             << "    \"phase2d_gpu_replay_wall_ms\": " << noise_4k_breakdown.p2d_replay_wall_ms << ",\n"
             << "    \"phase2d_total_latency_ms\": " << noise_4k_breakdown.p2d_total_ms << ",\n"
             << "    \"phase2e_overlap_saved_ms\": " << noise_4k_breakdown.p2e_overlap_saved_ms << ",\n"
             << "    \"phase2e_overlap_efficiency\": " << noise_4k_breakdown.p2e_overlap_efficiency << ",\n"
             << "    \"first_gpu_replay_launch_ms\": " << noise_4k_breakdown.first_gpu_launch_ms << ",\n"
             << "    \"final_host_index_completion_ms\": " << noise_4k_breakdown.final_host_index_ms << ",\n"
             << "    \"first_inverse_launch_ms\": " << noise_4k_breakdown.first_inverse_launch_ms << ",\n"
             << "    \"final_entropy_done_ms\": " << noise_4k_breakdown.final_entropy_done_ms << ",\n"
             << "    \"final_device_bgra_latency_ms\": " << noise_4k_breakdown.p2e_final_latency_ms << "\n"
             << "  },\n";

        // Write slot scaling
        auto write_slots = [&](const char* key, const std::vector<SlotScalingResult>& v, bool last) {
            json << "  \"" << key << "\": [\n";
            for (size_t i = 0; i < v.size(); ++i) {
                const auto& r = v[i];
                json << "    { \"slots\": " << r.slots
                     << ", \"fps\": " << r.fps
                     << ", \"megapixels_per_sec\": " << r.megapixels_per_sec
                     << ", \"latency_ms\": " << r.latency_ms << " }"
                     << (i + 1 < v.size() ? "," : "") << "\n";
            }
            json << (last ? "  ]\n" : "  ],\n");
        };

        write_slots("slot_scaling_1080p", slot_scaling_1080p, false);
        write_slots("slot_scaling_4k", slot_scaling_4k, false);

        json << "  \"peak_host_working_set_bytes\": " << PeakMemory() << ",\n"
             << "  \"gpu_memory_in_use_bytes\": " << PeakGpuMemory() << "\n";

        json << "}\n";
        json.close();

        std::cout << "\nBenchmark results successfully written to: " << output << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Benchmark failure: " << e.what() << std::endl;
        return 1;
    }
}
