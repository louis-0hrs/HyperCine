// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/wavelet.h"
#include <iostream>
#include <vector>
#include <cuda_runtime.h>
#include <cstdlib>

static bool CompareBuffers16(const int16_t* a, int a_stride,
                             const int16_t* b, int b_stride,
                             int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (a[y * a_stride + x] != b[y * b_stride + x]) return false;
        }
    }
    return true;
}

int main() {
    std::cout << "Running Phase 1B Pyramid and Quantization Tests...\n";
    if (!HyperCine_IsCUDAAvailable()) {
        std::cerr << "CUDA not available!\n";
        return -1;
    }

    const int W = 1920;
    const int H = 1080;

    // Deterministic test vector (Rule #6: explicit seed, no random_device)
    std::vector<int16_t> src(W * H);
    for (int i = 0; i < W * H; ++i) {
        src[i] = static_cast<int16_t>((i % 2000) - 1000);
    }

    const int half_w = W / 2;
    const int half_h = H / 2;
    const size_t band_elems = static_cast<size_t>(half_w) * half_h;

    // CPU reference via upstream production implementation
    std::vector<int16_t> ll(band_elems), lh(band_elems), hl(band_elems), hh(band_elems);
    HyperCine_ForwardSpatial2D_Upstream(
        src.data(), W, H, W,
        ll.data(), half_w,
        lh.data(), half_w,
        hl.data(), half_w,
        hh.data(), half_w,
        1, 2, 2, 2
    );

    // GPU CUDA reference implementation
    int16_t *d_src, *d_ll, *d_lh, *d_hl, *d_hh;
    cudaMalloc(&d_src, W * H * sizeof(int16_t));
    cudaMalloc(&d_ll,  band_elems * sizeof(int16_t));
    cudaMalloc(&d_lh,  band_elems * sizeof(int16_t));
    cudaMalloc(&d_hl,  band_elems * sizeof(int16_t));
    cudaMalloc(&d_hh,  band_elems * sizeof(int16_t));

    cudaMemcpy(d_src, src.data(), W * H * sizeof(int16_t), cudaMemcpyHostToDevice);

    HyperCine_ForwardSpatialQuant2D_CUDA(
        HYPERCINE_BACKEND_CUDA_REFERENCE,
        d_src, W, H, W,
        d_ll, half_w, d_lh, half_w, d_hl, half_w, d_hh, half_w,
        1, 2, 2, 2,
        0, // midpoint: for g_midpoint_prequant=2, divisor=2 → 2/2 - 1 = 0
        nullptr
    );
    cudaDeviceSynchronize();

    std::vector<int16_t> gpu_ll(band_elems), gpu_lh(band_elems);
    std::vector<int16_t> gpu_hl(band_elems), gpu_hh(band_elems);

    cudaMemcpy(gpu_ll.data(), d_ll, band_elems * sizeof(int16_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(gpu_lh.data(), d_lh, band_elems * sizeof(int16_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(gpu_hl.data(), d_hl, band_elems * sizeof(int16_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(gpu_hh.data(), d_hh, band_elems * sizeof(int16_t), cudaMemcpyDeviceToHost);

    bool pass_ll = CompareBuffers16(ll.data(), half_w, gpu_ll.data(), half_w, half_w, half_h);
    bool pass_lh = CompareBuffers16(lh.data(), half_w, gpu_lh.data(), half_w, half_w, half_h);
    bool pass_hl = CompareBuffers16(hl.data(), half_w, gpu_hl.data(), half_w, half_w, half_h);
    bool pass_hh = CompareBuffers16(hh.data(), half_w, gpu_hh.data(), half_w, half_w, half_h);

    cudaFree(d_src); cudaFree(d_ll); cudaFree(d_lh); cudaFree(d_hl); cudaFree(d_hh);

    if (pass_ll && pass_lh && pass_hl && pass_hh) {
        std::cout << "[+] Quantization Phase 1B Tests Passed (BIT-EXACT!)\n";
        return 0;
    }

    std::cout << "[-] Quantization Phase 1B Tests Failed!\n";
    std::cout << "  LL: " << (pass_ll ? "PASS" : "FAIL") << "\n";
    std::cout << "  LH: " << (pass_lh ? "PASS" : "FAIL") << "\n";
    std::cout << "  HL: " << (pass_hl ? "PASS" : "FAIL") << "\n";
    std::cout << "  HH: " << (pass_hh ? "PASS" : "FAIL") << "\n";

    // Print first mismatch for debugging
    for (size_t i = 0; i < band_elems; ++i) {
        if (ll[i] != gpu_ll[i] || lh[i] != gpu_lh[i] ||
            hl[i] != gpu_hl[i] || hh[i] != gpu_hh[i]) {
            std::cout << "First mismatch at index " << i
                      << " (row=" << (i / half_w) << ", col=" << (i % half_w) << "):\n";
            std::cout << "  LL: cpu=" << ll[i] << " gpu=" << gpu_ll[i] << "\n";
            std::cout << "  LH: cpu=" << lh[i] << " gpu=" << gpu_lh[i] << "\n";
            std::cout << "  HL: cpu=" << hl[i] << " gpu=" << gpu_hl[i] << "\n";
            std::cout << "  HH: cpu=" << hh[i] << " gpu=" << gpu_hh[i] << "\n";
            break;
        }
    }
    return 1;
}
