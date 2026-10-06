// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/decode_gpu_index.h"
#include <cuda_runtime.h>
#include <climits>
namespace HyperCine {
namespace {
__device__ uint64_t NowNs() {
    uint64_t t;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
    return t;
}
__device__ void Fail(GpuCheckpointIndexResult& r, GpuBandDecodeStatus status, uint32_t* error) {
    r.status = status;
    r.finished = 1;
    r.segment_count = 0;
    if (error) atomicCAS(error, 0u, static_cast<uint32_t>(status));
}
__global__ void IndexBand(const uint8_t* sample, size_t size,
    const GpuEntropyBandDescriptor* descriptor, int band,
    const GpuFsmEntry* tables, size_t table_entries,
    GpuEntropyMode mode, int fixed_segments,
    GpuEntropySegmentDescriptor* segments, uint32_t capacity,
    GpuCheckpointIndexResult* result, uint32_t* error, bool first, bool last) {
    if (threadIdx.x != 0) return;
    const uint64_t start = NowNs();
    const auto d = *descriptor;
    GpuCheckpointIndexResult r;
    // Diagnostics transfer whole records, including ABI padding. Initialize
    // every byte rather than relying on member-wise value initialization.
    if (first) memset(&r, 0, sizeof(r));
    else r = *result;
    if (!first && r.finished) return;
    const uint32_t states = d.codebook_id == 0 ? 136 : 518;
    const size_t base = d.codebook_id == 0 ? 0 : (d.codebook_id == 1 ? 2176 : 10464);
    if (first) {
        if (d.codebook_id < 0 || d.codebook_id > 2 || base + states * 16 > table_entries)
            Fail(r, GpuBandDecodeStatus::InvalidCodebook, error);
        else if (d.method != BandEncodingMethod::RunLengthVLC || d.has_peaks ||
                 d.initial_fsm_state >= states || !d.coefficient_count ||
                 d.coefficient_count > UINT_MAX - 8 || !capacity || capacity > 64 || band < 0 || band >= 27)
            Fail(r, GpuBandDecodeStatus::InvalidDescriptor, error);
        else if (d.payload_offset >= size || d.payload_bytes > size - d.payload_offset ||
                 d.payload_bytes > d.safe_upper_bound || d.payload_bytes > kGpuIndexMaxPayloadBytes)
            Fail(r, GpuBandDecodeStatus::InputTruncated, error);
        else {
            uint32_t target = 1;
            if (mode == GpuEntropyMode::ParallelAdaptive) {
                const auto n = d.payload_bytes;
                target = n < 512 ? 1 : n < 4096 ? 4 : n < 32768 ? 8 : n < 131072 ? 16 : n < 524288 ? 32 : 64;
            } else if (mode == GpuEntropyMode::ParallelConfigurable)
                target = fixed_segments > 0 ? fixed_segments : 1;
            if (target > capacity) Fail(r, GpuBandDecodeStatus::CheckpointCapacityOverflow, error);
            else {
                r.target_segments = target;
                r.target_nibbles = (d.payload_bytes * 2 + target - 1) / target;
                if (r.target_nibbles < 16) r.target_nibbles = 16;
                r.state = d.initial_fsm_state;
                r.segment_count = 1;
                GpuEntropySegmentDescriptor s{};
                memset(&s, 0, sizeof(s));
                s.band_index = band;
                s.initial_state = r.state;
                s.max_coeff_pos = static_cast<uint32_t>(d.coefficient_count);
                // Preserve the unscanned host small-band descriptor, but validate
                // its payload here before allowing replay.
                if (target == 1) s.end_byte_offset = d.payload_bytes;
                segments[0] = s;
            }
        }
    }
    const uint32_t end = d.payload_bytes <= kGpuIndexMaxPayloadBytes ? d.payload_bytes * 2 : 0;
    const uint32_t stop = r.next_nibble + kGpuIndexNibblesPerLaunch < end ? r.next_nibble + kGpuIndexNibblesPerLaunch : end;
    while (!r.finished && r.next_nibble < stop) {
        if (r.state >= states) { Fail(r, GpuBandDecodeStatus::InvalidFsmTransition, error); break; }
        const uint32_t n = r.next_nibble;
        const uint8_t byte = sample[d.payload_offset + n / 2];
        const uint8_t nib = n & 1 ? byte & 15 : byte >> 4;
        const auto e = tables[base + size_t(r.state) * 16 + nib];
        if (e.value0 == 32767) {
            if (r.target_segments > 1) {
                segments[r.segment_count - 1].end_byte_offset = n / 2;
                segments[r.segment_count - 1].end_nibble_idx = n & 1;
            }
            r.finished = 1;
            break;
        }
        const uint32_t pre = e.pre_post_skip & 511, post = (e.pre_post_skip >> 12) & 7;
        const uint64_t write = uint64_t(r.out_coeff_pos) + pre;
        if ((e.value0 && write >= d.coefficient_count) || (e.value1 && write + 1 >= d.coefficient_count) ||
            write + post > d.coefficient_count + 8) {
            Fail(r, GpuBandDecodeStatus::CoefficientOverflow, error); break;
        }
        if (e.next_state >= states) { Fail(r, GpuBandDecodeStatus::InvalidFsmTransition, error); break; }
        r.out_coeff_pos = static_cast<uint32_t>(write + post);
        r.state = e.next_state;
        ++r.next_nibble;
        if (r.next_nibble - r.segment_start_nibble >= r.target_nibbles && r.segment_count < r.target_segments) {
            if (r.segment_count >= capacity) { Fail(r, GpuBandDecodeStatus::CheckpointCapacityOverflow, error); break; }
            auto& prev = segments[r.segment_count - 1];
            prev.end_byte_offset = r.next_nibble / 2;
            prev.end_nibble_idx = r.next_nibble & 1;
            GpuEntropySegmentDescriptor s{};
            memset(&s, 0, sizeof(s));
            s.band_index = band;
            s.byte_offset = r.next_nibble / 2;
            s.nibble_idx = r.next_nibble & 1;
            s.initial_state = r.state;
            s.out_coeff_pos = r.out_coeff_pos;
            s.max_coeff_pos = static_cast<uint32_t>(d.coefficient_count);
            segments[r.segment_count++] = s;
            r.segment_start_nibble = r.next_nibble;
        }
    }
    if (!r.finished && (r.next_nibble == end || last)) Fail(r, GpuBandDecodeStatus::MissingEndMarker, error);
    const uint64_t elapsed_ns = NowNs() - start;
    r.total_ns += elapsed_ns;
    if (elapsed_ns > r.max_launch_ns) r.max_launch_ns = elapsed_ns;
    *result = r;
}
} // namespace
cudaError_t LaunchGenerateBandCheckpointsGpu(const uint8_t* sample, size_t size,
    const GpuEntropyBandDescriptor* desc, int band, const GpuFsmEntry* tables,
    size_t entries, GpuEntropyMode mode, int fixed,
    GpuEntropySegmentDescriptor* segments, uint32_t capacity,
    GpuCheckpointIndexResult* result, uint32_t* error, uint32_t hint, cudaStream_t stream) {
    if (!sample || !desc || !tables || !segments || !result || !entries || hint > kGpuIndexMaxPayloadBytes)
        return cudaErrorInvalidValue;
    // CUDA can lower struct assignment to member stores and omit padding even
    // after a local memset. Initialize device storage for whole-record copies.
    auto init_error = cudaMemsetAsync(result, 0, sizeof(*result), stream);
    if (init_error != cudaSuccess) return init_error;
    if (capacity && capacity <= kGpuIndexSegmentCapacity) {
        init_error = cudaMemsetAsync(segments, 0, capacity * sizeof(*segments), stream);
        if (init_error != cudaSuccess) return init_error;
    }
    const uint32_t launches = hint ? (hint * 2 + kGpuIndexNibblesPerLaunch - 1) / kGpuIndexNibblesPerLaunch : 1;
    for (uint32_t i = 0; i < launches; ++i) {
        IndexBand<<<1, 32, 0, stream>>>(sample, size, desc, band, tables, entries,
            mode, fixed, segments, capacity, result, error, i == 0, i + 1 == launches);
        const auto err = cudaGetLastError();
        if (err != cudaSuccess) return err;
    }
    return cudaSuccess;
}
} // namespace HyperCine
