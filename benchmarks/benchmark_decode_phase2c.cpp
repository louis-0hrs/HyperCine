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
#include <cuda_runtime.h>
#include "../tests/decode_phase0_support.h"
#include "../tests/decode_phase1_support.h"
#include "hypercine/decode_phase2b.h"
#include "hypercine/decode_phase2c.h"

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
    double p2c_entropy_ms;
    double p2c_host_index_ms;
    double p2c_vlc_ms;
    double p2c_diff_coding_ms;
    double p2c_total_ms;
    double entropy_speedup;
    double total_speedup;
    double ratio_vs_cpu;
};

} // namespace

int main(int argc, char** argv) {
    try {
        std::cout << "=== HyperCine Decode Phase 2C End-to-End Benchmark ===" << std::endl;

        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, 0);
        std::cout << "GPU Device: " << prop.name << " (CC " << prop.major << "." << prop.minor
                  << ", " << (prop.totalGlobalMem / (1024 * 1024)) << " MB VRAM)\n" << std::endl;

        std::string output = "benchmarks/results/decode_phase2c_windows_msvc.json";
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
            std::filesystem::create_directories(out_path.parent_path());
        }

        std::ofstream json(output);
        if (!json) {
            std::cerr << "Failed to open output file: " << output << " (" << strerror(errno) << ")" << std::endl;
            return 1;
        }

        json << "{\n"
             << "  \"benchmark\": \"decode_phase2c\",\n"
             << "  \"gpu\": \"" << prop.name << "\",\n"
             << "  \"compute_capability\": \"" << prop.major << "." << prop.minor << "\",\n"
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

            // Warmup
            for (auto& s : samples) {
                uint64_t t2b = pipeline_2b.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2b.WaitForDecode(t2b);
                pipeline_2b.ReleaseOutput(t2b);

                uint64_t t2c = pipeline_2c.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2c.WaitForDecode(t2c);
                pipeline_2c.ReleaseOutput(t2c);
            }

            // ----------------------------------------------------
            // Per-Pattern Benchmarking (Section 28)
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

                // 3. Phase 2C Parallel GPU timing (Adaptive Mode)
                double p2c_ent_sum_ms = 0;
                double p2c_index_sum_ms = 0;
                double p2c_vlc_sum_ms = 0;
                double p2c_diff_sum_ms = 0;
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
                    p2c_diff_sum_ms += timings.gpu_diff_coding_ms;
                    pipeline_2c.ReleaseOutput(t);
                }

                double p2c_avg_ent_ms = p2c_ent_sum_ms / p2c_runs;
                double p2c_avg_tot_ms = p2c_tot_sum_ms / p2c_runs;
                double ent_speedup = (p2c_avg_ent_ms > 0) ? (p2b_avg_ent_ms / p2c_avg_ent_ms) : 1.0;
                double tot_speedup = (p2c_avg_tot_ms > 0) ? (p2b_avg_tot_ms / p2c_avg_tot_ms) : 1.0;
                double ratio_cpu = (p2c_avg_tot_ms > 0) ? (cpu_avg_ms / p2c_avg_tot_ms) : 1.0;

                PatternBenchmarkResult pr;
                pr.pattern_name = PatternName(p);
                pr.cpu_total_ms = cpu_avg_ms;
                pr.p2b_entropy_ms = p2b_avg_ent_ms;
                pr.p2b_total_ms = p2b_avg_tot_ms;
                pr.p2c_entropy_ms = p2c_avg_ent_ms;
                pr.p2c_host_index_ms = p2c_index_sum_ms / p2c_runs;
                pr.p2c_vlc_ms = p2c_vlc_sum_ms / p2c_runs;
                pr.p2c_diff_coding_ms = p2c_diff_sum_ms / p2c_runs;
                pr.p2c_total_ms = p2c_avg_tot_ms;
                pr.entropy_speedup = ent_speedup;
                pr.total_speedup = tot_speedup;
                pr.ratio_vs_cpu = ratio_cpu;
                pattern_results.push_back(pr);
            }

            // Print Per-Pattern Disclosure Table (Section 28)
            std::cout << "\nPer-Pattern Disclosure for " << cfg.name << ":" << std::endl;
            std::cout << std::string(110, '-') << std::endl;
            std::cout << std::left << std::setw(12) << "Pattern"
                      << std::right
                      << std::setw(12) << "CPU Tot(ms)"
                      << std::setw(14) << "2B Ent(ms)"
                      << std::setw(14) << "2C Ent(ms)"
                      << std::setw(14) << "2C Tot(ms)"
                      << std::setw(14) << "Ent Speedup"
                      << std::setw(14) << "Tot Speedup"
                      << std::setw(14) << "Ratio vs CPU"
                      << std::endl;
            std::cout << std::string(110, '-') << std::endl;

            for (const auto& pr : pattern_results) {
                std::cout << std::left << std::setw(12) << pr.pattern_name
                          << std::right << std::fixed << std::setprecision(2)
                          << std::setw(12) << pr.cpu_total_ms
                          << std::setw(14) << pr.p2b_entropy_ms
                          << std::setw(14) << pr.p2c_entropy_ms
                          << std::setw(14) << pr.p2c_total_ms
                          << std::setw(13) << pr.entropy_speedup << "x"
                          << std::setw(13) << pr.total_speedup << "x"
                          << std::setw(13) << pr.ratio_vs_cpu << "x"
                          << std::endl;
            }
            std::cout << std::string(110, '-') << std::endl;

            // ----------------------------------------------------
            // Five-Pattern Headline Benchmark (Section 29)
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
            std::vector<double> p2c_ent_lat, p2c_index_lat, p2c_vlc_lat, p2c_inv_lat, p2c_color_lat;

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

                        Phase2cSlotStageTimings timings{};
                        pipeline_2c.GetStageTimings(ticket, &timings);
                        p2c_ent_lat.push_back(timings.gpu_entropy_ms);
                        p2c_index_lat.push_back(timings.host_index_ms);
                        p2c_vlc_lat.push_back(timings.gpu_vlc_ms);
                        p2c_inv_lat.push_back(timings.inverse_wavelet_ms);
                        p2c_color_lat.push_back(timings.color_convert_ms);

                        pipeline_2c.ReleaseOutput(ticket);
                    }
                }
                p2c_headline_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            double mean_p2b_fps = std::accumulate(p2b_headline_fps.begin(), p2b_headline_fps.end(), 0.0) / p2b_headline_fps.size();
            double mean_p2c_fps = std::accumulate(p2c_headline_fps.begin(), p2c_headline_fps.end(), 0.0) / p2c_headline_fps.size();
            double mean_p2b_lat = std::accumulate(p2b_headline_lat.begin(), p2b_headline_lat.end(), 0.0) / p2b_headline_lat.size();
            double mean_p2c_lat = std::accumulate(p2c_headline_lat.begin(), p2c_headline_lat.end(), 0.0) / p2c_headline_lat.size();
            double mean_p2c_ent = std::accumulate(p2c_ent_lat.begin(), p2c_ent_lat.end(), 0.0) / p2c_ent_lat.size();

            std::cout << "Headline Results for " << cfg.name << ":" << std::endl;
            std::cout << "  Phase 2B DeviceBGRA: " << std::fixed << std::setprecision(2) << mean_p2b_fps << " FPS (" << mean_p2b_lat << " ms)" << std::endl;
            std::cout << "  Phase 2C DeviceBGRA: " << std::fixed << std::setprecision(2) << mean_p2c_fps << " FPS (" << mean_p2c_lat << " ms)" << std::endl;
            std::cout << "  Phase 2C Throughput: " << std::fixed << std::setprecision(2) << (mean_p2c_fps * megapixels) << " MP/s" << std::endl;
            std::cout << "  Overall Speedup vs Phase 2B: " << (mean_p2c_fps / mean_p2b_fps) << "x" << std::endl;

            // ----------------------------------------------------
            // Multi-Frame Concurrency Experiment (Section 33)
            // ----------------------------------------------------
            std::cout << "\nMulti-Frame Concurrency Scaling (" << cfg.name << "):" << std::endl;
            std::vector<int> slot_counts = {1, 2, 3, 4};
            for (int sc : slot_counts) {
                CudaDecoderPipelinePhase2C pipe_multi;
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
                std::cout << "  Slots=" << sc << ": " << std::fixed << std::setprecision(2) << fps << " FPS ("
                          << (fps * megapixels) << " MP/s, Latency: " << (total_ms / total_frames) << " ms/frame)" << std::endl;
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
                 << "      \"phase2c_throughput_mpps\": " << (mean_p2c_fps * megapixels) << ",\n"
                 << "      \"speedup_vs_phase2b\": " << (mean_p2c_fps / mean_p2b_fps) << ",\n"
                 << "      \"patterns\": [\n";

            for (size_t pi = 0; pi < pattern_results.size(); ++pi) {
                const auto& pr = pattern_results[pi];
                json << "        {\n"
                     << "          \"pattern\": \"" << pr.pattern_name << "\",\n"
                     << "          \"cpu_total_ms\": " << pr.cpu_total_ms << ",\n"
                     << "          \"phase2b_entropy_ms\": " << pr.p2b_entropy_ms << ",\n"
                     << "          \"phase2b_total_ms\": " << pr.p2b_total_ms << ",\n"
                     << "          \"phase2c_entropy_ms\": " << pr.p2c_entropy_ms << ",\n"
                     << "          \"phase2c_host_index_ms\": " << pr.p2c_host_index_ms << ",\n"
                     << "          \"phase2c_vlc_ms\": " << pr.p2c_vlc_ms << ",\n"
                     << "          \"phase2c_diff_coding_ms\": " << pr.p2c_diff_coding_ms << ",\n"
                     << "          \"phase2c_total_ms\": " << pr.p2c_total_ms << ",\n"
                     << "          \"entropy_speedup\": " << pr.entropy_speedup << ",\n"
                     << "          \"total_speedup\": " << pr.total_speedup << ",\n"
                     << "          \"ratio_vs_cpu\": " << pr.ratio_vs_cpu << "\n"
                     << "        }" << (pi + 1 < pattern_results.size() ? "," : "") << "\n";
            }
            json << "      ]\n"
                 << "    }";
        }

        json << "\n  ]\n}\n";
        json.close();

        std::cout << "\nBenchmark results successfully written to: " << output << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Benchmark failure: " << e.what() << std::endl;
        return 1;
    }
}
