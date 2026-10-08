// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "decode_hybrid_entropy.cuh"
#include <cuda_runtime.h>

namespace HyperCine {
namespace HybridEntropy {

namespace {

__device__ inline unsigned GetNibble(const uint8_t* p, unsigned n) {
    return (n & 1) ? (p[n / 2] & 15) : (p[n / 2] >> 4);
}

__global__ void HybridSummariesKernel(
    const uint8_t* __restrict__ p,
    unsigned bytes,
    const GpuFsmEntry* __restrict__ t,
    unsigned states,
    unsigned chunk,
    unsigned count,
    Summary* __restrict__ out
) {
    unsigned c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= count) return;
    unsigned begin = c * chunk * 2;
    unsigned end = min(bytes * 2, begin + chunk * 2);
    Summary r{0, 0, 65535};
    for (unsigned n = begin; n < end; ++n) {
        auto e = t[r.state * 16 + GetNibble(p, n)];
        if (e.value0 == Marker) {
            r.terminal = uint16_t(n - begin);
            break;
        }
        r.advance += (e.pre_post_skip & 511) + ((e.pre_post_skip >> 12) & 7);
        r.state = e.next_state;
    }
    out[c] = r;
}

__global__ void HybridReplayKernel(
    const uint8_t* __restrict__ p,
    const GpuFsmEntry* __restrict__ t,
    const Restart* __restrict__ cp,
    unsigned count,
    unsigned marker,
    unsigned cap,
    int quant,
    int16_t* __restrict__ output,
    unsigned* __restrict__ error,
    int width,
    size_t pitch_samples
) {
    unsigned c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= count) return;
    unsigned end = (c + 1 < count) ? cp[c + 1].nibble : marker;
    uint64_t pos = cp[c].position;
    unsigned state = cp[c].state;
    bool pitched = (pitch_samples != 0 && width > 0 && pitch_samples != size_t(width));
    for (unsigned n = cp[c].nibble; n < end; ++n) {
        auto e = t[state * 16 + GetNibble(p, n)];
        unsigned pre = e.pre_post_skip & 511;
        unsigned post = (e.pre_post_skip >> 12) & 7;
        if (e.value0 == Marker) {
            if (error) atomicExch(error, 1);
            return;
        }
        pos += pre;
        if ((e.value0 && pos >= cap) || (e.value1 && (pos + 1) >= cap) || (pos + post > uint64_t(cap) + 8)) {
            if (error) atomicExch(error, 2);
            return;
        }
        if (e.value0) {
            size_t idx0 = pitched ? ((pos / width) * pitch_samples + (pos % width)) : pos;
            output[idx0] = int16_t(int(e.value0) * quant);
        }
        if (e.value1) {
            size_t idx1 = pitched ? (((pos + 1) / width) * pitch_samples + ((pos + 1) % width)) : (pos + 1);
            output[idx1] = int16_t(int(e.value1) * quant);
        }
        pos += post;
        state = e.next_state;
    }
    if (c + 1 < count && (pos != cp[c + 1].position || state != cp[c + 1].state)) {
        if (error) atomicExch(error, 3);
    }
}

__global__ void HybridDifferenceKernel(int16_t* __restrict__ p, int w, int h, size_t pitch_samples) {
    unsigned y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= unsigned(h)) return;
    int16_t* line = p + y * pitch_samples;
    for (int x = 1; x < w; ++x) {
        line[x] = int16_t(int(line[x]) + line[x - 1]);
    }
}

} // anonymous namespace

cudaError_t LaunchHybridSummariesGpu(
    const uint8_t* d_payload,
    uint32_t bytes,
    const GpuFsmEntry* d_table,
    unsigned states,
    uint32_t chunk,
    uint32_t count,
    Summary* d_out,
    cudaStream_t stream
) {
    if (!d_payload || !d_table || !d_out || count == 0 || chunk == 0) {
        return cudaErrorInvalidValue;
    }
    unsigned block = 128;
    unsigned grid = (count + block - 1) / block;
    HybridSummariesKernel<<<grid, block, 0, stream>>>(d_payload, bytes, d_table, states, chunk, count, d_out);
    return cudaGetLastError();
}

cudaError_t LaunchHybridReplayGpu(
    const uint8_t* d_payload,
    const GpuFsmEntry* d_table,
    const Restart* d_checkpoints,
    uint32_t checkpoint_count,
    uint32_t marker_nibble,
    uint32_t cap,
    int quant,
    int16_t* d_output,
    uint32_t* d_error,
    cudaStream_t stream,
    int width,
    size_t pitch_samples
) {
    if (!d_payload || !d_table || !d_checkpoints || !d_output || checkpoint_count == 0) {
        return cudaErrorInvalidValue;
    }
    unsigned block = 128;
    unsigned grid = (checkpoint_count + block - 1) / block;
    HybridReplayKernel<<<grid, block, 0, stream>>>(
        d_payload, d_table, d_checkpoints, checkpoint_count, marker_nibble, cap, quant, d_output, d_error,
        width, pitch_samples
    );
    return cudaGetLastError();
}

cudaError_t LaunchHybridDifferenceGpu(
    int16_t* d_output,
    int width,
    int height,
    size_t pitch_samples,
    cudaStream_t stream
) {
    if (!d_output || width <= 0 || height <= 0 || pitch_samples < size_t(width)) {
        return cudaErrorInvalidValue;
    }
    unsigned block = 128;
    unsigned grid = (unsigned(height) + block - 1) / block;
    HybridDifferenceKernel<<<grid, block, 0, stream>>>(d_output, width, height, pitch_samples);
    return cudaGetLastError();
}

} // namespace HybridEntropy
} // namespace HyperCine
