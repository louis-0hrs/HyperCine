// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Diagnostic control for NVIDIA Compute Sanitizer sessions. This is not a codec
// test: it exists so an evidence report can show that the installed sanitizer
// actually instruments device code and reports defects on this machine, and so
// a --kernel-name filter can be shown to include or exclude a named kernel.
//
// Run `--violate` only under compute-sanitizer. Without the sanitizer the second
// kernel writes one element past the end of its device allocation.
#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>

__global__ void sanitizer_control_clean(int* p) { p[threadIdx.x] = int(threadIdx.x); }

// Allocation is 32 ints; thread 0 writes element 32.
__global__ void sanitizer_control_out_of_bounds(int* p) { p[32 + threadIdx.x] = 1; }

static bool Check(cudaError_t status, const char* what) {
    if (status == cudaSuccess) return true;
    std::printf("cuda error in %s: %s\n", what, cudaGetErrorString(status));
    return false;
}

int main(int argc, char** argv) {
    bool violate = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--violate") == 0) violate = true;
        else if (std::strcmp(argv[i], "--clean") == 0) violate = false;
        else { std::printf("usage: sanitizer_launch_control [--clean|--violate]\n"); return 2; }
    }
    int device = 0;
    cudaDeviceProp properties{};
    if (!Check(cudaGetDevice(&device), "cudaGetDevice")) return 1;
    if (!Check(cudaGetDeviceProperties(&properties, device), "cudaGetDeviceProperties")) return 1;
    std::printf("device=%s multiProcessorCount=%d\n", properties.name, properties.multiProcessorCount);

    int* buffer = nullptr;
    if (!Check(cudaMalloc(&buffer, 32 * sizeof(int)), "cudaMalloc")) return 1;
    if (!Check(cudaMemset(buffer, 0, 32 * sizeof(int)), "cudaMemset")) return 1;

    sanitizer_control_clean<<<1, 32>>>(buffer);
    if (!Check(cudaGetLastError(), "launch clean")) return 1;
    if (!Check(cudaDeviceSynchronize(), "synchronize clean")) return 1;

    int host[32] = {};
    if (!Check(cudaMemcpy(host, buffer, sizeof(host), cudaMemcpyDeviceToHost), "cudaMemcpy")) return 1;
    bool ordered = true;
    for (int i = 0; i < 32; ++i) ordered = ordered && host[i] == i;
    std::printf("clean kernel launches=1 values_ordered=%s\n", ordered ? "yes" : "no");
    if (!ordered) return 1;

    if (violate) {
        sanitizer_control_out_of_bounds<<<1, 32>>>(buffer);
        if (!Check(cudaGetLastError(), "launch out_of_bounds")) return 1;
        if (!Check(cudaDeviceSynchronize(), "synchronize out_of_bounds")) return 1;
        std::printf("out-of-bounds kernel launches=1\n");
    }

    if (!Check(cudaFree(buffer), "cudaFree")) return 1;
    std::printf("control complete\n");
    return 0;
}
