// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_entropy_cuda.h"
#include <cuda_runtime.h>
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

} // namespace HyperCine
