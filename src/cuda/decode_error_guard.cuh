// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include <cuda_runtime.h>
#include <cstdint>
namespace HyperCine {
// Call uniformly across the block, before any per-thread early return.
// One atomic read per block prevents the error flag becoming a pixel bottleneck.
__device__ inline bool DecoderFrameFailed(uint32_t* error) {
    if (!error) return false;
    __shared__ uint32_t observed;
    if (threadIdx.x == 0 && threadIdx.y == 0 && threadIdx.z == 0)
        observed = atomicAdd(error, 0u);
    __syncthreads();
    return observed != 0;
}
} // namespace HyperCine
