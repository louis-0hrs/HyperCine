// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#define NOMINMAX
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
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

struct Stats {
    double mean = 0;
    double median = 0;
    double min_v = 0;
    double max_v = 0;
    double stddev = 0;
};

Stats CalcStats(std::vector<double> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    double mean = sum / v.size();
    double median = (v.size() % 2 == 0) ? (v[v.size()/2 - 1] + v[v.size()/2]) / 2.0 : v[v.size()/2];
    double var = 0;
    for (double x : v) var += (x - mean) * (x - mean);
    var /= v.size();
    return { mean, median, v.front(), v.back(), std::sqrt(var) };
}

struct ModeTiming {
    std::string mode_name;
    Stats stats;
    std::vector<double> runs;
};

struct HeadlineRunRecord {
    int run_index;
    double p2d_ms;
    double p2e_ms;
    double p2fc_static_ms;
    double p2fc_adapt_ms;
    double speedup_static_vs_2e;
    double speedup_static_vs_2d;
};

struct HeadlineCaseResult {
    std::string name;
    int width;
    int height;
    std::vector<HeadlineRunRecord> runs;
    Stats p2d_stats;
    Stats p2e_stats;
    Stats p2fc_static_stats;
    Stats p2fc_adapt_stats;
    double median_run_mean_static_ms;
    double median_run_mean_2e_ms;
    double speedup_vs_2e;
};

struct FivePatternResult {
    std::string resolution;
    std::string pattern;
    double p2d_ms;
    double p2e_ms;
    double p2fc_static_ms;
    double p2fc_adapt_ms;
    double oracle_ms;
    double static_regret_ms;
    double adapt_regret_ms;
    double static_efficiency_pct;
    double adapt_efficiency_pct;
    double cpu_ms;
};

} // anonymous namespace

int main(int argc, char** argv) {
    try {
        std::cout << "======================================================================" << std::endl;
        std::cout << "HyperCine Decode Phase 2F-C Audit Benchmark Reproduction Harness" << std::endl;
        std::cout << "======================================================================" << std::endl;

        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, 0);
        std::cout << "GPU: " << prop.name << " (CC " << prop.major << "." << prop.minor
                  << ", " << (prop.totalGlobalMem / (1024 * 1024)) << " MB VRAM)" << std::endl;

        std::filesystem::create_directories("benchmarks/results");

        // Prepare samples
        auto samples_1080p = Generate(1920, 1080, 5);
        auto samples_4k = Generate(3840, 2160, 5);

        // ------------------------------------------------------------------
        // PART 1: Three Independent Controlled Headline Reproduction Runs (Items 7, 8, 9, 10, 37)
        // Cases: 1080p Noise, 4K Noise, 4K Texture
        // ------------------------------------------------------------------
        struct HeadlineTestConfig {
            std::string name;
            int width;
            int height;
            Sample sample;
            int iters;
        };

        std::vector<HeadlineTestConfig> headline_configs = {
            { "1080p_noise", 1920, 1080, samples_1080p[2], 15 },
            { "4k_noise", 3840, 2160, samples_4k[2], 10 },
            { "4k_texture", 3840, 2160, samples_4k[3], 10 }
        };

        std::vector<HeadlineCaseResult> headline_results;

        for (const auto& cfg : headline_configs) {
            std::cout << "\n--------------------------------------------------" << std::endl;
            std::cout << "Executing 3 Independent Runs for: " << cfg.name << " (" << cfg.width << "x" << cfg.height << ")" << std::endl;
            std::cout << "--------------------------------------------------" << std::endl;

            HeadlineCaseResult case_res{};
            case_res.name = cfg.name;
            case_res.width = cfg.width;
            case_res.height = cfg.height;

            std::vector<double> all_p2d_means, all_p2e_means, all_static_means, all_adapt_means;

            for (int r = 1; r <= 3; ++r) {
                std::cout << "  [Run " << r << "/3]: Warmup & measuring..." << std::endl;

                // Setup pipelines
                CudaDecoderPipelinePhase2D p2d;
                p2d.Initialize(cfg.width, cfg.height, 3, 10, 0);

                CudaDecoderPipelinePhase2E p2e;
                p2e.Initialize(cfg.width, cfg.height, 3, 10, 8, 4);

                CudaDecoderPipelinePhase2E p2fc;
                p2fc.Initialize(cfg.width, cfg.height, 3, 10, 8, 4);

                const auto& s = cfg.sample;

                // 1. Phase 2D
                uint64_t tw = p2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2d.WaitForDecode(tw); p2d.ReleaseOutput(tw);
                auto t0 = Clock::now();
                for (int i = 0; i < cfg.iters; ++i) {
                    uint64_t t = p2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2d.WaitForDecode(t); p2d.ReleaseOutput(t);
                }
                auto t1 = Clock::now();
                double p2d_mean = std::chrono::duration<double, std::milli>(t1 - t0).count() / cfg.iters;

                // 2. Phase 2E
                p2e.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
                tw = p2e.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2e.WaitForDecode(tw); p2e.ReleaseOutput(tw);
                t0 = Clock::now();
                for (int i = 0; i < cfg.iters; ++i) {
                    uint64_t t = p2e.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2e.WaitForDecode(t); p2e.ReleaseOutput(t);
                }
                t1 = Clock::now();
                double p2e_mean = std::chrono::duration<double, std::milli>(t1 - t0).count() / cfg.iters;

                // 3. Phase 2F-C Static Heuristic
                p2fc.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame);
                p2fc.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::StaticHeuristic);
                p2fc.SetPhase2FCChunkSize(256);
                tw = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2fc.WaitForDecode(tw); p2fc.ReleaseOutput(tw);
                t0 = Clock::now();
                for (int i = 0; i < cfg.iters; ++i) {
                    uint64_t t = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2fc.WaitForDecode(t); p2fc.ReleaseOutput(t);
                }
                t1 = Clock::now();
                double static_mean = std::chrono::duration<double, std::milli>(t1 - t0).count() / cfg.iters;

                // 4. Phase 2F-C Adaptive Dynamic
                p2fc.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::AdaptiveDynamic);
                tw = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2fc.WaitForDecode(tw); p2fc.ReleaseOutput(tw);
                t0 = Clock::now();
                for (int i = 0; i < cfg.iters; ++i) {
                    uint64_t t = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2fc.WaitForDecode(t); p2fc.ReleaseOutput(t);
                }
                t1 = Clock::now();
                double adapt_mean = std::chrono::duration<double, std::milli>(t1 - t0).count() / cfg.iters;

                all_p2d_means.push_back(p2d_mean);
                all_p2e_means.push_back(p2e_mean);
                all_static_means.push_back(static_mean);
                all_adapt_means.push_back(adapt_mean);

                HeadlineRunRecord rec{};
                rec.run_index = r;
                rec.p2d_ms = p2d_mean;
                rec.p2e_ms = p2e_mean;
                rec.p2fc_static_ms = static_mean;
                rec.p2fc_adapt_ms = adapt_mean;
                rec.speedup_static_vs_2e = p2e_mean / static_mean;
                rec.speedup_static_vs_2d = p2d_mean / static_mean;
                case_res.runs.push_back(rec);

                std::cout << "    Run " << r << " -> P2D: " << std::fixed << std::setprecision(2) << p2d_mean
                          << " ms | P2E: " << p2e_mean << " ms | P2F-C Static: " << static_mean
                          << " ms | Speedup vs 2E: " << std::setprecision(3) << rec.speedup_static_vs_2e << "x" << std::endl;
            }

            case_res.p2d_stats = CalcStats(all_p2d_means);
            case_res.p2e_stats = CalcStats(all_p2e_means);
            case_res.p2fc_static_stats = CalcStats(all_static_means);
            case_res.p2fc_adapt_stats = CalcStats(all_adapt_means);
            case_res.median_run_mean_static_ms = case_res.p2fc_static_stats.median;
            case_res.median_run_mean_2e_ms = case_res.p2e_stats.median;
            case_res.speedup_vs_2e = case_res.median_run_mean_2e_ms / case_res.median_run_mean_static_ms;
            headline_results.push_back(case_res);

            std::cout << "  ==> Summary for " << cfg.name << ":" << std::endl;
            std::cout << "      Phase 2E median: " << case_res.median_run_mean_2e_ms << " ms" << std::endl;
            std::cout << "      Phase 2F-C Static median: " << case_res.median_run_mean_static_ms << " ms" << std::endl;
            std::cout << "      Speedup vs 2E: " << case_res.speedup_vs_2e << "x" << std::endl;
        }

        // ------------------------------------------------------------------
        // PART 2: Five-Pattern Aggregate Benchmark & Oracle Regret (Items 16, 17, 35, 36)
        // ------------------------------------------------------------------
        std::cout << "\n========================================================" << std::endl;
        std::cout << "Executing Five-Pattern Aggregate & Oracle Regret Benchmark" << std::endl;
        std::cout << "========================================================" << std::endl;

        std::vector<FivePatternResult> pattern_results;

        struct AggregateConfig {
            std::string res_name;
            int width;
            int height;
            std::vector<Sample> samples;
        };

        std::vector<AggregateConfig> agg_configs = {
            { "1080p", 1920, 1080, samples_1080p },
            { "4k", 3840, 2160, samples_4k }
        };

        for (auto& ac : agg_configs) {
            CudaDecoderPipelinePhase2D p2d;
            p2d.Initialize(ac.width, ac.height, 3, 10, 0);

            CudaDecoderPipelinePhase2E p2e;
            p2e.Initialize(ac.width, ac.height, 3, 10, 8, 4);

            CudaDecoderPipelinePhase2E p2fc;
            p2fc.Initialize(ac.width, ac.height, 3, 10, 8, 4);

            for (int p = 0; p < 5; ++p) {
                auto& s = ac.samples[p];
                std::string pat_name = PatternName(p);

                // CPU
                ReferenceDecoder ref_bgra(s, CFHD_PIXEL_FORMAT_BGRA);
                auto t0 = Clock::now();
                ref_bgra.Decode(s);
                auto t1 = Clock::now();
                double cpu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

                // P2D
                int iters = (ac.width == 3840) ? 8 : 12;
                uint64_t tw = p2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2d.WaitForDecode(tw); p2d.ReleaseOutput(tw);
                t0 = Clock::now();
                for (int i = 0; i < iters; ++i) {
                    uint64_t t = p2d.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2d.WaitForDecode(t); p2d.ReleaseOutput(t);
                }
                t1 = Clock::now();
                double p2d_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;

                // P2E
                p2e.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
                tw = p2e.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2e.WaitForDecode(tw); p2e.ReleaseOutput(tw);
                t0 = Clock::now();
                for (int i = 0; i < iters; ++i) {
                    uint64_t t = p2e.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2e.WaitForDecode(t); p2e.ReleaseOutput(t);
                }
                t1 = Clock::now();
                double p2e_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;

                // P2FC Static
                p2fc.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame);
                p2fc.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::StaticHeuristic);
                tw = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2fc.WaitForDecode(tw); p2fc.ReleaseOutput(tw);
                t0 = Clock::now();
                for (int i = 0; i < iters; ++i) {
                    uint64_t t = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2fc.WaitForDecode(t); p2fc.ReleaseOutput(t);
                }
                t1 = Clock::now();
                double static_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;

                // P2FC Adaptive
                p2fc.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::AdaptiveDynamic);
                tw = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                p2fc.WaitForDecode(tw); p2fc.ReleaseOutput(tw);
                t0 = Clock::now();
                for (int i = 0; i < iters; ++i) {
                    uint64_t t = p2fc.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                    p2fc.WaitForDecode(t); p2fc.ReleaseOutput(t);
                }
                t1 = Clock::now();
                double adapt_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;

                double oracle_ms = std::min({ static_ms, adapt_ms, p2e_ms });
                double static_regret = static_ms - oracle_ms;
                double adapt_regret = adapt_ms - oracle_ms;
                double static_eff = (static_ms > 0) ? (oracle_ms / static_ms) * 100.0 : 100.0;
                double adapt_eff = (adapt_ms > 0) ? (oracle_ms / adapt_ms) * 100.0 : 100.0;

                FivePatternResult fpr{};
                fpr.resolution = ac.res_name;
                fpr.pattern = pat_name;
                fpr.p2d_ms = p2d_ms;
                fpr.p2e_ms = p2e_ms;
                fpr.p2fc_static_ms = static_ms;
                fpr.p2fc_adapt_ms = adapt_ms;
                fpr.oracle_ms = oracle_ms;
                fpr.static_regret_ms = static_regret;
                fpr.adapt_regret_ms = adapt_regret;
                fpr.static_efficiency_pct = static_eff;
                fpr.adapt_efficiency_pct = adapt_eff;
                fpr.cpu_ms = cpu_ms;
                pattern_results.push_back(fpr);

                std::cout << "  " << ac.res_name << " " << std::setw(8) << pat_name
                          << " | P2D: " << std::fixed << std::setprecision(2) << p2d_ms
                          << " ms | P2E: " << p2e_ms
                          << " ms | Static: " << static_ms
                          << " ms | Adapt: " << adapt_ms
                          << " ms | Oracle: " << oracle_ms
                          << " ms | Static Regret: " << std::setprecision(3) << static_regret
                          << " ms | Adapt Regret: " << adapt_regret << " ms" << std::endl;
            }
        }

        // ------------------------------------------------------------------
        // PART 3: Multi-Slot Throughput Sweep on 4K Noise (Item 34, 46)
        // ------------------------------------------------------------------
        std::cout << "\n========================================================" << std::endl;
        std::cout << "Pipeline Slot Scaling Sweep on 4K Noise (1..4 Slots)" << std::endl;
        std::cout << "========================================================" << std::endl;

        struct SlotRecord {
            int slots;
            double fps;
            double mpps;
            double latency_ms;
        };
        std::vector<SlotRecord> slot_records;

        const auto& s_4k_noise = samples_4k[2];
        for (int sc = 1; sc <= 4; ++sc) {
            CudaDecoderPipelinePhase2E pipe;
            pipe.Initialize(3840, 2160, sc, 10, 8, 4);
            pipe.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame);
            pipe.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::StaticHeuristic);

            int n_frames = 12;
            std::vector<uint64_t> tickets;
            auto t0 = Clock::now();
            for (int i = 0; i < n_frames; ++i) {
                uint64_t tk = 0;
                while ((tk = pipe.QueueDecode(s_4k_noise.compressed.data(), s_4k_noise.compressed.size(), DeviceSurfaceType::BGRA8)) == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                tickets.push_back(tk);
                if (tickets.size() >= size_t(sc)) {
                    pipe.WaitForDecode(tickets.front());
                    pipe.ReleaseOutput(tickets.front());
                    tickets.erase(tickets.begin());
                }
            }
            for (uint64_t tk : tickets) {
                pipe.WaitForDecode(tk);
                pipe.ReleaseOutput(tk);
            }
            auto t1 = Clock::now();
            double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double fps = (n_frames * 1000.0) / total_ms;
            double mpps = fps * 8.2944;
            double lat_ms = total_ms / n_frames;

            slot_records.push_back({ sc, fps, mpps, lat_ms });
            std::cout << "  Slot count " << sc << " | FPS: " << std::fixed << std::setprecision(1) << fps
                      << " | MP/s: " << mpps << " | Latency: " << std::setprecision(2) << lat_ms << " ms" << std::endl;
        }

        // ------------------------------------------------------------------
        // Output Machine-Readable Reproduction Artifact (Item 42)
        // ------------------------------------------------------------------
        std::ofstream json_out("benchmarks/results/decode_phase2fc_reproduction.json");
        json_out << "{\n"
                 << "  \"benchmark\": \"HyperCine Phase 2F-C Audit Reproduction Pass\",\n"
                 << "  \"gpu\": \"" << prop.name << "\",\n"
                 << "  \"headline_runs\": [\n";

        for (size_t i = 0; i < headline_results.size(); ++i) {
            const auto& hr = headline_results[i];
            json_out << "    {\n"
                     << "      \"case\": \"" << hr.name << "\",\n"
                     << "      \"width\": " << hr.width << ",\n"
                     << "      \"height\": " << hr.height << ",\n"
                     << "      \"runs\": [\n";
            for (size_t r = 0; r < hr.runs.size(); ++r) {
                const auto& run = hr.runs[r];
                json_out << "        {\n"
                         << "          \"run_index\": " << run.run_index << ",\n"
                         << "          \"p2d_ms\": " << run.p2d_ms << ",\n"
                         << "          \"p2e_ms\": " << run.p2e_ms << ",\n"
                         << "          \"p2fc_static_ms\": " << run.p2fc_static_ms << ",\n"
                         << "          \"p2fc_adapt_ms\": " << run.p2fc_adapt_ms << ",\n"
                         << "          \"speedup_vs_2e\": " << run.speedup_static_vs_2e << ",\n"
                         << "          \"speedup_vs_2d\": " << run.speedup_static_vs_2d << "\n"
                         << "        }" << (r + 1 < hr.runs.size() ? ",\n" : "\n");
            }
            json_out << "      ],\n"
                     << "      \"p2e_median_ms\": " << hr.median_run_mean_2e_ms << ",\n"
                     << "      \"p2fc_static_median_ms\": " << hr.median_run_mean_static_ms << ",\n"
                     << "      \"speedup_vs_2e\": " << hr.speedup_vs_2e << "\n"
                     << "    }" << (i + 1 < headline_results.size() ? ",\n" : "\n");
        }

        json_out << "  ],\n  \"five_pattern_evaluations\": [\n";
        for (size_t i = 0; i < pattern_results.size(); ++i) {
            const auto& pr = pattern_results[i];
            json_out << "    {\n"
                     << "      \"resolution\": \"" << pr.resolution << "\",\n"
                     << "      \"pattern\": \"" << pr.pattern << "\",\n"
                     << "      \"p2d_ms\": " << pr.p2d_ms << ",\n"
                     << "      \"p2e_ms\": " << pr.p2e_ms << ",\n"
                     << "      \"p2fc_static_ms\": " << pr.p2fc_static_ms << ",\n"
                     << "      \"p2fc_adapt_ms\": " << pr.p2fc_adapt_ms << ",\n"
                     << "      \"oracle_ms\": " << pr.oracle_ms << ",\n"
                     << "      \"static_regret_ms\": " << pr.static_regret_ms << ",\n"
                     << "      \"adapt_regret_ms\": " << pr.adapt_regret_ms << ",\n"
                     << "      \"static_efficiency_pct\": " << pr.static_efficiency_pct << ",\n"
                     << "      \"adapt_efficiency_pct\": " << pr.adapt_efficiency_pct << ",\n"
                     << "      \"cpu_ms\": " << pr.cpu_ms << "\n"
                     << "    }" << (i + 1 < pattern_results.size() ? ",\n" : "\n");
        }

        json_out << "  ],\n  \"slot_scaling\": [\n";
        for (size_t i = 0; i < slot_records.size(); ++i) {
            const auto& sr = slot_records[i];
            json_out << "    {\n"
                     << "      \"slots\": " << sr.slots << ",\n"
                     << "      \"fps\": " << sr.fps << ",\n"
                     << "      \"mpps\": " << sr.mpps << ",\n"
                     << "      \"latency_ms\": " << sr.latency_ms << "\n"
                     << "    }" << (i + 1 < slot_records.size() ? ",\n" : "\n");
        }
        json_out << "  ]\n}\n";
        json_out.close();

        std::cout << "\nReproduction benchmark completed and saved to benchmarks/results/decode_phase2fc_reproduction.json!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Reproduction benchmark error: " << e.what() << std::endl;
        return 1;
    }
}
