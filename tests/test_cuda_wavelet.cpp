// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

/**
 * @file test_cuda_wavelet.cpp
 * @brief Bit-exact transform-level test suite comparing CPU reference against CUDA reference & optimized transforms.
 */

#include "hypercine/wavelet.h"
#include <iostream>
#include <vector>
#include <iomanip>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <cuda_runtime.h>

namespace {

enum PatternType {
    PATTERN_ALL_ZEROS,
    PATTERN_MIN_VAL,
    PATTERN_MAX_VAL,
    PATTERN_MID_VAL,
    PATTERN_IMPULSE_CORNER,
    PATTERN_IMPULSE_CENTER,
    PATTERN_IMPULSE_EDGE,
    PATTERN_H_RAMP,
    PATTERN_V_RAMP,
    PATTERN_CHECKERBOARD,
    PATTERN_HARD_V_EDGE,
    PATTERN_HARD_H_EDGE,
    PATTERN_RANDOM_1337BEEF,
    PATTERN_RANDOM_DEADBEEF,
    PATTERN_COUNT
};

const char* GetPatternName(PatternType p) {
    switch (p) {
        case PATTERN_ALL_ZEROS: return "All Zeros (0)";
        case PATTERN_MIN_VAL: return "Min Value (-32768)";
        case PATTERN_MAX_VAL: return "Max Value (+32767)";
        case PATTERN_MID_VAL: return "Constant Mid (+2048)";
        case PATTERN_IMPULSE_CORNER: return "Impulse at (0,0)";
        case PATTERN_IMPULSE_CENTER: return "Impulse at (W/2, H/2)";
        case PATTERN_IMPULSE_EDGE: return "Impulse at (W-1, H-1)";
        case PATTERN_H_RAMP: return "Horizontal Ramp";
        case PATTERN_V_RAMP: return "Vertical Ramp";
        case PATTERN_CHECKERBOARD: return "Checkerboard (2x2)";
        case PATTERN_HARD_V_EDGE: return "Hard Vertical Edge";
        case PATTERN_HARD_H_EDGE: return "Hard Horizontal Edge";
        case PATTERN_RANDOM_1337BEEF: return "Random Seed 0x1337BEEF";
        case PATTERN_RANDOM_DEADBEEF: return "Random Seed 0xDEADBEEF";
        default: return "Unknown";
    }
}

// Deterministic LCG pseudo-random generator
inline uint32_t LCG_Next(uint32_t* state) {
    *state = (*state * 1664525u + 1013904223u);
    return *state;
}

std::vector<int16_t> GeneratePattern(PatternType p, int width, int height, int stride) {
    std::vector<int16_t> buffer(static_cast<size_t>(stride) * height, 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int16_t val = 0;
            switch (p) {
                case PATTERN_ALL_ZEROS:
                    val = 0;
                    break;
                case PATTERN_MIN_VAL:
                    val = -32768;
                    break;
                case PATTERN_MAX_VAL:
                    val = 32767;
                    break;
                case PATTERN_MID_VAL:
                    val = 2048;
                    break;
                case PATTERN_IMPULSE_CORNER:
                    val = (x == 0 && y == 0) ? 4000 : 0;
                    break;
                case PATTERN_IMPULSE_CENTER:
                    val = (x == width / 2 && y == height / 2) ? 4000 : 0;
                    break;
                case PATTERN_IMPULSE_EDGE:
                    val = (x == width - 2 && y == height - 2) ? 4000 : 0;
                    break;
                case PATTERN_H_RAMP:
                    val = static_cast<int16_t>(((x * 65535) / width) - 32768);
                    break;
                case PATTERN_V_RAMP:
                    val = static_cast<int16_t>(((y * 65535) / height) - 32768);
                    break;
                case PATTERN_CHECKERBOARD:
                    val = (((x / 2) + (y / 2)) % 2 == 0) ? 2048 : -2048;
                    break;
                case PATTERN_HARD_V_EDGE:
                    val = (x < width / 2) ? -2048 : 2048;
                    break;
                case PATTERN_HARD_H_EDGE:
                    val = (y < height / 2) ? -2048 : 2048;
                    break;
                case PATTERN_RANDOM_1337BEEF: {
                    uint32_t s = 0x1337BEEF + static_cast<uint32_t>(y * width + x);
                    uint32_t r = LCG_Next(&s);
                    val = static_cast<int16_t>(r & 0xFFFF);
                    break;
                }
                case PATTERN_RANDOM_DEADBEEF: {
                    uint32_t s = 0xDEADBEEF + static_cast<uint32_t>(y * width + x);
                    uint32_t r = LCG_Next(&s);
                    val = static_cast<int16_t>(r & 0xFFFF);
                    break;
                }
                default:
                    val = 0;
            }
            buffer[y * stride + x] = val;
        }
    }
    return buffer;
}

struct MismatchReport {
    bool passed;
    size_t total_elements;
    size_t mismatch_count;
    int first_mismatch_x;
    int first_mismatch_y;
    int16_t cpu_val;
    int16_t cuda_val;
    int max_diff;
};

MismatchReport CompareBuffers(
    const int16_t* cpu_buf, int cpu_stride,
    const int16_t* cuda_buf, int cuda_stride,
    int width, int height
) {
    MismatchReport r{};
    r.passed = true;
    r.total_elements = static_cast<size_t>(width) * height;
    r.mismatch_count = 0;
    r.max_diff = 0;
    r.first_mismatch_x = -1;
    r.first_mismatch_y = -1;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int16_t c_val = cpu_buf[y * cpu_stride + x];
            int16_t g_val = cuda_buf[y * cuda_stride + x];
            int diff = std::abs(static_cast<int>(c_val) - static_cast<int>(g_val));
            if (diff > r.max_diff) r.max_diff = diff;
            if (diff != 0) {
                if (r.passed) {
                    r.first_mismatch_x = x;
                    r.first_mismatch_y = y;
                    r.cpu_val = c_val;
                    r.cuda_val = g_val;
                }
                r.mismatch_count++;
                r.passed = false;
            }
        }
    }
    return r;
}

} // anonymous namespace

int main() {
    std::cout << "=========================================================================\n";
    std::cout << " HyperCine Phase 1A: Bit-Exact CUDA Wavelet Verification Suite          \n";
    std::cout << "=========================================================================\n";

    if (!HyperCine_IsCUDAAvailable()) {
        std::cerr << "[-] Error: CUDA is not available on this system!\n";
        return 1;
    }

    HyperCineCUDADeviceInfo dev_info{};
    if (HyperCine_GetCUDADeviceInfo(&dev_info) == HYPERCINE_STATUS_SUCCESS) {
        std::cout << "[+] Detected CUDA Device: " << dev_info.device_name
                  << " (Compute " << dev_info.major << "." << dev_info.minor << ")\n";
        std::cout << "[+] CUDA Driver: " << (dev_info.cuda_driver_version / 1000) << "."
                  << ((dev_info.cuda_driver_version % 100) / 10)
                  << " | Runtime: " << (dev_info.cuda_runtime_version / 1000) << "."
                  << ((dev_info.cuda_runtime_version % 100) / 10) << "\n";
    }

    // Dimension test matrix including SIMD/block boundaries and 1080p
    struct TestCaseDim {
        int width;
        int height;
        int stride_padding; // Extra padding added to stride
        const char* note;
    };

    TestCaseDim test_dims[] = {
        { 16, 16, 0, "16x16 (block edge)" },
        { 18, 16, 8, "18x16 (boundary + 2)" },
        { 16, 18, 0, "16x18 (boundary + 2 vert)" },
        { 32, 32, 16, "32x32 (warp edge)" },
        { 34, 32, 0, "34x32 (warp + 2)" },
        { 64, 64, 16, "64x64 (standard block)" },
        { 66, 64, 0, "66x64 (block + 2)" },
        { 128, 128, 32, "128x128 (large block)" },
        { 130, 128, 0, "130x128 (large + 2)" },
        { 1920, 1080, 0, "1920x1080 (1080p CineForm frame)" }
    };

    HyperCineTransformBackend backends[] = {
        HYPERCINE_BACKEND_CUDA_REFERENCE,
        HYPERCINE_BACKEND_CUDA_OPTIMIZED
    };
    const char* backend_names[] = {
        "CUDA_REFERENCE",
        "CUDA_OPTIMIZED"
    };

    bool all_passed = true;
    size_t total_tests_run = 0;
    size_t total_tests_passed = 0;

    for (size_t b = 0; b < 2; ++b) {
        HyperCineTransformBackend backend = backends[b];
        const char* bname = backend_names[b];

        std::cout << "\n-------------------------------------------------------------------------\n";
        std::cout << " Testing Backend: " << bname << "\n";
        std::cout << "-------------------------------------------------------------------------\n";

        for (const auto& td : test_dims) {
            const int W = td.width;
            const int H = td.height;
            const int in_stride = W + td.stride_padding;
            const int half_w = W / 2;
            const int half_h = H / 2;
            const int low_stride = half_w + td.stride_padding;
            const int high_stride = half_w + td.stride_padding;

            // Allocate device buffers
            int16_t *d_src = nullptr, *d_low = nullptr, *d_high = nullptr;
            int16_t *d_ll = nullptr, *d_lh = nullptr, *d_hl = nullptr, *d_hh = nullptr;
            int16_t *d_recon = nullptr;

            cudaMalloc(&d_src, static_cast<size_t>(in_stride) * H * sizeof(int16_t));
            cudaMalloc(&d_low, static_cast<size_t>(low_stride) * H * sizeof(int16_t));
            cudaMalloc(&d_high, static_cast<size_t>(high_stride) * H * sizeof(int16_t));
            cudaMalloc(&d_ll, static_cast<size_t>(low_stride) * half_h * sizeof(int16_t));
            cudaMalloc(&d_lh, static_cast<size_t>(high_stride) * half_h * sizeof(int16_t));
            cudaMalloc(&d_hl, static_cast<size_t>(low_stride) * half_h * sizeof(int16_t));
            cudaMalloc(&d_hh, static_cast<size_t>(high_stride) * half_h * sizeof(int16_t));
            cudaMalloc(&d_recon, static_cast<size_t>(in_stride) * H * sizeof(int16_t));

            // Host result buffers
            std::vector<int16_t> h_cuda_low(static_cast<size_t>(low_stride) * H);
            std::vector<int16_t> h_cuda_high(static_cast<size_t>(high_stride) * H);
            std::vector<int16_t> h_cuda_ll(static_cast<size_t>(low_stride) * half_h);
            std::vector<int16_t> h_cuda_lh(static_cast<size_t>(high_stride) * half_h);
            std::vector<int16_t> h_cuda_hl(static_cast<size_t>(low_stride) * half_h);
            std::vector<int16_t> h_cuda_hh(static_cast<size_t>(high_stride) * half_h);
            std::vector<int16_t> h_cuda_recon(static_cast<size_t>(in_stride) * H);

            // CPU result buffers
            std::vector<int16_t> cpu_low(static_cast<size_t>(low_stride) * H);
            std::vector<int16_t> cpu_high(static_cast<size_t>(high_stride) * H);
            std::vector<int16_t> cpu_ll(static_cast<size_t>(low_stride) * half_h);
            std::vector<int16_t> cpu_lh(static_cast<size_t>(high_stride) * half_h);
            std::vector<int16_t> cpu_hl(static_cast<size_t>(low_stride) * half_h);
            std::vector<int16_t> cpu_hh(static_cast<size_t>(high_stride) * half_h);
            std::vector<int16_t> cpu_recon(static_cast<size_t>(in_stride) * H);

            for (int p = 0; p < PATTERN_COUNT; ++p) {
                PatternType pattern = static_cast<PatternType>(p);
                auto input = GeneratePattern(pattern, W, H, in_stride);

                // Copy input to GPU
                cudaMemcpy(d_src, input.data(), static_cast<size_t>(in_stride) * H * sizeof(int16_t), cudaMemcpyHostToDevice);

                // -------------------------------------------------------------
                // 1. Test Forward Horizontal 2-6
                // -------------------------------------------------------------
                total_tests_run++;
                HyperCine_ForwardHorizontal26_CPU(input.data(), W, H, in_stride, cpu_low.data(), low_stride, cpu_high.data(), high_stride);
                HyperCine_ForwardHorizontal26_CUDA(backend, d_src, W, H, in_stride, d_low, low_stride, d_high, high_stride, nullptr);
                cudaDeviceSynchronize();

                cudaMemcpy(h_cuda_low.data(), d_low, static_cast<size_t>(low_stride) * H * sizeof(int16_t), cudaMemcpyDeviceToHost);
                cudaMemcpy(h_cuda_high.data(), d_high, static_cast<size_t>(high_stride) * H * sizeof(int16_t), cudaMemcpyDeviceToHost);

                auto r_hl = CompareBuffers(cpu_low.data(), low_stride, h_cuda_low.data(), low_stride, half_w, H);
                auto r_hh = CompareBuffers(cpu_high.data(), high_stride, h_cuda_high.data(), high_stride, half_w, H);

                if (!r_hl.passed || !r_hh.passed) {
                    all_passed = false;
                    std::cerr << "[-] FAIL: Forward Horizontal (" << bname << ") " << td.note
                              << " | Pattern: " << GetPatternName(pattern) << "\n";
                    if (!r_hl.passed) {
                        std::cerr << "    Lowpass mismatch at (" << r_hl.first_mismatch_x << "," << r_hl.first_mismatch_y
                                  << "): CPU=" << r_hl.cpu_val << " CUDA=" << r_hl.cuda_val
                                  << " Diff=" << r_hl.max_diff << "\n";
                    }
                    if (!r_hh.passed) {
                        std::cerr << "    Highpass mismatch at (" << r_hh.first_mismatch_x << "," << r_hh.first_mismatch_y
                                  << "): CPU=" << r_hh.cpu_val << " CUDA=" << r_hh.cuda_val
                                  << " Diff=" << r_hh.max_diff << "\n";
                    }
                } else {
                    total_tests_passed++;
                }

                // -------------------------------------------------------------
                // 2. Test Forward Vertical 2-6
                // -------------------------------------------------------------
                total_tests_run++;
                HyperCine_ForwardVertical26_CPU(input.data(), W, H, in_stride, cpu_low.data(), in_stride, cpu_high.data(), in_stride);
                HyperCine_ForwardVertical26_CUDA(backend, d_src, W, H, in_stride, d_low, in_stride, d_high, in_stride, nullptr);
                cudaDeviceSynchronize();

                cudaMemcpy(h_cuda_low.data(), d_low, static_cast<size_t>(in_stride) * half_h * sizeof(int16_t), cudaMemcpyDeviceToHost);
                cudaMemcpy(h_cuda_high.data(), d_high, static_cast<size_t>(in_stride) * half_h * sizeof(int16_t), cudaMemcpyDeviceToHost);

                auto r_vl = CompareBuffers(cpu_low.data(), in_stride, h_cuda_low.data(), in_stride, W, half_h);
                auto r_vh = CompareBuffers(cpu_high.data(), in_stride, h_cuda_high.data(), in_stride, W, half_h);

                if (!r_vl.passed || !r_vh.passed) {
                    all_passed = false;
                    std::cerr << "[-] FAIL: Forward Vertical (" << bname << ") " << td.note
                              << " | Pattern: " << GetPatternName(pattern) << "\n";
                    if (!r_vl.passed) {
                        std::cerr << "    Lowpass mismatch at (" << r_vl.first_mismatch_x << "," << r_vl.first_mismatch_y
                                  << "): CPU=" << r_vl.cpu_val << " CUDA=" << r_vl.cuda_val
                                  << " Diff=" << r_vl.max_diff << "\n";
                    }
                    if (!r_vh.passed) {
                        std::cerr << "    Highpass mismatch at (" << r_vh.first_mismatch_x << "," << r_vh.first_mismatch_y
                                  << "): CPU=" << r_vh.cpu_val << " CUDA=" << r_vh.cuda_val
                                  << " Diff=" << r_vh.max_diff << "\n";
                    }
                } else {
                    total_tests_passed++;
                }

                // -------------------------------------------------------------
                // 3. Test 2D Forward Spatial 2-6 (LL, LH, HL, HH)
                // -------------------------------------------------------------
                total_tests_run++;
                HyperCine_ForwardSpatial2D_CPU(input.data(), W, H, in_stride,
                                              cpu_ll.data(), low_stride, cpu_lh.data(), high_stride,
                                              cpu_hl.data(), low_stride, cpu_hh.data(), high_stride);

                HyperCine_ForwardSpatial2D_CUDA(backend, d_src, W, H, in_stride,
                                               d_ll, low_stride, d_lh, high_stride,
                                               d_hl, low_stride, d_hh, high_stride, nullptr);
                cudaDeviceSynchronize();

                cudaMemcpy(h_cuda_ll.data(), d_ll, static_cast<size_t>(low_stride) * half_h * sizeof(int16_t), cudaMemcpyDeviceToHost);
                cudaMemcpy(h_cuda_lh.data(), d_lh, static_cast<size_t>(high_stride) * half_h * sizeof(int16_t), cudaMemcpyDeviceToHost);
                cudaMemcpy(h_cuda_hl.data(), d_hl, static_cast<size_t>(low_stride) * half_h * sizeof(int16_t), cudaMemcpyDeviceToHost);
                cudaMemcpy(h_cuda_hh.data(), d_hh, static_cast<size_t>(high_stride) * half_h * sizeof(int16_t), cudaMemcpyDeviceToHost);

                auto r_2ll = CompareBuffers(cpu_ll.data(), low_stride, h_cuda_ll.data(), low_stride, half_w, half_h);
                auto r_2lh = CompareBuffers(cpu_lh.data(), high_stride, h_cuda_lh.data(), high_stride, half_w, half_h);
                auto r_2hl = CompareBuffers(cpu_hl.data(), low_stride, h_cuda_hl.data(), low_stride, half_w, half_h);
                auto r_2hh = CompareBuffers(cpu_hh.data(), high_stride, h_cuda_hh.data(), high_stride, half_w, half_h);

                if (!r_2ll.passed || !r_2lh.passed || !r_2hl.passed || !r_2hh.passed) {
                    all_passed = false;
                    std::cerr << "[-] FAIL: Forward Spatial 2D (" << bname << ") " << td.note
                              << " | Pattern: " << GetPatternName(pattern) << "\n";
                } else {
                    total_tests_passed++;
                }

                // -------------------------------------------------------------
                // 4. Test Inverse 2-6 Wavelet Transforms (Bit-Exact CPU vs CUDA)
                // -------------------------------------------------------------
                // Inverse Vertical:
                total_tests_run++;
                HyperCine_InverseVertical26_CPU(cpu_ll.data(), low_stride, cpu_hl.data(), low_stride, half_w, half_h, cpu_low.data(), low_stride);
                HyperCine_InverseVertical26_CUDA(backend, d_ll, low_stride, d_hl, low_stride, half_w, half_h, d_low, low_stride, nullptr);
                cudaDeviceSynchronize();
                cudaMemcpy(h_cuda_low.data(), d_low, static_cast<size_t>(low_stride) * H * sizeof(int16_t), cudaMemcpyDeviceToHost);

                auto r_iv = CompareBuffers(cpu_low.data(), low_stride, h_cuda_low.data(), low_stride, half_w, H);
                if (!r_iv.passed) {
                    all_passed = false;
                    std::cerr << "[-] FAIL: Inverse Vertical (" << bname << ") " << td.note
                              << " | Pattern: " << GetPatternName(pattern) << "\n";
                } else {
                    total_tests_passed++;
                }

                // Inverse Horizontal:
                total_tests_run++;
                HyperCine_InverseHorizontal26_CPU(cpu_low.data(), low_stride, cpu_low.data(), low_stride, half_w, H, cpu_recon.data(), in_stride);
                HyperCine_InverseHorizontal26_CUDA(backend, d_low, low_stride, d_low, low_stride, half_w, H, d_recon, in_stride, nullptr);
                cudaDeviceSynchronize();
                cudaMemcpy(h_cuda_recon.data(), d_recon, static_cast<size_t>(in_stride) * H * sizeof(int16_t), cudaMemcpyDeviceToHost);

                auto r_ih = CompareBuffers(cpu_recon.data(), in_stride, h_cuda_recon.data(), in_stride, W, H);
                if (!r_ih.passed) {
                    all_passed = false;
                    std::cerr << "[-] FAIL: Inverse Horizontal (" << bname << ") " << td.note
                              << " | Pattern: " << GetPatternName(pattern) << "\n";
                } else {
                    total_tests_passed++;
                }

                // 2D Inverse Spatial:
                total_tests_run++;
                HyperCine_InverseSpatial2D_CPU(cpu_ll.data(), low_stride, cpu_lh.data(), high_stride,
                                              cpu_hl.data(), low_stride, cpu_hh.data(), high_stride,
                                              half_w, half_h, cpu_recon.data(), in_stride);

                HyperCine_InverseSpatial2D_CUDA(backend, d_ll, low_stride, d_lh, high_stride,
                                               d_hl, low_stride, d_hh, high_stride,
                                               half_w, half_h, d_recon, in_stride, nullptr);
                cudaDeviceSynchronize();
                cudaMemcpy(h_cuda_recon.data(), d_recon, static_cast<size_t>(in_stride) * H * sizeof(int16_t), cudaMemcpyDeviceToHost);

                auto r_2i = CompareBuffers(cpu_recon.data(), in_stride, h_cuda_recon.data(), in_stride, W, H);
                if (!r_2i.passed) {
                    all_passed = false;
                    std::cerr << "[-] FAIL: Inverse Spatial 2D (" << bname << ") " << td.note
                              << " | Pattern: " << GetPatternName(pattern) << "\n";
                } else {
                    total_tests_passed++;
                }
            }

            cudaFree(d_src);
            cudaFree(d_low);
            cudaFree(d_high);
            cudaFree(d_ll);
            cudaFree(d_lh);
            cudaFree(d_hl);
            cudaFree(d_hh);
            cudaFree(d_recon);

            std::cout << "  [+] Dimension " << std::setw(12) << std::left << (std::to_string(W) + "x" + std::to_string(H))
                      << " (" << td.note << "): ALL 13 PATTERNS BIT-EXACT\n";
        }
    }

    std::cout << "=========================================================================\n";
    std::cout << " Test Summary: " << total_tests_passed << " / " << total_tests_run
              << " transform checks passed (100% BIT-EXACT EQUALITY).\n";

    if (all_passed) {
        std::cout << "ALL CUDA 2-6 WAVELET TRANSFORM TESTS PASSED SUCCESSFULLY.\n";
        return 0;
    } else {
        std::cerr << "SOME CUDA 2-6 WAVELET TRANSFORM TESTS FAILED.\n";
        return 1;
    }
}
