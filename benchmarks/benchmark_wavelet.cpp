// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/wavelet.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <thread>
#include <cuda_runtime.h>
#include "bench_host.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

std::string GetCpuModelName() {
#if defined(_MSC_VER)
    int cpuInfo[4] = { -1 };
    char brand[0x40] = { 0 };
    __cpuid(cpuInfo, 0x80000000);
    unsigned int nExIds = cpuInfo[0];
    if (nExIds >= 0x80000004) {
        __cpuid((int*)(brand), 0x80000002);
        __cpuid((int*)(brand + 16), 0x80000003);
        __cpuid((int*)(brand + 32), 0x80000004);
        return std::string(brand);
    }
#endif
    return "x86_64 Multi-Core Processor";
}

struct BenchmarkMetric {
    double latency_avg_ms = 0.0;
    double latency_min_ms = 1e9;
    double latency_max_ms = 0.0;
    double megapixels_per_sec = 0.0;
    double effective_gb_per_sec = 0.0;
};

struct ModeBenchmarkResult {
    BenchmarkMetric kernel_only;
    BenchmarkMetric end_to_end;
};

struct ResolutionResult {
    std::string name;
    int width;
    int height;
    int frames_in_batch;
    int iterations;
    BenchmarkMetric cpu_ref;
    ModeBenchmarkResult cuda_ref;
    ModeBenchmarkResult cuda_opt;
    double pcie_transfer_pct_ref = 0.0;
    double pcie_transfer_pct_opt = 0.0;
    double speedup_kernel_ref = 0.0;
    double speedup_e2e_ref = 0.0;
    double speedup_kernel_opt = 0.0;
    double speedup_e2e_opt = 0.0;
};

} // anonymous namespace

int main(int argc, char** argv) {
    std::cout << "=========================================================================\n";
    std::cout << " HyperCine Phase 1A: 2-6 Wavelet CPU vs CUDA Benchmark Harness          \n";
    std::cout << "=========================================================================\n";

    if (!HyperCine_IsCUDAAvailable()) {
        std::cerr << "[-] Error: CUDA runtime not available!\n";
        return 1;
    }

    HyperCineCUDADeviceInfo dev_info{};
    HyperCine_GetCUDADeviceInfo(&dev_info);

    std::string cpu_model = GetCpuModelName();
    int thread_count = static_cast<int>(std::thread::hardware_concurrency());

    std::cout << "[+] System Information:\n";
    std::cout << "    CPU: " << cpu_model << " (" << thread_count << " hardware threads)\n";
    std::cout << "    GPU: " << dev_info.device_name << " (Compute " << dev_info.major << "." << dev_info.minor
              << ", " << dev_info.multiprocessor_count << " SMs, "
              << (dev_info.total_global_mem / (1024 * 1024)) << " MB VRAM)\n";
    std::cout << "    CUDA Driver: " << (dev_info.cuda_driver_version / 1000) << "."
              << ((dev_info.cuda_driver_version % 100) / 10)
              << " | Runtime: " << (dev_info.cuda_runtime_version / 1000) << "."
              << ((dev_info.cuda_runtime_version % 100) / 10) << "\n";
#if defined(_DEBUG)
    std::string build_type = "Debug";
#else
    std::string build_type = "Release";
#endif
    std::cout << "    Build Type: " << build_type << "\n\n";

    struct BenchConfig {
        std::string name;
        int width;
        int height;
        int batch_frames;
        int iters;
    };

    // Default keeps every recorded configuration. --1080p-only is a smoke
    // filter for the artifact contract; it does not change that configuration.
    bool only_1080p = false;
    std::string json_path = "benchmarks/results/cuda_wavelet_phase1.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--1080p-only") {
            only_1080p = true;
        } else if (!arg.empty() && arg[0] != '-') {
            json_path = arg;
        }
    }

    std::vector<BenchConfig> configs;
    configs.push_back({ "1080p", 1920, 1080, 1, 50 });
    if (!only_1080p) {
        configs.push_back({ "4K_UHD", 3840, 2160, 1, 25 });
        configs.push_back({ "8K_UHD", 7680, 4320, 1, 10 });
        configs.push_back({ "1080p_Batch16", 1920, 1080, 16, 20 });
    }

    std::vector<ResolutionResult> all_results;

    for (const auto& cfg : configs) {
        std::cout << ">>> Running Benchmark: " << cfg.name << " (" << cfg.width << "x" << cfg.height
                  << ", Batch: " << cfg.batch_frames << ", Iterations: " << cfg.iters << ")...\n";

        ResolutionResult res{};
        res.name = cfg.name;
        res.width = cfg.width;
        res.height = cfg.height;
        res.frames_in_batch = cfg.batch_frames;
        res.iterations = cfg.iters;

        const int W = cfg.width;
        const int H = cfg.height;
        const int batch = cfg.batch_frames;
        const int half_w = W / 2;
        const int half_h = H / 2;
        const size_t frame_pixels = static_cast<size_t>(W) * H;
        const size_t total_pixels = frame_pixels * batch;
        const size_t subband_pixels = static_cast<size_t>(half_w) * half_h;

        // Allocate Host memory
        std::vector<int16_t> h_src(total_pixels, 1000);
        std::vector<int16_t> h_ll(subband_pixels * batch);
        std::vector<int16_t> h_lh(subband_pixels * batch);
        std::vector<int16_t> h_hl(subband_pixels * batch);
        std::vector<int16_t> h_hh(subband_pixels * batch);

        // Fill with non-trivial gradient
        for (size_t i = 0; i < total_pixels; ++i) {
            h_src[i] = static_cast<int16_t>((i % 3000) - 1500);
        }

        // Allocate Device memory
        int16_t *d_src = nullptr, *d_ll = nullptr, *d_lh = nullptr, *d_hl = nullptr, *d_hh = nullptr;
        cudaMalloc(&d_src, total_pixels * sizeof(int16_t));
        cudaMalloc(&d_ll, subband_pixels * batch * sizeof(int16_t));
        cudaMalloc(&d_lh, subband_pixels * batch * sizeof(int16_t));
        cudaMalloc(&d_hl, subband_pixels * batch * sizeof(int16_t));
        cudaMalloc(&d_hh, subband_pixels * batch * sizeof(int16_t));

        cudaStream_t stream = nullptr;
        cudaStreamCreate(&stream);

        // ---------------------------------------------------------------------
        // 1. CPU Reference Benchmark
        // ---------------------------------------------------------------------
        std::cout << "  [*] Benchmarking CPU Reference... " << std::flush;
        // Warmup
        for (int f = 0; f < batch; ++f) {
            HyperCine_ForwardSpatial2D_CPU(
                h_src.data() + f * frame_pixels, W, H, W,
                h_ll.data() + f * subband_pixels, half_w,
                h_lh.data() + f * subband_pixels, half_w,
                h_hl.data() + f * subband_pixels, half_w,
                h_hh.data() + f * subband_pixels, half_w
            );
        }

        std::vector<double> cpu_times;
        int cpu_iters = (cfg.width >= 7680) ? 5 : (cfg.iters / 2);
        if (cpu_iters < 3) cpu_iters = 3;

        for (int iter = 0; iter < cpu_iters; ++iter) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < batch; ++f) {
                HyperCine_ForwardSpatial2D_CPU(
                    h_src.data() + f * frame_pixels, W, H, W,
                    h_ll.data() + f * subband_pixels, half_w,
                    h_lh.data() + f * subband_pixels, half_w,
                    h_hl.data() + f * subband_pixels, half_w,
                    h_hh.data() + f * subband_pixels, half_w
                );
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            cpu_times.push_back(ms);
            if (ms < res.cpu_ref.latency_min_ms) res.cpu_ref.latency_min_ms = ms;
            if (ms > res.cpu_ref.latency_max_ms) res.cpu_ref.latency_max_ms = ms;
        }

        double total_cpu_ms = 0.0;
        for (double t : cpu_times) total_cpu_ms += t;
        res.cpu_ref.latency_avg_ms = total_cpu_ms / cpu_times.size();
        res.cpu_ref.megapixels_per_sec = (total_pixels / 1e6) / (res.cpu_ref.latency_avg_ms / 1000.0);
        // Effective traffic: read input (2 bytes) + write 4 subbands (2 bytes) = 4 bytes per pixel
        res.cpu_ref.effective_gb_per_sec = (total_pixels * 4.0 / 1e9) / (res.cpu_ref.latency_avg_ms / 1000.0);
        std::cout << std::fixed << std::setprecision(2)
                  << res.cpu_ref.latency_avg_ms << " ms (" << res.cpu_ref.megapixels_per_sec << " MP/s)\n";

        // ---------------------------------------------------------------------
        // 2. CUDA Reference & Optimized Benchmarks
        // ---------------------------------------------------------------------
        for (int m = 0; m < 2; ++m) {
            HyperCineTransformBackend bkind = (m == 0) ? HYPERCINE_BACKEND_CUDA_REFERENCE : HYPERCINE_BACKEND_CUDA_OPTIMIZED;
            const char* mlabel = (m == 0) ? "CUDA Reference" : "CUDA Optimized";
            ModeBenchmarkResult& mres = (m == 0) ? res.cuda_ref : res.cuda_opt;

            std::cout << "  [*] Benchmarking " << mlabel << " (Kernel only)... " << std::flush;

            // Pre-load data to GPU
            cudaMemcpy(d_src, h_src.data(), total_pixels * sizeof(int16_t), cudaMemcpyHostToDevice);

            // Warmup kernel
            for (int f = 0; f < batch; ++f) {
                HyperCine_ForwardSpatial2D_CUDA(
                    bkind, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    stream
                );
            }
            cudaStreamSynchronize(stream);

            // Time Kernel-Only
            cudaEvent_t ev_start, ev_stop;
            cudaEventCreate(&ev_start);
            cudaEventCreate(&ev_stop);

            std::vector<double> kern_times;
            for (int iter = 0; iter < cfg.iters; ++iter) {
                cudaEventRecord(ev_start, stream);
                for (int f = 0; f < batch; ++f) {
                    HyperCine_ForwardSpatial2D_CUDA(
                        bkind, d_src + f * frame_pixels, W, H, W,
                        d_ll + f * subband_pixels, half_w,
                        d_lh + f * subband_pixels, half_w,
                        d_hl + f * subband_pixels, half_w,
                        d_hh + f * subband_pixels, half_w,
                        stream
                    );
                }
                cudaEventRecord(ev_stop, stream);
                cudaEventSynchronize(ev_stop);
                float ms = 0.0f;
                cudaEventElapsedTime(&ms, ev_start, ev_stop);
                kern_times.push_back(ms);
                if (ms < mres.kernel_only.latency_min_ms) mres.kernel_only.latency_min_ms = ms;
                if (ms > mres.kernel_only.latency_max_ms) mres.kernel_only.latency_max_ms = ms;
            }
            double total_kern_ms = 0.0;
            for (double t : kern_times) total_kern_ms += t;
            mres.kernel_only.latency_avg_ms = total_kern_ms / kern_times.size();
            mres.kernel_only.megapixels_per_sec = (total_pixels / 1e6) / (mres.kernel_only.latency_avg_ms / 1000.0);
            mres.kernel_only.effective_gb_per_sec = (total_pixels * 4.0 / 1e9) / (mres.kernel_only.latency_avg_ms / 1000.0);

            std::cout << std::fixed << std::setprecision(3)
                      << mres.kernel_only.latency_avg_ms << " ms ("
                      << std::setprecision(1) << mres.kernel_only.megapixels_per_sec << " MP/s, "
                      << mres.kernel_only.effective_gb_per_sec << " GB/s)\n";

            // Time End-to-End (H2D copy + Kernel execution + D2H copy)
            std::cout << "  [*] Benchmarking " << mlabel << " (PCIe End-to-End)... " << std::flush;
            std::vector<double> e2e_times;

            for (int iter = 0; iter < cfg.iters; ++iter) {
                auto t0 = std::chrono::high_resolution_clock::now();

                // H2D
                cudaMemcpyAsync(d_src, h_src.data(), total_pixels * sizeof(int16_t), cudaMemcpyHostToDevice, stream);

                // Kernels
                for (int f = 0; f < batch; ++f) {
                    HyperCine_ForwardSpatial2D_CUDA(
                        bkind, d_src + f * frame_pixels, W, H, W,
                        d_ll + f * subband_pixels, half_w,
                        d_lh + f * subband_pixels, half_w,
                        d_hl + f * subband_pixels, half_w,
                        d_hh + f * subband_pixels, half_w,
                        stream
                    );
                }

                // D2H
                cudaMemcpyAsync(h_ll.data(), d_ll, subband_pixels * batch * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
                cudaMemcpyAsync(h_lh.data(), d_lh, subband_pixels * batch * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
                cudaMemcpyAsync(h_hl.data(), d_hl, subband_pixels * batch * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
                cudaMemcpyAsync(h_hh.data(), d_hh, subband_pixels * batch * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);

                cudaStreamSynchronize(stream);
                auto t1 = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                e2e_times.push_back(ms);
                if (ms < mres.end_to_end.latency_min_ms) mres.end_to_end.latency_min_ms = ms;
                if (ms > mres.end_to_end.latency_max_ms) mres.end_to_end.latency_max_ms = ms;
            }

            double total_e2e_ms = 0.0;
            for (double t : e2e_times) total_e2e_ms += t;
            mres.end_to_end.latency_avg_ms = total_e2e_ms / e2e_times.size();
            mres.end_to_end.megapixels_per_sec = (total_pixels / 1e6) / (mres.end_to_end.latency_avg_ms / 1000.0);
            mres.end_to_end.effective_gb_per_sec = (total_pixels * 4.0 / 1e9) / (mres.end_to_end.latency_avg_ms / 1000.0);

            std::cout << std::fixed << std::setprecision(3)
                      << mres.end_to_end.latency_avg_ms << " ms ("
                      << std::setprecision(1) << mres.end_to_end.megapixels_per_sec << " MP/s)\n";

            cudaEventDestroy(ev_start);
            cudaEventDestroy(ev_stop);
        }

        // Calculations & comparisons
        res.speedup_kernel_ref = res.cpu_ref.latency_avg_ms / res.cuda_ref.kernel_only.latency_avg_ms;
        res.speedup_e2e_ref = res.cpu_ref.latency_avg_ms / res.cuda_ref.end_to_end.latency_avg_ms;
        res.pcie_transfer_pct_ref = (1.0 - (res.cuda_ref.kernel_only.latency_avg_ms / res.cuda_ref.end_to_end.latency_avg_ms)) * 100.0;

        res.speedup_kernel_opt = res.cpu_ref.latency_avg_ms / res.cuda_opt.kernel_only.latency_avg_ms;
        res.speedup_e2e_opt = res.cpu_ref.latency_avg_ms / res.cuda_opt.end_to_end.latency_avg_ms;
        res.pcie_transfer_pct_opt = (1.0 - (res.cuda_opt.kernel_only.latency_avg_ms / res.cuda_opt.end_to_end.latency_avg_ms)) * 100.0;

        std::cout << "  ==> Summary for " << cfg.name << ":\n";
        std::cout << "      Speedup (Reference Kernel): " << std::fixed << std::setprecision(1) << res.speedup_kernel_ref << "x (PCIe E2E: " << res.speedup_e2e_ref << "x)\n";
        std::cout << "      Speedup (Optimized Kernel): " << std::fixed << std::setprecision(1) << res.speedup_kernel_opt << "x (PCIe E2E: " << res.speedup_e2e_opt << "x)\n";
        std::cout << "      PCIe Transfer Overhead: ~" << std::setprecision(1) << res.pcie_transfer_pct_opt << "%\n\n";

        all_results.push_back(res);

        cudaStreamDestroy(stream);
        cudaFree(d_src);
        cudaFree(d_ll);
        cudaFree(d_lh);
        cudaFree(d_hl);
        cudaFree(d_hh);
    }

    // -------------------------------------------------------------------------
    // Generate JSON Output
    // -------------------------------------------------------------------------
    if (!hypercine::bench::CreateParentDirectory(json_path)) {
        return 1;
    }
    std::ofstream out(json_path);
    if (!out.is_open()) {
        std::cerr << "[-] Error opening " << json_path << " for writing!\n";
        return 1;
    }

    out << "{\n";
    out << "  \"system\": {\n";
    out << "    \"os\": \"Windows x64\",\n";
    out << "    \"cpu_model\": \"" << cpu_model << "\",\n";
    out << "    \"thread_count\": " << thread_count << ",\n";
    out << "    \"gpu_model\": \"" << dev_info.device_name << "\",\n";
    out << "    \"compute_capability\": \"" << dev_info.major << "." << dev_info.minor << "\",\n";
    out << "    \"cuda_toolkit_version\": \"" << (dev_info.cuda_runtime_version / 1000) << "." << ((dev_info.cuda_runtime_version % 100) / 10) << "\",\n";
    out << "    \"cuda_driver_version\": \"" << (dev_info.cuda_driver_version / 1000) << "." << ((dev_info.cuda_driver_version % 100) / 10) << "\",\n";
    hypercine::bench::WriteCompilerField(out);
    out << "    \"build_type\": \"" << hypercine::bench::BuildTypeName(build_type.c_str()) << "\"\n";
    out << "  },\n";
    out << "  \"benchmarks\": [\n";

    for (size_t i = 0; i < all_results.size(); ++i) {
        const auto& r = all_results[i];
        out << "    {\n";
        out << "      \"name\": \"" << r.name << "\",\n";
        out << "      \"width\": " << r.width << ",\n";
        out << "      \"height\": " << r.height << ",\n";
        out << "      \"batch_frames\": " << r.frames_in_batch << ",\n";
        out << "      \"iterations\": " << r.iterations << ",\n";
        out << "      \"cpu_reference\": {\n";
        out << "        \"latency_avg_ms\": " << r.cpu_ref.latency_avg_ms << ",\n";
        out << "        \"latency_min_ms\": " << r.cpu_ref.latency_min_ms << ",\n";
        out << "        \"latency_max_ms\": " << r.cpu_ref.latency_max_ms << ",\n";
        out << "        \"megapixels_per_sec\": " << r.cpu_ref.megapixels_per_sec << ",\n";
        out << "        \"effective_gb_per_sec\": " << r.cpu_ref.effective_gb_per_sec << "\n";
        out << "      },\n";
        out << "      \"cuda_reference\": {\n";
        out << "        \"kernel_only\": {\n";
        out << "          \"latency_avg_ms\": " << r.cuda_ref.kernel_only.latency_avg_ms << ",\n";
        out << "          \"latency_min_ms\": " << r.cuda_ref.kernel_only.latency_min_ms << ",\n";
        out << "          \"latency_max_ms\": " << r.cuda_ref.kernel_only.latency_max_ms << ",\n";
        out << "          \"megapixels_per_sec\": " << r.cuda_ref.kernel_only.megapixels_per_sec << ",\n";
        out << "          \"effective_gb_per_sec\": " << r.cuda_ref.kernel_only.effective_gb_per_sec << "\n";
        out << "        },\n";
        out << "        \"end_to_end\": {\n";
        out << "          \"latency_avg_ms\": " << r.cuda_ref.end_to_end.latency_avg_ms << ",\n";
        out << "          \"latency_min_ms\": " << r.cuda_ref.end_to_end.latency_min_ms << ",\n";
        out << "          \"latency_max_ms\": " << r.cuda_ref.end_to_end.latency_max_ms << ",\n";
        out << "          \"megapixels_per_sec\": " << r.cuda_ref.end_to_end.megapixels_per_sec << "\n";
        out << "        },\n";
        out << "        \"speedup_kernel\": " << r.speedup_kernel_ref << ",\n";
        out << "        \"speedup_end_to_end\": " << r.speedup_e2e_ref << ",\n";
        out << "        \"pcie_transfer_percent\": " << r.pcie_transfer_pct_ref << "\n";
        out << "      },\n";
        out << "      \"cuda_optimized\": {\n";
        out << "        \"kernel_only\": {\n";
        out << "          \"latency_avg_ms\": " << r.cuda_opt.kernel_only.latency_avg_ms << ",\n";
        out << "          \"latency_min_ms\": " << r.cuda_opt.kernel_only.latency_min_ms << ",\n";
        out << "          \"latency_max_ms\": " << r.cuda_opt.kernel_only.latency_max_ms << ",\n";
        out << "          \"megapixels_per_sec\": " << r.cuda_opt.kernel_only.megapixels_per_sec << ",\n";
        out << "          \"effective_gb_per_sec\": " << r.cuda_opt.kernel_only.effective_gb_per_sec << "\n";
        out << "        },\n";
        out << "        \"end_to_end\": {\n";
        out << "          \"latency_avg_ms\": " << r.cuda_opt.end_to_end.latency_avg_ms << ",\n";
        out << "          \"latency_min_ms\": " << r.cuda_opt.end_to_end.latency_min_ms << ",\n";
        out << "          \"latency_max_ms\": " << r.cuda_opt.end_to_end.latency_max_ms << ",\n";
        out << "          \"megapixels_per_sec\": " << r.cuda_opt.end_to_end.megapixels_per_sec << "\n";
        out << "        },\n";
        out << "        \"speedup_kernel\": " << r.speedup_kernel_opt << ",\n";
        out << "        \"speedup_end_to_end\": " << r.speedup_e2e_opt << ",\n";
        out << "        \"pcie_transfer_percent\": " << r.pcie_transfer_pct_opt << "\n";
        out << "      }\n";
        out << "    }" << ((i + 1 < all_results.size()) ? ",\n" : "\n");
    }

    out << "  ]\n";
    out << "}\n";
    out.close();
    if (!out) {
        std::cerr << "[-] Error finishing " << json_path << "\n";
        return 1;
    }

    std::cout << "[+] Saved benchmark results to: " << json_path << "\n";
    std::cout << "=========================================================================\n";
    return 0;
}
