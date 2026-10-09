// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../core/decoder_backend.h"
#include "decode_error_guard.cuh"
namespace HyperCine {
namespace {
// Upstream DecodeBandFSM16sNoGapWithPeaks replaces dequantized escape values
// in coefficient order BEFORE horizontal difference reconstruction. This is
// a bounded metadata correction; the existing entropy algorithms are unchanged.
__global__ void RestorePeaks(const uint8_t* sample, size_t size,
    const GpuEntropyBandDescriptor* descriptors, int bands, uint32_t* frame_error) {
    if (DecoderFrameFailed(frame_error)) return;
    const int b = int(blockIdx.x);
    if (b >= bands || threadIdx.x != 0) return;
    const auto d = descriptors[b];
    if (!d.has_peaks || d.peak_level <= 0) return;
    uint32_t error = 0;
    if (!d.d_destination || d.peak_count <= 0 || d.peak_offset > size ||
        size_t(d.peak_count) * 2 > size - d.peak_offset) error = uint32_t(GpuBandDecodeStatus::InvalidDescriptor);
    int used = 0;
    if (!error) {
        for (size_t i = 0; i < d.coefficient_count; ++i) {
            const int value = d.d_destination[i];
            if (value > d.peak_level || value < -d.peak_level) {
                if (used >= d.peak_count) { error = uint32_t(GpuBandDecodeStatus::InputTruncated); break; }
                const auto p = sample + d.peak_offset + size_t(used++) * 2;
                d.d_destination[i] = int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
            }
        }
        if (!error && used != d.peak_count) {
            const auto p = sample + d.peak_offset + size_t(used) * 2;
            if (used + 1 != d.peak_count || p[0] || p[1]) error = uint32_t(GpuBandDecodeStatus::InvalidDescriptor);
        }
    }
    if (error && frame_error) atomicCAS(frame_error, 0u, error);
}
}
cudaError_t LaunchRestoreDecodePeaks(const uint8_t* sample,size_t size,
    const GpuEntropyBandDescriptor* descriptors,int bands,cudaStream_t stream,uint32_t* frame_error) {
    if (!sample || !descriptors || bands <= 0) return cudaErrorInvalidValue;
    RestorePeaks<<<bands,32,0,stream>>>(sample,size,descriptors,bands,frame_error);
    return cudaGetLastError();
}
} // namespace HyperCine
