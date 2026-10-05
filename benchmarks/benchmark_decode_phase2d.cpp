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
#include "hypercine/decode_phase2b.h"
#include "hypercine/decode_phase2c.h"
#include "hypercine/decode_phase2d.h"
#include "hypercine/decode_entropy_cuda.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;
using Clock = std::chrono::steady_clock;

double Percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double position = q * double(v.size() - 1);
    const auto low = size_t(position), high = (std::min)(low + 1, v.size() - 1);
    return v[low] + (v[high] - v[low]) * (position - double(low));
}

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
    double p2b_entropy_ms;
    double p2b_total_ms;
    double p2c_host_index_ms;
    double p2c_vlc_ms;
    double p2c_entropy_ms;
    double p2c_total_ms;
    double p2d_host_index_ms;
    double p2d_task_prep_ms;
    double p2d_worker_barrier_ms;
    double p2d_vlc_ms;
    double p2d_entropy_ms;
    double p2d_total_ms;
    double host_index_speedup;
    double total_speedup_vs_2c;
    double total_speedup_vs_2b;
    double ratio_vs_cpu;
};

struct WorkerScalingResult {
    int workers;
    double host_index_ms;
    double speedup_vs_1w;
    double efficiency;
    double total_decode_ms;
    double fps;
};

struct SchedulingResult {
    std::string policy_name;
    double host_index_ms;
    double total_decode_ms;
};

struct CriticalPathResult {
    std::string test_name;
    std::string policy;
    int workers = 0;
    uint32_t largest_band_bytes = 0;
    double serial_sum_ms = 0;
    double largest_task_serial_ms = 0;
    double slowest_task_parallel_ms = 0;
    double parallel_work_sum_ms = 0;
    double parallel_wall_ms = 0;
    double queue_overhead_ms = 0;
    double barrier_wait_ms = 0;
    double gather_ms = 0;
    double task_prep_ms = 0;
    double critical_path_limit = 0;
    double achieved_speedup = 0;
    double work_inflation = 0;
    double efficiency = 0;
};

struct SlotScalingResult {
    int slots;
    double fps;
    double megapixels_per_sec;
    double latency_ms;
};

} // namespace

int main(int argc, char** argv) {
    try {
        std::cout << "=== HyperCine Decode Phase 2D End-to-End Benchmark ===" << std::endl;

        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, 0);
        std::cout << "GPU Device: " << prop.name << " (CC " << prop.major << "." << prop.minor
                  << ", " << (prop.totalGlobalMem / (1024 * 1024)) << " MB VRAM)" << std::endl;
        unsigned int hardware_concurrency = std::thread::hardware_concurrency();
        std::cout << "CPU Hardware Concurrency: " << hardware_concurrency << " threads\n" << std::endl;

        std::string output = "benchmarks/results/decode_phase2d_windows_msvc.json";
        std::string resolution = "all";

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (arg == "--output") output = value;
            else if (arg == "--resolution") resolution = value;
            else throw std::runtime_error("unknown option: " + arg);
        }

        std::filesystem::path out_path(output);
        if (out_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }

        std::ofstream json(out_path, std::ios::out | std::ios::trunc);
        if (!json.is_open()) {
            std::cerr << "Failed to open output file: " << output << " (parent exists: "
                      << std::filesystem::exists(out_path.parent_path()) << ", errno: " << errno << ")" << std::endl;
            return 1;
        }

        json << "{\n"
             << "  \"benchmark\": \"decode_phase2d\",\n"
             << "  \"gpu\": \"" << prop.name << "\",\n"
             << "  \"compute_capability\": \"" << prop.major << "." << prop.minor << "\",\n"
             << "  \"cpu_hardware_threads\": " << hardware_concurrency << ",\n"
             << "  \"results\": [\n";

        struct ResConfig {
            int width;
            int height;
            int patterns;
            int trials;
            int cycles;
            const char* name;
        };

        const ResConfig configs[] = {
            { 1920, 1080, 5, 5, 3, "1080p" },
            { 3840, 2160, 5, 3, 2, "4K UHD" },
            { 7680, 4320, 1, 3, 1, "8K flat" }
        };

        bool first_res = true;
        std::vector<WorkerScalingResult> scaling_1080p_noise;
        std::vector<WorkerScalingResult> scaling_4k_noise;
        std::vector<SchedulingResult> sched_1080p_noise;
        std::vector<SchedulingResult> sched_4k_noise;
        std::vector<CriticalPathResult> critical_path_results;
        std::vector<SlotScalingResult> slot_scaling_1080p;
        std::vector<SlotScalingResult> slot_scaling_4k;

        for (const auto& cfg : configs) {
            if (resolution != "all" && resolution != cfg.name && resolution != std::string(cfg.name).substr(0, 4)) continue;
            int w = cfg.width;
            int h = cfg.height;
            int trials = cfg.trials;
            int cycles = cfg.cycles;
            double megapixels = (double(w) * h) / 1e6;
            int frames_per_trial = cfg.patterns * cycles;

            std::cout << "\n========================================================" << std::endl;
            std::cout << "Benchmarking " << cfg.name << " (" << w << "x" << h << ", "
                      << cfg.patterns << " patterns, " << trials << " trials x " << cycles << " cycles)" << std::endl;
            std::cout << "========================================================" << std::endl;

            auto samples = Generate(w, h, cfg.patterns);

            CudaDecoderPipelinePhase2B pipeline_2b;
            if (!pipeline_2b.Initialize(w, h, 3, 10)) {
                throw std::runtime_error("Failed to initialize Phase 2B pipeline");
            }

            CudaDecoderPipelinePhase2C pipeline_2c;
            if (!pipeline_2c.Initialize(w, h, 4, 10)) {
                throw std::runtime_error("Failed to initialize Phase 2C pipeline");
            }

            CudaDecoderPipelinePhase2D pipeline_2d;
            if (!pipeline_2d.Initialize(w, h, 4, 10)) {
                throw std::runtime_error("Failed to initialize Phase 2D pipeline");
            }

            // Warmup
            for (auto& s : samples) {
                uint64_t t2b = pipeline_2b.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2b.WaitForDecode(t2b);
                pipeline_2b.ReleaseOutput(t2b);

                uint64_t t2c = pipeline_2c.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2c.WaitForDecode(t2c);
                pipeline_2c.ReleaseOutput(t2c);

                uint64_t t2d = pipeline_2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2d.WaitForDecode(t2d);
                pipeline_2d.ReleaseOutput(t2d);
            }

            // ----------------------------------------------------
            // Per-Pattern Benchmarking
            // ----------------------------------------------------
            std::vector<PatternBenchmarkResult> pattern_results;

            for (int p = 0; p < cfg.patterns; ++p) {
                auto& s = samples[p];

                // 1. CPU Reference timing
                double cpu_sum_ms = 0;
                int cpu_runs = 3;
                for (int r = 0; r < cpu_runs; ++r) {
                    ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                    auto t0 = Clock::now();
                    ref.Decode(s);
                    auto t1 = Clock::now();
                    cpu_sum_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                }
                double cpu_avg_ms = cpu_sum_ms / cpu_runs;

                // 2. Phase 2B Serial GPU timing
                double p2b_ent_sum_ms = 0;
                double p2b_tot_sum_ms = 0;
                int p2b_runs = 3;
                for (int r = 0; r < p2b_runs; ++r) {
                    auto t0 = Clock::now();
                    uint64_t t = pipeline_2b.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    pipeline_2b.WaitForDecode(t);
                    auto t1 = Clock::now();
                    p2b_tot_sum_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

                    Phase2bSlotStageTimings timings{};
                    pipeline_2b.GetStageTimings(t, &timings);
                    p2b_ent_sum_ms += timings.gpu_entropy_ms;
                    pipeline_2b.ReleaseOutput(t);
                }
                double p2b_avg_ent_ms = p2b_ent_sum_ms / p2b_runs;
                double p2b_avg_tot_ms = p2b_tot_sum_ms / p2b_runs;

                // 3. Phase 2C Parallel GPU timing
                double p2c_ent_sum_ms = 0;
                double p2c_index_sum_ms = 0;
                double p2c_vlc_sum_ms = 0;
                double p2c_tot_sum_ms = 0;
                int p2c_runs = 5;

                for (int r = 0; r < p2c_runs; ++r) {
                    auto t0 = Clock::now();
                    uint64_t t = pipeline_2c.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    pipeline_2c.WaitForDecode(t);
                    auto t1 = Clock::now();
                    p2c_tot_sum_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

                    Phase2cSlotStageTimings timings{};
                    pipeline_2c.GetStageTimings(t, &timings);
                    p2c_ent_sum_ms += timings.gpu_entropy_ms;
                    p2c_index_sum_ms += timings.host_index_ms;
                    p2c_vlc_sum_ms += timings.gpu_vlc_ms;
                    pipeline_2c.ReleaseOutput(t);
                }
                double p2c_avg_ent_ms = p2c_ent_sum_ms / p2c_runs;
                double p2c_avg_index_ms = p2c_index_sum_ms / p2c_runs;
                double p2c_avg_vlc_ms = p2c_vlc_sum_ms / p2c_runs;
                double p2c_avg_tot_ms = p2c_tot_sum_ms / p2c_runs;

                // 4. Phase 2D Parallel Host + Parallel GPU timing
                double p2d_ent_sum_ms = 0;
                double p2d_index_sum_ms = 0;
                double p2d_prep_sum_ms = 0;
                double p2d_barrier_sum_ms = 0;
                double p2d_vlc_sum_ms = 0;
                double p2d_tot_sum_ms = 0;
                int p2d_runs = 5;

                for (int r = 0; r < p2d_runs; ++r) {
                    auto t0 = Clock::now();
                    uint64_t t = pipeline_2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    pipeline_2d.WaitForDecode(t);
                    auto t1 = Clock::now();
                    p2d_tot_sum_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

                    Phase2dSlotStageTimings timings{};
                    pipeline_2d.GetStageTimings(t, &timings);
                    p2d_ent_sum_ms += timings.gpu_entropy_ms;
                    p2d_index_sum_ms += timings.host_index_ms;
                    p2d_prep_sum_ms += timings.task_prep_ms;
                    p2d_barrier_sum_ms += timings.barrier_wait_ms;
                    p2d_vlc_sum_ms += timings.gpu_vlc_ms;
                    pipeline_2d.ReleaseOutput(t);
                }
                double p2d_avg_ent_ms = p2d_ent_sum_ms / p2d_runs;
                double p2d_avg_index_ms = p2d_index_sum_ms / p2d_runs;
                double p2d_avg_prep_ms = p2d_prep_sum_ms / p2d_runs;
                double p2d_avg_barrier_ms = p2d_barrier_sum_ms / p2d_runs;
                double p2d_avg_vlc_ms = p2d_vlc_sum_ms / p2d_runs;
                double p2d_avg_tot_ms = p2d_tot_sum_ms / p2d_runs;

                double host_index_speedup = (p2d_avg_index_ms > 0) ? (p2c_avg_index_ms / p2d_avg_index_ms) : 1.0;
                double total_speedup_vs_2c = (p2d_avg_tot_ms > 0) ? (p2c_avg_tot_ms / p2d_avg_tot_ms) : 1.0;
                double total_speedup_vs_2b = (p2d_avg_tot_ms > 0) ? (p2b_avg_tot_ms / p2d_avg_tot_ms) : 1.0;
                double ratio_cpu = (p2d_avg_tot_ms > 0) ? (cpu_avg_ms / p2d_avg_tot_ms) : 1.0;

                PatternBenchmarkResult pr;
                pr.pattern_name = PatternName(p);
                pr.cpu_total_ms = cpu_avg_ms;
                pr.p2b_entropy_ms = p2b_avg_ent_ms;
                pr.p2b_total_ms = p2b_avg_tot_ms;
                pr.p2c_host_index_ms = p2c_avg_index_ms;
                pr.p2c_vlc_ms = p2c_avg_vlc_ms;
                pr.p2c_entropy_ms = p2c_avg_ent_ms;
                pr.p2c_total_ms = p2c_avg_tot_ms;
                pr.p2d_host_index_ms = p2d_avg_index_ms;
                pr.p2d_task_prep_ms = p2d_avg_prep_ms;
                pr.p2d_worker_barrier_ms = p2d_avg_barrier_ms;
                pr.p2d_vlc_ms = p2d_avg_vlc_ms;
                pr.p2d_entropy_ms = p2d_avg_ent_ms;
                pr.p2d_total_ms = p2d_avg_tot_ms;
                pr.host_index_speedup = host_index_speedup;
                pr.total_speedup_vs_2c = total_speedup_vs_2c;
                pr.total_speedup_vs_2b = total_speedup_vs_2b;
                pr.ratio_vs_cpu = ratio_cpu;
                pattern_results.push_back(pr);
            }

            // Print Per-Pattern Disclosure Table
            std::cout << "\nPer-Pattern Disclosure for " << cfg.name << ":" << std::endl;
            std::cout << std::string(120, '-') << std::endl;
            std::cout << std::left << std::setw(12) << "Pattern"
                      << std::right
                      << std::setw(12) << "CPU Tot(ms)"
                      << std::setw(14) << "2C Idx(ms)"
                      << std::setw(14) << "2D Idx(ms)"
                      << std::setw(12) << "Idx Speed"
                      << std::setw(14) << "2C Tot(ms)"
                      << std::setw(14) << "2D Tot(ms)"
                      << std::setw(12) << "Tot Spd 2C"
                      << std::setw(14) << "Ratio vs CPU"
                      << std::endl;
            std::cout << std::string(120, '-') << std::endl;

            for (const auto& pr : pattern_results) {
                std::cout << std::left << std::setw(12) << pr.pattern_name
                          << std::right << std::fixed << std::setprecision(2)
                          << std::setw(12) << pr.cpu_total_ms
                          << std::setw(14) << pr.p2c_host_index_ms
                          << std::setw(14) << pr.p2d_host_index_ms
                          << std::setw(11) << pr.host_index_speedup << "x"
                          << std::setw(14) << pr.p2c_total_ms
                          << std::setw(14) << pr.p2d_total_ms
                          << std::setw(11) << pr.total_speedup_vs_2c << "x"
                          << std::setw(13) << pr.ratio_vs_cpu << "x"
                          << std::endl;
            }
            std::cout << std::string(120, '-') << std::endl;

            // ----------------------------------------------------
            // Five-Pattern Headline Benchmark
            // ----------------------------------------------------
            std::cout << "\nRunning Headline Aggregate Benchmark for " << cfg.name << "..." << std::endl;

            std::vector<double> p2b_headline_fps, p2b_headline_lat;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t0 = Clock::now();
                        uint64_t ticket = pipeline_2b.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2b.WaitForDecode(ticket);
                        auto t1 = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        trial_wall_ms += ms;
                        p2b_headline_lat.push_back(ms);
                        pipeline_2b.ReleaseOutput(ticket);
                    }
                }
                p2b_headline_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            std::vector<double> p2c_headline_fps, p2c_headline_lat;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t0 = Clock::now();
                        uint64_t ticket = pipeline_2c.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2c.WaitForDecode(ticket);
                        auto t1 = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        trial_wall_ms += ms;
                        p2c_headline_lat.push_back(ms);
                        pipeline_2c.ReleaseOutput(ticket);
                    }
                }
                p2c_headline_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            std::vector<double> p2d_headline_fps, p2d_headline_lat;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t0 = Clock::now();
                        uint64_t ticket = pipeline_2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2d.WaitForDecode(ticket);
                        auto t1 = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        trial_wall_ms += ms;
                        p2d_headline_lat.push_back(ms);
                        pipeline_2d.ReleaseOutput(ticket);
                    }
                }
                p2d_headline_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            double mean_p2b_fps = std::accumulate(p2b_headline_fps.begin(), p2b_headline_fps.end(), 0.0) / p2b_headline_fps.size();
            double mean_p2c_fps = std::accumulate(p2c_headline_fps.begin(), p2c_headline_fps.end(), 0.0) / p2c_headline_fps.size();
            double mean_p2d_fps = std::accumulate(p2d_headline_fps.begin(), p2d_headline_fps.end(), 0.0) / p2d_headline_fps.size();
            double mean_p2b_lat = std::accumulate(p2b_headline_lat.begin(), p2b_headline_lat.end(), 0.0) / p2b_headline_lat.size();
            double mean_p2c_lat = std::accumulate(p2c_headline_lat.begin(), p2c_headline_lat.end(), 0.0) / p2c_headline_lat.size();
            double mean_p2d_lat = std::accumulate(p2d_headline_lat.begin(), p2d_headline_lat.end(), 0.0) / p2d_headline_lat.size();

            std::cout << "Headline Results for " << cfg.name << ":" << std::endl;
            std::cout << "  Phase 2B DeviceBGRA: " << std::fixed << std::setprecision(2) << mean_p2b_fps << " FPS (" << mean_p2b_lat << " ms)" << std::endl;
            std::cout << "  Phase 2C DeviceBGRA: " << std::fixed << std::setprecision(2) << mean_p2c_fps << " FPS (" << mean_p2c_lat << " ms)" << std::endl;
            std::cout << "  Phase 2D DeviceBGRA: " << std::fixed << std::setprecision(2) << mean_p2d_fps << " FPS (" << mean_p2d_lat << " ms)" << std::endl;
            std::cout << "  Phase 2D Throughput: " << std::fixed << std::setprecision(2) << (mean_p2d_fps * megapixels) << " MP/s" << std::endl;
            std::cout << "  Speedup vs Phase 2C: " << (mean_p2d_fps / mean_p2c_fps) << "x" << std::endl;
            std::cout << "  Speedup vs Phase 2B: " << (mean_p2d_fps / mean_p2b_fps) << "x" << std::endl;

            // ----------------------------------------------------
            // Worker Scaling Sweep (Section 22) on Noise pattern
            // ----------------------------------------------------
            if (cfg.patterns >= 3) {
                std::cout << "\nWorker Scaling Sweep for " << cfg.name << " (Noise):" << std::endl;
                std::cout << std::string(80, '-') << std::endl;
                std::cout << std::left << std::setw(10) << "Workers"
                          << std::right
                          << std::setw(14) << "Host Idx(ms)"
                          << std::setw(14) << "Speedup"
                          << std::setw(14) << "Efficiency"
                          << std::setw(14) << "Total(ms)"
                          << std::setw(14) << "FPS"
                          << std::endl;
                std::cout << std::string(80, '-') << std::endl;

                auto& noise_sample = samples[2]; // Pattern 2 = Noise
                std::vector<int> worker_counts = {1, 2, 4, 6, 8, 12, 16};
                double base_idx_ms = 0;

                for (int wc : worker_counts) {
                    pipeline_2d.SetWorkerCount(wc);
                    double idx_sum = 0, tot_sum = 0;
                    int sweep_runs = 5;

                    for (int r = 0; r < sweep_runs; ++r) {
                        auto t0 = Clock::now();
                        uint64_t t = pipeline_2d.QueueDecode(noise_sample.compressed.data(), noise_sample.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2d.WaitForDecode(t);
                        auto t1 = Clock::now();
                        tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();

                        Phase2dSlotStageTimings timings{};
                        pipeline_2d.GetStageTimings(t, &timings);
                        idx_sum += timings.host_index_ms;
                        pipeline_2d.ReleaseOutput(t);
                    }

                    double avg_idx = idx_sum / sweep_runs;
                    double avg_tot = tot_sum / sweep_runs;
                    if (wc == 1) base_idx_ms = avg_idx;

                    double speedup = (avg_idx > 0) ? (base_idx_ms / avg_idx) : 1.0;
                    double efficiency = speedup / wc;
                    double fps = 1000.0 / avg_tot;

                    WorkerScalingResult wsr{ wc, avg_idx, speedup, efficiency, avg_tot, fps };
                    if (w == 1920) scaling_1080p_noise.push_back(wsr);
                    else if (w == 3840) scaling_4k_noise.push_back(wsr);

                    std::cout << std::left << std::setw(10) << wc
                              << std::right << std::fixed << std::setprecision(2)
                              << std::setw(14) << avg_idx
                              << std::setw(13) << speedup << "x"
                              << std::setw(13) << (efficiency * 100.0) << "%"
                              << std::setw(14) << avg_tot
                              << std::setw(14) << fps
                              << std::endl;
                }
                std::cout << std::string(80, '-') << std::endl;
                pipeline_2d.SetWorkerCount(0); // restore default
            }

            // ----------------------------------------------------
            // Scheduling Strategy Comparison (Section 23)
            // ----------------------------------------------------
            if (cfg.patterns >= 3) {
                std::cout << "\nScheduling Strategy Comparison for " << cfg.name << " (Noise):" << std::endl;
                auto& noise_sample = samples[2];

                auto eval_policy = [&](HostIndexSchedulingPolicy pol, const std::string& name) {
                    pipeline_2d.SetSchedulingPolicy(pol);
                    double idx_sum = 0, tot_sum = 0;
                    int runs = 5;
                    for (int r = 0; r < runs; ++r) {
                        auto t0 = Clock::now();
                        uint64_t t = pipeline_2d.QueueDecode(noise_sample.compressed.data(), noise_sample.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2d.WaitForDecode(t);
                        auto t1 = Clock::now();
                        tot_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();

                        Phase2dSlotStageTimings timings{};
                        pipeline_2d.GetStageTimings(t, &timings);
                        idx_sum += timings.host_index_ms;
                        pipeline_2d.ReleaseOutput(t);
                    }
                    SchedulingResult sr{ name, idx_sum / runs, tot_sum / runs };
                    if (w == 1920) sched_1080p_noise.push_back(sr);
                    else if (w == 3840) sched_4k_noise.push_back(sr);

                    std::cout << "  " << std::left << std::setw(20) << name
                              << ": Host Index = " << std::fixed << std::setprecision(2) << sr.host_index_ms << " ms"
                              << ", Total = " << sr.total_decode_ms << " ms" << std::endl;
                };

                eval_policy(HostIndexSchedulingPolicy::DescriptorOrder, "DescriptorOrder (FIFO)");
                eval_policy(HostIndexSchedulingPolicy::LargestFirst, "LargestFirst (LPT)");
                pipeline_2d.SetSchedulingPolicy(HostIndexSchedulingPolicy::DescriptorOrder);
            }

            // ----------------------------------------------------
            // Critical Path Analysis (Section 25)
            // Uses the pipeline's own per-task timestamps (no re-parsing).
            // ----------------------------------------------------
            if (cfg.patterns >= 3) {
                auto& noise_sample = samples[2];
                const int runs = 10;

                // Serial reference (Phase 2C mode) gives true serial work and
                // largest single-band traversal time.
                pipeline_2d.SetHostIndexMode(HostIndexExecutionMode::SingleThreadReference);
                double serial_sum = 0, serial_largest = 0;
                uint32_t largest_bytes = 0;
                for (int r = 0; r < runs; ++r) {
                    uint64_t t = pipeline_2d.QueueDecode(noise_sample.compressed.data(), noise_sample.compressed.size(), DeviceSurfaceType::BGRA8);
                    pipeline_2d.WaitForDecode(t);
                    Phase2dSlotStageTimings tm{};
                    pipeline_2d.GetStageTimings(t, &tm);
                    serial_sum += tm.checkpoint_traversal_ms;
                    serial_largest += tm.slowest_task_ms;
                    largest_bytes = tm.largest_band_bytes;
                    pipeline_2d.ReleaseOutput(t);
                }
                serial_sum /= runs;
                serial_largest /= runs;
                pipeline_2d.SetHostIndexMode(HostIndexExecutionMode::ParallelWorkerPool);

                for (auto pol : { HostIndexSchedulingPolicy::DescriptorOrder, HostIndexSchedulingPolicy::LargestFirst }) {
                    pipeline_2d.SetSchedulingPolicy(pol);
                    CriticalPathResult cpr{};
                    cpr.test_name = std::string(cfg.name) + " Noise";
                    cpr.policy = (pol == HostIndexSchedulingPolicy::LargestFirst) ? "LargestFirst" : "DescriptorOrder";
                    cpr.serial_sum_ms = serial_sum;
                    cpr.largest_task_serial_ms = serial_largest;
                    cpr.largest_band_bytes = largest_bytes;
                    for (int r = 0; r < runs; ++r) {
                        uint64_t t = pipeline_2d.QueueDecode(noise_sample.compressed.data(), noise_sample.compressed.size(), DeviceSurfaceType::BGRA8);
                        pipeline_2d.WaitForDecode(t);
                        Phase2dSlotStageTimings tm{};
                        pipeline_2d.GetStageTimings(t, &tm);
                        cpr.workers = tm.active_workers;
                        cpr.parallel_work_sum_ms += tm.checkpoint_traversal_ms;
                        cpr.parallel_wall_ms += tm.index_parallel_wall_ms;
                        cpr.queue_overhead_ms += tm.queue_overhead_ms;
                        cpr.barrier_wait_ms += tm.barrier_wait_ms;
                        cpr.gather_ms += tm.segment_gather_ms;
                        cpr.task_prep_ms += tm.task_prep_ms;
                        cpr.slowest_task_parallel_ms += tm.slowest_task_ms;
                        pipeline_2d.ReleaseOutput(t);
                    }
                    cpr.parallel_work_sum_ms /= runs;
                    cpr.parallel_wall_ms /= runs;
                    cpr.queue_overhead_ms /= runs;
                    cpr.barrier_wait_ms /= runs;
                    cpr.gather_ms /= runs;
                    cpr.task_prep_ms /= runs;
                    cpr.slowest_task_parallel_ms /= runs;
                    cpr.critical_path_limit = cpr.largest_task_serial_ms > 0 ? cpr.serial_sum_ms / cpr.largest_task_serial_ms : 1.0;
                    cpr.achieved_speedup = cpr.parallel_wall_ms > 0 ? cpr.serial_sum_ms / cpr.parallel_wall_ms : 1.0;
                    cpr.work_inflation = cpr.serial_sum_ms > 0 ? cpr.parallel_work_sum_ms / cpr.serial_sum_ms : 1.0;
                    cpr.efficiency = (cpr.workers > 0 && cpr.parallel_wall_ms > 0)
                        ? cpr.parallel_work_sum_ms / (cpr.workers * cpr.parallel_wall_ms) : 0.0;
                    critical_path_results.push_back(cpr);

                    std::cout << "\nCritical Path Analysis for " << cfg.name << " (Noise, " << cpr.policy << ", "
                              << cpr.workers << " workers):" << std::endl;
                    std::cout << std::fixed << std::setprecision(3)
                              << "  Serial traversal sum (1 thread):     " << cpr.serial_sum_ms << " ms" << std::endl
                              << "  Largest band traversal (serial):     " << cpr.largest_task_serial_ms << " ms ("
                              << cpr.largest_band_bytes << " B)" << std::endl
                              << "  Largest task under parallel load:    " << cpr.slowest_task_parallel_ms << " ms" << std::endl
                              << "  Parallel traversal work sum:         " << cpr.parallel_work_sum_ms << " ms (inflation "
                              << std::setprecision(2) << cpr.work_inflation << "x)" << std::endl
                              << std::setprecision(3)
                              << "  Parallel batch wall:                 " << cpr.parallel_wall_ms << " ms" << std::endl
                              << "  Dispatch latency (queue overhead):   " << cpr.queue_overhead_ms << " ms" << std::endl
                              << "  Join latency (barrier):              " << cpr.barrier_wait_ms << " ms" << std::endl
                              << "  Task prep / gather:                  " << cpr.task_prep_ms << " / " << cpr.gather_ms << " ms" << std::endl
                              << std::setprecision(2)
                              << "  Critical-path speedup limit:         " << cpr.critical_path_limit << "x" << std::endl
                              << "  Achieved speedup (serial/wall):      " << cpr.achieved_speedup << "x" << std::endl
                              << "  Worker utilization:                  " << (cpr.efficiency * 100.0) << "%" << std::endl;
                }
                pipeline_2d.SetSchedulingPolicy(HostIndexSchedulingPolicy::LargestFirst);
            }

            // ----------------------------------------------------
            // Multi-Frame Concurrency Experiment (Section 24)
            // ----------------------------------------------------
            std::cout << "\nMulti-Frame Concurrency Scaling (" << cfg.name << "):" << std::endl;
            std::vector<int> slot_counts = {1, 2, 3, 4};
            for (int sc : slot_counts) {
                CudaDecoderPipelinePhase2D pipe_multi;
                pipe_multi.Initialize(w, h, sc, 10);

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
                 << "      \"phase2b_fps\": " << mean_p2b_fps << ",\n"
                 << "      \"phase2b_latency_ms\": " << mean_p2b_lat << ",\n"
                 << "      \"phase2c_fps\": " << mean_p2c_fps << ",\n"
                 << "      \"phase2c_latency_ms\": " << mean_p2c_lat << ",\n"
                 << "      \"phase2d_fps\": " << mean_p2d_fps << ",\n"
                 << "      \"phase2d_latency_ms\": " << mean_p2d_lat << ",\n"
                 << "      \"phase2d_throughput_mpps\": " << (mean_p2d_fps * megapixels) << ",\n"
                 << "      \"speedup_vs_phase2c\": " << (mean_p2d_fps / mean_p2c_fps) << ",\n"
                 << "      \"speedup_vs_phase2b\": " << (mean_p2d_fps / mean_p2b_fps) << ",\n"
                 << "      \"patterns\": [\n";

            for (size_t pi = 0; pi < pattern_results.size(); ++pi) {
                const auto& pr = pattern_results[pi];
                json << "        {\n"
                     << "          \"pattern\": \"" << pr.pattern_name << "\",\n"
                     << "          \"cpu_total_ms\": " << pr.cpu_total_ms << ",\n"
                     << "          \"phase2b_entropy_ms\": " << pr.p2b_entropy_ms << ",\n"
                     << "          \"phase2b_total_ms\": " << pr.p2b_total_ms << ",\n"
                     << "          \"phase2c_host_index_ms\": " << pr.p2c_host_index_ms << ",\n"
                     << "          \"phase2c_vlc_ms\": " << pr.p2c_vlc_ms << ",\n"
                     << "          \"phase2c_entropy_ms\": " << pr.p2c_entropy_ms << ",\n"
                     << "          \"phase2c_total_ms\": " << pr.p2c_total_ms << ",\n"
                     << "          \"phase2d_host_index_ms\": " << pr.p2d_host_index_ms << ",\n"
                     << "          \"phase2d_task_prep_ms\": " << pr.p2d_task_prep_ms << ",\n"
                     << "          \"phase2d_worker_barrier_ms\": " << pr.p2d_worker_barrier_ms << ",\n"
                     << "          \"phase2d_vlc_ms\": " << pr.p2d_vlc_ms << ",\n"
                     << "          \"phase2d_entropy_ms\": " << pr.p2d_entropy_ms << ",\n"
                     << "          \"phase2d_total_ms\": " << pr.p2d_total_ms << ",\n"
                     << "          \"host_index_speedup\": " << pr.host_index_speedup << ",\n"
                     << "          \"total_speedup_vs_2c\": " << pr.total_speedup_vs_2c << ",\n"
                     << "          \"total_speedup_vs_2b\": " << pr.total_speedup_vs_2b << ",\n"
                     << "          \"ratio_vs_cpu\": " << pr.ratio_vs_cpu << "\n"
                     << "        }" << (pi + 1 < pattern_results.size() ? "," : "") << "\n";
            }
            json << "      ]\n"
                 << "    }";
        }

        json << "\n  ],\n";

        auto write_scaling = [&](const char* key, const std::vector<WorkerScalingResult>& v) {
            json << "  \"" << key << "\": [\n";
            for (size_t i = 0; i < v.size(); ++i) {
                const auto& r = v[i];
                json << "    { \"workers\": " << r.workers
                     << ", \"host_index_ms\": " << r.host_index_ms
                     << ", \"speedup_vs_1w\": " << r.speedup_vs_1w
                     << ", \"efficiency\": " << r.efficiency
                     << ", \"total_decode_ms\": " << r.total_decode_ms
                     << ", \"fps\": " << r.fps << " }"
                     << (i + 1 < v.size() ? "," : "") << "\n";
            }
            json << "  ],\n";
        };
        auto write_sched = [&](const char* key, const std::vector<SchedulingResult>& v) {
            json << "  \"" << key << "\": [\n";
            for (size_t i = 0; i < v.size(); ++i) {
                const auto& r = v[i];
                json << "    { \"policy\": \"" << r.policy_name << "\""
                     << ", \"host_index_ms\": " << r.host_index_ms
                     << ", \"total_decode_ms\": " << r.total_decode_ms << " }"
                     << (i + 1 < v.size() ? "," : "") << "\n";
            }
            json << "  ],\n";
        };
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

        write_scaling("worker_scaling_1080p_noise", scaling_1080p_noise);
        write_scaling("worker_scaling_4k_noise", scaling_4k_noise);
        write_sched("scheduling_comparison_1080p_noise", sched_1080p_noise);
        write_sched("scheduling_comparison_4k_noise", sched_4k_noise);

        json << "  \"critical_path_analysis\": [\n";
        for (size_t i = 0; i < critical_path_results.size(); ++i) {
            const auto& r = critical_path_results[i];
            json << "    { \"test\": \"" << r.test_name << "\""
                 << ", \"policy\": \"" << r.policy << "\""
                 << ", \"workers\": " << r.workers
                 << ", \"largest_band_bytes\": " << r.largest_band_bytes
                 << ", \"serial_sum_ms\": " << r.serial_sum_ms
                 << ", \"largest_task_serial_ms\": " << r.largest_task_serial_ms
                 << ", \"slowest_task_parallel_ms\": " << r.slowest_task_parallel_ms
                 << ", \"parallel_work_sum_ms\": " << r.parallel_work_sum_ms
                 << ", \"parallel_wall_ms\": " << r.parallel_wall_ms
                 << ", \"queue_overhead_ms\": " << r.queue_overhead_ms
                 << ", \"barrier_wait_ms\": " << r.barrier_wait_ms
                 << ", \"task_prep_ms\": " << r.task_prep_ms
                 << ", \"gather_ms\": " << r.gather_ms
                 << ", \"critical_path_limit\": " << r.critical_path_limit
                 << ", \"achieved_speedup\": " << r.achieved_speedup
                 << ", \"work_inflation\": " << r.work_inflation
                 << ", \"worker_utilization\": " << r.efficiency << " }"
                 << (i + 1 < critical_path_results.size() ? "," : "") << "\n";
        }
        json << "  ],\n";

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
