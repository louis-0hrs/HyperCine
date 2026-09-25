// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/wavelet.h"
#include "CFHDEncoder.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cuda_runtime.h>
#include <thread>
#include <cmath>

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

struct Metric {
    double latency_avg_ms = 0.0;
    double latency_min_ms = 1e9;
    double megapixels_per_sec = 0.0;
    double bandwidth_gb_sec = 0.0;
};

} // namespace

int main() {
    std::cout << "=========================================================================\n";
    std::cout << " HyperCine Phase 1B: GPU-Resident Pyramid & Quantization Benchmark\n";
    std::cout << "=========================================================================\n";

    if (!HyperCine_IsCUDAAvailable()) {
        std::cerr << "[-] Error: CUDA runtime not available!\n";
        return 1;
    }

    HyperCineCUDADeviceInfo dev_info{};
    HyperCine_GetCUDADeviceInfo(&dev_info);

    std::cout << "[+] System Information:\n";
    std::cout << "    CPU: " << GetCpuModelName() << " (" << std::thread::hardware_concurrency() << " threads)\n";
    std::cout << "    GPU: " << dev_info.device_name << "\n\n";

    const int W = 3840;
    const int H = 2160;
    const int frames = 10;
    const int iters = 5;
    const size_t frame_pixels = static_cast<size_t>(W) * H;
    const size_t total_pixels = frame_pixels * frames;

    std::cout << "[*] Resolution: " << W << "x" << H << " (" << frames << " frames/batch)\n\n";

    std::vector<int16_t> h_src(total_pixels);
    for (size_t i = 0; i < total_pixels; ++i) {
        h_src[i] = static_cast<int16_t>((i % 2000) - 1000);
    }

    // Allocate memory for subbands (Level 1)
    const int half_w = W / 2;
    const int half_h = H / 2;
    const size_t subband_pixels = static_cast<size_t>(half_w) * half_h;
    std::vector<int16_t> h_ll(subband_pixels * frames);
    std::vector<int16_t> h_lh(subband_pixels * frames);
    std::vector<int16_t> h_hl(subband_pixels * frames);
    std::vector<int16_t> h_hh(subband_pixels * frames);

    // 1. HYPERCINE_CPU_REFERENCE
    std::cout << "--- CPU Measurements ---\n";
    {
        std::cout << "  1. HYPERCINE_CPU_REFERENCE (portable C++)... " << std::flush;
        std::vector<double> times;
        for (int i = 0; i < iters; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < frames; ++f) {
                HyperCine_ForwardSpatial2D_CPU(
                    h_src.data() + f * frame_pixels, W, H, W,
                    h_ll.data() + f * subband_pixels, half_w,
                    h_lh.data() + f * subband_pixels, half_w,
                    h_hl.data() + f * subband_pixels, half_w,
                    h_hh.data() + f * subband_pixels, half_w
                );
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        double avg = 0; for(auto t : times) avg += t; avg /= times.size();
        std::cout << std::fixed << std::setprecision(2) << avg << " ms (" << (total_pixels / 1e6) / (avg / 1000.0) << " MP/s)\n";
    }

    // 2. UPSTREAM_CPU_PRODUCTION
    {
        std::cout << "  2. UPSTREAM_CPU_PRODUCTION (production SIMD)... " << std::flush;
        std::vector<double> times;
        for (int i = 0; i < iters; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < frames; ++f) {
                HyperCine_ForwardSpatial2D_Upstream(
                    h_src.data() + f * frame_pixels, W, H, W,
                    h_ll.data() + f * subband_pixels, half_w,
                    h_lh.data() + f * subband_pixels, half_w,
                    h_hl.data() + f * subband_pixels, half_w,
                    h_hh.data() + f * subband_pixels, half_w,
                    1, 2, 2, 2
                );
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        double avg = 0; for(auto t : times) avg += t; avg /= times.size();
        std::cout << std::fixed << std::setprecision(2) << avg << " ms (" << (total_pixels / 1e6) / (avg / 1000.0) << " MP/s)\n";
    }

    // 3. FULL_UPSTREAM_ENCODER
    {
        std::cout << "  3. FULL_UPSTREAM_ENCODER (full CineForm SDK encode)... " << std::flush;
        std::vector<uint8_t> frame_bgr10(W * H * 4, 128); // dummy BGRA
        std::vector<double> times;
        
        CFHD_EncoderRef encoder = nullptr;
        CFHD_OpenEncoder(&encoder, nullptr);
        CFHD_PrepareToEncode(encoder, W, H, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422, 0, CFHD_ENCODING_QUALITY_FILMSCAN3);

        for (int i = 0; i < iters; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < frames; ++f) {
                CFHD_EncodeSample(encoder, frame_bgr10.data(), W * 4);
                void* sData = nullptr;
                size_t sSize = 0;
                CFHD_GetSampleData(encoder, &sData, &sSize);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        CFHD_CloseEncoder(encoder);
        double avg = 0; for(auto t : times) avg += t; avg /= times.size();
        std::cout << std::fixed << std::setprecision(2) << avg << " ms (" << (total_pixels / 1e6) / (avg / 1000.0) << " MP/s)\n";
    }

    std::cout << "\n--- GPU Measurements (Phase 1B: GPU-resident pyramid + quant) ---\n";
    
    // Allocate GPU memory
    int16_t *d_src = nullptr, *d_ll = nullptr, *d_lh = nullptr, *d_hl = nullptr, *d_hh = nullptr;
    cudaMalloc(&d_src, total_pixels * sizeof(int16_t));
    cudaMalloc(&d_ll, subband_pixels * frames * sizeof(int16_t));
    cudaMalloc(&d_lh, subband_pixels * frames * sizeof(int16_t));
    cudaMalloc(&d_hl, subband_pixels * frames * sizeof(int16_t));
    cudaMalloc(&d_hh, subband_pixels * frames * sizeof(int16_t));

    cudaStream_t stream = nullptr;
    cudaStreamCreate(&stream);

    // Warmup
    for (int f = 0; f < frames; ++f) {
        HyperCine_ForwardSpatialQuant2D_CUDA(
            HYPERCINE_BACKEND_CUDA_REFERENCE, d_src + f * frame_pixels, W, H, W,
            d_ll + f * subband_pixels, half_w,
            d_lh + f * subband_pixels, half_w,
            d_hl + f * subband_pixels, half_w,
            d_hh + f * subband_pixels, half_w,
            1, 2, 2, 2, 0, stream
        );
    }
    cudaStreamSynchronize(stream);

    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    // A. Kernel Only
    std::cout << "  A. CUDA Reference (Kernel only)... " << std::flush;
    {
        std::vector<double> times;
        for (int i = 0; i < iters; ++i) {
            cudaEventRecord(start, stream);
            for (int f = 0; f < frames; ++f) {
                HyperCine_ForwardSpatialQuant2D_CUDA(
                    HYPERCINE_BACKEND_CUDA_REFERENCE, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    1, 2, 2, 2, 0, stream
                );
            }
            cudaEventRecord(stop, stream);
            cudaEventSynchronize(stop);
            float ms = 0;
            cudaEventElapsedTime(&ms, start, stop);
            times.push_back(ms);
        }
        double avg = 0; for(auto t : times) avg += t; avg /= times.size();
        std::cout << std::fixed << std::setprecision(2) << avg << " ms (" << (total_pixels / 1e6) / (avg / 1000.0) << " MP/s)\n";
    }

    // B. Host -> GPU -> Host
    std::cout << "  B. CUDA Reference (Host->GPU->Host)... " << std::flush;
    {
        std::vector<double> times;
        for (int i = 0; i < iters; ++i) {
            cudaEventRecord(start, stream);
            cudaMemcpyAsync(d_src, h_src.data(), total_pixels * sizeof(int16_t), cudaMemcpyHostToDevice, stream);
            for (int f = 0; f < frames; ++f) {
                HyperCine_ForwardSpatialQuant2D_CUDA(
                    HYPERCINE_BACKEND_CUDA_REFERENCE, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    1, 2, 2, 2, 0, stream
                );
            }
            cudaMemcpyAsync(h_ll.data(), d_ll, subband_pixels * frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_lh.data(), d_lh, subband_pixels * frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hl.data(), d_hl, subband_pixels * frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hh.data(), d_hh, subband_pixels * frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaEventRecord(stop, stream);
            cudaEventSynchronize(stop);
            float ms = 0;
            cudaEventElapsedTime(&ms, start, stop);
            times.push_back(ms);
        }
        double avg = 0; for(auto t : times) avg += t; avg /= times.size();
        std::cout << std::fixed << std::setprecision(2) << avg << " ms (" << (total_pixels / 1e6) / (avg / 1000.0) << " MP/s)\n";
    }

    cudaFree(d_src);
    cudaFree(d_ll);
    cudaFree(d_lh);
    cudaFree(d_hl);
    cudaFree(d_hh);
    cudaStreamDestroy(stream);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);

    return 0;
}
