// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/decode_entropy_cuda.h"
namespace HyperCine {
constexpr uint32_t kGpuIndexSegmentCapacity = 64;
constexpr uint32_t kGpuIndexNibblesPerLaunch = 262144;
constexpr uint32_t kGpuIndexMaxPayloadBytes = 32 * 1024 * 1024;
// Continuation state and diagnostics. Checkpoints retain the Phase 2C format.
struct GpuCheckpointIndexResult {
    GpuBandDecodeStatus status = GpuBandDecodeStatus::Complete;
    uint32_t segment_count = 0, next_nibble = 0, out_coeff_pos = 0;
    uint32_t segment_start_nibble = 0, target_nibbles = 0, target_segments = 0;
    uint32_t finished = 0;
    uint16_t state = 0;
    uint64_t total_ns = 0, max_launch_ns = 0;
};
// Enqueues bounded continuation launches. Payload hint determines dispatch
// count only; the device validates the descriptor. Short hints fail closed.
// No coefficient writes, host polling, allocation or checkpoint transfer.
cudaError_t LaunchGenerateBandCheckpointsGpu(
    const uint8_t* sample, size_t sample_size,
    const GpuEntropyBandDescriptor* descriptor, int band_index,
    const GpuFsmEntry* tables, size_t table_entries,
    GpuEntropyMode mode, int fixed_segments,
    GpuEntropySegmentDescriptor* segments, uint32_t segment_capacity,
    GpuCheckpointIndexResult* result, uint32_t* frame_error,
    uint32_t payload_hint, cudaStream_t stream);
} // namespace HyperCine
