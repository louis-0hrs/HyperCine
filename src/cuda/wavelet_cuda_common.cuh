// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_WAVELET_CUDA_COMMON_CUH
#define HYPERCINE_WAVELET_CUDA_COMMON_CUH

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include "hypercine/wavelet.h"

#define CUDA_CHECK(call) \
    do { \
        cudaError_t err = (call); \
        if (err != cudaSuccess) { \
            fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
            return HYPERCINE_STATUS_CUDA_ERROR; \
        } \
    } while (0)

#define CUDA_CHECK_KERNEL() \
    do { \
        cudaError_t err = cudaGetLastError(); \
        if (err != cudaSuccess) { \
            fprintf(stderr, "CUDA kernel error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
            return HYPERCINE_STATUS_CUDA_ERROR; \
        } \
    } while (0)

__device__ __forceinline__ int16_t CUDASaturate16S(int32_t val) {
#if defined(__CUDA_ARCH__)
    // Clamping to signed 16-bit range [-32768, 32767]
    return static_cast<int16_t>(max(-32768, min(32767, val)));
#else
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return static_cast<int16_t>(val);
#endif
}

#endif // HYPERCINE_WAVELET_CUDA_COMMON_CUH
