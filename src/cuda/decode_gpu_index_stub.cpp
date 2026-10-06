// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/decode_gpu_index.h"
namespace HyperCine {
cudaError_t LaunchGenerateBandCheckpointsGpu(const uint8_t*, size_t,
    const GpuEntropyBandDescriptor*, int, const GpuFsmEntry*, size_t,
    GpuEntropyMode, int, GpuEntropySegmentDescriptor*, uint32_t,
    GpuCheckpointIndexResult*, uint32_t*, uint32_t, cudaStream_t) {
    return cudaErrorNotSupported;
}
} // namespace HyperCine
