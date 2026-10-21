// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/entropy_gpu.h"

#include <cub/device/device_scan.cuh>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace hypercine {

int (*g_entropy_occ_fn)() = nullptr;
const char* g_launch_error = nullptr;

namespace {

constexpr int kBlock = 256;
constexpr int kMaxHighpassBands = 32;
constexpr int kMaxEntropyStreams = 8;

// Test-only, single-shot fault injection for the fail-closed capacity path.
size_t g_inject_workspace_alloc_failure_bytes = 0;

// Thread-local: why the most recent CreateGpuEntropyFrameSlot call failed.
thread_local std::string g_slot_creation_error;

std::string DescribeDeviceCapacityFailure(const std::string& what, size_t requested_bytes, cudaError_t error) {
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    cudaGetLastError();  // clear the non-sticky allocation error before querying
    const cudaError_t query = cudaMemGetInfo(&free_bytes, &total_bytes);
    std::string text = "UNSUPPORTED_RESOURCE_BOUND: ";
    text += what;
    text += " allocation failed: requested " + std::to_string(requested_bytes) + " B; ";
    if (query == cudaSuccess) {
        text += "free " + std::to_string(free_bytes) + " B of " + std::to_string(total_bytes) + " B; ";
    } else {
        text += "device memory query failed; ";
    }
    text += "CUDA ";
    text += cudaGetErrorName(error);
    text += " (";
    text += cudaGetErrorString(error);
    text += ")";
    return text;
}

__host__ __device__ uint32_t Bswap32(uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0x0000FF00u) | ((value << 8) & 0x00FF0000u) | (value << 24);
}

__device__ int CountRunDevice(const int16_t* covered, int run_length, int zeros) {
    int tokens = 0;
    while (zeros > 0) {
        const int index = zeros < run_length ? zeros : run_length - 1;
        const int step = covered[index];
        if (step <= 0) return -1;
        zeros -= step;
        ++tokens;
        if (tokens > 100000000) return -1;
    }
    return tokens;
}

__global__ void BlockScanKernel(const int16_t* __restrict__ src, int width, int height, int pitch_px,
                                int* __restrict__ prefix, int* __restrict__ suffix, int* __restrict__ nz_count,
                                int* __restrict__ all_zero, int* __restrict__ block_len,
                                int* __restrict__ local_pos, int* __restrict__ local_val) {
    const int virtual_len = height * pitch_px;
    const int block_id = static_cast<int>(blockIdx.x);
    const int origin = block_id * kBlock;
    if (origin >= virtual_len) return;
    const int valid_n = min(kBlock, virtual_len - origin);
    __shared__ int flags[kBlock];
    __shared__ int values[kBlock];
    const int local = static_cast<int>(threadIdx.x);
    int flag = 0;
    int value = 0;
    if (local < valid_n) {
        const int index = origin + local;
        const int row = index / pitch_px;
        const int col = index - row * pitch_px;
        if (col < width) value = src[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col)];
        flag = value != 0;
    }
    flags[local] = flag;
    values[local] = value;
    __syncthreads();
    if (local != 0) return;
    int running = 0;
    int first = -1;
    int last = -1;
    for (int i = 0; i < valid_n; ++i) {
        if (!flags[i]) continue;
        if (first < 0) first = i;
        last = i;
        local_pos[static_cast<size_t>(block_id) * kBlock + static_cast<size_t>(running)] = origin + i;
        local_val[static_cast<size_t>(block_id) * kBlock + static_cast<size_t>(running)] = values[i];
        ++running;
    }
    nz_count[block_id] = running;
    all_zero[block_id] = running == 0;
    prefix[block_id] = first < 0 ? valid_n : first;
    suffix[block_id] = running == 0 ? 0 : (valid_n - last - 1);
    block_len[block_id] = valid_n;
}

__global__ void MergeCarryKernel(const int* __restrict__ prefix, const int* __restrict__ suffix,
                                 const int* __restrict__ all_zero, const int* __restrict__ block_len,
                                 int blocks, int* __restrict__ carry, int* __restrict__ trailing) {
    (void)prefix;
    if (blockIdx.x != 0) return;
    __shared__ int s_r[kBlock];
    __shared__ int s_a[kBlock];
    const int tid = static_cast<int>(threadIdx.x);
    int pending = 0;
    for (int base = 0; base < blocks; base += kBlock) {
        const int b = base + tid;
        int r = 0;
        int a = 0;
        if (b < blocks) {
            const int az = all_zero[b];
            r = az ? 0 : 1;
            a = az ? block_len[b] : suffix[b];
        }
        s_r[tid] = r;
        s_a[tid] = a;
        __syncthreads();
        for (int step = 1; step < kBlock; step <<= 1) {
            int nr = s_r[tid];
            int na = s_a[tid];
            if (tid >= step) {
                const int lr = s_r[tid - step];
                const int la = s_a[tid - step];
                if (!nr) {
                    nr = lr;
                    na = la + na;
                }
            }
            __syncthreads();
            s_r[tid] = nr;
            s_a[tid] = na;
            __syncthreads();
        }
        if (b < blocks) {
            if (tid == 0) {
                carry[b] = pending;
            } else {
                carry[b] = s_r[tid - 1] ? s_a[tid - 1] : (pending + s_a[tid - 1]);
            }
        }
        const int valid = min(kBlock, blocks - base);
        const int next_pending = s_r[valid - 1] ? s_a[valid - 1] : (pending + s_a[valid - 1]);
        __syncthreads();
        pending = next_pending;
    }
    if (tid == 0) *trailing = pending;
}

__global__ void GatherKernel(const int* __restrict__ local_pos, const int* __restrict__ local_val,
                             const int* __restrict__ nz_count, const int* __restrict__ nz_offset, int blocks,
                             int* __restrict__ pos, int* __restrict__ val, int* __restrict__ owner) {
    const int band_block = static_cast<int>(blockIdx.x);
    if (band_block >= blocks) return;
    const int count = nz_count[band_block];
    const int dest = nz_offset[band_block];
    for (int i = static_cast<int>(threadIdx.x); i < count; i += static_cast<int>(blockDim.x)) {
        const int out = dest + i;
        pos[out] = local_pos[static_cast<size_t>(band_block) * kBlock + static_cast<size_t>(i)];
        val[out] = local_val[static_cast<size_t>(band_block) * kBlock + static_cast<size_t>(i)];
        owner[out] = band_block;
    }
}

__global__ void CountTokenKernel(const int* __restrict__ pos, const int* __restrict__ owner,
                                 const int* __restrict__ nz_offset, const int* __restrict__ carry,
                                 const int* __restrict__ prefix, const int* __restrict__ nnz, const int* __restrict__ trailing,
                                 const int16_t* __restrict__ covered, int run_length, int* __restrict__ counts) {
    const int nonzero = *nnz;
    const int stride = static_cast<int>(gridDim.x) * static_cast<int>(blockDim.x);
    for (int index = static_cast<int>(blockIdx.x) * static_cast<int>(blockDim.x) + static_cast<int>(threadIdx.x);
         index <= nonzero; index += stride) {
        int zeros = 0;
        if (index == nonzero) {
            zeros = *trailing;
        } else {
            const int band_block = owner[index];
            if (index == nz_offset[band_block]) zeros = carry[band_block] + prefix[band_block];
            else zeros = pos[index] - pos[index - 1] - 1;
        }
        const int tokens = zeros > 0 ? CountRunDevice(covered, run_length, zeros) : 0;
        counts[index] = tokens < 0 ? 0 : tokens + (index == nonzero ? 0 : 1);
    }
}

__device__ void EmitRunDevice(uint32_t* tokens, int& cursor, const int16_t* covered, const uint32_t* bits_unused,
                              int run_length, int zeros) {
    (void)bits_unused;
    while (zeros > 0) {
        const int index = zeros < run_length ? zeros : run_length - 1;
        const int step = covered[index];
        if (step <= 0) break;
        tokens[cursor++] = static_cast<uint32_t>(index);
        zeros -= step;
    }
}

__global__ void WriteTokenKernel(const int* __restrict__ pos, const int* __restrict__ val, const int* __restrict__ owner,
                                 const int* __restrict__ nz_offset, const int* __restrict__ carry,
                                 const int* __restrict__ prefix, const int* __restrict__ token_offset,
                                 const int* __restrict__ nnz, const int* __restrict__ trailing, const int16_t* __restrict__ covered, int run_length,
                                 int value_length, uint32_t* __restrict__ tokens) {
    const int nonzero = *nnz;
    const int stride = static_cast<int>(gridDim.x) * static_cast<int>(blockDim.x);
    for (int index = static_cast<int>(blockIdx.x) * static_cast<int>(blockDim.x) + static_cast<int>(threadIdx.x);
         index <= nonzero; index += stride) {
        int zeros = 0;
        if (index == nonzero) zeros = *trailing;
        else {
            const int band_block = owner[index];
            if (index == nz_offset[band_block]) zeros = carry[band_block] + prefix[band_block];
            else zeros = pos[index] - pos[index - 1] - 1;
        }
        int cursor = token_offset[index];
        if (zeros > 0) EmitRunDevice(tokens, cursor, covered, nullptr, run_length, zeros);
        if (index == nonzero) continue;
        int value = val[index];
        const int half = value_length >> 1;
        if (value < 0) {
            if (value <= -half) value = -(half - 1);
            value = value_length + value;
        } else if (value >= half) {
            value = half - 1;
        }
        tokens[cursor] = 0x80000000u | static_cast<uint32_t>(value);
    }
}

__global__ void CodebookKernel(const uint32_t* __restrict__ tokens, const int* __restrict__ token_total,
                               const uint32_t* __restrict__ run_bits, const int16_t* __restrict__ run_size,
                               const uint32_t* __restrict__ value_entry, uint32_t band_end_code, int band_end_size,
                               uint32_t* __restrict__ code, uint32_t* __restrict__ length) {
    const int token_count = *token_total;
    const int stride = static_cast<int>(gridDim.x) * static_cast<int>(blockDim.x);
    for (int index = static_cast<int>(blockIdx.x) * static_cast<int>(blockDim.x) + static_cast<int>(threadIdx.x);
         index <= token_count; index += stride) {
        if (index < token_count) {
            const uint32_t packed = tokens[index];
            if (packed & 0x80000000u) {
                const uint32_t entry = value_entry[packed & 0x7FFFFFFFu];
                code[index] = entry & 0x07FFFFFFu;
                length[index] = entry >> 27;
            } else {
                code[index] = run_bits[packed];
                length[index] = static_cast<uint32_t>(run_size[packed]);
            }
        } else {
            code[index] = band_end_code;
            length[index] = static_cast<uint32_t>(band_end_size);
        }
    }
}

__global__ void StoreNzKernel(const int* __restrict__ nz_offset, const int* __restrict__ nz_count, int blocks, int* __restrict__ nnz) {
    if (threadIdx.x == 0 && blockIdx.x == 0) *nnz = nz_offset[blocks - 1] + nz_count[blocks - 1];
}

__global__ void StoreTokenTotalKernel(const int* __restrict__ offset, const int* __restrict__ count, const int* __restrict__ nnz,
                                      int* __restrict__ total) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        const int last = *nnz;
        *total = offset[last] + count[last];
    }
}

__global__ void StoreBitsKernel(const uint32_t* __restrict__ offset, const uint32_t* __restrict__ length,
                                const int* __restrict__ token_total, uint32_t* __restrict__ bits) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        const int end = *token_total;
        *bits = offset[end] + length[end];
    }
}

struct BandScalarMeta {
    int token_total;
    uint32_t total_bits;
};

__global__ void StoreBandBitsAndTokensKernel(const uint32_t* __restrict__ offset,
                                             const uint32_t* __restrict__ length,
                                             const int* __restrict__ token_total,
                                             uint32_t* __restrict__ ws_bits,
                                             uint32_t* __restrict__ slot_bits,
                                             BandScalarMeta* __restrict__ mapped_scalar) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        const int tok = *token_total;
        const uint32_t b = offset[tok] + length[tok];
        *ws_bits = b;
        if (slot_bits) *slot_bits = b;
        if (mapped_scalar) {
            mapped_scalar->token_total = tok;
            mapped_scalar->total_bits = b;
        }
    }
}

__global__ void PackKernel(const uint32_t* __restrict__ code, const uint32_t* __restrict__ length,
                           const uint32_t* __restrict__ offset, const int* __restrict__ token_total,
                           const uint32_t* __restrict__ total_bits, uint32_t* __restrict__ words,
                           uint32_t word_limit, int* __restrict__ overflow) {
    const uint32_t word_count = (*total_bits + 31u) >> 5;
    if (word_count > word_limit) {
        if (threadIdx.x == 0 && blockIdx.x == 0 && overflow) atomicExch(overflow, 1);
        return;
    }
    const int codes = *token_total + 1;
    const uint32_t stride = static_cast<uint32_t>(gridDim.x) * blockDim.x;
    for (uint32_t word_index = static_cast<uint32_t>(blockIdx.x) * blockDim.x + threadIdx.x;
         word_index < word_count; word_index += stride) {
        const uint32_t bit_begin = word_index << 5;
        const uint32_t bit_end = bit_begin + 32u;
        int lo = 0;
        int hi = codes;
        while (lo < hi) {
            const int mid = (lo + hi) >> 1;
            const uint32_t end = offset[mid] + length[mid];
            if (end <= bit_begin) lo = mid + 1;
            else hi = mid;
        }
        uint32_t logical = 0;
        for (int i = lo; i < codes; ++i) {
            const uint32_t start = offset[i];
            const uint32_t bits = length[i];
            if (start >= bit_end) break;
            const uint32_t word = code[i];
            const uint32_t from = start > bit_begin ? start : bit_begin;
            const uint32_t to = (start + bits) < bit_end ? (start + bits) : bit_end;
            for (uint32_t bit = from; bit < to; ++bit) {
                const uint32_t src_bit = bits - 1u - (bit - start);
                if ((word >> src_bit) & 1u) logical |= 1u << (31u - (bit - bit_begin));
            }
        }
        words[word_index] = Bswap32(logical);
    }
}

struct FrameCompactMeta {
    uint32_t word_offset[kMaxHighpassBands];
    uint32_t word_count[kMaxHighpassBands];
    uint32_t total_words;
};

__global__ void ComputeFrameLayoutKernel(const uint32_t* __restrict__ d_total_bits, int count,
                                         FrameCompactMeta* __restrict__ d_meta,
                                         FrameCompactMeta* __restrict__ h_meta_mapped,
                                         uint32_t consolidated_limit, int* __restrict__ overflow) {
    if (threadIdx.x != 0 || blockIdx.x != 0) return;
    uint32_t sum = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t words = (d_total_bits[i] + 31u) >> 5;
        d_meta->word_offset[i] = sum;
        d_meta->word_count[i] = words;
        if (h_meta_mapped) {
            h_meta_mapped->word_offset[i] = sum;
            h_meta_mapped->word_count[i] = words;
        }
        sum += words;
    }
    d_meta->total_words = sum;
    if (h_meta_mapped) h_meta_mapped->total_words = sum;
    if (sum > consolidated_limit && overflow) atomicExch(overflow, 1);
}

__global__ void ConsolidateFrameWordsKernel(const uint32_t* __restrict__ src_all,
                                            const uint32_t* __restrict__ src_band_offsets,
                                            const FrameCompactMeta* __restrict__ meta,
                                            uint32_t* __restrict__ dst_consolidated,
                                            int count, uint32_t consolidated_limit,
                                            int* __restrict__ overflow) {
    if (overflow && *overflow) return;
    const int b = static_cast<int>(blockIdx.y);
    if (b >= count) return;
    const uint32_t n = meta->word_count[b];
    const uint32_t off = meta->word_offset[b];
    if (off >= consolidated_limit || n > consolidated_limit - off) {
        if (threadIdx.x == 0 && blockIdx.x == 0 && overflow) atomicExch(overflow, 1);
        return;
    }
    const uint32_t* src = src_all + src_band_offsets[b];
    uint32_t* dst = dst_consolidated + meta->word_offset[b];
    const uint32_t stride = static_cast<uint32_t>(gridDim.x) * blockDim.x;
    for (uint32_t i = static_cast<uint32_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n; i += stride) {
        dst[i] = src[i];
    }
}

// ============================================================================
// Phase 2D-B: Fused Kernels (preserved alongside Phase 2C reference kernels)
// ============================================================================

__global__ void BlockScanWarpKernel(const int16_t* __restrict__ src, int width, int height, int pitch_px,
                                    int* __restrict__ prefix, int* __restrict__ suffix, int* __restrict__ nz_count,
                                    int* __restrict__ all_zero, int* __restrict__ block_len,
                                    int* __restrict__ local_pos, int* __restrict__ local_val) {
    const int virtual_len = height * pitch_px;
    const int block_id = static_cast<int>(blockIdx.x);
    const int origin = block_id * kBlock;
    if (origin >= virtual_len) return;
    const int valid_n = min(kBlock, virtual_len - origin);
    const int local = static_cast<int>(threadIdx.x);
    int flag = 0;
    int value = 0;
    if (local < valid_n) {
        const int index = origin + local;
        const int row = index / pitch_px;
        const int col = index - row * pitch_px;
        if (col < width) value = src[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col)];
        flag = (value != 0) ? 1 : 0;
    }
    const int lane = local & 31;
    const int warp = local >> 5;
    const uint32_t mask = __ballot_sync(0xFFFFFFFFu, flag != 0);
    const int warp_nz = __popc(mask);
    const int lane_prefix = __popc(mask & ((1u << lane) - 1u));

    __shared__ int s_warp_nz[8];
    __shared__ uint32_t s_warp_mask[8];
    if (lane == 0) {
        s_warp_nz[warp] = warp_nz;
        s_warp_mask[warp] = mask;
    }
    __syncthreads();

    int warp_base = 0;
    #pragma unroll
    for (int k = 0; k < warp; ++k) warp_base += s_warp_nz[k];

    if (flag != 0) {
        const int out_idx = warp_base + lane_prefix;
        local_pos[static_cast<size_t>(block_id) * kBlock + static_cast<size_t>(out_idx)] = origin + local;
        local_val[static_cast<size_t>(block_id) * kBlock + static_cast<size_t>(out_idx)] = value;
    }

    if (local == 0) {
        int running = 0;
        int first = -1;
        int last = -1;
        #pragma unroll
        for (int k = 0; k < 8; ++k) {
            const uint32_t m = s_warp_mask[k];
            if (m != 0u) {
                if (first < 0) first = (k << 5) + (__ffs(m) - 1);
                last = (k << 5) + (31 - __clz(m));
                running += s_warp_nz[k];
            }
        }
        nz_count[block_id] = running;
        all_zero[block_id] = (running == 0) ? 1 : 0;
        prefix[block_id] = (first < 0) ? valid_n : first;
        suffix[block_id] = (running == 0) ? 0 : (valid_n - last - 1);
        block_len[block_id] = valid_n;
    }
}

__global__ void MergeCarryAndStoreNzFusedKernel(const int* __restrict__ prefix, const int* __restrict__ suffix,
                                                const int* __restrict__ all_zero, const int* __restrict__ block_len,
                                                const int* __restrict__ nz_offset, const int* __restrict__ nz_count,
                                                int blocks, int* __restrict__ carry,
                                                int* __restrict__ trailing, int* __restrict__ nnz) {
    (void)prefix;
    if (blockIdx.x != 0) return;
    __shared__ int s_r[kBlock];
    __shared__ int s_a[kBlock];
    const int tid = static_cast<int>(threadIdx.x);
    int pending = 0;
    for (int base = 0; base < blocks; base += kBlock) {
        const int b = base + tid;
        int r = 0;
        int a = 0;
        if (b < blocks) {
            const int az = all_zero[b];
            r = az ? 0 : 1;
            a = az ? block_len[b] : suffix[b];
        }
        s_r[tid] = r;
        s_a[tid] = a;
        __syncthreads();
        for (int step = 1; step < kBlock; step <<= 1) {
            int nr = s_r[tid];
            int na = s_a[tid];
            if (tid >= step) {
                const int lr = s_r[tid - step];
                const int la = s_a[tid - step];
                if (!nr) {
                    nr = lr;
                    na = la + na;
                }
            }
            __syncthreads();
            s_r[tid] = nr;
            s_a[tid] = na;
            __syncthreads();
        }
        if (b < blocks) {
            if (tid == 0) {
                carry[b] = pending;
            } else {
                carry[b] = s_r[tid - 1] ? s_a[tid - 1] : (pending + s_a[tid - 1]);
            }
        }
        const int valid = min(kBlock, blocks - base);
        const int next_pending = s_r[valid - 1] ? s_a[valid - 1] : (pending + s_a[valid - 1]);
        __syncthreads();
        pending = next_pending;
    }
    if (tid == 0) {
        *trailing = pending;
        *nnz = nz_offset[blocks - 1] + nz_count[blocks - 1];
    }
}

__global__ void GatherAndCountTokensFusedKernel(const int* __restrict__ local_pos, const int* __restrict__ local_val,
                                                const int* __restrict__ nz_count, const int* __restrict__ nz_offset,
                                                const int* __restrict__ carry, const int* __restrict__ prefix,
                                                const int* __restrict__ nnz, const int* __restrict__ trailing,
                                                const int16_t* __restrict__ covered, int run_length, int blocks,
                                                int* __restrict__ run_zeros, int* __restrict__ val,
                                                int* __restrict__ counts) {
    const int band_block = static_cast<int>(blockIdx.x);
    if (band_block == 0 && threadIdx.x == 0) {
        const int nonzero = *nnz;
        const int tr = *trailing;
        const int tokens = tr > 0 ? CountRunDevice(covered, run_length, tr) : 0;
        counts[nonzero] = tokens < 0 ? 0 : tokens;
    }
    if (band_block >= blocks) return;
    const int count = nz_count[band_block];
    if (count <= 0) return;
    const int dest = nz_offset[band_block];
    const size_t block_base = static_cast<size_t>(band_block) * kBlock;
    for (int i = static_cast<int>(threadIdx.x); i < count; i += static_cast<int>(blockDim.x)) {
        const int out = dest + i;
        const int cur_pos = local_pos[block_base + static_cast<size_t>(i)];
        const int zeros = (i == 0)
            ? (carry[band_block] + prefix[band_block])
            : (cur_pos - local_pos[block_base + static_cast<size_t>(i - 1)] - 1);
        run_zeros[out] = zeros;
        val[out] = local_val[block_base + static_cast<size_t>(i)];
        const int tokens = zeros > 0 ? CountRunDevice(covered, run_length, zeros) : 0;
        counts[out] = (tokens < 0 ? 0 : tokens) + 1;
    }
}

__global__ void TokenizeAndCodebookFusedKernel(const int* __restrict__ run_zeros, const int* __restrict__ val,
                                               const int* __restrict__ token_offset, const int* __restrict__ nnz,
                                               const int* __restrict__ trailing, const int16_t* __restrict__ covered,
                                               int run_length, int value_length,
                                               const uint32_t* __restrict__ run_bits, const int16_t* __restrict__ run_size,
                                               const uint32_t* __restrict__ value_entry,
                                               uint32_t band_end_code, int band_end_size,
                                               uint32_t* __restrict__ code, uint32_t* __restrict__ length) {
    const int nonzero = *nnz;
    const int stride = static_cast<int>(gridDim.x) * static_cast<int>(blockDim.x);
    const int half = value_length >> 1;
    for (int index = static_cast<int>(blockIdx.x) * static_cast<int>(blockDim.x) + static_cast<int>(threadIdx.x);
         index <= nonzero; index += stride) {
        int zeros = (index == nonzero) ? *trailing : run_zeros[index];
        int cursor = token_offset[index];
        while (zeros > 0) {
            const int r_idx = zeros < run_length ? zeros : run_length - 1;
            const int step = covered[r_idx];
            if (step <= 0) break;
            code[cursor] = run_bits[r_idx];
            length[cursor] = static_cast<uint32_t>(run_size[r_idx]);
            ++cursor;
            zeros -= step;
        }
        if (index == nonzero) {
            code[cursor] = band_end_code;
            length[cursor] = static_cast<uint32_t>(band_end_size);
            continue;
        }
        int value = val[index];
        if (value < 0) {
            if (value <= -half) value = -(half - 1);
            value = value_length + value;
        } else if (value >= half) {
            value = half - 1;
        }
        const uint32_t entry = value_entry[static_cast<uint32_t>(value)];
        code[cursor] = entry & 0x07FFFFFFu;
        length[cursor] = entry >> 27;
    }
}

// ============================================================================
// Phase 2D-C: Device-Bounded Prefix-Scan Kernels
// ============================================================================

__global__ void MergeCarryAndScanNzFusedKernel(const int* __restrict__ prefix, const int* __restrict__ suffix,
                                               const int* __restrict__ all_zero, const int* __restrict__ block_len,
                                               const int* __restrict__ nz_count, int blocks,
                                               int* __restrict__ carry, int* __restrict__ nz_offset,
                                               int* __restrict__ trailing, int* __restrict__ nnz) {
    (void)prefix;
    if (blockIdx.x != 0) return;
    __shared__ int s_r[kBlock];
    __shared__ int s_a[kBlock];
    __shared__ int s_nz[kBlock];
    const int tid = static_cast<int>(threadIdx.x);
    int pending = 0;
    int running_nz = 0;
    for (int base = 0; base < blocks; base += kBlock) {
        const int b = base + tid;
        int r = 0;
        int a = 0;
        int c = 0;
        if (b < blocks) {
            const int az = all_zero[b];
            r = az ? 0 : 1;
            a = az ? block_len[b] : suffix[b];
            c = nz_count[b];
        }
        s_r[tid] = r;
        s_a[tid] = a;
        s_nz[tid] = c;
        __syncthreads();
        for (int step = 1; step < kBlock; step <<= 1) {
            int nr = s_r[tid];
            int na = s_a[tid];
            int nc = s_nz[tid];
            if (tid >= step) {
                const int lr = s_r[tid - step];
                const int la = s_a[tid - step];
                if (!nr) {
                    nr = lr;
                    na = la + na;
                }
                nc += s_nz[tid - step];
            }
            __syncthreads();
            s_r[tid] = nr;
            s_a[tid] = na;
            s_nz[tid] = nc;
            __syncthreads();
        }
        if (b < blocks) {
            if (tid == 0) {
                carry[b] = pending;
                nz_offset[b] = running_nz;
            } else {
                carry[b] = s_r[tid - 1] ? s_a[tid - 1] : (pending + s_a[tid - 1]);
                nz_offset[b] = running_nz + s_nz[tid - 1];
            }
        }
        const int valid = min(kBlock, blocks - base);
        const int next_pending = s_r[valid - 1] ? s_a[valid - 1] : (pending + s_a[valid - 1]);
        const int next_running_nz = running_nz + s_nz[valid - 1];
        __syncthreads();
        pending = next_pending;
        running_nz = next_running_nz;
    }
    if (tid == 0) {
        *trailing = pending;
        *nnz = running_nz;
    }
}

__global__ void ScanLocalTilesIntKernel(const int* __restrict__ d_in,
                                        int* __restrict__ d_out,
                                        uint32_t* __restrict__ d_tile_sums,
                                        const int* __restrict__ d_last_index) {
    const int active_n = *d_last_index + 1;
    const int tile_idx = static_cast<int>(blockIdx.x);
    const int tile_base = tile_idx * 1024;
    if (tile_base >= active_n) return;
    const int tid = static_cast<int>(threadIdx.x);
    const int idx0 = tile_base + (tid << 2);
    const int v0 = (idx0 + 0 < active_n) ? d_in[idx0 + 0] : 0;
    const int v1 = (idx0 + 1 < active_n) ? d_in[idx0 + 1] : 0;
    const int v2 = (idx0 + 2 < active_n) ? d_in[idx0 + 2] : 0;
    const int v3 = (idx0 + 3 < active_n) ? d_in[idx0 + 3] : 0;
    const int thread_sum = v0 + v1 + v2 + v3;

    const int lane = tid & 31;
    const int warp = tid >> 5;
    int warp_incl = thread_sum;
    #pragma unroll
    for (int offset = 1; offset < 32; offset <<= 1) {
        const int n = __shfl_up_sync(0xFFFFFFFFu, warp_incl, offset);
        if (lane >= offset) warp_incl += n;
    }
    __shared__ int s_warp_sums[8];
    if (lane == 31) s_warp_sums[warp] = warp_incl;
    __syncthreads();
    int warp_excl = 0;
    #pragma unroll
    for (int k = 0; k < warp; ++k) warp_excl += s_warp_sums[k];
    const int thread_excl = warp_excl + (warp_incl - thread_sum);

    if (idx0 + 0 < active_n) d_out[idx0 + 0] = thread_excl;
    if (idx0 + 1 < active_n) d_out[idx0 + 1] = thread_excl + v0;
    if (idx0 + 2 < active_n) d_out[idx0 + 2] = thread_excl + v0 + v1;
    if (idx0 + 3 < active_n) d_out[idx0 + 3] = thread_excl + v0 + v1 + v2;
    if (tid == 255) {
        d_tile_sums[tile_idx] = static_cast<uint32_t>(warp_excl + warp_incl);
    }
}

__global__ void ScanAddBlockOffsetsAndStoreTotalIntKernel(int* __restrict__ d_out,
                                                          const uint32_t* __restrict__ d_tile_sums,
                                                          const int* __restrict__ d_last_index,
                                                          int* __restrict__ d_total) {
    const int active_n = *d_last_index + 1;
    const int active_tiles = (active_n + 1023) >> 10;
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= active_tiles && tile_idx > 0) return;
    const int tid = static_cast<int>(threadIdx.x);

    if (tile_idx == 0) {
        int acc = 0;
        for (int i = tid; i < active_tiles; i += 256) {
            acc += static_cast<int>(d_tile_sums[i]);
        }
        #pragma unroll
        for (int offset = 16; offset > 0; offset >>= 1) {
            acc += __shfl_down_sync(0xFFFFFFFFu, acc, offset);
        }
        __shared__ int s_red0[8];
        if ((tid & 31) == 0) s_red0[tid >> 5] = acc;
        __syncthreads();
        if (tid == 0) {
            int total = 0;
            #pragma unroll
            for (int k = 0; k < 8; ++k) total += s_red0[k];
            *d_total = total;
        }
        return;
    }

    int acc = 0;
    for (int i = tid; i < tile_idx; i += 256) {
        acc += static_cast<int>(d_tile_sums[i]);
    }
    #pragma unroll
    for (int offset = 16; offset > 0; offset >>= 1) {
        acc += __shfl_down_sync(0xFFFFFFFFu, acc, offset);
    }
    __shared__ int s_red[8];
    if ((tid & 31) == 0) s_red[tid >> 5] = acc;
    __syncthreads();
    __shared__ int s_block_offset;
    if (tid == 0) {
        int boff = 0;
        #pragma unroll
        for (int k = 0; k < 8; ++k) boff += s_red[k];
        s_block_offset = boff;
    }
    __syncthreads();
    const int block_offset = s_block_offset;
    const int idx0 = tile_idx * 1024 + (tid << 2);
    if (idx0 + 0 < active_n) d_out[idx0 + 0] += block_offset;
    if (idx0 + 1 < active_n) d_out[idx0 + 1] += block_offset;
    if (idx0 + 2 < active_n) d_out[idx0 + 2] += block_offset;
    if (idx0 + 3 < active_n) d_out[idx0 + 3] += block_offset;
}

__global__ void ScanLocalTilesBitsKernel(const uint32_t* __restrict__ d_in,
                                         uint32_t* __restrict__ d_out,
                                         uint32_t* __restrict__ d_tile_sums,
                                         const int* __restrict__ d_last_index) {
    const int active_n = *d_last_index + 1;
    const int tile_idx = static_cast<int>(blockIdx.x);
    const int tile_base = tile_idx * 1024;
    if (tile_base >= active_n) return;
    const int tid = static_cast<int>(threadIdx.x);
    const int idx0 = tile_base + (tid << 2);
    const uint32_t v0 = (idx0 + 0 < active_n) ? d_in[idx0 + 0] : 0u;
    const uint32_t v1 = (idx0 + 1 < active_n) ? d_in[idx0 + 1] : 0u;
    const uint32_t v2 = (idx0 + 2 < active_n) ? d_in[idx0 + 2] : 0u;
    const uint32_t v3 = (idx0 + 3 < active_n) ? d_in[idx0 + 3] : 0u;
    const uint32_t thread_sum = v0 + v1 + v2 + v3;

    const int lane = tid & 31;
    const int warp = tid >> 5;
    uint32_t warp_incl = thread_sum;
    #pragma unroll
    for (int offset = 1; offset < 32; offset <<= 1) {
        const uint32_t n = __shfl_up_sync(0xFFFFFFFFu, warp_incl, offset);
        if (lane >= offset) warp_incl += n;
    }
    __shared__ uint32_t s_warp_sums[8];
    if (lane == 31) s_warp_sums[warp] = warp_incl;
    __syncthreads();
    uint32_t warp_excl = 0u;
    #pragma unroll
    for (int k = 0; k < warp; ++k) warp_excl += s_warp_sums[k];
    const uint32_t thread_excl = warp_excl + (warp_incl - thread_sum);

    if (idx0 + 0 < active_n) d_out[idx0 + 0] = thread_excl;
    if (idx0 + 1 < active_n) d_out[idx0 + 1] = thread_excl + v0;
    if (idx0 + 2 < active_n) d_out[idx0 + 2] = thread_excl + v0 + v1;
    if (idx0 + 3 < active_n) d_out[idx0 + 3] = thread_excl + v0 + v1 + v2;
    if (tid == 255) {
        d_tile_sums[tile_idx] = warp_excl + warp_incl;
    }
}

__global__ void ScanAddBlockOffsetsAndStoreTotalBitsKernel(uint32_t* __restrict__ d_out,
                                                           const uint32_t* __restrict__ d_tile_sums,
                                                           const int* __restrict__ d_token_total,
                                                           uint32_t* __restrict__ ws_bits,
                                                           uint32_t* __restrict__ slot_bits,
                                                           BandScalarMeta* __restrict__ mapped_scalar) {
    const int tok = *d_token_total;
    const int active_n = tok + 1;
    const int active_tiles = (active_n + 1023) >> 10;
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= active_tiles && tile_idx > 0) return;
    const int tid = static_cast<int>(threadIdx.x);

    if (tile_idx == 0) {
        uint32_t acc = 0u;
        for (int i = tid; i < active_tiles; i += 256) {
            acc += d_tile_sums[i];
        }
        #pragma unroll
        for (int offset = 16; offset > 0; offset >>= 1) {
            acc += __shfl_down_sync(0xFFFFFFFFu, acc, offset);
        }
        __shared__ uint32_t s_red0[8];
        if ((tid & 31) == 0) s_red0[tid >> 5] = acc;
        __syncthreads();
        if (tid == 0) {
            uint32_t total_b = 0u;
            #pragma unroll
            for (int k = 0; k < 8; ++k) total_b += s_red0[k];
            *ws_bits = total_b;
            if (slot_bits) *slot_bits = total_b;
            if (mapped_scalar) {
                mapped_scalar->token_total = tok;
                mapped_scalar->total_bits = total_b;
            }
        }
        return;
    }

    uint32_t acc = 0u;
    for (int i = tid; i < tile_idx; i += 256) {
        acc += d_tile_sums[i];
    }
    #pragma unroll
    for (int offset = 16; offset > 0; offset >>= 1) {
        acc += __shfl_down_sync(0xFFFFFFFFu, acc, offset);
    }
    __shared__ uint32_t s_red[8];
    if ((tid & 31) == 0) s_red[tid >> 5] = acc;
    __syncthreads();
    __shared__ uint32_t s_block_offset;
    if (tid == 0) {
        uint32_t boff = 0u;
        #pragma unroll
        for (int k = 0; k < 8; ++k) boff += s_red[k];
        s_block_offset = boff;
    }
    __syncthreads();
    const uint32_t block_offset = s_block_offset;
    const int idx0 = tile_idx * 1024 + (tid << 2);
    if (idx0 + 0 < active_n) d_out[idx0 + 0] += block_offset;
    if (idx0 + 1 < active_n) d_out[idx0 + 1] += block_offset;
    if (idx0 + 2 < active_n) d_out[idx0 + 2] += block_offset;
    if (idx0 + 3 < active_n) d_out[idx0 + 3] += block_offset;
}

struct Workspace {
    int coeff_cap = 0;
    int block_cap = 0;
    int token_cap = 0;
    int* prefix = nullptr;
    int* suffix = nullptr;
    int* nz_count = nullptr;
    int* all_zero = nullptr;
    int* block_len = nullptr;
    int* carry = nullptr;
    int* nz_offset = nullptr;
    int* local_pos = nullptr;
    int* local_val = nullptr;
    int* pos = nullptr;
    int* val = nullptr;
    int* owner = nullptr;
    int* token_counts = nullptr;
    int* token_offset = nullptr;
    int* trailing = nullptr;
    int* nnz = nullptr;
    int* token_total = nullptr;
    uint32_t* total_bits = nullptr;
    uint32_t* tokens = nullptr;
    uint32_t* code = nullptr;
    uint32_t* length = nullptr;
    uint32_t* bit_offset = nullptr;
    uint32_t* words = nullptr;
    uint32_t* tile_sums = nullptr;
    uint32_t* run_bits[8] = {};
    int16_t* run_size[8] = {};
    int16_t* run_count[8] = {};
    uint32_t* value_entry[8] = {};
    int cb_run_len[8] = {};
    int cb_val_len[8] = {};
    void* scan_temp = nullptr;
    size_t scan_bytes = 0;
    size_t last_alloc_bytes = 0;
    cudaError_t last_alloc_error = cudaSuccess;
    const char* last_alloc_what = nullptr;
    int active_cb = 0;
    int active_segments = 0;
    GpuEntropyStop active_stop = GpuEntropyStop::Full;
    cudaStream_t stream = nullptr;
    cudaStream_t launch_stream = nullptr;
    cudaEvent_t ev[8] = {};
    struct PinnedScalars {
        int token_total;
        uint32_t total_bits;
        int trailing;
    };
    PinnedScalars* pinned = nullptr;
    uint32_t* pinned_words = nullptr;

    bool AllocDevice(void** ptr, size_t bytes, const char* what) {
        last_alloc_bytes = bytes;
        last_alloc_what = what;
        if (g_inject_workspace_alloc_failure_bytes != 0 &&
            bytes >= g_inject_workspace_alloc_failure_bytes) {
            g_inject_workspace_alloc_failure_bytes = 0;
            last_alloc_error = cudaErrorMemoryAllocation;
            return false;
        }
        if (cudaMalloc(ptr, bytes) != cudaSuccess) {
            last_alloc_error = cudaGetLastError();
            return false;
        }
        last_alloc_error = cudaSuccess;
        return true;
    }

    bool Ensure(int coeffs, int blocks, int tokens_needed, bool need_host_words = true) {
        if (coeffs <= coeff_cap && blocks <= block_cap && tokens_needed <= token_cap && stream &&
            tile_sums && (!need_host_words || (words && pinned_words))) {
            return true;
        }
        Release();
        last_alloc_bytes = 0;
        last_alloc_error = cudaSuccess;
        last_alloc_what = nullptr;
        coeff_cap = std::max(coeffs, 1);
        block_cap = std::max(blocks, 1);
        token_cap = std::max(tokens_needed, 1);
        const int max_tiles = (token_cap + 1023) / 1024 + 4;
        auto alloc = [this](void** ptr, size_t bytes) {
            if (!AllocDevice(ptr, bytes, "entropy workspace")) return false;
            if (cudaMemset(*ptr, 0, bytes) != cudaSuccess) {
                last_alloc_error = cudaGetLastError();
                return false;
            }
            return true;
        };
        if (!alloc(reinterpret_cast<void**>(&prefix), sizeof(int) * block_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&suffix), sizeof(int) * block_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&nz_count), sizeof(int) * block_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&all_zero), sizeof(int) * block_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&block_len), sizeof(int) * block_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&carry), sizeof(int) * block_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&nz_offset), sizeof(int) * (block_cap + 1))) return false;
        if (!alloc(reinterpret_cast<void**>(&local_pos), sizeof(int) * static_cast<size_t>(block_cap) * kBlock)) return false;
        if (!alloc(reinterpret_cast<void**>(&local_val), sizeof(int) * static_cast<size_t>(block_cap) * kBlock)) return false;
        if (!alloc(reinterpret_cast<void**>(&pos), sizeof(int) * coeff_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&val), sizeof(int) * coeff_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&owner), sizeof(int) * coeff_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&token_counts), sizeof(int) * token_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&token_offset), sizeof(int) * token_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&trailing), sizeof(int))) return false;
        if (!alloc(reinterpret_cast<void**>(&nnz), sizeof(int))) return false;
        if (!alloc(reinterpret_cast<void**>(&token_total), sizeof(int))) return false;
        if (!alloc(reinterpret_cast<void**>(&total_bits), sizeof(uint32_t))) return false;
        if (!alloc(reinterpret_cast<void**>(&tokens), sizeof(uint32_t) * token_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&code), sizeof(uint32_t) * token_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&length), sizeof(uint32_t) * token_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&bit_offset), sizeof(uint32_t) * token_cap)) return false;
        if (!alloc(reinterpret_cast<void**>(&tile_sums), sizeof(uint32_t) * max_tiles)) return false;
        if (need_host_words) {
            if (!alloc(reinterpret_cast<void**>(&words), sizeof(uint32_t) * token_cap)) return false;
            if (cudaHostAlloc(&pinned_words, sizeof(uint32_t) * token_cap, cudaHostAllocDefault) != cudaSuccess) return false;
        }
        if (cudaHostAlloc(&pinned, sizeof(PinnedScalars), cudaHostAllocDefault) != cudaSuccess) return false;
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) return false;
        for (auto& event : ev) {
            if (cudaEventCreate(&event) != cudaSuccess) return false;
        }
        size_t sb0 = 0;
        size_t sb1 = 0;
        size_t sb2 = 0;
        cub::DeviceScan::ExclusiveSum(nullptr, sb0, nz_count, nz_offset, block_cap, stream);
        cub::DeviceScan::ExclusiveSum(nullptr, sb1, token_counts, token_offset, token_cap, stream);
        cub::DeviceScan::ExclusiveSum(nullptr, sb2, length, bit_offset, token_cap, stream);
        scan_bytes = std::max({sb0, sb1, sb2});
        if (scan_bytes > 0) {
            if (!alloc(&scan_temp, scan_bytes)) return false;
        }
        cudaStreamSynchronize(0);
        return true;
    }

    bool UploadTables(const EntropyTables& tables, cudaStream_t use) {
        const int cb = (tables.codebook >= 0 && tables.codebook < 8) ? tables.codebook : 0;
        active_cb = cb;
        if (run_bits[cb] && cb_run_len[cb] == tables.run_length && cb_val_len[cb] == tables.value_length) {
            return true;
        }
        if (run_bits[cb]) cudaFree(run_bits[cb]);
        if (run_size[cb]) cudaFree(run_size[cb]);
        if (run_count[cb]) cudaFree(run_count[cb]);
        if (value_entry[cb]) cudaFree(value_entry[cb]);
        run_bits[cb] = nullptr;
        run_size[cb] = nullptr;
        run_count[cb] = nullptr;
        value_entry[cb] = nullptr;
        cb_run_len[cb] = tables.run_length;
        cb_val_len[cb] = tables.value_length;
        if (!AllocDevice(reinterpret_cast<void**>(&run_bits[cb]),
                         sizeof(uint32_t) * tables.run_length, "entropy codebook")) return false;
        if (!AllocDevice(reinterpret_cast<void**>(&run_size[cb]),
                         sizeof(int16_t) * tables.run_length, "entropy codebook")) return false;
        if (!AllocDevice(reinterpret_cast<void**>(&run_count[cb]),
                         sizeof(int16_t) * tables.run_length, "entropy codebook")) return false;
        if (!AllocDevice(reinterpret_cast<void**>(&value_entry[cb]),
                         sizeof(uint32_t) * tables.value_length, "entropy codebook")) return false;
        if (cudaMemcpyAsync(run_bits[cb], tables.run_bits.data(), sizeof(uint32_t) * tables.run_length, cudaMemcpyHostToDevice, use) != cudaSuccess) return false;
        if (cudaMemcpyAsync(run_size[cb], tables.run_size.data(), sizeof(int16_t) * tables.run_length, cudaMemcpyHostToDevice, use) != cudaSuccess) return false;
        if (cudaMemcpyAsync(run_count[cb], tables.run_count.data(), sizeof(int16_t) * tables.run_length, cudaMemcpyHostToDevice, use) != cudaSuccess) return false;
        if (cudaMemcpyAsync(value_entry[cb], tables.value_entry.data(), sizeof(uint32_t) * tables.value_length, cudaMemcpyHostToDevice, use) != cudaSuccess) return false;
        return true;
    }

    void Release() {
        cudaFree(prefix); cudaFree(suffix); cudaFree(nz_count); cudaFree(all_zero); cudaFree(block_len);
        cudaFree(carry); cudaFree(nz_offset); cudaFree(local_pos); cudaFree(local_val);
        cudaFree(pos); cudaFree(val); cudaFree(owner); cudaFree(token_counts); cudaFree(token_offset);
        cudaFree(trailing); cudaFree(nnz); cudaFree(token_total); cudaFree(total_bits);
        cudaFree(tokens); cudaFree(code); cudaFree(length); cudaFree(bit_offset); cudaFree(words);
        cudaFree(tile_sums);
        for (int c = 0; c < 8; ++c) {
            cudaFree(run_bits[c]); cudaFree(run_size[c]); cudaFree(run_count[c]); cudaFree(value_entry[c]);
            run_bits[c] = value_entry[c] = nullptr;
            run_size[c] = run_count[c] = nullptr;
            cb_run_len[c] = cb_val_len[c] = 0;
        }
        cudaFree(scan_temp);
        if (pinned) cudaFreeHost(pinned);
        if (pinned_words) cudaFreeHost(pinned_words);
        pinned = nullptr;
        pinned_words = nullptr;
        prefix = suffix = nz_count = all_zero = block_len = carry = nz_offset = nullptr;
        local_pos = local_val = pos = val = owner = token_counts = token_offset = trailing = nnz = token_total = nullptr;
        total_bits = nullptr;
        tokens = code = length = bit_offset = words = tile_sums = nullptr;
        scan_temp = nullptr;
        scan_bytes = 0;
        active_cb = 0;
        active_segments = 0;
        launch_stream = nullptr;
        coeff_cap = block_cap = token_cap = 0;
        if (stream) cudaStreamDestroy(stream);
        stream = nullptr;
        for (auto& event : ev) {
            if (event) cudaEventDestroy(event);
            event = nullptr;
        }
    }
};

std::string DescribeWorkspaceSetupFailure(const Workspace& ws, int stream, const char* what) {
    if (ws.last_alloc_error == cudaSuccess) {
        // Non-allocation setup failure (for example an async codebook copy).
        return "GPU entropy stream " + std::to_string(stream) + " " + what + " setup failed";
    }
    return DescribeDeviceCapacityFailure("GPU entropy stream " + std::to_string(stream) + " " + what,
                                         ws.last_alloc_bytes, ws.last_alloc_error);
}

bool ScanInts(Workspace* ws, int* input, int* output, int count, cudaStream_t stream) {
    size_t bytes = 0;
    if (cub::DeviceScan::ExclusiveSum(nullptr, bytes, input, output, count, stream) != cudaSuccess) return false;
    if (bytes > ws->scan_bytes) {
        if (ws->scan_temp) cudaFree(ws->scan_temp);
        if (!ws->AllocDevice(&ws->scan_temp, bytes, "entropy scan workspace")) return false;
        ws->scan_bytes = bytes;
    }
    return cub::DeviceScan::ExclusiveSum(ws->scan_temp, bytes, input, output, count, stream) == cudaSuccess;
}

bool ScanBits(Workspace* ws, uint32_t* input, uint32_t* output, int count, cudaStream_t stream) {
    size_t bytes = 0;
    if (cub::DeviceScan::ExclusiveSum(nullptr, bytes, input, output, count, stream) != cudaSuccess) return false;
    if (bytes > ws->scan_bytes) {
        if (ws->scan_temp) cudaFree(ws->scan_temp);
        if (!ws->AllocDevice(&ws->scan_temp, bytes, "entropy scan workspace")) return false;
        ws->scan_bytes = bytes;
    }
    return cub::DeviceScan::ExclusiveSum(ws->scan_temp, bytes, input, output, count, stream) == cudaSuccess;
}

float Elapsed(cudaEvent_t a, cudaEvent_t b) {
    float ms = 0.f;
    cudaEventElapsedTime(&ms, a, b);
    return ms;
}

int ScanOccupancy() {
    int blocks = 0;
    cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks, BlockScanKernel, kBlock, 0);
    return blocks;
}

struct OccRegister {
    OccRegister() { g_entropy_occ_fn = ScanOccupancy; }
};

OccRegister g_occ_register;

struct GraphCacheKey {
    GpuEntropySession* session = nullptr;
    int count = 0;
    int streams = 0;
    CompressedD2HMode d2h_mode = CompressedD2HMode::FrameConsolidated;
    GpuEntropyKernelMode kernel_mode = GpuEntropyKernelMode::ReferencePhase2C;
    bool lpt_schedule = true;
    const int16_t* device_coeff[kMaxHighpassBands] = {};
    int width[kMaxHighpassBands] = {};
    int height[kMaxHighpassBands] = {};
    int pitch_px[kMaxHighpassBands] = {};
};

}  // namespace

struct GpuEntropySession {
    Workspace slots[kMaxEntropyStreams];
    cudaEvent_t last_frame_done_ev = nullptr;
    bool has_last_frame_done = false;
};

struct GpuEntropyFrameSlot {
    int max_width = 0;
    int max_height = 0;
    int active_bands = 0;
    int active_streams = 1;
    CompressedD2HMode d2h_mode = CompressedD2HMode::FrameConsolidated;
    GpuEntropyKernelMode kernel_mode = GpuEntropyKernelMode::ReferencePhase2C;
    bool record_substage_events = false;
    bool d2h_started = false;
    bool d2h_FirstBandRecorded = false;

    uint32_t band_word_offset[kMaxHighpassBands] = {};
    uint32_t band_word_cap[kMaxHighpassBands] = {};
    uint32_t* h_band_offsets = nullptr;
    uint32_t* d_band_offsets = nullptr;
    bool offsets_uploaded = false;
    size_t total_band_words_cap = 0;
    size_t consolidated_words_cap = 0;

    uint32_t* d_band_words = nullptr;
    uint32_t* d_consolidated_words = nullptr;
    uint32_t* h_consolidated_words = nullptr;
    uint32_t* h_band_words = nullptr;

    uint32_t* d_total_bits = nullptr;
    BandScalarMeta* h_scalars = nullptr;
    BandScalarMeta* d_scalars = nullptr;

    FrameCompactMeta* d_frame_meta = nullptr;
    FrameCompactMeta* h_frame_meta = nullptr;
    FrameCompactMeta* d_frame_meta_mapped = nullptr;

    cudaStream_t consolidate_stream = nullptr;
    cudaStream_t d2h_stream = nullptr;

    cudaEvent_t entropy_start_ev = nullptr;
    cudaEvent_t consolidate_start_ev = nullptr;
    cudaEvent_t entropy_done_ev = nullptr;
    cudaEvent_t d2h_start_ev = nullptr;
    cudaEvent_t d2h_done_ev = nullptr;
    cudaEvent_t graph_fork_ev = nullptr;

    cudaEvent_t stream_done_ev[kMaxEntropyStreams] = {};
    cudaEvent_t band_pack_done_ev[kMaxHighpassBands] = {};
    cudaEvent_t band_d2h_start_ev[kMaxHighpassBands] = {};
    cudaEvent_t band_d2h_done_ev[kMaxHighpassBands] = {};
    cudaEvent_t band_ev[kMaxHighpassBands][8] = {};

    int band_state[kMaxHighpassBands] = {};  // 0: gpu packing, 1: d2h in flight, 2: host ready, 3: claimed

    cudaGraph_t graph = nullptr;
    cudaGraphExec_t graph_exec = nullptr;
    GraphCacheKey graph_key{};
    int total_kernel_launches = 0;
    int graph_kernel_nodes = 0;
    int graph_total_nodes = 0;
    bool graph_used = false;
    bool graph_captured_this_frame = false;
    double host_enqueue_ms = 0.0;
    double graph_capture_ms = 0.0;

    int* h_overflow = nullptr;
    int* d_overflow = nullptr;
    uint32_t test_band_word_cap_override = 0;
    uint32_t test_consolidated_word_cap_override = 0;
    uint32_t active_consolidated_limit = 0;
    bool failed = false;
    std::string error;
};

int GpuEntropyScanOccupancy() {
    return g_entropy_occ_fn ? g_entropy_occ_fn() : 0;
}

GpuEntropySession* CreateGpuEntropySession() {
    auto* session = new GpuEntropySession();
    cudaEventCreateWithFlags(&session->last_frame_done_ev, cudaEventDisableTiming);
    return session;
}

void DestroyGpuEntropySession(GpuEntropySession* session) {
    if (!session) return;
    if (session->last_frame_done_ev) cudaEventDestroy(session->last_frame_done_ev);
    for (auto& slot : session->slots) slot.Release();
    delete session;
}

namespace {

bool CopyFromDevice(Workspace& ws, void* host, const void* device, size_t bytes) {
    if (bytes == 0) return true;
    if (cudaMemcpyAsync(host, device, bytes, cudaMemcpyDeviceToHost, ws.launch_stream) != cudaSuccess) return false;
    return cudaStreamSynchronize(ws.launch_stream) == cudaSuccess;
}

bool LaunchHighpass(Workspace& ws, const int16_t* device_coeff, int width, int height, int pitch_px,
                    const EntropyTables& tables, GpuEntropyStop stop, cudaStream_t stream) {
    const int virtual_len = height * pitch_px;
    const int blocks = (virtual_len + kBlock - 1) / kBlock;
    const cudaStream_t before = ws.stream;
    if (!ws.Ensure(virtual_len + 8, blocks + 1, virtual_len + 8, true) || !ws.pinned || !ws.pinned_words || !ws.stream) {
        g_launch_error = "workspace allocation failed";
        return false;
    }
    if (!stream || stream == before) stream = ws.stream;
    if (!ws.UploadTables(tables, stream)) {
        g_launch_error = "codebook upload failed";
        return false;
    }
    ws.launch_stream = stream;
    ws.active_stop = stop;
    ws.active_segments = virtual_len + 1;
    const int segments = ws.active_segments;
    const int tok_blocks = std::min((segments + 127) / 128, 2048);
    cudaEventRecord(ws.ev[0], stream);
    BlockScanKernel<<<blocks, kBlock, 0, stream>>>(device_coeff, width, height, pitch_px, ws.prefix, ws.suffix,
                                                   ws.nz_count, ws.all_zero, ws.block_len, ws.local_pos, ws.local_val);
    cudaEventRecord(ws.ev[1], stream);
    if (!ScanInts(&ws, ws.nz_count, ws.nz_offset, blocks, stream)) {
        g_launch_error = "nonzero prefix scan failed";
        return false;
    }
    MergeCarryKernel<<<1, kBlock, 0, stream>>>(ws.prefix, ws.suffix, ws.all_zero, ws.block_len, blocks, ws.carry, ws.trailing);
    GatherKernel<<<blocks, 128, 0, stream>>>(ws.local_pos, ws.local_val, ws.nz_count, ws.nz_offset, blocks,
                                             ws.pos, ws.val, ws.owner);
    StoreNzKernel<<<1, 1, 0, stream>>>(ws.nz_offset, ws.nz_count, blocks, ws.nnz);
    cudaEventRecord(ws.ev[2], stream);
    CountTokenKernel<<<tok_blocks, 128, 0, stream>>>(ws.pos, ws.owner, ws.nz_offset, ws.carry, ws.prefix,
                                                     ws.nnz, ws.trailing, ws.run_count[ws.active_cb], tables.run_length, ws.token_counts);
    if (!ScanInts(&ws, ws.token_counts, ws.token_offset, segments, stream)) {
        g_launch_error = "token prefix scan failed";
        return false;
    }
    StoreTokenTotalKernel<<<1, 1, 0, stream>>>(ws.token_offset, ws.token_counts, ws.nnz, ws.token_total);
    WriteTokenKernel<<<tok_blocks, 128, 0, stream>>>(ws.pos, ws.val, ws.owner, ws.nz_offset, ws.carry,
                                                     ws.prefix, ws.token_offset, ws.nnz, ws.trailing, ws.run_count[ws.active_cb],
                                                     tables.run_length, tables.value_length, ws.tokens);
    cudaEventRecord(ws.ev[3], stream);
    if (stop == GpuEntropyStop::Tokens) {
        cudaEventRecord(ws.ev[4], stream);
        cudaEventRecord(ws.ev[5], stream);
        cudaEventRecord(ws.ev[6], stream);
        cudaEventRecord(ws.ev[7], stream);
        if (cudaMemcpyAsync(&ws.pinned->token_total, ws.token_total, sizeof(int), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
            g_launch_error = "token count copy failed";
            return false;
        }
        return true;
    }
    CodebookKernel<<<tok_blocks, 128, 0, stream>>>(ws.tokens, ws.token_total, ws.run_bits[ws.active_cb], ws.run_size[ws.active_cb],
                                                   ws.value_entry[ws.active_cb], tables.band_end_code, tables.band_end_size,
                                                   ws.code, ws.length);
    cudaEventRecord(ws.ev[4], stream);
    if (stop == GpuEntropyStop::Codewords) {
        cudaEventRecord(ws.ev[5], stream);
        cudaEventRecord(ws.ev[6], stream);
        cudaEventRecord(ws.ev[7], stream);
        if (cudaMemcpyAsync(&ws.pinned->token_total, ws.token_total, sizeof(int), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
            g_launch_error = "token count copy failed";
            return false;
        }
        return true;
    }
    if (!ScanBits(&ws, ws.length, ws.bit_offset, segments, stream)) {
        g_launch_error = "bit-offset scan failed";
        return false;
    }
    StoreBitsKernel<<<1, 1, 0, stream>>>(ws.bit_offset, ws.length, ws.token_total, ws.total_bits);
    cudaEventRecord(ws.ev[5], stream);
    const int word_limit = (segments * 31 + 31) / 32;
    const int pack_blocks = std::min((word_limit + 127) / 128, 2048);
    PackKernel<<<pack_blocks, 128, 0, stream>>>(ws.code, ws.length, ws.bit_offset, ws.token_total, ws.total_bits, ws.words,
                                                static_cast<uint32_t>(std::max(word_limit, 1)), nullptr);
    cudaEventRecord(ws.ev[6], stream);
    cudaEventRecord(ws.ev[7], stream);
    if (cudaMemcpyAsync(&ws.pinned->token_total, ws.token_total, sizeof(int), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        g_launch_error = "token count copy failed";
        return false;
    }
    if (cudaMemcpyAsync(&ws.pinned->total_bits, ws.total_bits, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        g_launch_error = "bit count copy failed";
        return false;
    }
    return true;
}

bool LaunchHighpassIntoFrameSlot(Workspace& ws, GpuEntropyFrameSlot* fslot, int band_idx,
                                 const GpuBandRequest& req, const EntropyTables& tables,
                                 GpuEntropyKernelMode kernel_mode, bool record_substage,
                                 bool in_graph_capture, cudaStream_t stream,
                                 int* out_kernel_launches = nullptr) {
    const int virtual_len = req.height * req.pitch_px;
    const int blocks = (virtual_len + kBlock - 1) / kBlock;
    if (!in_graph_capture) {
        if (!ws.UploadTables(tables, stream)) {
            g_launch_error = "codebook upload failed";
            return false;
        }
    } else {
        ws.active_cb = (tables.codebook >= 0 && tables.codebook < 8) ? tables.codebook : 0;
    }
    const int segments = virtual_len + 1;
    const int tok_blocks = std::min((segments + 127) / 128, 2048);
    const int scan_tiles = (segments + 1023) / 1024;
    cudaEvent_t* bev = fslot->band_ev[band_idx];
    const bool rec = record_substage && !in_graph_capture;
    int launches = 0;

    if (rec) cudaEventRecord(bev[0], stream);
    if (kernel_mode == GpuEntropyKernelMode::ReferencePhase2C) {
        BlockScanKernel<<<blocks, kBlock, 0, stream>>>(req.device_coeff, req.width, req.height, req.pitch_px,
                                                       ws.prefix, ws.suffix, ws.nz_count, ws.all_zero,
                                                       ws.block_len, ws.local_pos, ws.local_val);
        launches += 1;
        if (rec) cudaEventRecord(bev[1], stream);
        if (!ScanInts(&ws, ws.nz_count, ws.nz_offset, blocks, stream)) {
            g_launch_error = "nonzero prefix scan failed";
            return false;
        }
        launches += 2;  // CUB DeviceScanInitKernel + DeviceScanKernel
        MergeCarryKernel<<<1, kBlock, 0, stream>>>(ws.prefix, ws.suffix, ws.all_zero, ws.block_len, blocks, ws.carry, ws.trailing);
        launches += 1;
        if (rec) cudaEventRecord(bev[2], stream);
        GatherKernel<<<blocks, 128, 0, stream>>>(ws.local_pos, ws.local_val, ws.nz_count, ws.nz_offset, blocks,
                                                 ws.pos, ws.val, ws.owner);
        StoreNzKernel<<<1, 1, 0, stream>>>(ws.nz_offset, ws.nz_count, blocks, ws.nnz);
        launches += 2;
        if (rec) cudaEventRecord(bev[3], stream);

        CountTokenKernel<<<tok_blocks, 128, 0, stream>>>(ws.pos, ws.owner, ws.nz_offset, ws.carry, ws.prefix,
                                                         ws.nnz, ws.trailing, ws.run_count[ws.active_cb], tables.run_length, ws.token_counts);
        launches += 1;
        if (!ScanInts(&ws, ws.token_counts, ws.token_offset, segments, stream)) {
            g_launch_error = "token prefix scan failed";
            return false;
        }
        launches += 2;  // CUB DeviceScanInitKernel + DeviceScanKernel
        StoreTokenTotalKernel<<<1, 1, 0, stream>>>(ws.token_offset, ws.token_counts, ws.nnz, ws.token_total);
        WriteTokenKernel<<<tok_blocks, 128, 0, stream>>>(ws.pos, ws.val, ws.owner, ws.nz_offset, ws.carry,
                                                         ws.prefix, ws.token_offset, ws.nnz, ws.trailing, ws.run_count[ws.active_cb],
                                                         tables.run_length, tables.value_length, ws.tokens);
        launches += 2;
        if (rec) cudaEventRecord(bev[4], stream);

        CodebookKernel<<<tok_blocks, 128, 0, stream>>>(ws.tokens, ws.token_total, ws.run_bits[ws.active_cb], ws.run_size[ws.active_cb],
                                                       ws.value_entry[ws.active_cb], tables.band_end_code, tables.band_end_size,
                                                       ws.code, ws.length);
        launches += 1;
        if (rec) cudaEventRecord(bev[5], stream);

        if (!ScanBits(&ws, ws.length, ws.bit_offset, segments, stream)) {
            g_launch_error = "bit-offset scan failed";
            return false;
        }
        launches += 2;  // CUB DeviceScanInitKernel + DeviceScanKernel
        StoreBandBitsAndTokensKernel<<<1, 1, 0, stream>>>(ws.bit_offset, ws.length, ws.token_total,
                                                          ws.total_bits, &fslot->d_total_bits[band_idx],
                                                          &fslot->d_scalars[band_idx]);
        launches += 1;
        if (rec) cudaEventRecord(bev[6], stream);
    } else if (kernel_mode == GpuEntropyKernelMode::Fused) {
        BlockScanWarpKernel<<<blocks, kBlock, 0, stream>>>(req.device_coeff, req.width, req.height, req.pitch_px,
                                                           ws.prefix, ws.suffix, ws.nz_count, ws.all_zero,
                                                           ws.block_len, ws.local_pos, ws.local_val);
        launches += 1;
        if (rec) cudaEventRecord(bev[1], stream);
        if (!ScanInts(&ws, ws.nz_count, ws.nz_offset, blocks, stream)) {
            g_launch_error = "nonzero prefix scan failed";
            return false;
        }
        launches += 2;  // CUB DeviceScanInitKernel + DeviceScanKernel
        MergeCarryAndStoreNzFusedKernel<<<1, kBlock, 0, stream>>>(ws.prefix, ws.suffix, ws.all_zero, ws.block_len,
                                                                  ws.nz_offset, ws.nz_count, blocks,
                                                                  ws.carry, ws.trailing, ws.nnz);
        launches += 1;
        if (rec) cudaEventRecord(bev[2], stream);
        GatherAndCountTokensFusedKernel<<<blocks, 128, 0, stream>>>(ws.local_pos, ws.local_val, ws.nz_count, ws.nz_offset,
                                                                    ws.carry, ws.prefix, ws.nnz, ws.trailing,
                                                                    ws.run_count[ws.active_cb], tables.run_length, blocks,
                                                                    ws.pos, ws.val, ws.token_counts);
        launches += 1;
        if (rec) cudaEventRecord(bev[3], stream);

        if (!ScanInts(&ws, ws.token_counts, ws.token_offset, segments, stream)) {
            g_launch_error = "token prefix scan failed";
            return false;
        }
        launches += 2;  // CUB DeviceScanInitKernel + DeviceScanKernel
        StoreTokenTotalKernel<<<1, 1, 0, stream>>>(ws.token_offset, ws.token_counts, ws.nnz, ws.token_total);
        TokenizeAndCodebookFusedKernel<<<tok_blocks, 128, 0, stream>>>(
            ws.pos, ws.val, ws.token_offset, ws.nnz, ws.trailing,
            ws.run_count[ws.active_cb], tables.run_length, tables.value_length,
            ws.run_bits[ws.active_cb], ws.run_size[ws.active_cb], ws.value_entry[ws.active_cb],
            tables.band_end_code, tables.band_end_size, ws.code, ws.length);
        launches += 2;
        if (rec) {
            cudaEventRecord(bev[4], stream);
            cudaEventRecord(bev[5], stream);
        }

        if (!ScanBits(&ws, ws.length, ws.bit_offset, segments, stream)) {
            g_launch_error = "bit-offset scan failed";
            return false;
        }
        launches += 2;  // CUB DeviceScanInitKernel + DeviceScanKernel
        StoreBandBitsAndTokensKernel<<<1, 1, 0, stream>>>(ws.bit_offset, ws.length, ws.token_total,
                                                          ws.total_bits, &fslot->d_total_bits[band_idx],
                                                          &fslot->d_scalars[band_idx]);
        launches += 1;
        if (rec) cudaEventRecord(bev[6], stream);
    } else {
        // GpuEntropyKernelMode::FusedScanOptimized (Phase 2D-C)
        BlockScanWarpKernel<<<blocks, kBlock, 0, stream>>>(req.device_coeff, req.width, req.height, req.pitch_px,
                                                           ws.prefix, ws.suffix, ws.nz_count, ws.all_zero,
                                                           ws.block_len, ws.local_pos, ws.local_val);
        launches += 1;
        if (rec) cudaEventRecord(bev[1], stream);

        MergeCarryAndScanNzFusedKernel<<<1, kBlock, 0, stream>>>(ws.prefix, ws.suffix, ws.all_zero, ws.block_len,
                                                                 ws.nz_count, blocks, ws.carry, ws.nz_offset,
                                                                 ws.trailing, ws.nnz);
        launches += 1;
        if (rec) cudaEventRecord(bev[2], stream);

        GatherAndCountTokensFusedKernel<<<blocks, 128, 0, stream>>>(ws.local_pos, ws.local_val, ws.nz_count, ws.nz_offset,
                                                                    ws.carry, ws.prefix, ws.nnz, ws.trailing,
                                                                    ws.run_count[ws.active_cb], tables.run_length, blocks,
                                                                    ws.pos, ws.val, ws.token_counts);
        launches += 1;
        if (rec) cudaEventRecord(bev[3], stream);

        ScanLocalTilesIntKernel<<<scan_tiles, 256, 0, stream>>>(ws.token_counts, ws.token_offset, ws.tile_sums, ws.nnz);
        ScanAddBlockOffsetsAndStoreTotalIntKernel<<<scan_tiles, 256, 0, stream>>>(ws.token_offset, ws.tile_sums, ws.nnz, ws.token_total);
        TokenizeAndCodebookFusedKernel<<<tok_blocks, 128, 0, stream>>>(
            ws.pos, ws.val, ws.token_offset, ws.nnz, ws.trailing,
            ws.run_count[ws.active_cb], tables.run_length, tables.value_length,
            ws.run_bits[ws.active_cb], ws.run_size[ws.active_cb], ws.value_entry[ws.active_cb],
            tables.band_end_code, tables.band_end_size, ws.code, ws.length);
        launches += 3;
        if (rec) {
            cudaEventRecord(bev[4], stream);
            cudaEventRecord(bev[5], stream);
        }

        ScanLocalTilesBitsKernel<<<scan_tiles, 256, 0, stream>>>(ws.length, ws.bit_offset, ws.tile_sums, ws.token_total);
        ScanAddBlockOffsetsAndStoreTotalBitsKernel<<<scan_tiles, 256, 0, stream>>>(
            ws.bit_offset, ws.tile_sums, ws.token_total,
            ws.total_bits, &fslot->d_total_bits[band_idx], &fslot->d_scalars[band_idx]);
        launches += 2;
        if (rec) cudaEventRecord(bev[6], stream);
    }

    uint32_t* dst_words = fslot->d_band_words + fslot->band_word_offset[band_idx];
    const int word_limit = static_cast<int>(fslot->band_word_cap[band_idx]);
    const int pack_blocks = std::min((word_limit + 127) / 128, 2048);
    PackKernel<<<pack_blocks, 128, 0, stream>>>(ws.code, ws.length, ws.bit_offset, ws.token_total, ws.total_bits, dst_words,
                                                static_cast<uint32_t>(std::max(word_limit, 1)), fslot->d_overflow);
    launches += 1;
    if (rec) cudaEventRecord(bev[7], stream);

    if (!in_graph_capture && fslot->d2h_mode == CompressedD2HMode::BandStreamed) {
        cudaEventRecord(fslot->band_pack_done_ev[band_idx], stream);
    }
    if (out_kernel_launches) *out_kernel_launches += launches;
    return true;
}

void FillGpuTiming(Workspace& ws, GpuBandResult* result) {
    result->timing.scan_ms = Elapsed(ws.ev[0], ws.ev[2]);
    result->timing.block_scan_ms = Elapsed(ws.ev[0], ws.ev[1]);
    result->timing.merge_carry_ms = Elapsed(ws.ev[1], ws.ev[2]);
    result->timing.token_ms = Elapsed(ws.ev[2], ws.ev[3]);
    const double gpu_total = Elapsed(ws.ev[0], ws.ev[7]);
    if (ws.active_stop == GpuEntropyStop::Full) {
        result->timing.codebook_ms = Elapsed(ws.ev[3], ws.ev[4]);
        result->timing.prefix_ms = Elapsed(ws.ev[4], ws.ev[5]);
        result->timing.pack_ms = Elapsed(ws.ev[5], ws.ev[6]);
        result->timing.total_ms = gpu_total;
    } else if (ws.active_stop == GpuEntropyStop::Codewords) {
        result->timing.codebook_ms = Elapsed(ws.ev[3], ws.ev[4]);
        result->timing.total_ms = gpu_total + result->timing.prefix_ms + result->timing.pack_ms;
    } else {
        result->timing.total_ms = gpu_total + result->timing.codebook_ms + result->timing.prefix_ms + result->timing.pack_ms;
    }
}

bool CollectHighpass(Workspace& ws, const EntropyTables& tables, bool keep_symbols, GpuBandResult* result) {
    if (!result) return false;
    *result = GpuBandResult{};
    if (!ws.launch_stream || !ws.pinned || !ws.pinned_words) {
        result->error = "gpu entropy workspace is not ready";
        return false;
    }
    if (cudaStreamSynchronize(ws.launch_stream) != cudaSuccess) {
        result->error = "gpu entropy kernel failed";
        return false;
    }
    const int token_total = ws.pinned->token_total;
    if (token_total < 0 || token_total + 1 > ws.active_segments || token_total > ws.token_cap) {
        result->error = "token expansion exceeded the band";
        return false;
    }
    result->tokens = token_total;
    const int codes = token_total + 1;
    if (ws.active_stop == GpuEntropyStop::Tokens) {
        if (!CopyFromDevice(ws, ws.pinned_words, ws.tokens, sizeof(uint32_t) * static_cast<size_t>(token_total))) {
            result->error = "token download failed";
            return false;
        }
        EntropySymbols cpu;
        if (!FinishFromTokens(tables, token_total ? ws.pinned_words : nullptr, token_total, &cpu)) {
            result->error = cpu.error.empty() ? "cpu token finish failed" : cpu.error;
            return false;
        }
        result->coeff_payload = std::move(cpu.coeff_bytes);
        result->total_bits = cpu.total_bits;
        result->timing.codebook_ms = cpu.timing.codebook_ms;
        result->timing.prefix_ms = cpu.timing.prefix_ms;
        result->timing.pack_ms = cpu.timing.pack_ms;
        if (keep_symbols) {
            result->tokens_packed.assign(ws.pinned_words, ws.pinned_words + token_total);
            result->codeword = std::move(cpu.codeword);
            result->bit_length = std::move(cpu.bit_length);
            result->bit_offset = std::move(cpu.bit_offset);
        }
    } else if (ws.active_stop == GpuEntropyStop::Codewords) {
        std::vector<uint32_t> code(static_cast<size_t>(codes));
        std::vector<uint32_t> wide(static_cast<size_t>(codes));
        if (!CopyFromDevice(ws, ws.pinned_words, ws.code, sizeof(uint32_t) * static_cast<size_t>(codes))) {
            result->error = "codeword download failed";
            return false;
        }
        std::memcpy(code.data(), ws.pinned_words, sizeof(uint32_t) * static_cast<size_t>(codes));
        if (!CopyFromDevice(ws, ws.pinned_words, ws.length, sizeof(uint32_t) * static_cast<size_t>(codes))) {
            result->error = "bit length download failed";
            return false;
        }
        std::memcpy(wide.data(), ws.pinned_words, sizeof(uint32_t) * static_cast<size_t>(codes));
        std::vector<uint16_t> bits(static_cast<size_t>(codes));
        for (int i = 0; i < codes; ++i) bits[static_cast<size_t>(i)] = static_cast<uint16_t>(wide[static_cast<size_t>(i)]);
        EntropySymbols cpu;
        if (!FinishFromCodewords(code.data(), bits.data(), codes, &cpu)) {
            result->error = cpu.error.empty() ? "cpu codeword finish failed" : cpu.error;
            return false;
        }
        result->coeff_payload = std::move(cpu.coeff_bytes);
        result->total_bits = cpu.total_bits;
        result->timing.prefix_ms = cpu.timing.prefix_ms;
        result->timing.pack_ms = cpu.timing.pack_ms;
        if (keep_symbols) {
            if (token_total > 0 &&
                CopyFromDevice(ws, ws.pinned_words, ws.tokens, sizeof(uint32_t) * static_cast<size_t>(token_total))) {
                result->tokens_packed.assign(ws.pinned_words, ws.pinned_words + token_total);
            }
            result->codeword = std::move(code);
            result->bit_length = std::move(bits);
            result->bit_offset = std::move(cpu.bit_offset);
        }
    } else {
        const uint32_t total_bits = ws.pinned->total_bits;
        const int word_count = static_cast<int>((total_bits + 31u) >> 5);
        if (word_count < 0 || word_count > ws.token_cap) {
            result->error = "packed payload exceeded the workspace";
            return false;
        }
        if (!CopyFromDevice(ws, ws.pinned_words, ws.words, sizeof(uint32_t) * static_cast<size_t>(word_count))) {
            result->error = "payload download failed";
            return false;
        }
        result->coeff_payload.resize(sizeof(uint32_t) * static_cast<size_t>(word_count));
        if (word_count > 0) std::memcpy(result->coeff_payload.data(), ws.pinned_words, result->coeff_payload.size());
        result->total_bits = static_cast<int>(total_bits);
        if (keep_symbols) {
            if (token_total > 0) {
                if (!CopyFromDevice(ws, ws.pinned_words, ws.tokens, sizeof(uint32_t) * static_cast<size_t>(token_total))) {
                    result->error = "token download failed";
                    return false;
                }
                result->tokens_packed.assign(ws.pinned_words, ws.pinned_words + token_total);
            }
            if (!CopyFromDevice(ws, ws.pinned_words, ws.code, sizeof(uint32_t) * static_cast<size_t>(codes))) {
                result->error = "codeword download failed";
                return false;
            }
            result->codeword.assign(ws.pinned_words, ws.pinned_words + codes);
            if (!CopyFromDevice(ws, ws.pinned_words, ws.length, sizeof(uint32_t) * static_cast<size_t>(codes))) {
                result->error = "bit length download failed";
                return false;
            }
            result->bit_length.resize(static_cast<size_t>(codes));
            for (int i = 0; i < codes; ++i) result->bit_length[static_cast<size_t>(i)] = static_cast<uint16_t>(ws.pinned_words[i]);
            if (!CopyFromDevice(ws, ws.pinned_words, ws.bit_offset, sizeof(uint32_t) * static_cast<size_t>(codes))) {
                result->error = "bit offset download failed";
                return false;
            }
            result->bit_offset.assign(ws.pinned_words, ws.pinned_words + codes);
        }
    }
    result->coeff_bytes = static_cast<int>(result->coeff_payload.size());
    FillGpuTiming(ws, result);
    result->ok = true;
    return true;
}

}  // namespace

bool EncodeHighpassOnGpu(GpuEntropySession* session, const int16_t* device_coeff, int width, int height,
                         int pitch_px, const EntropyTables& tables, bool keep_symbols, GpuBandResult* result,
                         GpuEntropyStop stop) {
    if (!result) return false;
    *result = GpuBandResult{};
    if (!session || !device_coeff || width <= 0 || height <= 0 || pitch_px < width || tables.peaks) {
        result->error = "gpu entropy input is incomplete";
        return false;
    }
    Workspace& ws = session->slots[0];
    g_launch_error = nullptr;
    if (!LaunchHighpass(ws, device_coeff, width, height, pitch_px, tables, stop, ws.stream)) {
        const char* cuda_error = cudaGetErrorString(cudaGetLastError());
        result->error = std::string("gpu entropy launch failed: ") + (g_launch_error ? g_launch_error : "unknown") +
                        " (" + (cuda_error ? cuda_error : "") + ")";
        return false;
    }
    return CollectHighpass(ws, tables, keep_symbols, result);
}

bool EncodeHighpassBatch(GpuEntropySession* session, const GpuBandRequest* requests, GpuBandResult* results,
                         int count, const EntropyTables* tables, GpuEntropyDispatch dispatch, int streams,
                         EntropyTiming* wall, GpuEntropyStop stop) {
    if (!session || !requests || !results || !tables || count <= 0) return false;
    if (streams < 1) streams = 1;
    if (streams > 8) streams = 8;
    int slot_count = dispatch == GpuEntropyDispatch::OneStream ? 1 : streams;
    int max_virtual = 1;
    for (int i = 0; i < count; ++i) {
        if (!requests[i].device_coeff || requests[i].width <= 0 || requests[i].height <= 0 || requests[i].pitch_px < requests[i].width) {
            return false;
        }
        max_virtual = std::max(max_virtual, requests[i].height * requests[i].pitch_px);
    }
    const int blocks = (max_virtual + kBlock - 1) / kBlock;
    for (int slot = 0; slot < slot_count; ++slot) {
        if (!session->slots[slot].Ensure(max_virtual + 8, blocks + 1, max_virtual + 8, true)) return false;
        if (!session->slots[slot].UploadTables(tables[0], session->slots[slot].stream)) return false;
        if (cudaStreamSynchronize(session->slots[slot].stream) != cudaSuccess) return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    auto launch_one = [&](int slot, int index, cudaStream_t stream) {
        return LaunchHighpass(session->slots[slot], requests[index].device_coeff, requests[index].width, requests[index].height,
                              requests[index].pitch_px, tables[index], stop, stream);
    };
    auto collect_one = [&](int slot, int index) {
        return CollectHighpass(session->slots[slot], tables[index], false, &results[index]);
    };
    if (dispatch == GpuEntropyDispatch::MultiStream) {
        std::vector<int> owner(static_cast<size_t>(slot_count), -1);
        for (int i = 0; i < count; ++i) {
            const int slot = i % slot_count;
            if (owner[static_cast<size_t>(slot)] >= 0) ok = collect_one(slot, owner[static_cast<size_t>(slot)]) && ok;
            if (!launch_one(slot, i, session->slots[slot].stream)) {
                ok = false;
                cudaStreamSynchronize(session->slots[slot].stream);
                owner[static_cast<size_t>(slot)] = -1;
                continue;
            }
            owner[static_cast<size_t>(slot)] = i;
        }
        for (int slot = 0; slot < slot_count; ++slot) {
            if (owner[static_cast<size_t>(slot)] >= 0) ok = collect_one(slot, owner[static_cast<size_t>(slot)]) && ok;
        }
    } else if (dispatch == GpuEntropyDispatch::Batched) {
        cudaStream_t stream = session->slots[0].stream;
        for (int base = 0; base < count; base += slot_count) {
            const int wave = std::min(slot_count, count - base);
            for (int slot = 0; slot < wave; ++slot) ok = launch_one(slot, base + slot, stream) && ok;
            if (cudaStreamSynchronize(stream) != cudaSuccess) ok = false;
            for (int slot = 0; slot < wave; ++slot) ok = collect_one(slot, base + slot) && ok;
        }
    } else {
        for (int i = 0; i < count; ++i) {
            ok = launch_one(0, i, session->slots[0].stream) && ok;
            ok = collect_one(0, i) && ok;
        }
    }
    if (wall) {
        *wall = EntropyTiming{};
        wall->total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < count; ++i) {
            wall->scan_ms += results[i].timing.scan_ms;
            wall->block_scan_ms += results[i].timing.block_scan_ms;
            wall->merge_carry_ms += results[i].timing.merge_carry_ms;
            wall->token_ms += results[i].timing.token_ms;
            wall->codebook_ms += results[i].timing.codebook_ms;
            wall->prefix_ms += results[i].timing.prefix_ms;
            wall->pack_ms += results[i].timing.pack_ms;
        }
    }
    return ok;
}

GpuEntropyFrameSlot* CreateGpuEntropyFrameSlot(int max_width, int max_height) {
    g_slot_creation_error.clear();
    if (max_width <= 0 || max_height <= 0) return nullptr;
    auto* slot = new GpuEntropyFrameSlot();
    slot->max_width = max_width;
    slot->max_height = max_height;

    // Highpass virtual coefficients for 4:2:2 are below 2*W*H. Each band of N
    // coefficients is proven to need at most ceil(31*(N+1)/32) <= N+1 packed
    // words when the codebooks pass ValidateEntropyTablesForWordBound. The
    // per-band queue cap is N+4096, which is above that bound. The arena below
    // holds every band at that cap. The coefficient-word buffer does not store
    // band headers; those are built on the host.
    const size_t max_pixels = static_cast<size_t>(max_width) * static_cast<size_t>(max_height);
    const size_t word_mult = 2u;
    slot->total_band_words_cap = max_pixels * word_mult + static_cast<size_t>(kMaxHighpassBands) * 32768u;
    slot->consolidated_words_cap = max_pixels * word_mult + 262144u;

    auto fail = [&]() {
        DestroyGpuEntropyFrameSlot(slot);
        return static_cast<GpuEntropyFrameSlot*>(nullptr);
    };

    auto fail_device = [&](const char* what, size_t bytes, cudaError_t error) {
        g_slot_creation_error = DescribeDeviceCapacityFailure(what, bytes, error);
        return fail();
    };
    const size_t band_words_bytes = sizeof(uint32_t) * slot->total_band_words_cap;
    const size_t consolidated_bytes = sizeof(uint32_t) * slot->consolidated_words_cap;
    const size_t total_bits_bytes = sizeof(uint32_t) * kMaxHighpassBands;
    if (cudaMalloc(&slot->d_band_words, band_words_bytes) != cudaSuccess) {
        return fail_device("GPU entropy band arena", band_words_bytes, cudaGetLastError());
    }
    if (cudaMalloc(&slot->d_consolidated_words, consolidated_bytes) != cudaSuccess) {
        return fail_device("GPU entropy consolidated arena", consolidated_bytes, cudaGetLastError());
    }
    if (cudaMalloc(&slot->d_total_bits, total_bits_bytes) != cudaSuccess) {
        return fail_device("GPU entropy band metadata", total_bits_bytes, cudaGetLastError());
    }
    if (cudaMalloc(&slot->d_frame_meta, sizeof(FrameCompactMeta)) != cudaSuccess) {
        return fail_device("GPU entropy frame metadata", sizeof(FrameCompactMeta), cudaGetLastError());
    }
    cudaMemset(slot->d_band_words, 0, sizeof(uint32_t) * slot->total_band_words_cap);
    cudaMemset(slot->d_consolidated_words, 0, sizeof(uint32_t) * slot->consolidated_words_cap);
    cudaMemset(slot->d_total_bits, 0, sizeof(uint32_t) * kMaxHighpassBands);
    cudaMemset(slot->d_frame_meta, 0, sizeof(FrameCompactMeta));

    if (cudaHostAlloc(&slot->h_consolidated_words, sizeof(uint32_t) * slot->consolidated_words_cap, cudaHostAllocDefault) != cudaSuccess) return fail();
    if (cudaHostAlloc(&slot->h_band_words, sizeof(uint32_t) * slot->total_band_words_cap, cudaHostAllocDefault) != cudaSuccess) return fail();
    if (cudaHostAlloc(&slot->h_band_offsets, sizeof(uint32_t) * kMaxHighpassBands, cudaHostAllocMapped) != cudaSuccess) return fail();
    if (cudaHostGetDevicePointer(&slot->d_band_offsets, slot->h_band_offsets, 0) != cudaSuccess) return fail();
    if (cudaHostAlloc(&slot->h_scalars, sizeof(BandScalarMeta) * kMaxHighpassBands, cudaHostAllocMapped) != cudaSuccess) return fail();
    if (cudaHostGetDevicePointer(&slot->d_scalars, slot->h_scalars, 0) != cudaSuccess) return fail();
    if (cudaHostAlloc(&slot->h_frame_meta, sizeof(FrameCompactMeta), cudaHostAllocMapped) != cudaSuccess) return fail();
    if (cudaHostGetDevicePointer(&slot->d_frame_meta_mapped, slot->h_frame_meta, 0) != cudaSuccess) return fail();
    if (cudaHostAlloc(&slot->h_overflow, sizeof(int), cudaHostAllocMapped) != cudaSuccess) return fail();
    if (cudaHostGetDevicePointer(&slot->d_overflow, slot->h_overflow, 0) != cudaSuccess) return fail();
    *slot->h_overflow = 0;
    std::memset(slot->h_consolidated_words, 0, sizeof(uint32_t) * slot->consolidated_words_cap);
    std::memset(slot->h_band_words, 0, sizeof(uint32_t) * slot->total_band_words_cap);
    std::memset(slot->h_band_offsets, 0, sizeof(uint32_t) * kMaxHighpassBands);
    std::memset(slot->h_scalars, 0, sizeof(BandScalarMeta) * kMaxHighpassBands);
    std::memset(slot->h_frame_meta, 0, sizeof(FrameCompactMeta));
    cudaStreamSynchronize(0);

    if (cudaStreamCreateWithFlags(&slot->consolidate_stream, cudaStreamNonBlocking) != cudaSuccess) return fail();
    if (cudaStreamCreateWithFlags(&slot->d2h_stream, cudaStreamNonBlocking) != cudaSuccess) return fail();

    if (cudaEventCreate(&slot->entropy_start_ev) != cudaSuccess) return fail();
    if (cudaEventCreate(&slot->consolidate_start_ev) != cudaSuccess) return fail();
    if (cudaEventCreateWithFlags(&slot->entropy_done_ev, cudaEventBlockingSync) != cudaSuccess) return fail();
    if (cudaEventCreate(&slot->d2h_start_ev) != cudaSuccess) return fail();
    if (cudaEventCreateWithFlags(&slot->d2h_done_ev, cudaEventBlockingSync) != cudaSuccess) return fail();
    if (cudaEventCreateWithFlags(&slot->graph_fork_ev, cudaEventDisableTiming) != cudaSuccess) return fail();

    for (int s = 0; s < kMaxEntropyStreams; ++s) {
        if (cudaEventCreateWithFlags(&slot->stream_done_ev[s], cudaEventDisableTiming) != cudaSuccess) return fail();
    }
    for (int b = 0; b < kMaxHighpassBands; ++b) {
        if (cudaEventCreateWithFlags(&slot->band_pack_done_ev[b], cudaEventDisableTiming | cudaEventBlockingSync) != cudaSuccess) return fail();
        if (cudaEventCreate(&slot->band_d2h_start_ev[b]) != cudaSuccess) return fail();
        if (cudaEventCreateWithFlags(&slot->band_d2h_done_ev[b], cudaEventBlockingSync) != cudaSuccess) return fail();
        for (int e = 0; e < 8; ++e) {
            if (cudaEventCreate(&slot->band_ev[b][e]) != cudaSuccess) return fail();
        }
    }
    return slot;
}

void DestroyGpuEntropyFrameSlot(GpuEntropyFrameSlot* slot) {
    if (!slot) return;
    if (slot->graph_exec) cudaGraphExecDestroy(slot->graph_exec);
    if (slot->graph) cudaGraphDestroy(slot->graph);
    cudaFree(slot->d_band_words);
    cudaFree(slot->d_consolidated_words);
    cudaFree(slot->d_total_bits);
    cudaFree(slot->d_frame_meta);
    if (slot->h_consolidated_words) cudaFreeHost(slot->h_consolidated_words);
    if (slot->h_band_words) cudaFreeHost(slot->h_band_words);
    if (slot->h_band_offsets) cudaFreeHost(slot->h_band_offsets);
    if (slot->h_scalars) cudaFreeHost(slot->h_scalars);
    if (slot->h_frame_meta) cudaFreeHost(slot->h_frame_meta);
    if (slot->h_overflow) cudaFreeHost(slot->h_overflow);
    if (slot->consolidate_stream) cudaStreamDestroy(slot->consolidate_stream);
    if (slot->d2h_stream) cudaStreamDestroy(slot->d2h_stream);
    if (slot->entropy_start_ev) cudaEventDestroy(slot->entropy_start_ev);
    if (slot->consolidate_start_ev) cudaEventDestroy(slot->consolidate_start_ev);
    if (slot->entropy_done_ev) cudaEventDestroy(slot->entropy_done_ev);
    if (slot->d2h_start_ev) cudaEventDestroy(slot->d2h_start_ev);
    if (slot->d2h_done_ev) cudaEventDestroy(slot->d2h_done_ev);
    if (slot->graph_fork_ev) cudaEventDestroy(slot->graph_fork_ev);
    for (int s = 0; s < kMaxEntropyStreams; ++s) {
        if (slot->stream_done_ev[s]) cudaEventDestroy(slot->stream_done_ev[s]);
    }
    for (int b = 0; b < kMaxHighpassBands; ++b) {
        if (slot->band_pack_done_ev[b]) cudaEventDestroy(slot->band_pack_done_ev[b]);
        if (slot->band_d2h_start_ev[b]) cudaEventDestroy(slot->band_d2h_start_ev[b]);
        if (slot->band_d2h_done_ev[b]) cudaEventDestroy(slot->band_d2h_done_ev[b]);
        for (int e = 0; e < 8; ++e) {
            if (slot->band_ev[b][e]) cudaEventDestroy(slot->band_ev[b][e]);
        }
    }
    delete slot;
}

bool QueueGpuEntropyFrame(GpuEntropySession* session, GpuEntropyFrameSlot* slot,
                          const GpuBandRequest* requests, int count,
                          const EntropyTables* tables, const GpuEntropyFrameConfig& cfg,
                          void* wait_wavelet_event,
                          void* const* wait_band_events) {
    if (!session || !slot || !requests || !tables || count <= 0 || count > kMaxHighpassBands) return false;
    const auto t_enqueue0 = std::chrono::steady_clock::now();
    const int streams = std::clamp(cfg.entropy_streams, 1, kMaxEntropyStreams);
    slot->active_bands = count;
    slot->active_streams = streams;
    slot->d2h_mode = cfg.d2h_mode;
    slot->kernel_mode = cfg.kernel_mode;
    slot->record_substage_events = cfg.record_substage_events && !cfg.use_cuda_graph;
    slot->d2h_started = false;
    slot->d2h_FirstBandRecorded = false;
    slot->graph_used = cfg.use_cuda_graph;
    slot->graph_captured_this_frame = false;
    slot->graph_capture_ms = 0.0;
    slot->failed = false;
    slot->error.clear();
    if (slot->h_overflow) *slot->h_overflow = 0;

    uint32_t running_offset = 0;
    for (int b = 0; b < count; ++b) {
        std::string why;
        if (!ValidateEntropyTablesForWordBound(tables[b], &why)) {
            slot->failed = true;
            slot->error = why.empty() ? "entropy codebook violates the word-capacity proof" : why;
            return false;
        }
        const int vlen = requests[b].height * requests[b].pitch_px;
        const int proven = EntropyPackedWordUpperBound(vlen);
        uint32_t cap = static_cast<uint32_t>(vlen) + 4096u;
        if (slot->test_band_word_cap_override > 0) cap = slot->test_band_word_cap_override;
        else if (cap < static_cast<uint32_t>(proven)) {
            slot->failed = true;
            slot->error = "band word allocation is below the proven upper bound";
            return false;
        }
        if (static_cast<size_t>(running_offset) + cap > slot->total_band_words_cap) {
            slot->failed = true;
            slot->error = "sum of per-band word caps exceeds the frame arena";
            return false;
        }
        slot->band_word_offset[b] = running_offset;
        slot->h_band_offsets[b] = running_offset;
        slot->band_word_cap[b] = cap;
        slot->band_state[b] = 0;
        slot->h_scalars[b].token_total = 0;
        slot->h_scalars[b].total_bits = 0;
        running_offset += cap;
    }
    uint32_t consolidated_limit = static_cast<uint32_t>(
        std::min<size_t>(slot->consolidated_words_cap, 0xffffffffu));
    if (slot->test_consolidated_word_cap_override > 0) {
        consolidated_limit = slot->test_consolidated_word_cap_override;
    }
    slot->active_consolidated_limit = consolidated_limit;

    // Build per-stream band schedule (LPT load balancing across `streams`).
    std::vector<int> order(static_cast<size_t>(count));
    std::iota(order.begin(), order.end(), 0);
    if (cfg.lpt_schedule && streams > 1) {
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const int ca = requests[a].width * requests[a].height;
            const int cb = requests[b].width * requests[b].height;
            return ca > cb;
        });
    }

    std::vector<int> stream_bands[kMaxEntropyStreams];
    int64_t stream_load[kMaxEntropyStreams] = {};
    int stream_max_vlen[kMaxEntropyStreams] = {};
    for (int b : order) {
        int best_s = 0;
        if (cfg.lpt_schedule && streams > 1) {
            for (int s = 1; s < streams; ++s) {
                if (stream_load[s] < stream_load[best_s]) best_s = s;
            }
        } else {
            best_s = b % streams;
        }
        const int vlen = requests[b].height * requests[b].pitch_px;
        stream_bands[best_s].push_back(b);
        stream_load[best_s] += vlen;
        stream_max_vlen[best_s] = std::max(stream_max_vlen[best_s], vlen);
    }

    for (int s = 0; s < streams; ++s) {
        if (stream_bands[s].empty()) continue;
        const int mv = stream_max_vlen[s];
        const int blocks = (mv + kBlock - 1) / kBlock;
        if (!session->slots[s].Ensure(mv + 8, blocks + 1, mv + 8, false)) {
            slot->failed = true;
            const char* what = session->slots[s].last_alloc_what ? session->slots[s].last_alloc_what : "entropy workspace";
            slot->error = DescribeWorkspaceSetupFailure(session->slots[s], s, what);
            return false;
        }
        for (int b : stream_bands[s]) {
            const int cb = (tables[b].codebook >= 0 && tables[b].codebook < 8) ? tables[b].codebook : 0;
            const bool already_uploaded = session->slots[s].run_bits[cb] &&
                                          session->slots[s].cb_run_len[cb] == tables[b].run_length &&
                                          session->slots[s].cb_val_len[cb] == tables[b].value_length;
            if (!session->slots[s].UploadTables(tables[b], session->slots[s].stream)) {
                slot->failed = true;
                const char* what = session->slots[s].last_alloc_what ? session->slots[s].last_alloc_what : "entropy codebook";
                slot->error = DescribeWorkspaceSetupFailure(session->slots[s], s, what);
                return false;
            }
            if (!already_uploaded && cfg.use_cuda_graph) {
                cudaStreamSynchronize(session->slots[s].stream);
            }
        }
    }

    auto wait_ev = static_cast<cudaEvent_t>(wait_wavelet_event);

    if (cfg.use_cuda_graph) {
        GraphCacheKey key{};
        key.session = session;
        key.count = count;
        key.streams = streams;
        key.d2h_mode = cfg.d2h_mode;
        key.kernel_mode = cfg.kernel_mode;
        key.lpt_schedule = cfg.lpt_schedule;
        for (int b = 0; b < count; ++b) {
            key.device_coeff[b] = requests[b].device_coeff;
            key.width[b] = requests[b].width;
            key.height[b] = requests[b].height;
            key.pitch_px[b] = requests[b].pitch_px;
        }

        const bool cache_hit = (slot->graph_exec != nullptr) &&
                               (std::memcmp(&slot->graph_key, &key, sizeof(GraphCacheKey)) == 0);
        if (!cache_hit) {
            const auto t_cap0 = std::chrono::steady_clock::now();
            if (slot->graph_exec) {
                cudaGraphExecDestroy(slot->graph_exec);
                slot->graph_exec = nullptr;
            }
            if (slot->graph) {
                cudaGraphDestroy(slot->graph);
                slot->graph = nullptr;
            }

            if (cudaStreamBeginCapture(slot->consolidate_stream, cudaStreamCaptureModeThreadLocal) != cudaSuccess) {
                return false;
            }
            // Cleared on every replay. Quantizer tables are not graph nodes: wavelet
            // quantization is launched outside this capture, and the graph reads the
            // already-quantized coefficient buffers at the fixed device pointers in
            // GraphCacheKey. FSratelimiter changes do not recapture this graph.
            cudaMemsetAsync(slot->d_overflow, 0, sizeof(int), slot->consolidate_stream);
            cudaEventRecord(slot->graph_fork_ev, slot->consolidate_stream);

            int launches = 0;
            for (int s = 0; s < streams; ++s) {
                if (stream_bands[s].empty()) continue;
                cudaStream_t st = session->slots[s].stream;
                cudaStreamWaitEvent(st, slot->graph_fork_ev, 0);
                for (int b : stream_bands[s]) {
                    if (!LaunchHighpassIntoFrameSlot(session->slots[s], slot, b, requests[b], tables[b],
                                                     cfg.kernel_mode, false, true, st, &launches)) {
                        cudaGraph_t abandoned = nullptr;
                        cudaStreamEndCapture(slot->consolidate_stream, &abandoned);
                        if (abandoned) cudaGraphDestroy(abandoned);
                        slot->failed = true;
                        const char* what = session->slots[s].last_alloc_what
                            ? session->slots[s].last_alloc_what : "entropy workspace";
                        slot->error = DescribeWorkspaceSetupFailure(session->slots[s], s, what);
                        return false;
                    }
                }
                cudaEventRecord(slot->stream_done_ev[s], st);
                cudaStreamWaitEvent(slot->consolidate_stream, slot->stream_done_ev[s], 0);
            }

            if (cfg.d2h_mode == CompressedD2HMode::FrameConsolidated) {
                ComputeFrameLayoutKernel<<<1, 32, 0, slot->consolidate_stream>>>(
                    slot->d_total_bits, count, slot->d_frame_meta, slot->d_frame_meta_mapped,
                    slot->active_consolidated_limit, slot->d_overflow);
                const dim3 grid(64, static_cast<unsigned int>(count));
                ConsolidateFrameWordsKernel<<<grid, 256, 0, slot->consolidate_stream>>>(
                    slot->d_band_words, slot->d_band_offsets, slot->d_frame_meta, slot->d_consolidated_words, count,
                    slot->active_consolidated_limit, slot->d_overflow);
                launches += 2;
            }

            if (cudaStreamEndCapture(slot->consolidate_stream, &slot->graph) != cudaSuccess || !slot->graph) {
                return false;
            }

            size_t num_nodes = 0;
            cudaGraphGetNodes(slot->graph, nullptr, &num_nodes);
            int k_nodes = 0;
            if (num_nodes > 0) {
                std::vector<cudaGraphNode_t> nodes(num_nodes);
                if (cudaGraphGetNodes(slot->graph, nodes.data(), &num_nodes) == cudaSuccess) {
                    for (size_t ni = 0; ni < num_nodes; ++ni) {
                        cudaGraphNodeType ntype{};
                        if (cudaGraphNodeGetType(nodes[ni], &ntype) == cudaSuccess &&
                            ntype == cudaGraphNodeTypeKernel) {
                            ++k_nodes;
                        }
                    }
                }
            }

            if (cudaGraphInstantiate(&slot->graph_exec, slot->graph, 0) != cudaSuccess || !slot->graph_exec) {
                return false;
            }

            slot->graph_key = key;
            slot->total_kernel_launches = launches;
            slot->graph_kernel_nodes = k_nodes;
            slot->graph_total_nodes = static_cast<int>(num_nodes);
            slot->graph_captured_this_frame = true;
            slot->graph_capture_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_cap0).count();
        }

        if (wait_ev) {
            cudaStreamWaitEvent(slot->consolidate_stream, wait_ev, 0);
        }
        if (wait_band_events) {
            for (int b = 0; b < count; ++b) {
                if (wait_band_events[b]) {
                    cudaStreamWaitEvent(slot->consolidate_stream, static_cast<cudaEvent_t>(wait_band_events[b]), 0);
                }
            }
        }
        if (session->has_last_frame_done && session->last_frame_done_ev) {
            cudaStreamWaitEvent(slot->consolidate_stream, session->last_frame_done_ev, 0);
        }
        for (int s = 0; s < streams; ++s) {
            if (stream_bands[s].empty()) continue;
            // Ensure previous frame's entropy work on worker streams has completed before graph launch
            cudaEventRecord(slot->stream_done_ev[s], session->slots[s].stream);
            cudaStreamWaitEvent(slot->consolidate_stream, slot->stream_done_ev[s], 0);
        }

        cudaEventRecord(slot->entropy_start_ev, slot->consolidate_stream);
        if (cudaGraphLaunch(slot->graph_exec, slot->consolidate_stream) != cudaSuccess) {
            return false;
        }
        cudaEventRecord(slot->consolidate_start_ev, slot->consolidate_stream);
        cudaEventRecord(slot->entropy_done_ev, slot->consolidate_stream);
        if (session->last_frame_done_ev) {
            cudaEventRecord(session->last_frame_done_ev, slot->consolidate_stream);
            session->has_last_frame_done = true;
        }
        if (cfg.d2h_mode == CompressedD2HMode::BandStreamed) {
            for (int b = 0; b < count; ++b) {
                cudaEventRecord(slot->band_pack_done_ev[b], slot->consolidate_stream);
            }
        }
        for (int s = 0; s < streams; ++s) {
            if (stream_bands[s].empty()) continue;
            cudaStreamWaitEvent(session->slots[s].stream, slot->entropy_done_ev, 0);
        }
        slot->host_enqueue_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_enqueue0).count();
        return true;
    }

    int launches = 0;
    cudaMemsetAsync(slot->d_overflow, 0, sizeof(int), slot->consolidate_stream);
    cudaEventRecord(slot->graph_fork_ev, slot->consolidate_stream);
    for (int s = 0; s < streams; ++s) {
        if (stream_bands[s].empty()) continue;
        cudaStream_t st = session->slots[s].stream;
        cudaStreamWaitEvent(st, slot->graph_fork_ev, 0);
        if (wait_ev) cudaStreamWaitEvent(st, wait_ev, 0);
        if (session->has_last_frame_done && session->last_frame_done_ev) {
            cudaStreamWaitEvent(st, session->last_frame_done_ev, 0);
        }
        if (s == 0) {
            if (wait_band_events && wait_band_events[stream_bands[0][0]]) {
                cudaStreamWaitEvent(st, static_cast<cudaEvent_t>(wait_band_events[stream_bands[0][0]]), 0);
            }
            cudaEventRecord(slot->entropy_start_ev, st);
        }
        for (int b : stream_bands[s]) {
            if (wait_band_events && wait_band_events[b]) {
                cudaStreamWaitEvent(st, static_cast<cudaEvent_t>(wait_band_events[b]), 0);
            }
            if (!LaunchHighpassIntoFrameSlot(session->slots[s], slot, b, requests[b], tables[b],
                                             cfg.kernel_mode, cfg.record_substage_events, false, st, &launches)) {
                slot->failed = true;
                const char* what = session->slots[s].last_alloc_what
                    ? session->slots[s].last_alloc_what : "entropy workspace";
                slot->error = DescribeWorkspaceSetupFailure(session->slots[s], s, what);
                return false;
            }
        }
        cudaEventRecord(slot->stream_done_ev[s], st);
        cudaStreamWaitEvent(slot->consolidate_stream, slot->stream_done_ev[s], 0);
    }

    if (cfg.d2h_mode == CompressedD2HMode::FrameConsolidated) {
        cudaEventRecord(slot->consolidate_start_ev, slot->consolidate_stream);
        ComputeFrameLayoutKernel<<<1, 32, 0, slot->consolidate_stream>>>(
            slot->d_total_bits, count, slot->d_frame_meta, slot->d_frame_meta_mapped,
            slot->active_consolidated_limit, slot->d_overflow);
        const dim3 grid(64, static_cast<unsigned int>(count));
        ConsolidateFrameWordsKernel<<<grid, 256, 0, slot->consolidate_stream>>>(
            slot->d_band_words, slot->d_band_offsets, slot->d_frame_meta, slot->d_consolidated_words, count,
            slot->active_consolidated_limit, slot->d_overflow);
        launches += 2;
        cudaEventRecord(slot->entropy_done_ev, slot->consolidate_stream);
    } else {
        cudaEventRecord(slot->consolidate_start_ev, slot->consolidate_stream);
        cudaEventRecord(slot->entropy_done_ev, slot->consolidate_stream);
    }
    if (session->last_frame_done_ev) {
        cudaEventRecord(session->last_frame_done_ev, slot->consolidate_stream);
        session->has_last_frame_done = true;
    }
    for (int s = 0; s < streams; ++s) {
        if (stream_bands[s].empty()) continue;
        cudaStreamWaitEvent(session->slots[s].stream, slot->entropy_done_ev, 0);
    }
    slot->total_kernel_launches = launches;
    slot->graph_kernel_nodes = 0;
    slot->graph_total_nodes = 0;
    slot->host_enqueue_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_enqueue0).count();
    return true;
}

namespace {

void FailEntropySlot(GpuEntropyFrameSlot* slot, const char* message) {
    if (!slot) return;
    slot->failed = true;
    if (slot->error.empty() && message) slot->error = message;
}

}  // namespace

const char* GpuEntropyFrameError(const GpuEntropyFrameSlot* slot) {
    if (!slot || slot->error.empty()) return "";
    return slot->error.c_str();
}

bool GpuEntropyFrameHasFailed(const GpuEntropyFrameSlot* slot) {
    return slot && slot->failed;
}

const char* GpuEntropySlotCreationError() {
    return g_slot_creation_error.c_str();
}

void SetGpuEntropyAllocationFailureInjectionForTest(size_t min_bytes) {
    g_inject_workspace_alloc_failure_bytes = min_bytes;
}

void SetGpuEntropyCapacityOverrideForTest(GpuEntropyFrameSlot* slot,
                                          uint32_t per_band_words,
                                          uint32_t consolidated_words) {
    if (!slot) return;
    slot->test_band_word_cap_override = per_band_words;
    slot->test_consolidated_word_cap_override = consolidated_words;
}

GpuStageStatus PollGpuEntropyFrameD2HStatus(GpuEntropyFrameSlot* slot) {
    if (!slot || slot->d2h_mode != CompressedD2HMode::FrameConsolidated) {
        FailEntropySlot(slot, "FrameConsolidated D2H poll used on a slot that is not in that mode");
        return GpuStageStatus::Failed;
    }
    if (slot->failed) return GpuStageStatus::Failed;
    if (slot->d2h_started) return GpuStageStatus::Complete;

    const cudaError_t query = cudaEventQuery(slot->entropy_done_ev);
    if (query == cudaErrorNotReady) return GpuStageStatus::Pending;
    if (query != cudaSuccess) {
        FailEntropySlot(slot, "entropy completion event failed");
        return GpuStageStatus::Failed;
    }
    if (slot->h_overflow && *slot->h_overflow) {
        FailEntropySlot(slot, "entropy output exceeded the proven word capacity");
        return GpuStageStatus::Failed;
    }
    const uint32_t total_words = slot->h_frame_meta ? slot->h_frame_meta->total_words : 0;
    if (static_cast<size_t>(total_words) > slot->consolidated_words_cap ||
        total_words > slot->active_consolidated_limit) {
        FailEntropySlot(slot, "entropy output exceeded the proven word capacity");
        return GpuStageStatus::Failed;
    }
    cudaEventRecord(slot->d2h_start_ev, slot->d2h_stream);
    if (total_words > 0) {
        if (cudaMemcpyAsync(slot->h_consolidated_words, slot->d_consolidated_words,
                            static_cast<size_t>(total_words) * sizeof(uint32_t),
                            cudaMemcpyDeviceToHost, slot->d2h_stream) != cudaSuccess) {
            FailEntropySlot(slot, "consolidated D2H memcpy failed to queue");
            return GpuStageStatus::Failed;
        }
    }
    cudaEventRecord(slot->d2h_done_ev, slot->d2h_stream);
    slot->d2h_started = true;
    return GpuStageStatus::Complete;
}

bool PollGpuEntropyFrameD2H(GpuEntropyFrameSlot* slot) {
    return PollGpuEntropyFrameD2HStatus(slot) == GpuStageStatus::Complete;
}

bool WaitAndStartGpuEntropyFrameD2H(GpuEntropyFrameSlot* slot) {
    if (!slot || slot->d2h_mode != CompressedD2HMode::FrameConsolidated) {
        FailEntropySlot(slot, "FrameConsolidated D2H wait used on a slot that is not in that mode");
        return false;
    }
    if (slot->failed) return false;
    if (slot->d2h_started) return true;
    if (cudaEventSynchronize(slot->entropy_done_ev) != cudaSuccess) {
        FailEntropySlot(slot, "entropy completion wait failed");
        return false;
    }
    return PollGpuEntropyFrameD2HStatus(slot) == GpuStageStatus::Complete;
}

int PollNextStreamedBandReady(GpuEntropyFrameSlot* slot) {
    if (!slot || slot->failed || slot->d2h_mode != CompressedD2HMode::BandStreamed) {
        if (slot && slot->d2h_mode != CompressedD2HMode::BandStreamed) {
            FailEntropySlot(slot, "band-streamed poll used on a consolidated slot");
        }
        return slot && slot->failed ? -2 : -1;
    }
    if (slot->h_overflow && *slot->h_overflow) {
        FailEntropySlot(slot, "entropy output exceeded the proven word capacity");
        return -2;
    }
    const int count = slot->active_bands;
    // First, queue D2H for any band whose GPU pack kernel just completed.
    for (int b = 0; b < count; ++b) {
        if (slot->band_state[b] == 0) {
            const cudaError_t pack_query = cudaEventQuery(slot->band_pack_done_ev[b]);
            if (pack_query != cudaSuccess && pack_query != cudaErrorNotReady) {
                FailEntropySlot(slot, "band pack completion event failed");
                return -2;
            }
            if (pack_query == cudaSuccess) {
                const uint32_t bits = slot->h_scalars[b].total_bits;
                const uint32_t words = (bits + 31u) >> 5;
                const uint32_t off = slot->band_word_offset[b];
                if (words > slot->band_word_cap[b] ||
                    static_cast<size_t>(off) + words > slot->total_band_words_cap) {
                    FailEntropySlot(slot, "entropy output exceeded the proven word capacity");
                    return -2;
                }
                if (!slot->d2h_FirstBandRecorded) {
                    cudaEventRecord(slot->d2h_start_ev, slot->d2h_stream);
                    slot->d2h_FirstBandRecorded = true;
                }
                cudaEventRecord(slot->band_d2h_start_ev[b], slot->d2h_stream);
                if (words > 0) {
                    cudaMemcpyAsync(slot->h_band_words + off, slot->d_band_words + off,
                                    static_cast<size_t>(words) * sizeof(uint32_t),
                                    cudaMemcpyDeviceToHost, slot->d2h_stream);
                }
                cudaEventRecord(slot->band_d2h_done_ev[b], slot->d2h_stream);
                cudaEventRecord(slot->d2h_done_ev, slot->d2h_stream);
                slot->band_state[b] = 1;
            }
        }
    }
    // Second, check if any queued D2H copy has completed and is ready for host header/trailer wrap.
    for (int b = 0; b < count; ++b) {
        if (slot->band_state[b] == 1) {
            const cudaError_t d2h_query = cudaEventQuery(slot->band_d2h_done_ev[b]);
            if (d2h_query == cudaErrorNotReady) continue;
            if (d2h_query != cudaSuccess) {
                FailEntropySlot(slot, "band D2H completion event failed");
                return -2;
            }
            slot->band_state[b] = 2;
            return b;
        }
    }
    return -1;
}

bool IsGpuEntropyFrameComplete(GpuEntropyFrameSlot* slot) {
    if (!slot || slot->failed) return false;
    if (slot->d2h_mode == CompressedD2HMode::FrameConsolidated) {
        const GpuStageStatus status = PollGpuEntropyFrameD2HStatus(slot);
        if (status != GpuStageStatus::Complete) return false;
        const cudaError_t query = cudaEventQuery(slot->d2h_done_ev);
        if (query == cudaErrorNotReady) return false;
        if (query != cudaSuccess) {
            FailEntropySlot(slot, "consolidated D2H completion event failed");
            return false;
        }
        return true;
    } else {
        for (int b = 0; b < slot->active_bands; ++b) {
            if (slot->band_state[b] < 2) return false;
        }
        return cudaEventQuery(slot->entropy_done_ev) == cudaSuccess;
    }
}

bool WaitGpuEntropyFrameComplete(GpuEntropyFrameSlot* slot) {
    if (!slot) return false;
    if (slot->d2h_mode == CompressedD2HMode::FrameConsolidated) {
        if (!WaitAndStartGpuEntropyFrameD2H(slot)) return false;
        return cudaEventSynchronize(slot->d2h_done_ev) == cudaSuccess;
    } else {
        for (int b = 0; b < slot->active_bands; ++b) {
            if (slot->band_state[b] == 0) {
                if (cudaEventSynchronize(slot->band_pack_done_ev[b]) != cudaSuccess) return false;
                PollNextStreamedBandReady(slot);
            }
            if (slot->band_state[b] == 1) {
                if (cudaEventSynchronize(slot->band_d2h_done_ev[b]) != cudaSuccess) return false;
                slot->band_state[b] = 2;
            }
        }
        return cudaEventSynchronize(slot->entropy_done_ev) == cudaSuccess;
    }
}

bool IsGpuEntropyComputeDone(GpuEntropyFrameSlot* slot) {
    if (!slot || slot->failed) return false;
    const cudaError_t query = cudaEventQuery(slot->entropy_done_ev);
    if (query == cudaErrorNotReady) return false;
    if (query != cudaSuccess) {
        FailEntropySlot(slot, "entropy completion event failed");
        return false;
    }
    return true;
}

bool WaitGpuEntropyComputeDone(GpuEntropyFrameSlot* slot) {
    if (!slot) return false;
    return cudaEventSynchronize(slot->entropy_done_ev) == cudaSuccess;
}

bool WaitGpuEvent(void* cuda_event) {
    if (!cuda_event) return true;
    return cudaEventSynchronize(static_cast<cudaEvent_t>(cuda_event)) == cudaSuccess;
}

void* GetGpuEntropyComputeDoneEvent(GpuEntropyFrameSlot* slot) {
    return slot ? static_cast<void*>(slot->entropy_done_ev) : nullptr;
}

bool GetGpuBandCompressedView(GpuEntropyFrameSlot* slot, int band_index, GpuBandCompressedView* view) {
    if (!slot || !view || band_index < 0 || band_index >= slot->active_bands) return false;
    const uint32_t bits = slot->h_scalars[band_index].total_bits;
    const int tokens = slot->h_scalars[band_index].token_total;
    const uint32_t words = (bits + 31u) >> 5;
    view->total_bits = static_cast<int>(bits);
    view->token_count = tokens;
    view->byte_count = static_cast<int>(words * sizeof(uint32_t));
    if (slot->d2h_mode == CompressedD2HMode::FrameConsolidated) {
        const uint32_t off = slot->h_frame_meta->word_offset[band_index];
        view->bytes = reinterpret_cast<const uint8_t*>(slot->h_consolidated_words + off);
    } else {
        const uint32_t off = slot->band_word_offset[band_index];
        view->bytes = reinterpret_cast<const uint8_t*>(slot->h_band_words + off);
    }
    return true;
}

bool CollectGpuFrameEntropyStats(GpuEntropyFrameSlot* slot, GpuFrameEntropyStats* stats) {
    if (!slot || !stats) return false;
    *stats = GpuFrameEntropyStats{};
    stats->active_streams = slot->active_streams;
    stats->max_active_blocks_per_sm = GpuEntropyScanOccupancy();
    stats->total_kernel_launches = slot->total_kernel_launches;
    stats->graph_kernel_nodes = slot->graph_kernel_nodes;
    stats->graph_total_nodes = slot->graph_total_nodes;
    stats->graph_used = slot->graph_used;
    stats->graph_captured_this_frame = slot->graph_captured_this_frame;
    stats->host_enqueue_ms = slot->host_enqueue_ms;
    stats->graph_capture_ms = slot->graph_capture_ms;
    stats->gpu_entropy_wall_ms = Elapsed(slot->entropy_start_ev, slot->entropy_done_ev);
    stats->gpu_consolidate_ms = Elapsed(slot->consolidate_start_ev, slot->entropy_done_ev);

    if (slot->d2h_mode == CompressedD2HMode::FrameConsolidated) {
        stats->compressed_d2h_ms = Elapsed(slot->d2h_start_ev, slot->d2h_done_ev);
        stats->compressed_d2h_bytes = static_cast<size_t>(slot->h_frame_meta->total_words) * sizeof(uint32_t);
    } else {
        double sum_d2h = 0.0;
        size_t sum_bytes = 0;
        for (int b = 0; b < slot->active_bands; ++b) {
            sum_d2h += Elapsed(slot->band_d2h_start_ev[b], slot->band_d2h_done_ev[b]);
            const uint32_t words = (slot->h_scalars[b].total_bits + 31u) >> 5;
            sum_bytes += static_cast<size_t>(words) * sizeof(uint32_t);
        }
        stats->compressed_d2h_ms = sum_d2h;
        stats->compressed_d2h_bytes = sum_bytes;
    }

    if (slot->record_substage_events) {
        for (int b = 0; b < slot->active_bands; ++b) {
            cudaEvent_t* bev = slot->band_ev[b];
            EntropyTiming& bt = stats->band_timings[b];
            bt.block_scan_ms = Elapsed(bev[0], bev[1]);
            bt.merge_carry_ms = Elapsed(bev[1], bev[2]);
            bt.gather_ms = Elapsed(bev[2], bev[3]);
            bt.scan_ms = Elapsed(bev[0], bev[3]);
            bt.token_ms = Elapsed(bev[3], bev[4]);
            bt.codebook_ms = Elapsed(bev[4], bev[5]);
            bt.prefix_ms = Elapsed(bev[5], bev[6]);
            bt.pack_ms = Elapsed(bev[6], bev[7]);
            bt.total_ms = Elapsed(bev[0], bev[7]);

            stats->substage_sum.block_scan_ms += bt.block_scan_ms;
            stats->substage_sum.merge_carry_ms += bt.merge_carry_ms;
            stats->substage_sum.gather_ms += bt.gather_ms;
            stats->substage_sum.scan_ms += bt.scan_ms;
            stats->substage_sum.token_ms += bt.token_ms;
            stats->substage_sum.codebook_ms += bt.codebook_ms;
            stats->substage_sum.prefix_ms += bt.prefix_ms;
            stats->substage_sum.pack_ms += bt.pack_ms;
            stats->substage_sum.total_ms += bt.total_ms;
        }
    }
    return true;
}

}  // namespace hypercine
