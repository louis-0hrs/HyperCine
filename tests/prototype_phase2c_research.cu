// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cuda_runtime.h>
#include <device_launch_parameters.h>

#include "hypercine/decode_entropy_cuda.h"
#include "decode_phase0_support.h"
#include "decode_phase1_support.h"

namespace HyperCine {
bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>& out_tables);
}

using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;

namespace {

constexpr int16_t BAND_END_MARKER = 32767;

// Checkpoint structure for Design A
struct GpuEntropyCheckpoint {
    uint32_t byte_offset;
    uint8_t  nibble_idx;    // 0 = high nibble, 1 = low nibble
    uint16_t fsm_state;
    uint32_t out_coeff_pos;
    uint32_t end_byte_offset;
    uint8_t  end_nibble_idx;
};

// Transition summary for Design B (chunk of compressed bitstream)
struct ChunkSummary {
    uint16_t exit_state;
    uint32_t out_advance;
    uint16_t term_nibble; // 0xFFFF if not terminated
};

// ============================================================================
// CUDA Kernels for Design A
// ============================================================================

// Cooperative Band Zeroing Kernel
__global__ void ZeroBandMemoryKernel(int16_t* dst, size_t total_samples) {
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = gridDim.x * blockDim.x;
    for (size_t i = idx; i < total_samples; i += stride) {
        dst[i] = 0;
    }
}

// Pass A1 (GPU Sequential Indexing Kernel)
// Evaluates FSM transitions without writing any coefficients or touching destination memory
__global__ void DesignA_IndexGpuKernel(
    const uint8_t* __restrict__ src,
    uint32_t payload_bytes,
    size_t codebook_base,
    const GpuFsmEntry* __restrict__ fsm_tables,
    int target_segments,
    GpuEntropyCheckpoint* __restrict__ out_checkpoints,
    int* __restrict__ out_actual_segments
) {
    if (threadIdx.x != 0 || blockIdx.x != 0) return;

    uint32_t target_nibbles_per_seg = (payload_bytes * 2 + target_segments - 1) / target_segments;
    if (target_nibbles_per_seg < 16) target_nibbles_per_seg = 16;

    uint32_t byte_idx = 0;
    uint32_t out_pos = 0;
    uint16_t state = 0;
    int seg_count = 0;

    out_checkpoints[0].byte_offset = 0;
    out_checkpoints[0].nibble_idx = 0;
    out_checkpoints[0].fsm_state = 0;
    out_checkpoints[0].out_coeff_pos = 0;

    uint32_t seg_start_nibble = 0;
    uint32_t curr_nibble_global = 0;
    bool terminated = false;

    while (byte_idx < payload_bytes && !terminated) {
        uint8_t byte = src[byte_idx];

        for (int nib_in_byte = 0; nib_in_byte < 2; ++nib_in_byte) {
            uint8_t nibble = (nib_in_byte == 0) ? (byte >> 4) : (byte & 0x0f);
            size_t entry_idx = codebook_base + state * 16 + nibble;
            GpuFsmEntry entry = fsm_tables[entry_idx];

            if (entry.value0 == BAND_END_MARKER) {
                terminated = true;
                out_checkpoints[seg_count].end_byte_offset = byte_idx;
                out_checkpoints[seg_count].end_nibble_idx = nib_in_byte;
                seg_count++;
                break;
            }

            uint32_t pre = entry.pre_post_skip & 0x1ff;
            uint32_t post = (entry.pre_post_skip >> 12) & 0x7;
            out_pos += pre + post;
            state = entry.next_state;
            curr_nibble_global++;

            // Check if we should record a checkpoint
            if ((curr_nibble_global - seg_start_nibble) >= target_nibbles_per_seg &&
                seg_count < target_segments - 1) {
                // Determine next nibble position
                uint32_t next_byte = (nib_in_byte == 0) ? byte_idx : (byte_idx + 1);
                uint8_t next_nib = (nib_in_byte == 0) ? 1 : 0;

                out_checkpoints[seg_count].end_byte_offset = next_byte;
                out_checkpoints[seg_count].end_nibble_idx = next_nib;
                seg_count++;

                out_checkpoints[seg_count].byte_offset = next_byte;
                out_checkpoints[seg_count].nibble_idx = next_nib;
                out_checkpoints[seg_count].fsm_state = state;
                out_checkpoints[seg_count].out_coeff_pos = out_pos;
                seg_start_nibble = curr_nibble_global;
            }
        }
        byte_idx++;
    }

    if (!terminated && seg_count < target_segments) {
        out_checkpoints[seg_count].end_byte_offset = payload_bytes;
        out_checkpoints[seg_count].end_nibble_idx = 0;
        seg_count++;
    }

    *out_actual_segments = seg_count;
}

// Pass A2 (GPU Parallel Replay Kernel)
// Each block handles one validated checkpoint segment concurrently
__global__ void DesignA_ReplayGpuKernel(
    const uint8_t* __restrict__ src,
    size_t codebook_base,
    const GpuFsmEntry* __restrict__ fsm_tables,
    const GpuEntropyCheckpoint* __restrict__ checkpoints,
    int total_segments,
    int16_t* __restrict__ dst,
    size_t total_capacity,
    int quant
) {
    int seg_idx = blockIdx.x;
    if (seg_idx >= total_segments) return;

    const GpuEntropyCheckpoint cp = checkpoints[seg_idx];

    // Thread 0 of each block decodes this segment
    if (threadIdx.x == 0) {
        uint32_t byte_idx = cp.byte_offset;
        uint8_t nib_in_byte = cp.nibble_idx;
        uint32_t end_byte = cp.end_byte_offset;
        uint8_t end_nib = cp.end_nibble_idx;

        uint16_t state = cp.fsm_state;
        uint32_t out_pos = cp.out_coeff_pos;

        while (true) {
            if (byte_idx > end_byte || (byte_idx == end_byte && nib_in_byte >= end_nib)) {
                break;
            }

            uint8_t byte = src[byte_idx];
            uint8_t nibble = (nib_in_byte == 0) ? (byte >> 4) : (byte & 0x0f);
            size_t entry_idx = codebook_base + state * 16 + nibble;
            GpuFsmEntry entry = fsm_tables[entry_idx];

            if (entry.value0 == BAND_END_MARKER) {
                break;
            }

            uint32_t pre = entry.pre_post_skip & 0x1ff;
            uint32_t post = (entry.pre_post_skip >> 12) & 0x7;

            out_pos += pre;
            if (entry.value0 != 0 && out_pos < total_capacity) {
                dst[out_pos] = static_cast<int16_t>(entry.value0 * quant);
            }
            if (entry.value1 != 0 && (out_pos + 1) < total_capacity) {
                dst[out_pos + 1] = static_cast<int16_t>(entry.value1 * quant);
            }
            out_pos += post;
            state = entry.next_state;

            // Advance nibble
            if (nib_in_byte == 0) {
                nib_in_byte = 1;
            } else {
                nib_in_byte = 0;
                byte_idx++;
            }
        }
    }
}

// Cooperative Difference Coding Kernel
__global__ void CooperativeDifferenceCodingKernel(
    int16_t* dst,
    int width,
    int height,
    size_t pitch_samples
) {
    int y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= height) return;

    int16_t* line = dst + y * pitch_samples;
    for (int x = 1; x < width; ++x) {
        line[x] += line[x - 1];
    }
}

// ============================================================================
// CUDA Kernels for Design B
// ============================================================================

constexpr int CHUNK_SIZE_BYTES = 128;
constexpr int CHUNK_NIBBLES = CHUNK_SIZE_BYTES * 2; // 256 nibbles

// Pass B1: Generate transition summary for all 518 states per chunk
// Grid: num_chunks. Block: 256 or 518 threads (each thread evaluates one incoming state)
__global__ void DesignB_ChunkSummaryKernel(
    const uint8_t* __restrict__ src,
    uint32_t payload_bytes,
    size_t codebook_base,
    const GpuFsmEntry* __restrict__ fsm_tables,
    int num_chunks,
    int num_states,
    ChunkSummary* __restrict__ out_summaries
) {
    int chunk_idx = blockIdx.x;
    if (chunk_idx >= num_chunks) return;

    int s = threadIdx.x;
    if (s >= num_states) return;

    uint32_t chunk_start_byte = chunk_idx * CHUNK_SIZE_BYTES;
    uint32_t chunk_bytes = CHUNK_SIZE_BYTES;
    if (chunk_start_byte + chunk_bytes > payload_bytes) {
        chunk_bytes = (payload_bytes > chunk_start_byte) ? (payload_bytes - chunk_start_byte) : 0;
    }

    uint16_t state = s;
    uint32_t out_adv = 0;
    uint16_t term_nib = 0xFFFF;

    uint32_t nib_idx = 0;
    for (uint32_t b = 0; b < chunk_bytes; ++b) {
        uint8_t byte = src[chunk_start_byte + b];
        for (int nib_in_byte = 0; nib_in_byte < 2; ++nib_in_byte) {
            uint8_t nibble = (nib_in_byte == 0) ? (byte >> 4) : (byte & 0x0f);
            size_t entry_idx = codebook_base + state * 16 + nibble;
            GpuFsmEntry entry = fsm_tables[entry_idx];

            if (entry.value0 == BAND_END_MARKER) {
                term_nib = static_cast<uint16_t>(nib_idx);
                break;
            }

            uint32_t pre = entry.pre_post_skip & 0x1ff;
            uint32_t post = (entry.pre_post_skip >> 12) & 0x7;
            out_adv += pre + post;
            state = entry.next_state;
            nib_idx++;
        }
        if (term_nib != 0xFFFF) break;
    }

    size_t out_idx = static_cast<size_t>(chunk_idx) * num_states + s;
    out_summaries[out_idx].exit_state = state;
    out_summaries[out_idx].out_advance = out_adv;
    out_summaries[out_idx].term_nibble = term_nib;
}

// Pass B2: Sequential/Prefix resolution of chunks from chunk 0
__global__ void DesignB_PrefixResolveKernel(
    const ChunkSummary* __restrict__ summaries,
    int num_chunks,
    int num_states,
    uint16_t* __restrict__ out_chunk_start_states,
    uint32_t* __restrict__ out_chunk_start_positions,
    int* __restrict__ out_actual_chunks
) {
    if (threadIdx.x != 0 || blockIdx.x != 0) return;

    uint16_t state = 0;
    uint32_t out_pos = 0;
    int actual_chunks = 0;

    for (int k = 0; k < num_chunks; ++k) {
        out_chunk_start_states[k] = state;
        out_chunk_start_positions[k] = out_pos;
        actual_chunks++;

        size_t idx = static_cast<size_t>(k) * num_states + state;
        const ChunkSummary s = summaries[idx];

        if (s.term_nibble != 0xFFFF) {
            // Band terminated inside chunk k
            break;
        }

        state = s.exit_state;
        out_pos += s.out_advance;
    }

    *out_actual_chunks = actual_chunks;
}

// Pass B3: Replay resolved chunks
__global__ void DesignB_ReplayChunksKernel(
    const uint8_t* __restrict__ src,
    uint32_t payload_bytes,
    size_t codebook_base,
    const GpuFsmEntry* __restrict__ fsm_tables,
    const uint16_t* __restrict__ chunk_start_states,
    const uint32_t* __restrict__ chunk_start_positions,
    int total_chunks,
    int16_t* __restrict__ dst,
    size_t total_capacity,
    int quant
) {
    int chunk_idx = blockIdx.x;
    if (chunk_idx >= total_chunks) return;

    if (threadIdx.x == 0) {
        uint32_t chunk_start_byte = chunk_idx * CHUNK_SIZE_BYTES;
        uint32_t chunk_bytes = CHUNK_SIZE_BYTES;
        if (chunk_start_byte + chunk_bytes > payload_bytes) {
            chunk_bytes = (payload_bytes > chunk_start_byte) ? (payload_bytes - chunk_start_byte) : 0;
        }

        uint16_t state = chunk_start_states[chunk_idx];
        uint32_t out_pos = chunk_start_positions[chunk_idx];

        for (uint32_t b = 0; b < chunk_bytes; ++b) {
            uint8_t byte = src[chunk_start_byte + b];
            for (int nib_in_byte = 0; nib_in_byte < 2; ++nib_in_byte) {
                uint8_t nibble = (nib_in_byte == 0) ? (byte >> 4) : (byte & 0x0f);
                size_t entry_idx = codebook_base + state * 16 + nibble;
                GpuFsmEntry entry = fsm_tables[entry_idx];

                if (entry.value0 == BAND_END_MARKER) {
                    return;
                }

                uint32_t pre = entry.pre_post_skip & 0x1ff;
                uint32_t post = (entry.pre_post_skip >> 12) & 0x7;

                out_pos += pre;
                if (entry.value0 != 0 && out_pos < total_capacity) {
                    dst[out_pos] = static_cast<int16_t>(entry.value0 * quant);
                }
                if (entry.value1 != 0 && (out_pos + 1) < total_capacity) {
                    dst[out_pos + 1] = static_cast<int16_t>(entry.value1 * quant);
                }
                out_pos += post;
                state = entry.next_state;
            }
        }
    }
}

// Host CPU Indexing implementation
void GenerateCheckpointsCpu(
    const uint8_t* src,
    uint32_t payload_bytes,
    size_t codebook_base,
    const std::vector<GpuFsmEntry>& fsm_tables,
    int target_segments,
    std::vector<GpuEntropyCheckpoint>& out_checkpoints
) {
    out_checkpoints.clear();
    out_checkpoints.resize(target_segments);

    uint32_t target_nibbles_per_seg = (payload_bytes * 2 + target_segments - 1) / target_segments;
    if (target_nibbles_per_seg < 16) target_nibbles_per_seg = 16;

    uint32_t byte_idx = 0;
    uint32_t out_pos = 0;
    uint16_t state = 0;
    int seg_count = 0;

    out_checkpoints[0].byte_offset = 0;
    out_checkpoints[0].nibble_idx = 0;
    out_checkpoints[0].fsm_state = 0;
    out_checkpoints[0].out_coeff_pos = 0;

    uint32_t seg_start_nibble = 0;
    uint32_t curr_nibble_global = 0;
    bool terminated = false;

    while (byte_idx < payload_bytes && !terminated) {
        uint8_t byte = src[byte_idx];

        for (int nib_in_byte = 0; nib_in_byte < 2; ++nib_in_byte) {
            uint8_t nibble = (nib_in_byte == 0) ? (byte >> 4) : (byte & 0x0f);
            size_t entry_idx = codebook_base + state * 16 + nibble;
            const auto& entry = fsm_tables[entry_idx];

            if (entry.value0 == BAND_END_MARKER) {
                terminated = true;
                out_checkpoints[seg_count].end_byte_offset = byte_idx;
                out_checkpoints[seg_count].end_nibble_idx = nib_in_byte;
                seg_count++;
                break;
            }

            uint32_t pre = entry.pre_post_skip & 0x1ff;
            uint32_t post = (entry.pre_post_skip >> 12) & 0x7;
            out_pos += pre + post;
            state = entry.next_state;
            curr_nibble_global++;

            if ((curr_nibble_global - seg_start_nibble) >= target_nibbles_per_seg &&
                seg_count < target_segments - 1) {
                uint32_t next_byte = (nib_in_byte == 0) ? byte_idx : (byte_idx + 1);
                uint8_t next_nib = (nib_in_byte == 0) ? 1 : 0;

                out_checkpoints[seg_count].end_byte_offset = next_byte;
                out_checkpoints[seg_count].end_nibble_idx = next_nib;
                seg_count++;

                out_checkpoints[seg_count].byte_offset = next_byte;
                out_checkpoints[seg_count].nibble_idx = next_nib;
                out_checkpoints[seg_count].fsm_state = state;
                out_checkpoints[seg_count].out_coeff_pos = out_pos;
                seg_start_nibble = curr_nibble_global;
            }
        }
        byte_idx++;
    }

    if (!terminated && seg_count < target_segments) {
        out_checkpoints[seg_count].end_byte_offset = payload_bytes;
        out_checkpoints[seg_count].end_nibble_idx = 0;
        seg_count++;
    }

    out_checkpoints.resize(seg_count);
}

} // anonymous namespace

int main(int argc, char** argv) {
    try {
        std::cout << "=================================================================\n";
        std::cout << "   HyperCine Decode Phase 2C Research Gate: Design A vs Design B\n";
        std::cout << "=================================================================\n\n";

        // Initialize CUDA & FSM tables
        GpuFsmTableManager fsm_mgr;
        if (!fsm_mgr.Initialize(0)) {
            throw std::runtime_error("failed to initialize GpuFsmTableManager");
        }
        std::vector<GpuFsmEntry> host_tables;
        ExtractCineFormBaseFsmTables(host_tables);

        cudaStream_t stream = nullptr;
        cudaStreamCreate(&stream);

        // Patterns to evaluate
        struct PatternTest {
            int width;
            int height;
            int pattern_id;
            const char* name;
        };

        std::vector<PatternTest> tests = {
            {1920, 1080, 0, "1080p flat"},
            {1920, 1080, 1, "1080p gradient"},
            {1920, 1080, 4, "1080p edges"},
            {1920, 1080, 3, "1080p texture"},
            {1920, 1080, 2, "1080p noise"},
            {3840, 2160, 0, "4K flat"},
            {3840, 2160, 1, "4K gradient"},
            {3840, 2160, 4, "4K edges"},
            {3840, 2160, 3, "4K texture"},
            {3840, 2160, 2, "4K noise"}
        };

        for (const auto& t : tests) {
            std::cout << "\n-----------------------------------------------------------------\n";
            std::cout << "Evaluating Pattern: " << t.name << " (" << t.width << "x" << t.height << ")\n";
            std::cout << "-----------------------------------------------------------------\n";

            auto samples = Generate(t.width, t.height, 5);
            auto& s = samples[t.pattern_id];

            // 1. Pristine CPU decode for reference
            ReferenceDecoder ref_bgra(s, CFHD_PIXEL_FORMAT_BGRA);
            ref_bgra.Decode(s);
            auto* d_native = ref_bgra.GetNative();
            auto cpu_bands = ExtractBandsFromNative(d_native);

            // 2. Parse sample tags
            std::vector<GpuEntropyBandDescriptor> parsed_bands;
            std::string parse_err;
            if (!ParseCineFormSample(s.compressed.data(), s.compressed.size(), 24, parsed_bands, &parse_err)) {
                throw std::runtime_error("Parse failed: " + parse_err);
            }

            // Find the critical (largest payload) highpass band
            int crit_band_idx = -1;
            uint32_t max_payload = 0;
            for (size_t b = 0; b < parsed_bands.size(); ++b) {
                if (parsed_bands[b].method == BandEncodingMethod::RunLengthVLC) {
                    if (parsed_bands[b].payload_bytes > max_payload) {
                        max_payload = parsed_bands[b].payload_bytes;
                        crit_band_idx = static_cast<int>(b);
                    }
                }
            }

            if (crit_band_idx < 0) {
                std::cout << "No highpass VLC bands found!\n";
                continue;
            }

            const auto& desc = parsed_bands[crit_band_idx];
            std::cout << "Critical Band: #" << crit_band_idx << " (Channel " << desc.channel
                      << ", Subband " << desc.subband << ", " << desc.width << "x" << desc.height
                      << ", Payload: " << desc.payload_bytes << " bytes, Quant: " << desc.quantization
                      << ", DiffCoding: " << (desc.difference_coding ? "YES" : "NO") << ")\n";

            size_t codebook_base = fsm_mgr.GetCodebookOffset(desc.codebook_id);
            int num_states = (desc.codebook_id == 0) ? 136 : 518;

            // Allocate device memory for this band
            uint8_t* d_payload = nullptr;
            cudaMalloc(&d_payload, desc.payload_bytes);
            cudaMemcpy(d_payload, s.compressed.data() + desc.payload_offset, desc.payload_bytes, cudaMemcpyHostToDevice);

            int16_t* d_dst = nullptr;
            size_t total_samples = desc.coefficient_count;
            cudaMalloc(&d_dst, total_samples * sizeof(int16_t));

            // Timing events
            cudaEvent_t ev_start, ev_stop;
            cudaEventCreate(&ev_start);
            cudaEventCreate(&ev_stop);

            // -------------------------------------------------------------
            // A. Baseline Phase 2B Serial GPU Decode
            // -------------------------------------------------------------
            GpuBandDecodeResult* d_result_serial = nullptr;
            cudaMalloc(&d_result_serial, sizeof(GpuBandDecodeResult));
            GpuEntropyBandDescriptor desc_serial = desc;
            desc_serial.d_destination = d_dst;
            desc_serial.payload_offset = 0; // relative to d_payload

            GpuEntropyBandDescriptor* d_desc_serial = nullptr;
            cudaMalloc(&d_desc_serial, sizeof(GpuEntropyBandDescriptor));
            cudaMemcpy(d_desc_serial, &desc_serial, sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice);

            cudaEventRecord(ev_start, stream);
            LaunchDecodeEntropyGpu(
                d_payload,
                desc.payload_bytes,
                d_desc_serial,
                1,
                fsm_mgr.GetDeviceTable(),
                d_result_serial,
                stream
            );
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);

            float time_phase2b_ms = 0.0f;
            cudaEventElapsedTime(&time_phase2b_ms, ev_start, ev_stop);

            // Verify Phase 2B exactness vs CPU
            std::vector<int16_t> h_dst(total_samples);
            cudaMemcpy(h_dst.data(), d_dst, total_samples * sizeof(int16_t), cudaMemcpyDeviceToHost);

            int diff_count_2b = 0;
            const auto& ref_band = cpu_bands[crit_band_idx];
            for (int y = 0; y < desc.height; ++y) {
                const int16_t* ref_row = ref_band.host_data + y * (ref_band.pitch / sizeof(int16_t));
                const int16_t* gpu_row = h_dst.data() + y * desc.pitch_samples;
                for (int x = 0; x < desc.width; ++x) {
                    if (ref_row[x] != gpu_row[x]) diff_count_2b++;
                }
            }

            std::cout << "  Phase 2B Serial GPU Time: " << std::fixed << std::setprecision(3)
                      << time_phase2b_ms << " ms (Diffs vs CPU: " << diff_count_2b << ")\n";

            // -------------------------------------------------------------
            // B. Evaluate Design A (Checkpoint / Index + Parallel Replay)
            // -------------------------------------------------------------
            // Test multiple segment counts
            std::vector<int> seg_counts = {2, 4, 8, 16, 32, 64};

            std::cout << "  Design A (Checkpoint / Index + Parallel Replay):\n";

            // Measure CPU indexing time
            auto t0_cpu = std::chrono::high_resolution_clock::now();
            std::vector<GpuEntropyCheckpoint> h_checkpoints;
            GenerateCheckpointsCpu(
                s.compressed.data() + desc.payload_offset,
                desc.payload_bytes,
                codebook_base,
                host_tables,
                32,
                h_checkpoints
            );
            auto t1_cpu = std::chrono::high_resolution_clock::now();
            float time_cpu_index_ms = std::chrono::duration<float, std::milli>(t1_cpu - t0_cpu).count();

            // Measure GPU indexing time
            GpuEntropyCheckpoint* d_checkpoints = nullptr;
            cudaMalloc(&d_checkpoints, 128 * sizeof(GpuEntropyCheckpoint));
            int* d_actual_segs = nullptr;
            cudaMalloc(&d_actual_segs, sizeof(int));

            cudaEventRecord(ev_start, stream);
            DesignA_IndexGpuKernel<<<1, 1, 0, stream>>>(
                d_payload,
                desc.payload_bytes,
                codebook_base,
                fsm_mgr.GetDeviceTable(),
                32,
                d_checkpoints,
                d_actual_segs
            );
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float time_gpu_index_ms = 0.0f;
            cudaEventElapsedTime(&time_gpu_index_ms, ev_start, ev_stop);

            std::cout << "    Indexing Time: CPU = " << time_cpu_index_ms << " ms | GPU = " << time_gpu_index_ms << " ms\n";

            for (int S : seg_counts) {
                // Generate S checkpoints
                GenerateCheckpointsCpu(
                    s.compressed.data() + desc.payload_offset,
                    desc.payload_bytes,
                    codebook_base,
                    host_tables,
                    S,
                    h_checkpoints
                );

                cudaMemcpy(d_checkpoints, h_checkpoints.data(), h_checkpoints.size() * sizeof(GpuEntropyCheckpoint), cudaMemcpyHostToDevice);

                // Zero dst
                ZeroBandMemoryKernel<<<(total_samples + 255) / 256, 256, 0, stream>>>(d_dst, total_samples);

                // Replay
                cudaEventRecord(ev_start, stream);
                DesignA_ReplayGpuKernel<<<static_cast<int>(h_checkpoints.size()), 64, 0, stream>>>(
                    d_payload,
                    codebook_base,
                    fsm_mgr.GetDeviceTable(),
                    d_checkpoints,
                    static_cast<int>(h_checkpoints.size()),
                    d_dst,
                    total_samples,
                    desc.quantization
                );
                if (desc.difference_coding) {
                    CooperativeDifferenceCodingKernel<<<(desc.height + 63) / 64, 64, 0, stream>>>(
                        d_dst,
                        desc.width,
                        desc.height,
                        desc.pitch_samples
                    );
                }
                cudaEventRecord(ev_stop, stream);
                cudaEventSynchronize(ev_stop);

                float time_replay_ms = 0.0f;
                cudaEventElapsedTime(&time_replay_ms, ev_start, ev_stop);

                // Verify exactness
                cudaMemcpy(h_dst.data(), d_dst, total_samples * sizeof(int16_t), cudaMemcpyDeviceToHost);
                int diff_count = 0;
                for (int y = 0; y < desc.height; ++y) {
                    const int16_t* ref_row = ref_band.host_data + y * (ref_band.pitch / sizeof(int16_t));
                    const int16_t* gpu_row = h_dst.data() + y * desc.pitch_samples;
                    for (int x = 0; x < desc.width; ++x) {
                        if (ref_row[x] != gpu_row[x]) diff_count++;
                    }
                }

                float total_a_cpu_ms = time_cpu_index_ms + time_replay_ms;
                float speedup_a = time_phase2b_ms / total_a_cpu_ms;

                std::cout << "    S=" << std::setw(2) << S << " (" << std::setw(2) << h_checkpoints.size() << " actual segs): "
                          << "Replay = " << std::setw(6) << time_replay_ms << " ms | "
                          << "Total(CPU+GPU) = " << std::setw(6) << total_a_cpu_ms << " ms | "
                          << "Speedup = " << std::setw(5) << speedup_a << "x | "
                          << "Exact = " << (diff_count == 0 ? "YES" : "NO") << "\n";
            }

            // -------------------------------------------------------------
            // C. Evaluate Design B (Transition Summary + Parallel Prefix Composition)
            // -------------------------------------------------------------
            std::cout << "  Design B (Transition Summary + Parallel Prefix Composition):\n";
            int num_chunks = (desc.payload_bytes + CHUNK_SIZE_BYTES - 1) / CHUNK_SIZE_BYTES;
            size_t summary_elements = static_cast<size_t>(num_chunks) * num_states;
            size_t summary_bytes = summary_elements * sizeof(ChunkSummary);

            std::cout << "    Chunks: " << num_chunks << " (" << CHUNK_SIZE_BYTES << " bytes/chunk), "
                      << "States: " << num_states << ", Scratch: " << (summary_bytes / 1024) << " KB\n";

            ChunkSummary* d_summaries = nullptr;
            cudaMalloc(&d_summaries, summary_bytes);
            uint16_t* d_resolved_states = nullptr;
            cudaMalloc(&d_resolved_states, num_chunks * sizeof(uint16_t));
            uint32_t* d_resolved_positions = nullptr;
            cudaMalloc(&d_resolved_positions, num_chunks * sizeof(uint32_t));
            int* d_actual_chunks = nullptr;
            cudaMalloc(&d_actual_chunks, sizeof(int));

            // Pass B1: Chunk Summary
            cudaEventRecord(ev_start, stream);
            DesignB_ChunkSummaryKernel<<<num_chunks, num_states, 0, stream>>>(
                d_payload,
                desc.payload_bytes,
                codebook_base,
                fsm_mgr.GetDeviceTable(),
                num_chunks,
                num_states,
                d_summaries
            );
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float time_b1_ms = 0.0f;
            cudaEventElapsedTime(&time_b1_ms, ev_start, ev_stop);

            // Pass B2: Prefix Resolve
            cudaEventRecord(ev_start, stream);
            DesignB_PrefixResolveKernel<<<1, 1, 0, stream>>>(
                d_summaries,
                num_chunks,
                num_states,
                d_resolved_states,
                d_resolved_positions,
                d_actual_chunks
            );
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float time_b2_ms = 0.0f;
            cudaEventElapsedTime(&time_b2_ms, ev_start, ev_stop);

            int actual_chunks = 0;
            cudaMemcpy(&actual_chunks, d_actual_chunks, sizeof(int), cudaMemcpyDeviceToHost);

            // Pass B3: Replay Chunks
            ZeroBandMemoryKernel<<<(total_samples + 255) / 256, 256, 0, stream>>>(d_dst, total_samples);

            cudaEventRecord(ev_start, stream);
            DesignB_ReplayChunksKernel<<<actual_chunks, 64, 0, stream>>>(
                d_payload,
                desc.payload_bytes,
                codebook_base,
                fsm_mgr.GetDeviceTable(),
                d_resolved_states,
                d_resolved_positions,
                actual_chunks,
                d_dst,
                total_samples,
                desc.quantization
            );
            if (desc.difference_coding) {
                CooperativeDifferenceCodingKernel<<<(desc.height + 63) / 64, 64, 0, stream>>>(
                    d_dst,
                    desc.width,
                    desc.height,
                    desc.pitch_samples
                );
            }
            cudaEventRecord(ev_stop, stream);
            cudaEventSynchronize(ev_stop);
            float time_b3_ms = 0.0f;
            cudaEventElapsedTime(&time_b3_ms, ev_start, ev_stop);

            float total_b_ms = time_b1_ms + time_b2_ms + time_b3_ms;
            float speedup_b = time_phase2b_ms / total_b_ms;

            // Verify exactness
            cudaMemcpy(h_dst.data(), d_dst, total_samples * sizeof(int16_t), cudaMemcpyDeviceToHost);
            int diff_count_b = 0;
            for (int y = 0; y < desc.height; ++y) {
                const int16_t* ref_row = ref_band.host_data + y * (ref_band.pitch / sizeof(int16_t));
                const int16_t* gpu_row = h_dst.data() + y * desc.pitch_samples;
                for (int x = 0; x < desc.width; ++x) {
                    if (ref_row[x] != gpu_row[x]) diff_count_b++;
                }
            }

            std::cout << "    B1 (Summary): " << std::setw(6) << time_b1_ms << " ms | "
                      << "B2 (Scan): " << std::setw(6) << time_b2_ms << " ms | "
                      << "B3 (Replay): " << std::setw(6) << time_b3_ms << " ms | "
                      << "Total: " << std::setw(6) << total_b_ms << " ms | "
                      << "Speedup = " << std::setw(5) << speedup_b << "x | "
                      << "Exact = " << (diff_count_b == 0 ? "YES" : "NO") << "\n";

            // Cleanup pattern buffers
            cudaFree(d_payload);
            cudaFree(d_dst);
            cudaFree(d_result_serial);
            cudaFree(d_desc_serial);
            cudaFree(d_checkpoints);
            cudaFree(d_actual_segs);
            cudaFree(d_summaries);
            cudaFree(d_resolved_states);
            cudaFree(d_resolved_positions);
            cudaFree(d_actual_chunks);
            cudaEventDestroy(ev_start);
            cudaEventDestroy(ev_stop);
        }

        cudaStreamDestroy(stream);
        fsm_mgr.Release();

        std::cout << "\nResearch Gate Prototype Evaluation Completed Successfully.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
}
