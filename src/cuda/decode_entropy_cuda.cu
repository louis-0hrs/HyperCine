// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_entropy_cuda.h"
#include "../core/decoder_backend.h"
#include "hypercine/decode_gpu_index.h"
#include <cuda_runtime.h>
#include "decode_error_guard.cuh"
#include <device_launch_parameters.h>
#include <vector>
#include <stdexcept>
#include <iostream>

namespace HyperCine {

// External declaration from decode_fsm_tables.cpp
bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>& out_tables);

constexpr size_t CS9_NUM_STATES = 136;
constexpr size_t CS17_NUM_STATES = 518;
constexpr size_t CS18_NUM_STATES = 518;
constexpr size_t CS9_ENTRIES = CS9_NUM_STATES * 16;   // 2176
constexpr size_t CS17_ENTRIES = CS17_NUM_STATES * 16; // 8288
constexpr size_t CS18_ENTRIES = CS18_NUM_STATES * 16; // 8288
constexpr size_t TOTAL_FSM_ENTRIES = CS9_ENTRIES + CS17_ENTRIES + CS18_ENTRIES; // 18752

constexpr size_t CS9_BASE_OFFSET = 0;
constexpr size_t CS17_BASE_OFFSET = CS9_ENTRIES;
constexpr size_t CS18_BASE_OFFSET = CS9_ENTRIES + CS17_ENTRIES;

constexpr int16_t BAND_END_MARKER = 32767;

// ============================================================================
// GpuFsmTableManager Implementation
// ============================================================================

struct GpuFsmTableManager::Impl {
    GpuFsmEntry* d_fsm_tables = nullptr;
    bool initialized = false;
    int cuda_device = 0;
};

GpuFsmTableManager::GpuFsmTableManager() : impl_(std::make_unique<Impl>()) {}

GpuFsmTableManager::~GpuFsmTableManager() {
    Release();
}

bool GpuFsmTableManager::Initialize(int cuda_device) {
    if (impl_->initialized) {
        return true;
    }

    std::vector<GpuFsmEntry> host_tables;
    if (!ExtractCineFormBaseFsmTables(host_tables)) {
        return false;
    }

    if (host_tables.size() != TOTAL_FSM_ENTRIES) {
        return false;
    }

    impl_->cuda_device = cuda_device;
    cudaError_t err = cudaSetDevice(cuda_device);
    if (err != cudaSuccess) return false;

    size_t alloc_bytes = TOTAL_FSM_ENTRIES * sizeof(GpuFsmEntry);
    err = cudaMalloc(&impl_->d_fsm_tables, alloc_bytes);
    if (err != cudaSuccess) {
        impl_->d_fsm_tables = nullptr;
        return false;
    }

    err = cudaMemcpy(impl_->d_fsm_tables, host_tables.data(), alloc_bytes, cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        cudaFree(impl_->d_fsm_tables);
        impl_->d_fsm_tables = nullptr;
        return false;
    }

    impl_->initialized = true;
    return true;
}

void GpuFsmTableManager::Release() {
    if (impl_ && impl_->d_fsm_tables) {
        cudaSetDevice(impl_->cuda_device);
        cudaFree(impl_->d_fsm_tables);
        impl_->d_fsm_tables = nullptr;
        impl_->initialized = false;
    }
}

bool GpuFsmTableManager::IsInitialized() const {
    return impl_ && impl_->initialized;
}

const GpuFsmEntry* GpuFsmTableManager::GetDeviceTable() const {
    return impl_ ? impl_->d_fsm_tables : nullptr;
}

size_t GpuFsmTableManager::GetCodebookOffset(int codebook_id) const {
    if (codebook_id == 1) return CS17_BASE_OFFSET;
    if (codebook_id == 2) return CS18_BASE_OFFSET;
    return CS9_BASE_OFFSET;
}

// ============================================================================
// CUDA Kernels
// ============================================================================

__global__ void DecodeLowpassBandsCudaKernel(
    const uint8_t* __restrict__ d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* __restrict__ d_descriptors,
    int num_ll_bands
) {
    int band_idx = blockIdx.x;
    if (band_idx >= num_ll_bands) return;

    const GpuEntropyBandDescriptor desc = d_descriptors[band_idx];
    if (!desc.d_destination || desc.payload_offset >= compressed_size) return;

    const uint8_t* src = d_compressed_sample + desc.payload_offset;
    int16_t* dst = desc.d_destination;

    int width = desc.width;
    int height = desc.height;
    int pitch_samples = static_cast<int>(desc.pitch_samples);
    int total_samples = width * height;
    int channel_offset = desc.channel_offset;

    // Check for solid color lowpass
    int32_t solid_color = -1;
    if (desc.payload_bytes >= 16) {
        uint32_t first_word = (static_cast<uint32_t>(src[0]) << 24) |
                              (static_cast<uint32_t>(src[1]) << 16) |
                              (static_cast<uint32_t>(src[2]) << 8) |
                              static_cast<uint32_t>(src[3]);
        if (first_word == 0xFFFFFFFFu) {
            uint32_t solid_val = (static_cast<uint32_t>(src[4]) << 24) |
                                 (static_cast<uint32_t>(src[5]) << 16) |
                                 (static_cast<uint32_t>(src[6]) << 8) |
                                 static_cast<uint32_t>(src[7]);
            uint32_t solid_w = (static_cast<uint32_t>(src[8]) << 24) |
                               (static_cast<uint32_t>(src[9]) << 16) |
                               (static_cast<uint32_t>(src[10]) << 8) |
                               static_cast<uint32_t>(src[11]);
            uint32_t solid_h = (static_cast<uint32_t>(src[12]) << 24) |
                               (static_cast<uint32_t>(src[13]) << 16) |
                               (static_cast<uint32_t>(src[14]) << 8) |
                               static_cast<uint32_t>(src[15]);
            if (solid_w == static_cast<uint32_t>(width) && solid_h == static_cast<uint32_t>(height)) {
                solid_color = static_cast<int16_t>(solid_val & 0xFFFF);
            }
        }
    }

    // Cooperative clear of entire allocated band area
    for (int i = threadIdx.x; i < static_cast<int>(desc.coefficient_count); i += blockDim.x) {
        dst[i] = 0;
    }
    __syncthreads();

    if (solid_color != -1) {
        int32_t px = solid_color + channel_offset;
        if (px > 32767) px = 32767;
        if (px < -32768) px = -32768;
        int16_t s_px = static_cast<int16_t>(px);

        for (int idx = threadIdx.x; idx < total_samples; idx += blockDim.x) {
            int y = idx / width;
            int x = idx % width;
            dst[y * pitch_samples + x] = s_px;
        }
        return;
    }

    // Decode 16-bit big-endian words
    for (int idx = threadIdx.x; idx < total_samples; idx += blockDim.x) {
        int y = idx / width;
        int x = idx % width;

        size_t byte_pos = static_cast<size_t>(idx) * 2;
        if (byte_pos + 1 < desc.payload_bytes) {
            uint16_t be_val = (static_cast<uint16_t>(src[byte_pos]) << 8) |
                               static_cast<uint16_t>(src[byte_pos + 1]);
            int32_t px = static_cast<int16_t>(be_val) + channel_offset;
            if (px > 32767) px = 32767;
            if (px < -32768) px = -32768;
            dst[y * pitch_samples + x] = static_cast<int16_t>(px);
        }
    }
}

__global__ void DecodeHighpassVlcBandsCudaKernel(
    const uint8_t* __restrict__ d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* __restrict__ d_descriptors,
    int num_vlc_bands,
    const GpuFsmEntry* __restrict__ d_fsm_tables,
    GpuBandDecodeResult* __restrict__ d_results
) {
    int band_idx = blockIdx.x;
    if (band_idx >= num_vlc_bands) return;

    const GpuEntropyBandDescriptor desc = d_descriptors[band_idx];
    if (!desc.d_destination) return;

    int16_t* dst = desc.d_destination;
    int total_capacity = static_cast<int>(desc.coefficient_count);

    // 1. Cooperative clear: all threads zero out coefficient memory
    for (int i = threadIdx.x; i < total_capacity; i += blockDim.x) {
        dst[i] = 0;
    }
    __syncthreads();

    // Shared state for block synchronization
    __shared__ GpuBandDecodeResult s_result;

    if (threadIdx.x == 0) {
        uint64_t start_clk = clock64();

        GpuBandDecodeStatus status = GpuBandDecodeStatus::Complete;
        uint32_t consumed_bytes = 0;
        uint32_t out_pos = 0;
        uint32_t nonzero_count = 0;
        uint16_t state = desc.initial_fsm_state;
        bool terminated = false;

        size_t codebook_base = (desc.codebook_id == 1) ? CS17_BASE_OFFSET :
                              ((desc.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);

        int quant = desc.quantization;

        if (desc.payload_offset + desc.payload_bytes > compressed_size) {
            status = GpuBandDecodeStatus::InputTruncated;
        } else {
            const uint8_t* src = d_compressed_sample + desc.payload_offset;
            uint32_t max_bytes = desc.payload_bytes;

            while (consumed_bytes < max_bytes && !terminated) {
                uint8_t byte = src[consumed_bytes++];

                // Nibble 0 (high 4 bits)
                uint8_t nibble0 = byte >> 4;
                size_t idx0 = codebook_base + (state * 16 + nibble0);
                GpuFsmEntry e0 = d_fsm_tables[idx0];

                if (e0.value0 == BAND_END_MARKER) {
                    terminated = true;
                    break;
                }

                uint32_t pre0 = e0.pre_post_skip & 0x1ff;
                uint32_t post0 = (e0.pre_post_skip >> 12) & 0x7;
                uint32_t write_pos0 = out_pos + pre0;

                if ((e0.value0 != 0 && write_pos0 >= desc.coefficient_count) ||
                    (e0.value1 != 0 && (write_pos0 + 1) >= desc.coefficient_count) ||
                    (write_pos0 + post0 > desc.coefficient_count + 8)) {
                    status = GpuBandDecodeStatus::CoefficientOverflow;
                    break;
                }

                out_pos += pre0;
                if (e0.value0 != 0) {
                    dst[out_pos] = static_cast<int16_t>(e0.value0 * quant);
                    nonzero_count++;
                }
                if (e0.value1 != 0) {
                    dst[out_pos + 1] = static_cast<int16_t>(e0.value1 * quant);
                    nonzero_count++;
                }
                out_pos += post0;
                state = e0.next_state;

                // Nibble 1 (low 4 bits)
                uint8_t nibble1 = byte & 0x0f;
                size_t idx1 = codebook_base + (state * 16 + nibble1);
                GpuFsmEntry e1 = d_fsm_tables[idx1];

                if (e1.value0 == BAND_END_MARKER) {
                    terminated = true;
                    break;
                }

                uint32_t pre1 = e1.pre_post_skip & 0x1ff;
                uint32_t post1 = (e1.pre_post_skip >> 12) & 0x7;
                uint32_t write_pos1 = out_pos + pre1;

                if ((e1.value0 != 0 && write_pos1 >= desc.coefficient_count) ||
                    (e1.value1 != 0 && (write_pos1 + 1) >= desc.coefficient_count) ||
                    (write_pos1 + post1 > desc.coefficient_count + 8)) {
                    status = GpuBandDecodeStatus::CoefficientOverflow;
                    break;
                }

                out_pos += pre1;
                if (e1.value0 != 0) {
                    dst[out_pos] = static_cast<int16_t>(e1.value0 * quant);
                    nonzero_count++;
                }
                if (e1.value1 != 0) {
                    dst[out_pos + 1] = static_cast<int16_t>(e1.value1 * quant);
                    nonzero_count++;
                }
                out_pos += post1;
                state = e1.next_state;
            }

            if (!terminated && status == GpuBandDecodeStatus::Complete) {
                status = GpuBandDecodeStatus::MissingEndMarker;
            }
        }

        uint64_t end_clk = clock64();

        s_result.status = status;
        s_result.consumed_bytes = consumed_bytes;
        s_result.final_fsm_state = state;
        s_result.final_coeff_pos = out_pos;
        s_result.nonzero_count = nonzero_count;
        s_result.duration_cycles = end_clk - start_clk;
    }
    __syncthreads();

    // 2. Cooperative difference coding (if enabled)
    if (desc.difference_coding && s_result.status == GpuBandDecodeStatus::Complete) {
        int width = desc.width;
        int height = desc.height;
        int pitch_samples = static_cast<int>(desc.pitch_samples);

        for (int y = threadIdx.x; y < height; y += blockDim.x) {
            int16_t* line = dst + y * pitch_samples;
            for (int x = 1; x < width; ++x) {
                line[x] += line[x - 1];
            }
        }
    }

    if (threadIdx.x == 0 && d_results) {
        d_results[band_idx] = s_result;
    }
}

__global__ void DecodeEntropyDiagnosticCudaKernel(
    const uint8_t* __restrict__ d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* __restrict__ d_desc,
    const GpuFsmEntry* __restrict__ d_fsm_tables,
    DecodedSymbolDiagnostic* __restrict__ d_symbols,
    int max_symbols,
    int* __restrict__ d_actual_symbols,
    GpuBandDecodeResult* __restrict__ d_result
) {
    if (threadIdx.x != 0 || blockIdx.x != 0) return;

    const GpuEntropyBandDescriptor desc = *d_desc;
    size_t codebook_base = (desc.codebook_id == 1) ? CS17_BASE_OFFSET :
                          ((desc.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);

    uint32_t consumed_bytes = 0;
    uint32_t out_pos = 0;
    uint32_t nonzero_count = 0;
    uint16_t state = desc.initial_fsm_state;
    int sym_count = 0;
    bool terminated = false;
    GpuBandDecodeStatus status = GpuBandDecodeStatus::Complete;

    const uint8_t* src = d_compressed_sample + desc.payload_offset;
    uint32_t max_bytes = desc.payload_bytes;

    while (consumed_bytes < max_bytes && !terminated) {
        uint8_t byte = src[consumed_bytes++];

        // Nibble 0
        uint8_t nibble0 = byte >> 4;
        size_t idx0 = codebook_base + (state * 16 + nibble0);
        GpuFsmEntry e0 = d_fsm_tables[idx0];

        if (e0.value0 == BAND_END_MARKER) {
            terminated = true;
            break;
        }

        uint32_t pre0 = e0.pre_post_skip & 0x1ff;
        uint32_t post0 = (e0.pre_post_skip >> 12) & 0x7;

        if (sym_count < max_symbols && d_symbols) {
            DecodedSymbolDiagnostic diag{};
            diag.nibble_index = static_cast<uint16_t>((consumed_bytes - 1) * 2);
            diag.state_before = state;
            diag.state_after = e0.next_state;
            diag.pre_skip = static_cast<uint16_t>(pre0);
            diag.post_skip = static_cast<uint16_t>(post0);
            diag.raw_val0 = e0.value0;
            diag.raw_val1 = e0.value1;
            diag.out_pos = out_pos + pre0;
            d_symbols[sym_count++] = diag;
        }

        out_pos += pre0;
        if (e0.value0 != 0) nonzero_count++;
        if (e0.value1 != 0) nonzero_count++;
        out_pos += post0;
        state = e0.next_state;

        // Nibble 1
        uint8_t nibble1 = byte & 0x0f;
        size_t idx1 = codebook_base + (state * 16 + nibble1);
        GpuFsmEntry e1 = d_fsm_tables[idx1];

        if (e1.value0 == BAND_END_MARKER) {
            terminated = true;
            break;
        }

        uint32_t pre1 = e1.pre_post_skip & 0x1ff;
        uint32_t post1 = (e1.pre_post_skip >> 12) & 0x7;

        if (sym_count < max_symbols && d_symbols) {
            DecodedSymbolDiagnostic diag{};
            diag.nibble_index = static_cast<uint16_t>((consumed_bytes - 1) * 2 + 1);
            diag.state_before = state;
            diag.state_after = e1.next_state;
            diag.pre_skip = static_cast<uint16_t>(pre1);
            diag.post_skip = static_cast<uint16_t>(post1);
            diag.raw_val0 = e1.value0;
            diag.raw_val1 = e1.value1;
            diag.out_pos = out_pos + pre1;
            d_symbols[sym_count++] = diag;
        }

        out_pos += pre1;
        if (e1.value0 != 0) nonzero_count++;
        if (e1.value1 != 0) nonzero_count++;
        out_pos += post1;
        state = e1.next_state;
    }

    if (d_actual_symbols) *d_actual_symbols = sym_count;
    if (d_result) {
        d_result->status = (!terminated && status == GpuBandDecodeStatus::Complete) ?
                            GpuBandDecodeStatus::MissingEndMarker : status;
        d_result->consumed_bytes = consumed_bytes;
        d_result->final_fsm_state = state;
        d_result->final_coeff_pos = out_pos;
        d_result->nonzero_count = nonzero_count;
        d_result->duration_cycles = 0;
    }
}

// ============================================================================
// Launch APIs
// ============================================================================

cudaError_t LaunchDecodeLowpassGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_descriptors,
    int num_ll_bands,
    cudaStream_t stream
) {
    if (!d_compressed_sample || !d_descriptors || num_ll_bands <= 0) {
        return cudaErrorInvalidValue;
    }

    dim3 grid(num_ll_bands);
    dim3 block(128);

    DecodeLowpassBandsCudaKernel<<<grid, block, 0, stream>>>(
        d_compressed_sample,
        compressed_size,
        d_descriptors,
        num_ll_bands
    );

    return cudaGetLastError();
}

cudaError_t LaunchDecodeEntropyGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_descriptors,
    int num_vlc_bands,
    const GpuFsmEntry* d_fsm_tables,
    GpuBandDecodeResult* d_results,
    cudaStream_t stream
) {
    if (!d_compressed_sample || !d_descriptors || num_vlc_bands <= 0 || !d_fsm_tables) {
        return cudaErrorInvalidValue;
    }

    dim3 grid(num_vlc_bands);
    dim3 block(128);

    DecodeHighpassVlcBandsCudaKernel<<<grid, block, 0, stream>>>(
        d_compressed_sample,
        compressed_size,
        d_descriptors,
        num_vlc_bands,
        d_fsm_tables,
        d_results
    );

    return cudaGetLastError();
}

cudaError_t LaunchDecodeEntropyDiagnosticGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_desc,
    const GpuFsmEntry* d_fsm_tables,
    DecodedSymbolDiagnostic* d_symbols,
    int max_symbols,
    int* d_actual_symbols,
    GpuBandDecodeResult* d_result,
    cudaStream_t stream
) {
    if (!d_compressed_sample || !d_desc || !d_fsm_tables) {
        return cudaErrorInvalidValue;
    }

    DecodeEntropyDiagnosticCudaKernel<<<1, 1, 0, stream>>>(
        d_compressed_sample,
        compressed_size,
        d_desc,
        d_fsm_tables,
        d_symbols,
        max_symbols,
        d_actual_symbols,
        d_result
    );

    return cudaGetLastError();
}

// ============================================================================
// Phase 2C Intra-Band Parallel Kernels & CPU Indexing (Rules 5, 10, 14, 15)
// ============================================================================

int DetermineAdaptiveSegmentCount(size_t payload_bytes, size_t coeff_count) {
    (void)coeff_count;
    // Rule 15: Small-band fallback (payload < 512 bytes executes as 1 segment)
    if (payload_bytes < 512) return 1;
    if (payload_bytes < 4096) return 4;
    if (payload_bytes < 32768) return 8;
    if (payload_bytes < 131072) return 16;
    if (payload_bytes < 524288) return 32;
    // Rule 14 & 20: High-entropy dense noise / large bands
    return 64;
}

static bool GenerateCheckpoints(
    const uint8_t* compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor& desc,
    int band_index,
    const GpuFsmEntry* fsm_tables,
    size_t fsm_tables_size,
    GpuEntropyMode mode,
    int fixed_segments_per_band,
    std::vector<GpuEntropySegmentDescriptor>& out_segments,
    std::string* out_error,
    bool strict
) {
    out_segments.clear();
    if (!compressed_sample || !fsm_tables || fsm_tables_size == 0) {
        if (out_error) *out_error = "null sample or invalid fsm tables";
        return false;
    }

    if (desc.payload_offset >= compressed_size) {
        if (out_error) *out_error = "payload offset exceeds compressed buffer size";
        return false;
    }

    uint32_t payload_bytes = desc.payload_bytes;
    if (desc.payload_offset + payload_bytes > compressed_size) {
        if (out_error) *out_error = "payload is truncated within compressed buffer";
        return false;
    }

    if (desc.codebook_id < 0 || desc.codebook_id > 2) {
        if (out_error) *out_error = "invalid codebook id";
        return false;
    }

    if (desc.coefficient_count == 0) {
        if (out_error) *out_error = "invalid descriptor: zero coefficient count";
        return false;
    }

    int target_segs = 1;
    if (mode == GpuEntropyMode::ParallelAdaptive) {
        target_segs = DetermineAdaptiveSegmentCount(payload_bytes, desc.coefficient_count);
    } else if (mode == GpuEntropyMode::ParallelConfigurable) {
        target_segs = (fixed_segments_per_band > 0) ? fixed_segments_per_band : 1;
    } else {
        target_segs = 1;
    }

    if (!strict && (target_segs <= 1 || payload_bytes == 0)) {
        GpuEntropySegmentDescriptor seg{};
        seg.band_index = band_index;
        seg.byte_offset = 0;
        seg.nibble_idx = 0;
        seg.initial_state = desc.initial_fsm_state;
        seg.out_coeff_pos = 0;
        seg.end_byte_offset = payload_bytes;
        seg.end_nibble_idx = 0;
        seg.max_coeff_pos = static_cast<uint32_t>(desc.coefficient_count);
        out_segments.push_back(seg);
        return true;
    }

    size_t codebook_base = (desc.codebook_id == 1) ? CS17_BASE_OFFSET :
                          ((desc.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);

    const uint8_t* src = compressed_sample + desc.payload_offset;
    uint32_t target_nibbles_per_seg = (payload_bytes * 2 + target_segs - 1) / target_segs;
    if (target_nibbles_per_seg < 16) target_nibbles_per_seg = 16;

    GpuEntropySegmentDescriptor initial_seg{};
    initial_seg.band_index = band_index;
    initial_seg.byte_offset = 0;
    initial_seg.nibble_idx = 0;
    initial_seg.initial_state = desc.initial_fsm_state;
    initial_seg.out_coeff_pos = 0;
    initial_seg.max_coeff_pos = static_cast<uint32_t>(desc.coefficient_count);
    out_segments.push_back(initial_seg);

    uint32_t byte_idx = 0;
    uint32_t out_pos = 0;
    uint16_t state = desc.initial_fsm_state;
    uint32_t seg_start_nibble = 0;
    uint32_t curr_nibble_global = 0;
    int current_seg_count = 1;
    bool terminated = false;

    while (byte_idx < payload_bytes && !terminated) {
        uint8_t byte = src[byte_idx];

        for (int nib_in_byte = 0; nib_in_byte < 2; ++nib_in_byte) {
            uint8_t nibble = (nib_in_byte == 0) ? (byte >> 4) : (byte & 0x0f);
            size_t entry_idx = codebook_base + state * 16 + nibble;
            if (entry_idx >= fsm_tables_size) {
                if (out_error) *out_error = "fsm table index out of bounds";
                return false;
            }
            const auto& entry = fsm_tables[entry_idx];

            if (entry.value0 == BAND_END_MARKER) {
                terminated = true;
                out_segments.back().end_byte_offset = byte_idx;
                out_segments.back().end_nibble_idx = static_cast<uint8_t>(nib_in_byte);
                break;
            }

            uint32_t pre = entry.pre_post_skip & 0x1ff;
            uint32_t post = (entry.pre_post_skip >> 12) & 0x7;
            out_pos += pre + post;
            if (out_pos > desc.coefficient_count + 8) {
                if (out_error) *out_error = "coefficient overflow during indexing";
                return false;
            }

            state = entry.next_state;
            curr_nibble_global++;

            if ((curr_nibble_global - seg_start_nibble) >= target_nibbles_per_seg &&
                current_seg_count < target_segs) {
                uint32_t next_byte = (nib_in_byte == 0) ? byte_idx : (byte_idx + 1);
                uint8_t next_nib = (nib_in_byte == 0) ? 1 : 0;

                out_segments.back().end_byte_offset = next_byte;
                out_segments.back().end_nibble_idx = next_nib;

                GpuEntropySegmentDescriptor next_seg{};
                next_seg.band_index = band_index;
                next_seg.byte_offset = next_byte;
                next_seg.nibble_idx = next_nib;
                next_seg.initial_state = state;
                next_seg.out_coeff_pos = out_pos;
                next_seg.max_coeff_pos = static_cast<uint32_t>(desc.coefficient_count);
                out_segments.push_back(next_seg);

                seg_start_nibble = curr_nibble_global;
                current_seg_count++;
            }
        }
        byte_idx++;
    }

    if (!terminated) {
        if (strict) {
            if (out_error) *out_error = "missing end marker / truncated entropy";
            return false;
        }
        out_segments.back().end_byte_offset = payload_bytes;
        out_segments.back().end_nibble_idx = 0;
    }

    return true;
}

bool GenerateSingleBandCheckpointsCpu(const uint8_t* p, size_t n,
    const GpuEntropyBandDescriptor& d, int b, const GpuFsmEntry* t, size_t count,
    GpuEntropyMode mode, int segments, std::vector<GpuEntropySegmentDescriptor>& out, std::string* error) {
    return GenerateCheckpoints(p,n,d,b,t,count,mode,segments,out,error,false);
}
bool GenerateSingleBandCheckpointsValidated(const uint8_t* p, size_t n,
    const GpuEntropyBandDescriptor& d, int b, const GpuFsmEntry* t, size_t count,
    GpuEntropyMode mode, int segments, std::vector<GpuEntropySegmentDescriptor>& out, std::string* error) {
    return GenerateCheckpoints(p,n,d,b,t,count,mode,segments,out,error,true);
}

bool GenerateBandCheckpointsCpu(
    const uint8_t* compressed_sample,
    size_t compressed_size,
    const std::vector<GpuEntropyBandDescriptor>& vlc_bands,
    const std::vector<GpuFsmEntry>& fsm_tables,
    GpuEntropyMode mode,
    int fixed_segments_per_band,
    std::vector<GpuEntropySegmentDescriptor>& out_segments
) {
    out_segments.clear();
    if (!compressed_sample || vlc_bands.empty() || fsm_tables.empty()) {
        return false;
    }

    std::vector<GpuEntropySegmentDescriptor> band_segs;
    for (size_t b = 0; b < vlc_bands.size(); ++b) {
        const auto& desc = vlc_bands[b];
        if (desc.payload_offset >= compressed_size) {
            continue;
        }
        band_segs.clear();
        if (GenerateSingleBandCheckpointsCpu(
                compressed_sample,
                compressed_size,
                desc,
                static_cast<int>(b),
                fsm_tables.data(),
                fsm_tables.size(),
                mode,
                fixed_segments_per_band,
                band_segs,
                nullptr
            )) {
            out_segments.insert(out_segments.end(), band_segs.begin(), band_segs.end());
        }
    }

    return true;
}

__global__ void ZeroAllVlcBandsKernel(
    const GpuEntropyBandDescriptor* __restrict__ d_descriptors,
    int num_vlc_bands
) {
    int band_idx = blockIdx.x;
    if (band_idx >= num_vlc_bands) return;

    const GpuEntropyBandDescriptor desc = d_descriptors[band_idx];
    if (!desc.d_destination) return;

    int16_t* dst = desc.d_destination;
    size_t total_capacity = desc.coefficient_count;

    for (size_t i = threadIdx.x; i < total_capacity; i += blockDim.x) {
        dst[i] = 0;
    }
}

__global__ void DecodeHighpassVlcParallelCudaKernel(
    const uint8_t* __restrict__ d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* __restrict__ d_band_descriptors,
    int num_vlc_bands,
    const GpuEntropySegmentDescriptor* __restrict__ d_segments,
    int num_segments,
    const GpuFsmEntry* __restrict__ d_fsm_tables,
    GpuSegmentDecodeResult* __restrict__ d_seg_results,
    const GpuCheckpointIndexResult* d_index_result,
    uint32_t* d_frame_error
) {
    int seg_idx = blockIdx.x;
    if (d_index_result && (!d_index_result->finished ||
        d_index_result->status != GpuBandDecodeStatus::Complete ||
        seg_idx >= d_index_result->segment_count)) return;
    if (DecoderFrameFailed(d_frame_error)) return;
    if (seg_idx >= num_segments) return;

    const GpuEntropySegmentDescriptor seg = d_segments[seg_idx];
    if (seg.band_index < 0 || seg.band_index >= num_vlc_bands) {
        if (threadIdx.x == 0 && d_seg_results) {
            d_seg_results[seg_idx].status = GpuBandDecodeStatus::InvalidDescriptor;
        }
        return;
    }

    const GpuEntropyBandDescriptor desc = d_band_descriptors[seg.band_index];
    if (!desc.d_destination) {
        if (threadIdx.x == 0 && d_seg_results) {
            d_seg_results[seg_idx].status = GpuBandDecodeStatus::InvalidDescriptor;
        }
        return;
    }

    int16_t* dst = desc.d_destination;
    size_t coeff_capacity = desc.coefficient_count;
    int quant = desc.quantization;

    size_t codebook_base = (desc.codebook_id == 1) ? CS17_BASE_OFFSET :
                          ((desc.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);

    if (threadIdx.x == 0) {
        uint64_t start_clk = clock64();
        GpuBandDecodeStatus status = GpuBandDecodeStatus::Complete;
        uint32_t nonzero_count = 0;
        uint32_t byte_idx = seg.byte_offset;
        uint16_t state = seg.initial_state;
        uint32_t out_pos = seg.out_coeff_pos;

        if (desc.payload_offset + seg.end_byte_offset > compressed_size) {
            status = GpuBandDecodeStatus::InputTruncated;
        } else {
            const uint8_t* src = d_compressed_sample + desc.payload_offset;

            uint8_t nib_idx = seg.nibble_idx;
            uint32_t end_byte = seg.end_byte_offset;
            uint8_t end_nib = seg.end_nibble_idx;

            while (true) {
                if (byte_idx > end_byte || (byte_idx == end_byte && nib_idx >= end_nib)) {
                    break;
                }

                uint8_t byte = src[byte_idx];
                uint8_t nibble = (nib_idx == 0) ? (byte >> 4) : (byte & 0x0f);
                size_t entry_idx = codebook_base + state * 16 + nibble;
                GpuFsmEntry entry = d_fsm_tables[entry_idx];

                if (entry.value0 == BAND_END_MARKER) {
                    break;
                }

                uint32_t pre = entry.pre_post_skip & 0x1ff;
                uint32_t post = (entry.pre_post_skip >> 12) & 0x7;
                uint32_t write_pos = out_pos + pre;

                if ((entry.value0 != 0 && write_pos >= coeff_capacity) ||
                    (entry.value1 != 0 && (write_pos + 1) >= coeff_capacity) ||
                    (write_pos + post > coeff_capacity + 8)) {
                    status = GpuBandDecodeStatus::CoefficientOverflow;
                    break;
                }

                out_pos += pre;
                if (entry.value0 != 0) {
                    dst[out_pos] = static_cast<int16_t>(entry.value0 * quant);
                    nonzero_count++;
                }
                if (entry.value1 != 0) {
                    dst[out_pos + 1] = static_cast<int16_t>(entry.value1 * quant);
                    nonzero_count++;
                }
                out_pos += post;
                state = entry.next_state;

                if (nib_idx == 0) {
                    nib_idx = 1;
                } else {
                    nib_idx = 0;
                    byte_idx++;
                }
            }
        }

        uint64_t end_clk = clock64();
        if (d_frame_error && status != GpuBandDecodeStatus::Complete)
            atomicCAS(d_frame_error, 0u, static_cast<uint32_t>(status));

        if (d_seg_results) {
            d_seg_results[seg_idx].status = status;
            d_seg_results[seg_idx].consumed_bytes = (status == GpuBandDecodeStatus::InputTruncated) ? 0 : (byte_idx - seg.byte_offset);
            d_seg_results[seg_idx].final_fsm_state = state;
            d_seg_results[seg_idx].final_coeff_pos = out_pos;
            d_seg_results[seg_idx].nonzero_count = nonzero_count;
            d_seg_results[seg_idx].duration_cycles = end_clk - start_clk;
        }
    }
}

__global__ void CooperativeDifferenceCodingAllBandsKernel(
    const GpuEntropyBandDescriptor* __restrict__ d_descriptors,
    int num_vlc_bands, uint32_t* d_frame_error
) {
    int band_idx = blockIdx.x;
    if (band_idx >= num_vlc_bands) return;

    const GpuEntropyBandDescriptor desc = d_descriptors[band_idx];
    if (DecoderFrameFailed(d_frame_error)) return;
    if (!desc.difference_coding || !desc.d_destination) return;

    int width = desc.width;
    int height = desc.height;
    size_t pitch_samples = desc.pitch_samples;
    int16_t* dst = desc.d_destination;

    for (int y = threadIdx.x; y < height; y += blockDim.x) {
        int16_t* line = dst + y * pitch_samples;
        for (int x = 1; x < width; ++x) {
            line[x] += line[x - 1];
        }
    }
}

__global__ void DecodeEntropyParallelDiagnosticCudaKernel(
    const uint8_t* __restrict__ d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* __restrict__ d_band_desc,
    const GpuEntropySegmentDescriptor* __restrict__ d_segments,
    int num_segments,
    const GpuFsmEntry* __restrict__ d_fsm_tables,
    GpuSegmentDiagnostic* __restrict__ d_diagnostics
) {
    int seg_idx = blockIdx.x;
    if (seg_idx >= num_segments || threadIdx.x != 0) return;

    const GpuEntropySegmentDescriptor seg = d_segments[seg_idx];
    const GpuEntropyBandDescriptor desc = *d_band_desc;

    size_t codebook_base = (desc.codebook_id == 1) ? CS17_BASE_OFFSET :
                          ((desc.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);

    GpuSegmentDiagnostic diag{};
    diag.start_byte = seg.byte_offset;
    diag.start_nibble = seg.nibble_idx;
    diag.start_state = seg.initial_state;
    diag.start_out_pos = seg.out_coeff_pos;

    if (desc.payload_offset + seg.end_byte_offset > compressed_size) {
        diag.termination_status = GpuBandDecodeStatus::InputTruncated;
    } else {
        const uint8_t* src = d_compressed_sample + desc.payload_offset;
        uint32_t byte_idx = seg.byte_offset;
        uint8_t nib_idx = seg.nibble_idx;
        uint32_t end_byte = seg.end_byte_offset;
        uint8_t end_nib = seg.end_nibble_idx;

        uint16_t state = seg.initial_state;
        uint32_t out_pos = seg.out_coeff_pos;
        uint32_t sym_count = 0;

        while (true) {
            if (byte_idx > end_byte || (byte_idx == end_byte && nib_idx >= end_nib)) {
                break;
            }

            uint8_t byte = src[byte_idx];
            uint8_t nibble = (nib_idx == 0) ? (byte >> 4) : (byte & 0x0f);
            size_t entry_idx = codebook_base + state * 16 + nibble;
            GpuFsmEntry entry = d_fsm_tables[entry_idx];

            if (entry.value0 == BAND_END_MARKER) {
                break;
            }

            uint32_t pre = entry.pre_post_skip & 0x1ff;
            uint32_t post = (entry.pre_post_skip >> 12) & 0x7;
            out_pos += pre;
            if (entry.value0 != 0 || entry.value1 != 0) {
                sym_count++;
            }
            out_pos += post;
            state = entry.next_state;

            if (nib_idx == 0) {
                nib_idx = 1;
            } else {
                nib_idx = 0;
                byte_idx++;
            }
        }

        diag.end_byte = byte_idx;
        diag.end_nibble = nib_idx;
        diag.end_state = state;
        diag.end_out_pos = out_pos;
        diag.emitted_symbols = sym_count;
        diag.termination_status = GpuBandDecodeStatus::Complete;
    }

    if (d_diagnostics) {
        d_diagnostics[seg_idx] = diag;
    }
}

// ============================================================================
// Phase 2C Launch APIs
// ============================================================================

cudaError_t LaunchZeroVlcBandsGpu(
    const GpuEntropyBandDescriptor* d_descriptors,
    int num_vlc_bands,
    cudaStream_t stream
) {
    if (!d_descriptors || num_vlc_bands <= 0) return cudaErrorInvalidValue;

    ZeroAllVlcBandsKernel<<<num_vlc_bands, 256, 0, stream>>>(d_descriptors, num_vlc_bands);
    return cudaGetLastError();
}

cudaError_t LaunchDecodeEntropyParallelGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_band_descriptors,
    int num_vlc_bands,
    const GpuEntropySegmentDescriptor* d_segments,
    int num_segments,
    const GpuFsmEntry* d_fsm_tables,
    GpuSegmentDecodeResult* d_seg_results,
    cudaStream_t stream,
    const GpuCheckpointIndexResult* d_index_result,
    uint32_t* d_frame_error
) {
    if (!d_compressed_sample || !d_band_descriptors || num_vlc_bands <= 0 ||
        !d_segments || num_segments <= 0 || !d_fsm_tables) {
        return cudaErrorInvalidValue;
    }

    DecodeHighpassVlcParallelCudaKernel<<<num_segments, 64, 0, stream>>>(
        d_compressed_sample,
        compressed_size,
        d_band_descriptors,
        num_vlc_bands,
        d_segments,
        num_segments,
        d_fsm_tables,
        d_seg_results, d_index_result, d_frame_error
    );

    return cudaGetLastError();
}

cudaError_t LaunchCooperativeDifferenceCodingGpu(
    const GpuEntropyBandDescriptor* d_band_descriptors,
    int num_vlc_bands,
    cudaStream_t stream, uint32_t* d_frame_error
) {
    if (!d_band_descriptors || num_vlc_bands <= 0) return cudaErrorInvalidValue;

    CooperativeDifferenceCodingAllBandsKernel<<<num_vlc_bands, 128, 0, stream>>>(
        d_band_descriptors,
        num_vlc_bands, d_frame_error
    );

    return cudaGetLastError();
}

cudaError_t LaunchDecodeEntropyParallelDiagnosticGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_band_desc,
    const GpuEntropySegmentDescriptor* d_segments,
    int num_segments,
    const GpuFsmEntry* d_fsm_tables,
    GpuSegmentDiagnostic* d_diagnostics,
    cudaStream_t stream
) {
    if (!d_compressed_sample || !d_band_desc || !d_segments || num_segments <= 0 || !d_fsm_tables) {
        return cudaErrorInvalidValue;
    }

    DecodeEntropyParallelDiagnosticCudaKernel<<<num_segments, 1, 0, stream>>>(
        d_compressed_sample,
        compressed_size,
        d_band_desc,
        d_segments,
        num_segments,
        d_fsm_tables,
        d_diagnostics
    );

    return cudaGetLastError();
}

} // namespace HyperCine
