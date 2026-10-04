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
#include <cuda_runtime.h>
#include "../tests/decode_phase0_support.h"
#include "../tests/decode_phase1_support.h"
#include "hypercine/decode_phase2a.h"
#include "hypercine/decode_phase2b.h"

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

} // namespace

int main(int argc, char** argv) {
    try {
        std::cout << "=== HyperCine Decode Phase 2B End-to-End Benchmark ===" << std::endl;

        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, 0);
        std::cout << "GPU Device: " << prop.name << " (CC " << prop.major << "." << prop.minor
                  << ", " << (prop.totalGlobalMem / (1024 * 1024)) << " MB VRAM)\n" << std::endl;

        std::string output = "benchmarks/results/decode_phase2b_windows_msvc.json";
        std::string resolution = "all";

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (arg == "--output") output = value;
            else if (arg == "--resolution") resolution = value;
            else throw std::runtime_error("unknown option: " + arg);
        }

        std::ofstream json(output);
        if (!json) {
            std::cerr << "Failed to open output file: " << output << " (" << strerror(errno) << ")" << std::endl;
            return 1;
        }

        json << "{\n"
             << "  \"benchmark\": \"decode_phase2b\",\n"
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

            CudaDecoderPipelinePhase2A pipeline_2a;
            if (!pipeline_2a.Initialize(w, h, 10)) {
                throw std::runtime_error("Failed to initialize Phase 2A pipeline");
            }

            CudaDecoderPipelinePhase2B pipeline_2b;
            if (!pipeline_2b.Initialize(w, h, 3, 10)) {
                throw std::runtime_error("Failed to initialize Phase 2B pipeline");
            }

            // Warmup Phase 2A and Phase 2B
            for (auto& s : samples) {
                ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                ref.Decode(s);
                auto* d = ref.GetNative();
                auto bands = ExtractBandsFromNative(d);
                uint64_t t2a = pipeline_2a.QueueDecode(bands, DeviceSurfaceType::BGRA8);
                pipeline_2a.WaitForDecode(t2a);
                pipeline_2a.ReleaseOutput(t2a);

                uint64_t t2b = pipeline_2b.QueueDecode(s.compressed.data(), s.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline_2b.WaitForDecode(t2b);
                pipeline_2b.ReleaseOutput(t2b);
            }

            // ----------------------------------------------------
            // 1. ReferenceCpu (Pristine CPU decode to BGRA8)
            // ----------------------------------------------------
            std::cout << "Benchmarking 1. ReferenceCpu (Pristine CPU BGRA8)..." << std::endl;
            std::vector<double> cpu_trial_fps;
            std::vector<double> cpu_all_latencies;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                        auto t0 = Clock::now();
                        ref.Decode(s);
                        auto t1 = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        trial_wall_ms += ms;
                        cpu_all_latencies.push_back(ms);
                    }
                }
                cpu_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            // ----------------------------------------------------
            // 2. Phase2ADeviceBGRA (CPU Entropy + Coeff H2D + CUDA Inv + Color)
            // ----------------------------------------------------
            std::cout << "Benchmarking 2. Phase2ADeviceBGRA (CPU Entropy + Coeff H2D + CUDA Inv + Color)..." << std::endl;
            std::vector<double> p2a_trial_fps;
            std::vector<double> p2a_all_latencies;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t_start = Clock::now();

                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                        ref.Decode(s);
                        auto* d = ref.GetNative();
                        auto bands = ExtractBandsFromNative(d);

                        uint64_t ticket = pipeline_2a.QueueDecode(bands, DeviceSurfaceType::BGRA8);
                        pipeline_2a.WaitForDecode(ticket);

                        auto t_finish = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();
                        trial_wall_ms += ms;
                        p2a_all_latencies.push_back(ms);

                        pipeline_2a.ReleaseOutput(ticket);
                    }
                }
                p2a_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            // ----------------------------------------------------
            // 3. Phase2BGpuEntropyDeviceBGRA (HostParse + CompressedH2D + GPU LL/VLC + CUDA Inv + Color)
            // ----------------------------------------------------
            std::cout << "Benchmarking 3. Phase2BGpuEntropyDeviceBGRA (Exact GPU Entropy + Native DeviceOutput)..." << std::endl;
            std::vector<double> p2b_trial_fps;
            std::vector<double> p2b_all_latencies;
            std::vector<double> stage_parse_ms;
            std::vector<double> stage_h2d_ms;
            std::vector<double> stage_ll_ms;
            std::vector<double> stage_vlc_ms;
            std::vector<double> stage_ent_ms;
            std::vector<double> stage_inv_ms;
            std::vector<double> stage_color_ms;
            std::vector<double> stage_total_gpu_ms;

            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t_start = Clock::now();

                        uint64_t ticket = pipeline_2b.QueueDecode(
                            s.compressed.data(),
                            s.compressed.size(),
                            DeviceSurfaceType::BGRA8,
                            ColorMatrix::BT709,
                            ColorRange::CG,
                            RowOrientation::BottomUp
                        );
                        pipeline_2b.WaitForDecode(ticket);

                        auto t_finish = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();
                        trial_wall_ms += ms;
                        p2b_all_latencies.push_back(ms);

                        Phase2bSlotStageTimings timings{};
                        pipeline_2b.GetStageTimings(ticket, &timings);
                        stage_parse_ms.push_back(timings.host_parse_ms);
                        stage_h2d_ms.push_back(timings.compressed_h2d_ms);
                        stage_ll_ms.push_back(timings.gpu_ll_ms);
                        stage_vlc_ms.push_back(timings.gpu_vlc_ms);
                        stage_ent_ms.push_back(timings.gpu_entropy_ms);
                        stage_inv_ms.push_back(timings.inverse_wavelet_ms);
                        stage_color_ms.push_back(timings.color_convert_ms);
                        stage_total_gpu_ms.push_back(timings.total_gpu_ms);

                        pipeline_2b.ReleaseOutput(ticket);
                    }
                }
                p2b_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            // ----------------------------------------------------
            // 4. Phase2BHostBGRA (Phase2B DeviceBGRA + BGRA D2H to Host RAM)
            // ----------------------------------------------------
            std::cout << "Benchmarking 4. Phase2BHostBGRA (Phase2B DeviceBGRA + D2H to Host RAM)..." << std::endl;
            std::vector<double> host_bgra_trial_fps;
            std::vector<double> host_bgra_all_latencies;
            std::vector<double> stage_bgra_d2h_ms;
            int bgra_pitch = w * 4;
            std::vector<uint8_t> host_bgra_buf(bgra_pitch * h, 0);

            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t_start = Clock::now();

                        uint64_t ticket = pipeline_2b.QueueDecode(
                            s.compressed.data(),
                            s.compressed.size(),
                            DeviceSurfaceType::BGRA8,
                            ColorMatrix::BT709,
                            ColorRange::CG,
                            RowOrientation::BottomUp
                        );
                        pipeline_2b.WaitForDecode(ticket);

                        auto t_d2h0 = Clock::now();
                        pipeline_2b.DownloadBgraToHost(ticket, host_bgra_buf.data(), bgra_pitch);
                        auto t_d2h1 = Clock::now();
                        double d2h_ms = std::chrono::duration<double, std::milli>(t_d2h1 - t_d2h0).count();

                        auto t_finish = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();
                        trial_wall_ms += ms;
                        host_bgra_all_latencies.push_back(ms);
                        stage_bgra_d2h_ms.push_back(d2h_ms);

                        pipeline_2b.ReleaseOutput(ticket);
                    }
                }
                host_bgra_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            auto avg = [](const std::vector<double>& v) {
                return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size();
            };

            double cpu_med_fps = Percentile(cpu_trial_fps, 0.5);
            double p2a_med_fps = Percentile(p2a_trial_fps, 0.5);
            double p2b_med_fps = Percentile(p2b_trial_fps, 0.5);
            double host_bgra_med_fps = Percentile(host_bgra_trial_fps, 0.5);

            double cpu_mean_lat = avg(cpu_all_latencies);
            double p2a_mean_lat = avg(p2a_all_latencies);
            double p2b_mean_lat = avg(p2b_all_latencies);
            double host_bgra_mean_lat = avg(host_bgra_all_latencies);

            double p2a_mp_s = p2a_med_fps * megapixels;
            double p2b_mp_s = p2b_med_fps * megapixels;

            double m_parse = avg(stage_parse_ms);
            double m_h2d = avg(stage_h2d_ms);
            double m_ll = avg(stage_ll_ms);
            double m_vlc = avg(stage_vlc_ms);
            double m_ent = avg(stage_ent_ms);
            double m_inv = avg(stage_inv_ms);
            double m_color = avg(stage_color_ms);
            double m_gpu = avg(stage_total_gpu_ms);
            double m_d2h = avg(stage_bgra_d2h_ms);

            std::cout << "\nResults Summary for " << cfg.name << " (" << w << "x" << h << "):" << std::endl;
            std::cout << "  1. ReferenceCpu:            " << std::setw(7) << std::fixed << std::setprecision(2)
                      << cpu_med_fps << " FPS | " << cpu_mean_lat << " ms" << std::endl;
            std::cout << "  2. Phase2ADeviceBGRA:       " << std::setw(7) << p2a_med_fps << " FPS | "
                      << p2a_mean_lat << " ms (" << p2a_mp_s << " MP/s)" << std::endl;
            std::cout << "  3. Phase2BGpuEntropyBGRA:   " << std::setw(7) << p2b_med_fps << " FPS | "
                      << p2b_mean_lat << " ms (" << p2b_mp_s << " MP/s)" << std::endl;
            std::cout << "  4. Phase2BHostBGRA:         " << std::setw(7) << host_bgra_med_fps << " FPS | "
                      << host_bgra_mean_lat << " ms" << std::endl;
            std::cout << "  Phase 2B Stage Breakdown:" << std::endl;
            std::cout << "    Host Parse:     " << m_parse << " ms" << std::endl;
            std::cout << "    Compressed H2D: " << m_h2d << " ms" << std::endl;
            std::cout << "    GPU LowPass:    " << m_ll << " ms" << std::endl;
            std::cout << "    GPU VLC/FSM:    " << m_vlc << " ms" << std::endl;
            std::cout << "    GPU Entropy:    " << m_ent << " ms" << std::endl;
            std::cout << "    CUDA Inverse:   " << m_inv << " ms" << std::endl;
            std::cout << "    CUDA Color:     " << m_color << " ms" << std::endl;
            std::cout << "    Total GPU:      " << m_gpu << " ms" << std::endl;
            std::cout << "    BGRA D2H:       " << m_d2h << " ms" << std::endl;

            if (!first_res) json << ",\n";
            first_res = false;

            json << "    {\n"
                 << "      \"name\": \"" << cfg.name << "\", \"width\": " << w << ", \"height\": " << h << ",\n"
                 << "      \"reference_cpu\": { \"fps\": " << cpu_med_fps << ", \"latency_ms\": " << cpu_mean_lat << " },\n"
                 << "      \"phase2a_device_bgra\": {\n"
                 << "        \"fps\": " << p2a_med_fps << ", \"latency_ms\": " << p2a_mean_lat << ",\n"
                 << "        \"megapixels_per_sec\": " << p2a_mp_s << "\n"
                 << "      },\n"
                 << "      \"phase2b_device_bgra\": {\n"
                 << "        \"fps\": " << p2b_med_fps << ", \"latency_ms\": " << p2b_mean_lat << ",\n"
                 << "        \"megapixels_per_sec\": " << p2b_mp_s << ",\n"
                 << "        \"stages_ms\": {\n"
                 << "          \"host_parse\": " << m_parse << ",\n"
                 << "          \"compressed_h2d\": " << m_h2d << ",\n"
                 << "          \"gpu_ll\": " << m_ll << ",\n"
                 << "          \"gpu_vlc\": " << m_vlc << ",\n"
                 << "          \"gpu_entropy\": " << m_ent << ",\n"
                 << "          \"cuda_inverse\": " << m_inv << ",\n"
                 << "          \"cuda_color_conversion\": " << m_color << ",\n"
                 << "          \"total_gpu\": " << m_gpu << "\n"
                 << "        }\n"
                 << "      },\n"
                 << "      \"phase2b_host_bgra\": {\n"
                 << "        \"fps\": " << host_bgra_med_fps << ", \"latency_ms\": " << host_bgra_mean_lat << ",\n"
                 << "        \"d2h_ms\": " << m_d2h << "\n"
                 << "      }\n"
                 << "    }";
        }

        json << "\n  ],\n"
             << "  \"memory\": {\n"
             << "    \"peak_working_set_bytes\": " << PeakMemory() << ",\n"
             << "    \"peak_gpu_allocated_bytes\": " << PeakGpuMemory() << "\n"
             << "  }\n}\n";

        std::cout << "\nSUCCESS: Phase 2B Benchmark completed and results saved to: " << output << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "BENCHMARK ERROR: " << e.what() << std::endl;
        return 1;
    }
}
