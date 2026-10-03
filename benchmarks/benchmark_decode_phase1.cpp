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
#include <cuda_runtime.h>
#include "../tests/decode_phase0_support.h"
#include "../tests/decode_phase1_support.h"
#include "hypercine/decode_inverse_cuda.h"

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
        int trials = 5, cycles = 3;
        std::string resolution = "all";
        std::string output = "benchmarks/results/decode_phase1_windows_msvc.json";

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (arg == "--trials") trials = std::stoi(value);
            else if (arg == "--cycles") cycles = std::stoi(value);
            else if (arg == "--resolution") resolution = value;
            else if (arg == "--output") output = value;
            else throw std::runtime_error("unknown option: " + arg);
        }

        if (trials < 1 || cycles < 1 || trials > 100 || cycles > 1000)
            throw std::runtime_error("trials/cycles out of range");
        if (resolution != "all" && resolution != "1080" && resolution != "4k" && resolution != "8k")
            throw std::runtime_error("resolution must be all/1080/4k/8k");

        std::ofstream json_file;
        if (output != "-") {
            json_file.open(output);
            if (!json_file.is_open()) {
                throw std::runtime_error("cannot open output file: " + output + " (" + strerror(errno) + ")");
            }
        }
        std::ostream& json = output == "-" ? std::cout : json_file;

        std::cout << "=== HyperCine Phase 1 Decoder Benchmark ===" << std::endl;
        std::cout << "Configuration: trials=" << trials << ", cycles=" << cycles
                  << ", resolution=" << resolution << std::endl;

        json << std::fixed << std::setprecision(6)
             << "{\n  \"schema\": \"hypercine_decode_phase1_v1\",\n"
             << "  \"contract\": \"compressed sample -> CPU entropy -> CUDA inverse wavelet -> D2H -> CPU YU64 conversion\",\n"
             << "  \"input\": \"YUY2 YUV422 FilmScan3\", \"seed\": 322420463,\n"
             << "  \"trials\": " << trials << ", \"cycles_per_trial\": " << cycles << ",\n"
             << "  \"resolutions\": [\n";

        const int widths[] = {1920, 3840, 7680};
        const int heights[] = {1080, 2160, 4320};
        const char* names[] = {"1080", "4k", "8k"};

        bool first_res = true;

        for (int r = 0; r < 3; ++r) {
            if (resolution != "all" && resolution != names[r]) continue;

            int w = widths[r];
            int h = heights[r];
            int num_patterns = (r == 2) ? 1 : 5;

            std::cout << "\n--- Running " << names[r] << " (" << w << "x" << h << ") ---" << std::endl;
            auto samples = Generate(w, h, num_patterns);

            CudaInverseWaveletPipeline pipeline;
            if (!pipeline.Initialize(w, h, 10)) {
                throw std::runtime_error("Failed to initialize CUDA pipeline");
            }

            int frames_per_trial = num_patterns * cycles;

            // Warmup
            std::cout << "Warming up..." << std::endl;
            for (auto& s : samples) {
                ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_YU64);
                ref.Decode(s);
                auto* d = ref.GetNative();
                auto bands = ExtractBandsFromNative(d);
                pipeline.UploadCoefficients(bands);
                pipeline.Reconstruct();
                std::vector<int16_t> wy(w * h, 0), wv((w/2)*h, 0), wu((w/2)*h, 0);
                pipeline.DownloadPlanes(wy.data(), w * 2, wv.data(), w, wu.data(), w);
                std::vector<uint8_t> wyu64(ref.pitch * h, 0);
                ConvertPlanarToYU64(wy.data(), w * 2, wv.data(), w, wu.data(), w, w, h, 10, wyu64.data(), ref.pitch);
            }

            // Benchmark CPU Baseline
            std::cout << "Benchmarking CPU Reference Decoder (YU64)..." << std::endl;
            std::vector<double> cpu_trial_fps;
            std::vector<double> cpu_all_latencies;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_YU64);
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

            // Benchmark Hybrid Path (CPU Entropy + CUDA Inverse + CPU Packing)
            std::cout << "Benchmarking Hybrid Path (CPU Entropy + CUDA Inverse Wavelet + CPU YU64)..." << std::endl;
            std::vector<double> hybrid_trial_fps;
            std::vector<double> hybrid_all_latencies;

            std::vector<double> stage_cpu_entropy_ms;
            std::vector<double> stage_h2d_ms;
            std::vector<double> stage_cuda_inv_ms;
            std::vector<double> stage_d2h_ms;
            std::vector<double> stage_cpu_convert_ms;

            std::vector<int16_t> host_y(w * h, 0);
            std::vector<int16_t> host_v((w / 2) * h, 0);
            std::vector<int16_t> host_u((w / 2) * h, 0);

            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_YU64);
                        std::vector<uint8_t> out_yu64(ref.pitch * h, 0);

                        auto t_start = Clock::now();

                        // 1. CPU Entropy Decode
                        auto t_ent0 = Clock::now();
                        ref.Decode(s);
                        auto* d = ref.GetNative();
                        auto bands = ExtractBandsFromNative(d);
                        auto t_ent1 = Clock::now();
                        double ent_ms = std::chrono::duration<double, std::milli>(t_ent1 - t_ent0).count();

                        // 2. Coefficient H2D
                        pipeline.UploadCoefficients(bands);
                        double h2d_ms = pipeline.GetH2DTimeMs();

                        // 3. CUDA Inverse Wavelet DAG
                        pipeline.Reconstruct();
                        double inv_ms = pipeline.GetInverseTimeMs();

                        // 4. Planar D2H
                        pipeline.DownloadPlanes(host_y.data(), w * 2, host_v.data(), w, host_u.data(), w);
                        double d2h_ms = pipeline.GetD2HTimeMs();

                        // 5. CPU final output conversion
                        auto t_conv0 = Clock::now();
                        ConvertPlanarToYU64(
                            host_y.data(), w * 2, host_v.data(), w, host_u.data(), w,
                            w, h, d->codec.precision, out_yu64.data(), ref.pitch
                        );
                        auto t_conv1 = Clock::now();
                        double conv_ms = std::chrono::duration<double, std::milli>(t_conv1 - t_conv0).count();

                        auto t_finish = Clock::now();
                        double total_frame_ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();

                        trial_wall_ms += total_frame_ms;
                        hybrid_all_latencies.push_back(total_frame_ms);

                        stage_cpu_entropy_ms.push_back(ent_ms);
                        stage_h2d_ms.push_back(h2d_ms);
                        stage_cuda_inv_ms.push_back(inv_ms);
                        stage_d2h_ms.push_back(d2h_ms);
                        stage_cpu_convert_ms.push_back(conv_ms);

                        // Verify exactness on every timed frame
                        if (std::memcmp(out_yu64.data(), ref.output.data(), out_yu64.size()) != 0) {
                            throw std::runtime_error("Benchmark exactness check failed!");
                        }
                    }
                }
                hybrid_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            auto avg = [](const std::vector<double>& v) {
                return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
            };

            double cpu_median_fps = Percentile(cpu_trial_fps, 0.5);
            double hybrid_median_fps = Percentile(hybrid_trial_fps, 0.5);

            double cpu_mean_lat = avg(cpu_all_latencies);
            double hybrid_mean_lat = avg(hybrid_all_latencies);

            double megapixels = (double(w) * h) / 1e6;
            double hybrid_mp_per_sec = hybrid_median_fps * megapixels;

            double mean_ent = avg(stage_cpu_entropy_ms);
            double mean_h2d = avg(stage_h2d_ms);
            double mean_inv = avg(stage_cuda_inv_ms);
            double mean_d2h = avg(stage_d2h_ms);
            double mean_conv = avg(stage_cpu_convert_ms);

            std::cout << "Results for " << names[r] << ":" << std::endl;
            std::cout << "  CPU Baseline:  " << cpu_median_fps << " FPS (mean latency: " << cpu_mean_lat << " ms)" << std::endl;
            std::cout << "  Hybrid CUDA:   " << hybrid_median_fps << " FPS (mean latency: " << hybrid_mean_lat << " ms, " << hybrid_mp_per_sec << " MP/s)" << std::endl;
            std::cout << "  Stage Breakdown:" << std::endl;
            std::cout << "    CPU Entropy:      " << mean_ent << " ms (" << (mean_ent / hybrid_mean_lat * 100.0) << "%)" << std::endl;
            std::cout << "    Coefficient H2D:  " << mean_h2d << " ms (" << (mean_h2d / hybrid_mean_lat * 100.0) << "%)" << std::endl;
            std::cout << "    CUDA Inverse:     " << mean_inv << " ms (" << (mean_inv / hybrid_mean_lat * 100.0) << "%)" << std::endl;
            std::cout << "    Planar D2H:       " << mean_d2h << " ms (" << (mean_d2h / hybrid_mean_lat * 100.0) << "%)" << std::endl;
            std::cout << "    CPU Final Conv:   " << mean_conv << " ms (" << (mean_conv / hybrid_mean_lat * 100.0) << "%)" << std::endl;

            if (!first_res) json << ",\n";
            first_res = false;

            json << "    {\n"
                 << "      \"name\": \"" << names[r] << "\",\n"
                 << "      \"width\": " << w << ",\n"
                 << "      \"height\": " << h << ",\n"
                 << "      \"frames_per_trial\": " << frames_per_trial << ",\n"
                 << "      \"cpu_baseline\": {\n"
                 << "        \"median_fps\": " << cpu_median_fps << ",\n"
                 << "        \"mean_latency_ms\": " << cpu_mean_lat << ",\n"
                 << "        \"median_latency_ms\": " << Percentile(cpu_all_latencies, 0.5) << ",\n"
                 << "        \"min_latency_ms\": " << *std::min_element(cpu_all_latencies.begin(), cpu_all_latencies.end()) << ",\n"
                 << "        \"max_latency_ms\": " << *std::max_element(cpu_all_latencies.begin(), cpu_all_latencies.end()) << ",\n"
                 << "        \"p95_latency_ms\": " << Percentile(cpu_all_latencies, 0.95) << "\n"
                 << "      },\n"
                 << "      \"hybrid_cuda_inverse\": {\n"
                 << "        \"median_fps\": " << hybrid_median_fps << ",\n"
                 << "        \"megapixels_per_sec\": " << hybrid_mp_per_sec << ",\n"
                 << "        \"mean_latency_ms\": " << hybrid_mean_lat << ",\n"
                 << "        \"median_latency_ms\": " << Percentile(hybrid_all_latencies, 0.5) << ",\n"
                 << "        \"min_latency_ms\": " << *std::min_element(hybrid_all_latencies.begin(), hybrid_all_latencies.end()) << ",\n"
                 << "        \"max_latency_ms\": " << *std::max_element(hybrid_all_latencies.begin(), hybrid_all_latencies.end()) << ",\n"
                 << "        \"p95_latency_ms\": " << Percentile(hybrid_all_latencies, 0.95) << ",\n"
                 << "        \"p99_latency_ms\": " << Percentile(hybrid_all_latencies, 0.99) << "\n"
                 << "      },\n"
                 << "      \"stages_ms\": {\n"
                 << "        \"cpu_entropy\": " << mean_ent << ",\n"
                 << "        \"coefficient_h2d\": " << mean_h2d << ",\n"
                 << "        \"cuda_inverse_wavelet\": " << mean_inv << ",\n"
                 << "        \"planar_d2h\": " << mean_d2h << ",\n"
                 << "        \"cpu_final_conversion\": " << mean_conv << "\n"
                 << "      },\n"
                 << "      \"memory\": {\n"
                 << "        \"peak_host_working_set_bytes\": " << PeakMemory() << ",\n"
                 << "        \"peak_gpu_allocated_bytes\": " << PeakGpuMemory() << "\n"
                 << "      }\n"
                 << "    }";
        }

        json << "\n  ]\n}\n";
        json.flush();
        std::cout << "\nBenchmark results written to: " << output << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Benchmark Error: " << e.what() << std::endl;
        return 1;
    }
}
