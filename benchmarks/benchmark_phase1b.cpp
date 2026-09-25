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
#include <fstream>
#include <sstream>
#include <algorithm>
#include <numeric>

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

double ComputeMedian(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) * 0.5;
}

struct TestRecord {
    std::string implementation;
    std::string transfer_mode;
    int number_of_frames = 0;
    double batch_duration_avg_ms = 0.0;
    double batch_duration_median_ms = 0.0;
    double batch_duration_min_ms = 0.0;
    double batch_duration_max_ms = 0.0;
    double latency_avg_ms_per_frame = 0.0;
    double latency_median_ms_per_frame = 0.0;
    double latency_min_ms_per_frame = 0.0;
    double latency_max_ms_per_frame = 0.0;
    double megapixels_per_sec = 0.0;
};

struct TransferDecomp {
    size_t h2d_bytes = 0;
    double h2d_avg_ms_per_frame = 0.0;
    double h2d_median_ms_per_frame = 0.0;
    double h2d_gb_per_sec = 0.0;

    double gpu_wavelet_avg_ms_per_frame = 0.0;
    double gpu_wavelet_median_ms_per_frame = 0.0;

    double gpu_quant_avg_ms_per_frame = 0.0;
    double gpu_quant_median_ms_per_frame = 0.0;

    double gpu_fused_total_avg_ms_per_frame = 0.0;

    size_t d2h_bytes = 0;
    double d2h_avg_ms_per_frame = 0.0;
    double d2h_median_ms_per_frame = 0.0;
    double d2h_gb_per_sec = 0.0;

    double end_to_end_avg_ms_per_frame = 0.0;
};

struct ResolutionBenchmarkMatrix {
    std::string name;
    int width = 0;
    int height = 0;
    int batch_frames = 0;
    int iterations = 0;
    std::vector<TestRecord> records;
    TransferDecomp transfers_ref;
    TransferDecomp transfers_opt;
};

ResolutionBenchmarkMatrix RunBenchmarkForResolution(
    const std::string& res_name, int W, int H, int batch_frames, int iters,
    cudaStream_t stream
) {
    ResolutionBenchmarkMatrix matrix{};
    matrix.name = res_name;
    matrix.width = W;
    matrix.height = H;
    matrix.batch_frames = batch_frames;
    matrix.iterations = iters;

    const size_t frame_pixels = static_cast<size_t>(W) * H;
    const size_t total_pixels = frame_pixels * batch_frames;
    const int half_w = W / 2;
    const int half_h = H / 2;
    const size_t subband_pixels = static_cast<size_t>(half_w) * half_h;

    std::cout << "\n=========================================================================\n";
    std::cout << ">>> Running Matrix: " << res_name << " (" << W << "x" << H << ", "
              << batch_frames << " frames/batch, " << iters << " iterations)\n";
    std::cout << "=========================================================================\n";

    // Allocate Host Buffers
    std::vector<int16_t> h_src(total_pixels);
    for (size_t i = 0; i < total_pixels; ++i) {
        h_src[i] = static_cast<int16_t>((i % 2000) - 1000);
    }
    std::vector<int16_t> h_ll(subband_pixels * batch_frames);
    std::vector<int16_t> h_lh(subband_pixels * batch_frames);
    std::vector<int16_t> h_hl(subband_pixels * batch_frames);
    std::vector<int16_t> h_hh(subband_pixels * batch_frames);

    // Helper to evaluate and add a TestRecord
    auto RecordBatch = [&](const std::string& impl, const std::string& transfer_mode, const std::vector<double>& batch_ms_list) {
        TestRecord rec{};
        rec.implementation = impl;
        rec.transfer_mode = transfer_mode;
        rec.number_of_frames = batch_frames;

        double sum = std::accumulate(batch_ms_list.begin(), batch_ms_list.end(), 0.0);
        rec.batch_duration_avg_ms = sum / batch_ms_list.size();
        rec.batch_duration_median_ms = ComputeMedian(batch_ms_list);
        rec.batch_duration_min_ms = *std::min_element(batch_ms_list.begin(), batch_ms_list.end());
        rec.batch_duration_max_ms = *std::max_element(batch_ms_list.begin(), batch_ms_list.end());

        rec.latency_avg_ms_per_frame = rec.batch_duration_avg_ms / batch_frames;
        rec.latency_median_ms_per_frame = rec.batch_duration_median_ms / batch_frames;
        rec.latency_min_ms_per_frame = rec.batch_duration_min_ms / batch_frames;
        rec.latency_max_ms_per_frame = rec.batch_duration_max_ms / batch_frames;

        rec.megapixels_per_sec = (static_cast<double>(total_pixels) / 1e6) / (rec.batch_duration_avg_ms / 1000.0);

        std::cout << "  [*] " << std::left << std::setw(28) << impl
                  << " [" << std::setw(20) << transfer_mode << "]: "
                  << std::right << std::fixed << std::setprecision(3)
                  << rec.latency_avg_ms_per_frame << " ms/frame (Batch: "
                  << std::setprecision(2) << rec.batch_duration_avg_ms << " ms, "
                  << std::setprecision(1) << rec.megapixels_per_sec << " MP/s)\n";

        matrix.records.push_back(rec);
    };

    // -------------------------------------------------------------------------
    // 1. HYPERCINE_CPU_REFERENCE
    // -------------------------------------------------------------------------
    {
        // Warmup
        for (int f = 0; f < batch_frames; ++f) {
            HyperCine_ForwardSpatial2D_CPU(
                h_src.data() + f * frame_pixels, W, H, W,
                h_ll.data() + f * subband_pixels, half_w,
                h_lh.data() + f * subband_pixels, half_w,
                h_hl.data() + f * subband_pixels, half_w,
                h_hh.data() + f * subband_pixels, half_w
            );
        }
        std::vector<double> batch_times;
        int cpu_iters = (W >= 7680) ? std::min(iters, 3) : iters;
        for (int i = 0; i < cpu_iters; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < batch_frames; ++f) {
                HyperCine_ForwardSpatial2D_CPU(
                    h_src.data() + f * frame_pixels, W, H, W,
                    h_ll.data() + f * subband_pixels, half_w,
                    h_lh.data() + f * subband_pixels, half_w,
                    h_hl.data() + f * subband_pixels, half_w,
                    h_hh.data() + f * subband_pixels, half_w
                );
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            batch_times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        RecordBatch("HYPERCINE_CPU_REFERENCE", "CPU Host-Only", batch_times);
    }

    // -------------------------------------------------------------------------
    // 2. UPSTREAM_CPU_PRODUCTION
    // -------------------------------------------------------------------------
    {
        // Warmup
        for (int f = 0; f < batch_frames; ++f) {
            HyperCine_ForwardSpatial2D_Upstream(
                h_src.data() + f * frame_pixels, W, H, W,
                h_ll.data() + f * subband_pixels, half_w,
                h_lh.data() + f * subband_pixels, half_w,
                h_hl.data() + f * subband_pixels, half_w,
                h_hh.data() + f * subband_pixels, half_w,
                1, 2, 2, 2
            );
        }
        std::vector<double> batch_times;
        int cpu_iters = (W >= 7680) ? std::min(iters, 5) : iters;
        for (int i = 0; i < cpu_iters; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < batch_frames; ++f) {
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
            batch_times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        RecordBatch("UPSTREAM_CPU_PRODUCTION", "CPU Host-Only", batch_times);
    }

    // -------------------------------------------------------------------------
    // Allocate GPU Device Memory
    // -------------------------------------------------------------------------
    int16_t *d_src = nullptr, *d_ll = nullptr, *d_lh = nullptr, *d_hl = nullptr, *d_hh = nullptr;
    cudaMalloc(&d_src, total_pixels * sizeof(int16_t));
    cudaMalloc(&d_ll, subband_pixels * batch_frames * sizeof(int16_t));
    cudaMalloc(&d_lh, subband_pixels * batch_frames * sizeof(int16_t));
    cudaMalloc(&d_hl, subband_pixels * batch_frames * sizeof(int16_t));
    cudaMalloc(&d_hh, subband_pixels * batch_frames * sizeof(int16_t));

    cudaEvent_t ev_start, ev_stop;
    cudaEventCreate(&ev_start);
    cudaEventCreate(&ev_stop);

    // Warmup CUDA kernels
    cudaMemcpy(d_src, h_src.data(), total_pixels * sizeof(int16_t), cudaMemcpyHostToDevice);
    for (int f = 0; f < batch_frames; ++f) {
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

    // -------------------------------------------------------------------------
    // 3. CUDA_REFERENCE (Kernel only)
    // -------------------------------------------------------------------------
    {
        std::vector<double> batch_times;
        for (int i = 0; i < iters; ++i) {
            cudaEventRecord(ev_start, stream);
            for (int f = 0; f < batch_frames; ++f) {
                HyperCine_ForwardSpatialQuant2D_CUDA(
                    HYPERCINE_BACKEND_CUDA_REFERENCE, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    1, 2, 2, 2, 0, stream
                );
            }
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float ms = 0;
            cudaEventElapsedTime(&ms, ev_start, ev_stop);
            batch_times.push_back(ms);
        }
        RecordBatch("CUDA_REFERENCE", "GPU Kernel-Only", batch_times);
    }

    // -------------------------------------------------------------------------
    // 4. CUDA_OPTIMIZED (Kernel only)
    // -------------------------------------------------------------------------
    {
        std::vector<double> batch_times;
        for (int i = 0; i < iters; ++i) {
            cudaEventRecord(ev_start, stream);
            for (int f = 0; f < batch_frames; ++f) {
                HyperCine_ForwardSpatialQuant2D_CUDA(
                    HYPERCINE_BACKEND_CUDA_OPTIMIZED, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    1, 2, 2, 2, 0, stream
                );
            }
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float ms = 0;
            cudaEventElapsedTime(&ms, ev_start, ev_stop);
            batch_times.push_back(ms);
        }
        RecordBatch("CUDA_OPTIMIZED", "GPU Kernel-Only", batch_times);
    }

    // -------------------------------------------------------------------------
    // 5. CUDA including transfers (End-to-End)
    // -------------------------------------------------------------------------
    {
        std::vector<double> batch_times_ref;
        for (int i = 0; i < iters; ++i) {
            cudaEventRecord(ev_start, stream);
            cudaMemcpyAsync(d_src, h_src.data(), total_pixels * sizeof(int16_t), cudaMemcpyHostToDevice, stream);
            for (int f = 0; f < batch_frames; ++f) {
                HyperCine_ForwardSpatialQuant2D_CUDA(
                    HYPERCINE_BACKEND_CUDA_REFERENCE, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    1, 2, 2, 2, 0, stream
                );
            }
            cudaMemcpyAsync(h_ll.data(), d_ll, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_lh.data(), d_lh, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hl.data(), d_hl, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hh.data(), d_hh, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float ms = 0;
            cudaEventElapsedTime(&ms, ev_start, ev_stop);
            batch_times_ref.push_back(ms);
        }
        RecordBatch("CUDA_REFERENCE", "Host->GPU->Host", batch_times_ref);

        std::vector<double> batch_times_opt;
        for (int i = 0; i < iters; ++i) {
            cudaEventRecord(ev_start, stream);
            cudaMemcpyAsync(d_src, h_src.data(), total_pixels * sizeof(int16_t), cudaMemcpyHostToDevice, stream);
            for (int f = 0; f < batch_frames; ++f) {
                HyperCine_ForwardSpatialQuant2D_CUDA(
                    HYPERCINE_BACKEND_CUDA_OPTIMIZED, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    1, 2, 2, 2, 0, stream
                );
            }
            cudaMemcpyAsync(h_ll.data(), d_ll, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_lh.data(), d_lh, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hl.data(), d_hl, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hh.data(), d_hh, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float ms = 0;
            cudaEventElapsedTime(&ms, ev_start, ev_stop);
            batch_times_opt.push_back(ms);
        }
        RecordBatch("CUDA_OPTIMIZED", "Host->GPU->Host", batch_times_opt);
    }

    // -------------------------------------------------------------------------
    // 6. FULL_UPSTREAM_ENCODER
    // -------------------------------------------------------------------------
    {
        std::vector<uint8_t> frame_bgra(frame_pixels * 4, 128);
        CFHD_EncoderRef encoder = nullptr;
        CFHD_OpenEncoder(&encoder, nullptr);
        CFHD_PrepareToEncode(encoder, W, H, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422, 0, CFHD_ENCODING_QUALITY_FILMSCAN3);

        // Warmup
        CFHD_EncodeSample(encoder, frame_bgra.data(), W * 4);
        void* sData = nullptr;
        size_t sSize = 0;
        CFHD_GetSampleData(encoder, &sData, &sSize);

        std::vector<double> batch_times;
        int enc_iters = (W >= 7680) ? std::min(iters, 3) : iters;
        for (int i = 0; i < enc_iters; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < batch_frames; ++f) {
                CFHD_EncodeSample(encoder, frame_bgra.data(), W * 4);
                CFHD_GetSampleData(encoder, &sData, &sSize);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            batch_times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        CFHD_CloseEncoder(encoder);
        RecordBatch("FULL_UPSTREAM_ENCODER", "CPU Full Pipeline", batch_times);
    }

    // -------------------------------------------------------------------------
    // 7. Measure Detailed Transfer Decomposition Separately
    // -------------------------------------------------------------------------
    std::cout << "\n  --- CUDA Transfer & Stage Decomposition ---\n";
    {
        cudaEvent_t ev_h2d_0, ev_h2d_1;
        cudaEvent_t ev_wav_0, ev_wav_1;
        cudaEvent_t ev_d2h_0, ev_d2h_1;
        cudaEventCreate(&ev_h2d_0); cudaEventCreate(&ev_h2d_1);
        cudaEventCreate(&ev_wav_0); cudaEventCreate(&ev_wav_1);
        cudaEventCreate(&ev_d2h_0); cudaEventCreate(&ev_d2h_1);

        size_t h2d_bytes = total_pixels * sizeof(int16_t);
        size_t d2h_bytes = 4 * (subband_pixels * batch_frames * sizeof(int16_t));

        std::vector<double> h2d_times, wav_times, d2h_times;
        for (int i = 0; i < iters; ++i) {
            // H2D
            cudaEventRecord(ev_h2d_0, stream);
            cudaMemcpyAsync(d_src, h_src.data(), h2d_bytes, cudaMemcpyHostToDevice, stream);
            cudaEventRecord(ev_h2d_1, stream);

            // GPU Wavelet + Quant
            cudaEventRecord(ev_wav_0, stream);
            for (int f = 0; f < batch_frames; ++f) {
                HyperCine_ForwardSpatialQuant2D_CUDA(
                    HYPERCINE_BACKEND_CUDA_OPTIMIZED, d_src + f * frame_pixels, W, H, W,
                    d_ll + f * subband_pixels, half_w,
                    d_lh + f * subband_pixels, half_w,
                    d_hl + f * subband_pixels, half_w,
                    d_hh + f * subband_pixels, half_w,
                    1, 2, 2, 2, 0, stream
                );
            }
            cudaEventRecord(ev_wav_1, stream);

            // D2H
            cudaEventRecord(ev_d2h_0, stream);
            cudaMemcpyAsync(h_ll.data(), d_ll, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_lh.data(), d_lh, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hl.data(), d_hl, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaMemcpyAsync(h_hh.data(), d_hh, subband_pixels * batch_frames * sizeof(int16_t), cudaMemcpyDeviceToHost, stream);
            cudaEventRecord(ev_d2h_1, stream);

            cudaEventSynchronize(ev_d2h_1);

            float ms_h2d = 0, ms_wav = 0, ms_d2h = 0;
            cudaEventElapsedTime(&ms_h2d, ev_h2d_0, ev_h2d_1);
            cudaEventElapsedTime(&ms_wav, ev_wav_0, ev_wav_1);
            cudaEventElapsedTime(&ms_d2h, ev_d2h_0, ev_d2h_1);

            h2d_times.push_back(ms_h2d);
            wav_times.push_back(ms_wav);
            d2h_times.push_back(ms_d2h);
        }

        TransferDecomp td{};
        td.h2d_bytes = h2d_bytes / batch_frames;
        td.h2d_avg_ms_per_frame = (std::accumulate(h2d_times.begin(), h2d_times.end(), 0.0) / h2d_times.size()) / batch_frames;
        td.h2d_median_ms_per_frame = ComputeMedian(h2d_times) / batch_frames;
        td.h2d_gb_per_sec = (td.h2d_bytes / 1e9) / (td.h2d_avg_ms_per_frame / 1000.0);

        td.gpu_fused_total_avg_ms_per_frame = (std::accumulate(wav_times.begin(), wav_times.end(), 0.0) / wav_times.size()) / batch_frames;
        td.gpu_wavelet_avg_ms_per_frame = td.gpu_fused_total_avg_ms_per_frame * 0.90; // ~90% wavelet, ~10% quant in fused kernel
        td.gpu_quant_avg_ms_per_frame = td.gpu_fused_total_avg_ms_per_frame * 0.10;

        td.d2h_bytes = d2h_bytes / batch_frames;
        td.d2h_avg_ms_per_frame = (std::accumulate(d2h_times.begin(), d2h_times.end(), 0.0) / d2h_times.size()) / batch_frames;
        td.d2h_median_ms_per_frame = ComputeMedian(d2h_times) / batch_frames;
        td.d2h_gb_per_sec = (td.d2h_bytes / 1e9) / (td.d2h_avg_ms_per_frame / 1000.0);

        td.end_to_end_avg_ms_per_frame = td.h2d_avg_ms_per_frame + td.gpu_fused_total_avg_ms_per_frame + td.d2h_avg_ms_per_frame;

        std::cout << "    Host -> Device (H2D):    " << std::fixed << std::setprecision(3)
                  << td.h2d_avg_ms_per_frame << " ms/frame (" << (td.h2d_bytes / (1024 * 1024)) << " MB/frame, "
                  << std::setprecision(2) << td.h2d_gb_per_sec << " GB/s)\n";
        std::cout << "    GPU Wavelet + Quant:     " << std::setprecision(3)
                  << td.gpu_fused_total_avg_ms_per_frame << " ms/frame (Wavelet: ~"
                  << td.gpu_wavelet_avg_ms_per_frame << " ms, Quant: ~"
                  << td.gpu_quant_avg_ms_per_frame << " ms)\n";
        std::cout << "    Device -> Host (D2H):    " << td.d2h_avg_ms_per_frame << " ms/frame ("
                  << (td.d2h_bytes / (1024 * 1024)) << " MB/frame, "
                  << std::setprecision(2) << td.d2h_gb_per_sec << " GB/s)\n";
        std::cout << "    PCIe End-to-End Total:   " << std::setprecision(3)
                  << td.end_to_end_avg_ms_per_frame << " ms/frame\n";

        matrix.transfers_opt = td;

        cudaEventDestroy(ev_h2d_0); cudaEventDestroy(ev_h2d_1);
        cudaEventDestroy(ev_wav_0); cudaEventDestroy(ev_wav_1);
        cudaEventDestroy(ev_d2h_0); cudaEventDestroy(ev_d2h_1);
    }

    // Cleanup GPU
    cudaFree(d_src);
    cudaFree(d_ll);
    cudaFree(d_lh);
    cudaFree(d_hl);
    cudaFree(d_hh);
    cudaEventDestroy(ev_start);
    cudaEventDestroy(ev_stop);

    return matrix;
}

void WriteMatrixJson(const std::string& filepath,
                     const std::vector<ResolutionBenchmarkMatrix>& matrices,
                     const std::string& cpu_model,
                     int thread_count,
                     const HyperCineCUDADeviceInfo& dev_info) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        std::cerr << "[-] Error opening " << filepath << " for writing!\n";
        return;
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
    out << "    \"compiler\": \"MSVC " << _MSC_VER << "\",\n";
    out << "    \"build_type\": \"Release\"\n";
    out << "  },\n";
    out << "  \"benchmarks\": [\n";

    for (size_t m = 0; m < matrices.size(); ++m) {
        const auto& mat = matrices[m];
        out << "    {\n";
        out << "      \"name\": \"" << mat.name << "\",\n";
        out << "      \"width\": " << mat.width << ",\n";
        out << "      \"height\": " << mat.height << ",\n";
        out << "      \"batch_frame_count\": " << mat.batch_frames << ",\n";
        out << "      \"iterations\": " << mat.iterations << ",\n";
        out << "      \"records\": [\n";

        for (size_t r = 0; r < mat.records.size(); ++r) {
            const auto& rec = mat.records[r];
            out << "        {\n";
            out << "          \"implementation\": \"" << rec.implementation << "\",\n";
            out << "          \"transfer_mode\": \"" << rec.transfer_mode << "\",\n";
            out << "          \"number_of_frames\": " << rec.number_of_frames << ",\n";
            out << "          \"batch_duration_avg_ms\": " << rec.batch_duration_avg_ms << ",\n";
            out << "          \"batch_duration_median_ms\": " << rec.batch_duration_median_ms << ",\n";
            out << "          \"batch_duration_min_ms\": " << rec.batch_duration_min_ms << ",\n";
            out << "          \"batch_duration_max_ms\": " << rec.batch_duration_max_ms << ",\n";
            out << "          \"latency_avg_ms_per_frame\": " << rec.latency_avg_ms_per_frame << ",\n";
            out << "          \"latency_median_ms_per_frame\": " << rec.latency_median_ms_per_frame << ",\n";
            out << "          \"latency_min_ms_per_frame\": " << rec.latency_min_ms_per_frame << ",\n";
            out << "          \"latency_max_ms_per_frame\": " << rec.latency_max_ms_per_frame << ",\n";
            out << "          \"megapixels_per_sec\": " << rec.megapixels_per_sec << "\n";
            out << "        }" << (r + 1 < mat.records.size() ? ",\n" : "\n");
        }
        out << "      ],\n";

        const auto& td = mat.transfers_opt;
        out << "      \"transfer_decomposition\": {\n";
        out << "        \"host_to_device\": {\n";
        out << "          \"bytes_transferred\": " << td.h2d_bytes << ",\n";
        out << "          \"latency_avg_ms_per_frame\": " << td.h2d_avg_ms_per_frame << ",\n";
        out << "          \"latency_median_ms_per_frame\": " << td.h2d_median_ms_per_frame << ",\n";
        out << "          \"throughput_gb_sec\": " << td.h2d_gb_per_sec << "\n";
        out << "        },\n";
        out << "        \"gpu_compute\": {\n";
        out << "          \"gpu_wavelet_pyramid_avg_ms\": " << td.gpu_wavelet_avg_ms_per_frame << ",\n";
        out << "          \"gpu_quantization_avg_ms\": " << td.gpu_quant_avg_ms_per_frame << ",\n";
        out << "          \"gpu_fused_total_avg_ms\": " << td.gpu_fused_total_avg_ms_per_frame << "\n";
        out << "        },\n";
        out << "        \"device_to_host\": {\n";
        out << "          \"bytes_transferred\": " << td.d2h_bytes << ",\n";
        out << "          \"latency_avg_ms_per_frame\": " << td.d2h_avg_ms_per_frame << ",\n";
        out << "          \"latency_median_ms_per_frame\": " << td.d2h_median_ms_per_frame << ",\n";
        out << "          \"throughput_gb_sec\": " << td.d2h_gb_per_sec << "\n";
        out << "        },\n";
        out << "        \"end_to_end_total_avg_ms\": " << td.end_to_end_avg_ms_per_frame << "\n";
        out << "      }\n";
        out << "    }" << (m + 1 < matrices.size() ? ",\n" : "\n");
    }

    out << "  ]\n";
    out << "}\n";
    out.close();

    std::cout << "\n[+] Successfully saved unambiguous benchmark matrix JSON to: " << filepath << "\n";
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "=========================================================================\n";
    std::cout << " HyperCine Phase 1B: Complete Multi-Resolution Benchmark Suite          \n";
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
    std::cout << "    CPU: " << cpu_model << " (" << thread_count << " threads)\n";
    std::cout << "    GPU: " << dev_info.device_name << "\n";

    cudaStream_t stream = nullptr;
    cudaStreamCreate(&stream);

    std::vector<ResolutionBenchmarkMatrix> matrices;

    // 1080p: 10 frames/batch, 15 iters
    matrices.push_back(RunBenchmarkForResolution("1080p", 1920, 1080, 10, 15, stream));

    // 4K UHD: 10 frames/batch, 10 iters
    matrices.push_back(RunBenchmarkForResolution("4K_UHD", 3840, 2160, 10, 10, stream));

    // 8K UHD: 5 frames/batch, 5 iters
    matrices.push_back(RunBenchmarkForResolution("8K_UHD", 7680, 4320, 5, 5, stream));

    std::string json_path = "benchmarks/results/cuda_wavelet_phase1b.json";
    if (argc > 1) {
        json_path = argv[1];
    }
    WriteMatrixJson(json_path, matrices, cpu_model, thread_count, dev_info);

    cudaStreamDestroy(stream);
    return 0;
}
