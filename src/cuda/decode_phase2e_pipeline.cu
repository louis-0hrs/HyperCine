// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2e.h"
#include "hypercine/decode_color_cuda.h"
#include "decode_inverse_kernels.cuh"
#include "decode_hybrid_entropy.cuh"
#include "../core/decode_fieldplus.h"
#include "../core/decoder_backend.h"
#include "hypercine/research/decoder_diagnostics.h"

#include <cuda_runtime.h>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstring>
#include <thread>
#include <sstream>
#include <iomanip>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace HyperCine {

// External declaration from decode_fsm_tables.cpp
bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>& out_tables);

namespace {

using Clock = std::chrono::steady_clock;
double ThreadCpuMs() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        return (k.QuadPart + u.QuadPart) * 1e-4;
    }
#endif
    return 0;
}
constexpr int kMaxVlcBands = 27;
constexpr int kFieldPlusVlcBands = 36;
constexpr int kMaxBandSegmentScratch = kMaxVlcBands > kFieldPlusVlcBands ? kMaxVlcBands : kFieldPlusVlcBands;
constexpr int kFieldPlusMaxSegmentsPerBand = 64;
constexpr int kFieldPlusSegmentCapacity = kFieldPlusVlcBands * kFieldPlusMaxSegmentsPerBand;
constexpr int kMaxReplayStreams = 8;
constexpr int kMaxSegmentsPerBand = 64;
constexpr int kTotalSegmentCapacity = kMaxVlcBands * kMaxSegmentsPerBand; // 1,728

constexpr size_t CS9_NUM_STATES = 136;
constexpr size_t CS17_NUM_STATES = 518;
constexpr size_t CS18_NUM_STATES = 518;
constexpr size_t CS9_ENTRIES = CS9_NUM_STATES * 16;
constexpr size_t CS17_ENTRIES = CS17_NUM_STATES * 16;
constexpr size_t CS18_ENTRIES = CS18_NUM_STATES * 16;
constexpr size_t CS9_BASE_OFFSET = 0;
constexpr size_t CS17_BASE_OFFSET = CS9_ENTRIES;
constexpr size_t CS18_BASE_OFFSET = CS9_ENTRIES + CS17_ENTRIES;

using namespace HyperCine::HybridEntropy;

static __global__ void DecodeFieldPlusRaw16Kernel(const uint8_t* sample, size_t sample_size,
    size_t payload_offset, int16_t* destination, int pitch_samples, int width, int height,
    uint32_t* frame_error) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const size_t source = payload_offset + (size_t(y) * size_t(width) + size_t(x)) * 2;
    if (!sample || source > sample_size || sample_size - source < 2) {
        if (frame_error) atomicExch(frame_error, 1u);
        return;
    }
    const uint16_t bits = uint16_t((uint16_t(sample[source]) << 8) | sample[source + 1]);
    destination[size_t(y) * size_t(pitch_samples) + size_t(x)] = static_cast<int16_t>(bits);
}

struct SlotChannelBuffers {
    int16_t* d_l2_band[4] = {nullptr, nullptr, nullptr, nullptr};
    int16_t* d_l1_band[4] = {nullptr, nullptr, nullptr, nullptr};
    int16_t* d_l0_band[4] = {nullptr, nullptr, nullptr, nullptr};

    int16_t* d_ll1 = nullptr;
    int16_t* d_ll0 = nullptr;
    int16_t* d_vert_low = nullptr;
    int16_t* d_vert_high = nullptr;
    int16_t* d_final_plane = nullptr;

    size_t l2_pitch = 0;
    size_t l1_pitch = 0;
    size_t l0_pitch = 0;
    size_t ll1_pitch = 0;
    size_t ll0_pitch = 0;
    size_t vert_low_pitch = 0;
    size_t vert_high_pitch = 0;
    size_t final_plane_pitch = 0;
};

struct FieldPlusSlotBuffers {
    // Preserve the w3 high branch until inverse spatial reconstruction; the
    // existing level-1 buffers are then reused for the independent w4 low branch.
    int16_t* d_w3_band[3][4] = {};
    size_t w3_pitch[3] = {};
    int16_t* d_gop_low[3] = {};
    int16_t* d_gop_high[3] = {};
    size_t gop_high_pitch[3] = {};
    GpuEntropyBandDescriptor* d_ll_descriptors = nullptr;
    GpuEntropyBandDescriptor* d_vlc_descriptors = nullptr;
    GpuEntropyBandDescriptor* host_pinned_ll_descriptors = nullptr;
    GpuEntropyBandDescriptor* host_pinned_vlc_descriptors = nullptr;
    GpuEntropySegmentDescriptor* d_segments = nullptr;
    GpuSegmentDecodeResult* d_segment_results = nullptr;
    GpuEntropySegmentDescriptor* host_pinned_segments = nullptr;
    std::vector<GpuEntropyBandDescriptor> ll_descriptors;
    std::vector<GpuEntropyBandDescriptor> vlc_descriptors;
    std::vector<GpuEntropySegmentDescriptor> segments;
    std::array<GpuEntropyBandDescriptor, kFieldPlusBandCount> bands{};
    bool allocated = false;
    size_t allocated_device_bytes = 0;
    size_t allocated_pinned_bytes = 0;
    uint64_t allocation_calls = 0;
};

enum class BandState : uint32_t {
    Unindexed = 0,
    Indexing = 1,
    CheckpointReady = 2,
    GpuQueued = 3,
    GpuComplete = 4,
    Failed = 5
};

struct ChannelInverseState {
    bool l2_enqueued = false;
    bool l1_enqueued = false;
    bool l0_enqueued = false;
};

struct TaskStat {
    Clock::time_point start{};
    Clock::time_point end{};
    float duration_ms = 0.0f;
    int worker_id = -1;
    float thread_cpu_ms = 0;
};

struct DecodeSlot2E {
    int slot_index = -1;
    uint64_t ticket = 0;
    bool in_use = false;
    bool production_guard = false;
    size_t allocated_device_bytes = 0;
    size_t allocated_pinned_bytes = 0;
    DecodeTicketStatus status = DecodeTicketStatus::Invalid;

    int max_width = 0;
    int max_height = 0;
    int width = 0;
    int height = 0;
    int precision = 10;
    int cuda_device = 0;

    Phase2EPipelineMode pipeline_mode = Phase2EPipelineMode::Phase2EFullOverlap;
    Phase2ESchedulingPolicy scheduling_policy = Phase2ESchedulingPolicy::PolicyA_LargestFirst;
    GpuEntropyMode entropy_mode = GpuEntropyMode::ParallelAdaptive;
    int fixed_segments_per_band = 32;
    int num_replay_streams = 4;

    DeviceSurfaceType surface_type = DeviceSurfaceType::BGRA8;
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorRange range = ColorRange::CG;
    RowOrientation orientation = RowOrientation::BottomUp;

    SlotChannelBuffers channels[3]; // 0: Y, 1: V, 2: U
    uint8_t* d_bgra = nullptr;
    size_t bgra_pitch = 0;
    FieldPlusSlotBuffers fieldplus;
    bool fieldplus_mode = false;
    int frame_index = 0;

    // Compressed sample staging
    uint8_t* host_pinned_compressed = nullptr;
    uint8_t* d_compressed_sample = nullptr;
    size_t compressed_capacity = 32 * 1024 * 1024;
    size_t current_compressed_size = 0;

    // Descriptors and results
    GpuEntropyBandDescriptor* d_ll_descriptors = nullptr;
    GpuEntropyBandDescriptor* d_vlc_descriptors = nullptr;
    GpuBandDecodeResult* d_vlc_results = nullptr;

    // Pre-allocated pinned and device segment storage (64 segments per band x 27 bands)
    GpuEntropySegmentDescriptor* host_pinned_segments = nullptr;
    GpuEntropySegmentDescriptor* d_segments = nullptr;
    GpuSegmentDecodeResult* d_seg_results = nullptr;
    GpuCheckpointIndexResult* d_index_results = nullptr;
    uint32_t* d_frame_error = nullptr;
    uint32_t* host_frame_error = nullptr;
    cudaEvent_t ev_index_start[kMaxVlcBands] = {}, ev_index_done[kMaxVlcBands] = {};
    bool GpuIndex() const { return pipeline_mode == Phase2EPipelineMode::Phase2FAGpuIndexBatch; }
    uint32_t* ErrorGuard() const { return (GpuIndex() || production_guard) ? d_frame_error : nullptr; }

    // Reusable per-band host checkpoint buffers
    std::vector<GpuEntropySegmentDescriptor> band_segments[kMaxBandSegmentScratch];
    std::vector<GpuEntropyBandDescriptor> host_bands;
    std::vector<GpuEntropyBandDescriptor> vlc_descs;
    std::vector<GpuEntropyBandDescriptor> ll_descs;
    std::vector<GpuEntropySegmentDescriptor> host_segments; // canonical gathered
    GpuBandDecodeResult host_vlc_results[kMaxVlcBands];
    std::vector<GpuSegmentDecodeResult> host_seg_results;

    // Phase 2F-C Hybrid Entropy parameters & buffers (Section 7, 8, 15, 27)
    Phase2FCSelectorPolicy fc_selector_policy = Phase2FCSelectorPolicy::StaticHeuristic;
    uint32_t fc_chunk_size = 256;
    std::vector<BandEntropyMethod> fc_oracle_decisions;
    BandEntropyMethod band_method[kMaxVlcBands] = {};
    Phase2FCBandRecord fc_band_records[kMaxVlcBands] = {};
    HyperCine::HybridEntropy::Resolved reusable_resolved[kMaxVlcBands];
    size_t band_chunk_offset[kMaxVlcBands] = {};
    size_t band_chunk_count[kMaxVlcBands] = {};

    HyperCine::HybridEntropy::Summary* d_fc_summaries = nullptr;
    HyperCine::HybridEntropy::Summary* host_pinned_fc_summaries = nullptr;
    HyperCine::HybridEntropy::Restart* d_fc_checkpoints = nullptr;
    HyperCine::HybridEntropy::Restart* host_pinned_fc_checkpoints = nullptr;
    uint32_t* d_fc_errors = nullptr;
    uint32_t* host_pinned_fc_errors = nullptr;
    cudaEvent_t ev_summary_start[kMaxVlcBands] = {};
    cudaEvent_t ev_summary_done[kMaxVlcBands] = {};

    // CUDA Streams
    cudaStream_t copy_stream = nullptr;
    cudaStream_t replay_streams[kMaxReplayStreams] = {};
    cudaStream_t channel_streams[3] = {};
    cudaStream_t main_stream = nullptr;

    // Events for synchronization and timeline instrumentation
    cudaEvent_t ev_frame_start = nullptr;
    cudaEvent_t ev_h2d_start = nullptr;
    cudaEvent_t ev_h2d_stop = nullptr;
    cudaEvent_t ev_compressed_h2d_ready = nullptr;
    cudaEvent_t ev_ll_ready = nullptr;

    cudaEvent_t ev_band_start[kMaxVlcBands] = {};
    cudaEvent_t ev_band_done[kMaxVlcBands] = {};

    cudaEvent_t ev_inv_l2_start[3] = {};
    cudaEvent_t ev_inv_l2_stop[3] = {};
    cudaEvent_t ev_inv_l1_start[3] = {};
    cudaEvent_t ev_inv_l1_stop[3] = {};
    cudaEvent_t ev_inv_l0_start[3] = {};
    cudaEvent_t ev_inv_l0_stop[3] = {};
    cudaEvent_t ev_field_start[3] = {};
    cudaEvent_t ev_field_stop[3] = {};
    cudaEvent_t ev_ch_done[3] = {};
    bool interlaced_gop1 = false;

    cudaEvent_t ev_color_start = nullptr;
    cudaEvent_t ev_color_stop = nullptr;
    cudaEvent_t ev_ready = nullptr;

    // Dispatch and DAG tracking state
    std::mutex dispatch_mutex;
    BandState band_state[kMaxVlcBands] = {};
    int band_assigned_stream[kMaxVlcBands] = {};
    int next_replay_stream = 0;
    int band_map[3][3][4] = {}; // [channel][level][band] -> vlc_descs index (0..26)
    ChannelInverseState ch_state[3] = {};
    bool color_enqueued = false;

    std::atomic<bool> has_error{false};
    std::atomic<int> active_index_workers{0}, peak_index_workers{0};
    double host_dispatch_cpu_ms = 0;
    std::string error_msg;

    // Host timing captures
    Clock::time_point t_frame_start;
    Clock::time_point t_parse_end;
    Clock::time_point t_prep_start;
    Clock::time_point t_prep_end;
    Clock::time_point t_batch_submit;
    Clock::time_point t_batch_return;
    TaskStat task_stats[kMaxVlcBands];

    CudaDecoderPipelinePhase2E::DelayHookFn delay_hook = nullptr;
    const GpuFsmEntry* d_fsm_tables = nullptr;

    Phase2eSlotStageTimings timings{};

    void FreeBuffers() {
        // Drain before freeing memory, including early destruction/rejected jobs.
        if (copy_stream) cudaStreamSynchronize(copy_stream);
        for (auto s : replay_streams) if (s) cudaStreamSynchronize(s);
        for (auto s : channel_streams) if (s) cudaStreamSynchronize(s);
        if (main_stream) cudaStreamSynchronize(main_stream);
        if (d_index_results) { cudaFree(d_index_results); d_index_results = nullptr; }
        if (d_frame_error) { cudaFree(d_frame_error); d_frame_error = nullptr; }
        if (host_frame_error) { cudaFreeHost(host_frame_error); host_frame_error = nullptr; }
        for (int i = 0; i < kMaxVlcBands; ++i) {
            if (ev_index_start[i]) cudaEventDestroy(ev_index_start[i]);
            if (ev_index_done[i]) cudaEventDestroy(ev_index_done[i]);
        }
        for (int ch = 0; ch < 3; ++ch) {
            auto& c = channels[ch];
            for (int b = 0; b < 4; ++b) {
                if (c.d_l2_band[b]) { cudaFree(c.d_l2_band[b]); c.d_l2_band[b] = nullptr; }
            }
            for (int b = 1; b < 4; ++b) {
                if (c.d_l1_band[b]) { cudaFree(c.d_l1_band[b]); c.d_l1_band[b] = nullptr; }
                if (c.d_l0_band[b]) { cudaFree(c.d_l0_band[b]); c.d_l0_band[b] = nullptr; }
            }
            if (c.d_ll1) { cudaFree(c.d_ll1); c.d_ll1 = nullptr; }
            if (c.d_ll0) { cudaFree(c.d_ll0); c.d_ll0 = nullptr; }
            if (c.d_vert_low) { cudaFree(c.d_vert_low); c.d_vert_low = nullptr; }
            if (c.d_vert_high) { cudaFree(c.d_vert_high); c.d_vert_high = nullptr; }
            if (c.d_final_plane) { cudaFree(c.d_final_plane); c.d_final_plane = nullptr; }
            for (int b = 0; b < 4; ++b) {
                if (fieldplus.d_w3_band[ch][b]) {
                    cudaFree(fieldplus.d_w3_band[ch][b]);
                    fieldplus.d_w3_band[ch][b] = nullptr;
                }
            }
            if (fieldplus.d_gop_high[ch]) { cudaFree(fieldplus.d_gop_high[ch]); fieldplus.d_gop_high[ch] = nullptr; }
            if (fieldplus.d_gop_low[ch]) { cudaFree(fieldplus.d_gop_low[ch]); fieldplus.d_gop_low[ch] = nullptr; }
            fieldplus.w3_pitch[ch] = 0;
            fieldplus.gop_high_pitch[ch] = 0;
        }
        if (d_bgra) { cudaFree(d_bgra); d_bgra = nullptr; }

        if (host_pinned_compressed) { cudaFreeHost(host_pinned_compressed); host_pinned_compressed = nullptr; }
        if (host_pinned_segments) { cudaFreeHost(host_pinned_segments); host_pinned_segments = nullptr; }
        if (d_compressed_sample) { cudaFree(d_compressed_sample); d_compressed_sample = nullptr; }
        if (d_ll_descriptors) { cudaFree(d_ll_descriptors); d_ll_descriptors = nullptr; }
        if (d_vlc_descriptors) { cudaFree(d_vlc_descriptors); d_vlc_descriptors = nullptr; }
        if (d_vlc_results) { cudaFree(d_vlc_results); d_vlc_results = nullptr; }
        if (d_segments) { cudaFree(d_segments); d_segments = nullptr; }
        if (d_seg_results) { cudaFree(d_seg_results); d_seg_results = nullptr; }
        if (fieldplus.d_ll_descriptors) { cudaFree(fieldplus.d_ll_descriptors); fieldplus.d_ll_descriptors = nullptr; }
        if (fieldplus.d_vlc_descriptors) { cudaFree(fieldplus.d_vlc_descriptors); fieldplus.d_vlc_descriptors = nullptr; }
        if (fieldplus.host_pinned_ll_descriptors) { cudaFreeHost(fieldplus.host_pinned_ll_descriptors); fieldplus.host_pinned_ll_descriptors = nullptr; }
        if (fieldplus.host_pinned_vlc_descriptors) { cudaFreeHost(fieldplus.host_pinned_vlc_descriptors); fieldplus.host_pinned_vlc_descriptors = nullptr; }
        if (fieldplus.d_segments) { cudaFree(fieldplus.d_segments); fieldplus.d_segments = nullptr; }
        if (fieldplus.d_segment_results) { cudaFree(fieldplus.d_segment_results); fieldplus.d_segment_results = nullptr; }
        if (fieldplus.host_pinned_segments) { cudaFreeHost(fieldplus.host_pinned_segments); fieldplus.host_pinned_segments = nullptr; }
        fieldplus.ll_descriptors.clear();
        fieldplus.vlc_descriptors.clear();
        fieldplus.segments.clear();
        fieldplus.allocated = false;
        fieldplus.allocated_device_bytes = 0;
        fieldplus.allocated_pinned_bytes = 0;
        fieldplus.allocation_calls = 0;

        if (d_fc_summaries) { cudaFree(d_fc_summaries); d_fc_summaries = nullptr; }
        if (host_pinned_fc_summaries) { cudaFreeHost(host_pinned_fc_summaries); host_pinned_fc_summaries = nullptr; }
        if (d_fc_checkpoints) { cudaFree(d_fc_checkpoints); d_fc_checkpoints = nullptr; }
        if (host_pinned_fc_checkpoints) { cudaFreeHost(host_pinned_fc_checkpoints); host_pinned_fc_checkpoints = nullptr; }
        if (d_fc_errors) { cudaFree(d_fc_errors); d_fc_errors = nullptr; }
        if (host_pinned_fc_errors) { cudaFreeHost(host_pinned_fc_errors); host_pinned_fc_errors = nullptr; }
        for (int i = 0; i < kMaxVlcBands; ++i) {
            if (ev_summary_start[i]) { cudaEventDestroy(ev_summary_start[i]); ev_summary_start[i] = nullptr; }
            if (ev_summary_done[i])  { cudaEventDestroy(ev_summary_done[i]);  ev_summary_done[i] = nullptr; }
        }

        if (copy_stream) { cudaStreamDestroy(copy_stream); copy_stream = nullptr; }
        for (int i = 0; i < kMaxReplayStreams; ++i) {
            if (replay_streams[i]) { cudaStreamDestroy(replay_streams[i]); replay_streams[i] = nullptr; }
        }
        for (int ch = 0; ch < 3; ++ch) {
            if (channel_streams[ch]) { cudaStreamDestroy(channel_streams[ch]); channel_streams[ch] = nullptr; }
            if (ev_ch_done[ch]) { cudaEventDestroy(ev_ch_done[ch]); ev_ch_done[ch] = nullptr; }
            if (ev_inv_l2_start[ch]) { cudaEventDestroy(ev_inv_l2_start[ch]); ev_inv_l2_start[ch] = nullptr; }
            if (ev_inv_l2_stop[ch])  { cudaEventDestroy(ev_inv_l2_stop[ch]);  ev_inv_l2_stop[ch] = nullptr; }
            if (ev_inv_l1_start[ch]) { cudaEventDestroy(ev_inv_l1_start[ch]); ev_inv_l1_start[ch] = nullptr; }
            if (ev_inv_l1_stop[ch])  { cudaEventDestroy(ev_inv_l1_stop[ch]);  ev_inv_l1_stop[ch] = nullptr; }
            if (ev_inv_l0_start[ch]) { cudaEventDestroy(ev_inv_l0_start[ch]); ev_inv_l0_start[ch] = nullptr; }
            if (ev_inv_l0_stop[ch])  { cudaEventDestroy(ev_inv_l0_stop[ch]);  ev_inv_l0_stop[ch] = nullptr; }
            if (ev_field_start[ch]) { cudaEventDestroy(ev_field_start[ch]); ev_field_start[ch] = nullptr; }
            if (ev_field_stop[ch])  { cudaEventDestroy(ev_field_stop[ch]);  ev_field_stop[ch] = nullptr; }
        }
        if (main_stream) { cudaStreamDestroy(main_stream); main_stream = nullptr; }

        if (ev_frame_start) { cudaEventDestroy(ev_frame_start); ev_frame_start = nullptr; }
        if (ev_h2d_start)   { cudaEventDestroy(ev_h2d_start);   ev_h2d_start = nullptr; }
        if (ev_h2d_stop)    { cudaEventDestroy(ev_h2d_stop);    ev_h2d_stop = nullptr; }
        if (ev_compressed_h2d_ready) { cudaEventDestroy(ev_compressed_h2d_ready); ev_compressed_h2d_ready = nullptr; }
        if (ev_ll_ready)    { cudaEventDestroy(ev_ll_ready);    ev_ll_ready = nullptr; }

        for (int b = 0; b < kMaxVlcBands; ++b) {
            if (ev_band_start[b]) { cudaEventDestroy(ev_band_start[b]); ev_band_start[b] = nullptr; }
            if (ev_band_done[b])  { cudaEventDestroy(ev_band_done[b]);  ev_band_done[b] = nullptr; }
        }
        for (int b = 0; b < kMaxBandSegmentScratch; ++b) {
            band_segments[b].clear();
        }

        if (ev_color_start) { cudaEventDestroy(ev_color_start); ev_color_start = nullptr; }
        if (ev_color_stop)  { cudaEventDestroy(ev_color_stop);  ev_color_stop = nullptr; }
        if (ev_ready)       { cudaEventDestroy(ev_ready);       ev_ready = nullptr; }

        in_use = false;
        ticket = 0;
        fieldplus_mode = false;
        frame_index = 0;
        status = DecodeTicketStatus::Invalid;
    }

    bool EnsureFieldPlusBuffers() {
        if (fieldplus.allocated) return true;
        size_t device_bytes = 0;
        size_t pinned_bytes = 0;
        uint64_t allocations = 0;
        auto device_alloc = [&](void** pointer, size_t bytes) {
            if (cudaMalloc(pointer, bytes) != cudaSuccess) return false;
            device_bytes += bytes;
            ++allocations;
            return true;
        };
        auto pinned_alloc = [&](void** pointer, size_t bytes) {
            if (cudaMallocHost(pointer, bytes) != cudaSuccess) return false;
            pinned_bytes += bytes;
            ++allocations;
            return true;
        };
        for (int ch = 0; ch < 3; ++ch) {
            const int channel_width = ch == 0 ? max_width : max_width / 2;
            fieldplus.w3_pitch[ch] = (size_t(channel_width / 4) * sizeof(int16_t) + 15) & ~size_t(15);
            fieldplus.gop_high_pitch[ch] = (size_t(channel_width / 2) * sizeof(int16_t) + 15) & ~size_t(15);
            for (int band = 0; band < 4; ++band) {
                if (!device_alloc(reinterpret_cast<void**>(&fieldplus.d_w3_band[ch][band]),
                    fieldplus.w3_pitch[ch] * size_t(max_height / 4))) goto fail;
            }
            if (!device_alloc(reinterpret_cast<void**>(&fieldplus.d_gop_high[ch]),
                fieldplus.gop_high_pitch[ch] * size_t(max_height / 2))) goto fail;
            if (!device_alloc(reinterpret_cast<void**>(&fieldplus.d_gop_low[ch]),
                fieldplus.gop_high_pitch[ch] * size_t(max_height / 2))) goto fail;
        }
        if (!device_alloc(reinterpret_cast<void**>(&fieldplus.d_ll_descriptors),
                3 * sizeof(GpuEntropyBandDescriptor)) ||
            !device_alloc(reinterpret_cast<void**>(&fieldplus.d_vlc_descriptors),
                kFieldPlusVlcBands * sizeof(GpuEntropyBandDescriptor)) ||
            !device_alloc(reinterpret_cast<void**>(&fieldplus.d_segments),
                kFieldPlusSegmentCapacity * sizeof(GpuEntropySegmentDescriptor)) ||
            !device_alloc(reinterpret_cast<void**>(&fieldplus.d_segment_results),
                kFieldPlusSegmentCapacity * sizeof(GpuSegmentDecodeResult)) ||
            !pinned_alloc(reinterpret_cast<void**>(&fieldplus.host_pinned_ll_descriptors),
                3 * sizeof(GpuEntropyBandDescriptor)) ||
            !pinned_alloc(reinterpret_cast<void**>(&fieldplus.host_pinned_vlc_descriptors),
                kFieldPlusVlcBands * sizeof(GpuEntropyBandDescriptor)) ||
            !pinned_alloc(reinterpret_cast<void**>(&fieldplus.host_pinned_segments),
                kFieldPlusSegmentCapacity * sizeof(GpuEntropySegmentDescriptor))) goto fail;

        fieldplus.allocated = true;
        fieldplus.allocated_device_bytes = device_bytes;
        fieldplus.allocated_pinned_bytes = pinned_bytes;
        fieldplus.allocation_calls = allocations;
        allocated_device_bytes += device_bytes;
        allocated_pinned_bytes += pinned_bytes;
        return true;

    fail:
        for (int ch = 0; ch < 3; ++ch) {
            for (int band = 0; band < 4; ++band) {
                if (fieldplus.d_w3_band[ch][band]) {
                    cudaFree(fieldplus.d_w3_band[ch][band]);
                    fieldplus.d_w3_band[ch][band] = nullptr;
                }
            }
            if (fieldplus.d_gop_high[ch]) { cudaFree(fieldplus.d_gop_high[ch]); fieldplus.d_gop_high[ch] = nullptr; }
            if (fieldplus.d_gop_low[ch]) { cudaFree(fieldplus.d_gop_low[ch]); fieldplus.d_gop_low[ch] = nullptr; }
            fieldplus.w3_pitch[ch] = 0;
            fieldplus.gop_high_pitch[ch] = 0;
        }
        if (fieldplus.d_ll_descriptors) { cudaFree(fieldplus.d_ll_descriptors); fieldplus.d_ll_descriptors = nullptr; }
        if (fieldplus.d_vlc_descriptors) { cudaFree(fieldplus.d_vlc_descriptors); fieldplus.d_vlc_descriptors = nullptr; }
        if (fieldplus.d_segments) { cudaFree(fieldplus.d_segments); fieldplus.d_segments = nullptr; }
        if (fieldplus.d_segment_results) { cudaFree(fieldplus.d_segment_results); fieldplus.d_segment_results = nullptr; }
        if (fieldplus.host_pinned_ll_descriptors) { cudaFreeHost(fieldplus.host_pinned_ll_descriptors); fieldplus.host_pinned_ll_descriptors = nullptr; }
        if (fieldplus.host_pinned_vlc_descriptors) { cudaFreeHost(fieldplus.host_pinned_vlc_descriptors); fieldplus.host_pinned_vlc_descriptors = nullptr; }
        if (fieldplus.host_pinned_segments) { cudaFreeHost(fieldplus.host_pinned_segments); fieldplus.host_pinned_segments = nullptr; }
        return false;
    }

    ~DecodeSlot2E() {
        FreeBuffers();
    }

    void LaunchLevel2Inverse(int ch, cudaStream_t s, bool overflow_protected = false) {
        auto& c = channels[ch];
        int ch_w = (ch == 0) ? width : (width / 2);
        int ch_h = height;
        int w_l2 = ch_w / 8;
        int h_l2 = ch_h / 8;

        dim3 block(16, 16);
        dim3 grid_v((w_l2 + block.x - 1) / block.x, (h_l2 + block.y - 1) / block.y);
        VerticalInverseKernel<<<grid_v, block, 0, s>>>(
            c.d_l2_band[0], int(c.l2_pitch),
            c.d_l2_band[1], int(c.l2_pitch),
            c.d_l2_band[2], int(c.l2_pitch),
            c.d_l2_band[3], int(c.l2_pitch),
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            w_l2, h_l2, overflow_protected ? 3 : 0, ErrorGuard()
        );

        dim3 block_h(64, 4);
        dim3 grid_h((w_l2 + block_h.x - 1) / block_h.x, (2 * h_l2 + block_h.y - 1) / block_h.y);
        HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            c.d_ll1, int(c.ll1_pitch),
            w_l2, 2 * h_l2, overflow_protected ? 4 : 0, ErrorGuard()
        );
    }

    void LaunchLevel1Inverse(int ch, cudaStream_t s, bool fieldplus_scalar_math = false) {
        auto& c = channels[ch];
        int ch_w = (ch == 0) ? width : (width / 2);
        int ch_h = height;
        int w_l1 = ch_w / 4;
        int h_l1 = ch_h / 4;

        dim3 block(16, 16);
        dim3 grid_v((w_l1 + block.x - 1) / block.x, (h_l1 + block.y - 1) / block.y);
        VerticalInverseKernel<<<grid_v, block, 0, s>>>(
            c.d_ll1, int(c.ll1_pitch),
            c.d_l1_band[1], int(c.l1_pitch),
            c.d_l1_band[2], int(c.l1_pitch),
            c.d_l1_band[3], int(c.l1_pitch),
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            w_l1, h_l1, 2, ErrorGuard()
        );

        dim3 block_h(64, 4);
        dim3 grid_h((w_l1 + block_h.x - 1) / block_h.x, (2 * h_l1 + block_h.y - 1) / block_h.y);
        // The FIELDPLUS production path matches upstream's 32-bit scalar
        // accumulator on narrow chroma bands. Existing GOP-1 paths retain the
        // saturating 16-bit implementation used by their established vectors.
        const int horizontal_descale = fieldplus_scalar_math ? 3 : 2;
        HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            c.d_ll0, int(c.ll0_pitch),
            w_l1, 2 * h_l1, horizontal_descale, ErrorGuard()
        );
    }

    void LaunchLevel0Inverse(int ch, cudaStream_t s, bool fieldplus_scalar_math = false) {
        auto& c = channels[ch];
        int ch_w = (ch == 0) ? width : (width / 2);
        int ch_h = height;
        int w_l0 = ch_w / 2;
        int h_l0 = ch_h / 2;

        // Progressive: spatial vertical, then spatial horizontal, then output.
        // Interlaced GOP-1: horizontal inverse of the temporal bands, then the
        // field combine. The field kernel is not inserted into the progressive
        // schedule. Levels 2 and 1 stay on the spatial kernels either way.
        if (interlaced_gop1) {
            dim3 block_h(64, 4);
            dim3 grid_h((w_l0 + block_h.x - 1) / block_h.x, (h_l0 + block_h.y - 1) / block_h.y);
            HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
                c.d_ll0, int(c.ll0_pitch),
                c.d_l0_band[1], int(c.l0_pitch),
                c.d_vert_low, int(c.vert_low_pitch),
                w_l0, h_l0, 0, ErrorGuard()
            );
            HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
                c.d_l0_band[2], int(c.l0_pitch),
                c.d_l0_band[3], int(c.l0_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                w_l0, h_l0, 0, ErrorGuard()
            );
            cudaEventRecord(ev_field_start[ch], s);
            dim3 block(32, 8);
            dim3 grid((ch_w + block.x - 1) / block.x, (h_l0 + block.y - 1) / block.y);
            FieldCombineKernel<<<grid, block, 0, s>>>(
                c.d_vert_low, int(c.vert_low_pitch),
                c.d_vert_high, int(c.vert_high_pitch),
                c.d_final_plane, int(c.final_plane_pitch),
                ch_w, h_l0, ErrorGuard()
            );
            cudaEventRecord(ev_field_stop[ch], s);
            return;
        }

        dim3 block(16, 16);
        dim3 grid_v((w_l0 + block.x - 1) / block.x, (h_l0 + block.y - 1) / block.y);
        VerticalInverseKernel<<<grid_v, block, 0, s>>>(
            c.d_ll0, int(c.ll0_pitch),
            c.d_l0_band[1], int(c.l0_pitch),
            c.d_l0_band[2], int(c.l0_pitch),
            c.d_l0_band[3], int(c.l0_pitch),
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            // InvertSpatialQuant16s uses 32-bit scalar accumulators for its
            // vertical pass. Existing GOP-1 vectors retain the 16-bit path.
            w_l0, h_l0, fieldplus_scalar_math ? 5 : 0, ErrorGuard()
        );

        dim3 block_h(64, 4);
        dim3 grid_h((w_l0 + block_h.x - 1) / block_h.x, (2 * h_l0 + block_h.y - 1) / block_h.y);
        HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            c.d_final_plane, int(c.final_plane_pitch),
            w_l0, 2 * h_l0, 0, ErrorGuard()
        );
    }

    void CheckAdvanceInverseDag(int ch) {
        // Must be called with dispatch_mutex held
        if (has_error.load(std::memory_order_relaxed)) return;

        // 1. Check Level 2 Inverse
        if (!ch_state[ch].l2_enqueued) {
            int b1 = band_map[ch][2][1];
            int b2 = band_map[ch][2][2];
            int b3 = band_map[ch][2][3];
            if (band_state[b1] == BandState::GpuQueued &&
                band_state[b2] == BandState::GpuQueued &&
                band_state[b3] == BandState::GpuQueued)
            {
                cudaStream_t s = channel_streams[ch];
                cudaStreamWaitEvent(s, ev_ll_ready, 0);
                cudaStreamWaitEvent(s, ev_band_done[b1], 0);
                cudaStreamWaitEvent(s, ev_band_done[b2], 0);
                cudaStreamWaitEvent(s, ev_band_done[b3], 0);

                cudaEventRecord(ev_inv_l2_start[ch], s);
                LaunchLevel2Inverse(ch, s);
                cudaEventRecord(ev_inv_l2_stop[ch], s);

                ch_state[ch].l2_enqueued = true;
            }
        }

        // 2. Check Level 1 Inverse
        if (ch_state[ch].l2_enqueued && !ch_state[ch].l1_enqueued) {
            int b1 = band_map[ch][1][1];
            int b2 = band_map[ch][1][2];
            int b3 = band_map[ch][1][3];
            if (band_state[b1] == BandState::GpuQueued &&
                band_state[b2] == BandState::GpuQueued &&
                band_state[b3] == BandState::GpuQueued)
            {
                cudaStream_t s = channel_streams[ch];
                cudaStreamWaitEvent(s, ev_band_done[b1], 0);
                cudaStreamWaitEvent(s, ev_band_done[b2], 0);
                cudaStreamWaitEvent(s, ev_band_done[b3], 0);

                cudaEventRecord(ev_inv_l1_start[ch], s);
                LaunchLevel1Inverse(ch, s);
                cudaEventRecord(ev_inv_l1_stop[ch], s);

                ch_state[ch].l1_enqueued = true;
            }
        }

        // 3. Check Level 0 Inverse
        if (ch_state[ch].l1_enqueued && !ch_state[ch].l0_enqueued) {
            int b1 = band_map[ch][0][1];
            int b2 = band_map[ch][0][2];
            int b3 = band_map[ch][0][3];
            if (band_state[b1] == BandState::GpuQueued &&
                band_state[b2] == BandState::GpuQueued &&
                band_state[b3] == BandState::GpuQueued)
            {
                cudaStream_t s = channel_streams[ch];
                cudaStreamWaitEvent(s, ev_band_done[b1], 0);
                cudaStreamWaitEvent(s, ev_band_done[b2], 0);
                cudaStreamWaitEvent(s, ev_band_done[b3], 0);

                cudaEventRecord(ev_inv_l0_start[ch], s);
                LaunchLevel0Inverse(ch, s);
                cudaEventRecord(ev_inv_l0_stop[ch], s);

                cudaEventRecord(ev_ch_done[ch], s);
                ch_state[ch].l0_enqueued = true;
            }
        }

        // 4. Check Color Conversion
        if (!color_enqueued &&
            ch_state[0].l0_enqueued &&
            ch_state[1].l0_enqueued &&
            ch_state[2].l0_enqueued)
        {
            cudaStreamWaitEvent(main_stream, ev_ch_done[0], 0);
            cudaStreamWaitEvent(main_stream, ev_ch_done[1], 0);
            cudaStreamWaitEvent(main_stream, ev_ch_done[2], 0);

            if (surface_type == DeviceSurfaceType::BGRA8) {
                cudaEventRecord(ev_color_start, main_stream);
                LaunchYuv422ToBgra8(
                    channels[0].d_final_plane, channels[0].final_plane_pitch,
                    channels[1].d_final_plane, channels[1].final_plane_pitch,
                    channels[2].d_final_plane, channels[2].final_plane_pitch,
                    d_bgra, bgra_pitch,
                    width, height,
                    matrix, range, orientation,
                    main_stream, ErrorGuard(), interlaced_gop1
                );
                cudaEventRecord(ev_color_stop, main_stream);
            }

            if (GpuIndex())
                cudaMemcpyAsync(host_frame_error, d_frame_error, sizeof(uint32_t), cudaMemcpyDeviceToHost, main_stream);
            cudaEventRecord(ev_ready, main_stream);
            color_enqueued = true;
        }
    }

    bool DispatchGpuIndex(int b) {
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        const int stream_id = next_replay_stream++ % num_replay_streams;
        band_assigned_stream[b] = stream_id;
        const auto stream = replay_streams[stream_id];
        if (cudaStreamWaitEvent(stream, ev_compressed_h2d_ready, 0) != cudaSuccess) return false;
        cudaEventRecord(ev_index_start[b], stream);
        auto err = LaunchGenerateBandCheckpointsGpu(d_compressed_sample, current_compressed_size,
            d_vlc_descriptors + b, b, d_fsm_tables, 18752, entropy_mode, fixed_segments_per_band,
            d_segments + b * kMaxSegmentsPerBand, kMaxSegmentsPerBand, d_index_results + b,
            d_frame_error, vlc_descs[b].payload_bytes, stream);
        if (err != cudaSuccess) return false;
        cudaEventRecord(ev_index_done[b], stream);
        cudaEventRecord(ev_band_start[b], stream);
        err = LaunchZeroVlcBandsGpu(d_vlc_descriptors + b, 1, stream);
        if (err != cudaSuccess) return false;
        err = LaunchDecodeEntropyParallelGpu(d_compressed_sample, current_compressed_size,
            d_vlc_descriptors, kMaxVlcBands, d_segments + b * kMaxSegmentsPerBand,
            kMaxSegmentsPerBand, d_fsm_tables, d_seg_results + b * kMaxSegmentsPerBand,
            stream, d_index_results + b, d_frame_error);
        if (err != cudaSuccess) return false;
        if (vlc_descs[b].has_peaks && vlc_descs[b].peak_level > 0)
            LaunchRestoreDecodePeaks(d_compressed_sample,current_compressed_size,d_vlc_descriptors+b,1,stream,ErrorGuard());
        if (vlc_descs[b].difference_coding)
            LaunchCooperativeDifferenceCodingGpu(d_vlc_descriptors + b, 1, stream, d_frame_error);
        cudaEventRecord(ev_band_done[b], stream);
        band_state[b] = BandState::GpuQueued;
        CheckAdvanceInverseDag(vlc_descs[b].channel);
        return cudaGetLastError() == cudaSuccess;
    }

    void DispatchBandGpu(int band_idx, int worker_id) {
        (void)worker_id;
        if (has_error.load(std::memory_order_relaxed)) return;

        std::lock_guard<std::mutex> lock(dispatch_mutex);
        if (has_error.load(std::memory_order_relaxed)) return;
        const double dispatch_cpu_start = ThreadCpuMs();

        int s_idx = next_replay_stream % num_replay_streams;
        next_replay_stream++;
        cudaStream_t s = replay_streams[s_idx];
        band_assigned_stream[band_idx] = s_idx;

        // 1. Wait until compressed sample H2D transfer completes
        cudaStreamWaitEvent(s, ev_compressed_h2d_ready, 0);

        // 2. Record band GPU start event
        cudaEventRecord(ev_band_start[band_idx], s);

        // 3. Stage pinned segment descriptors to device slice asynchronously
        int num_segs = static_cast<int>(band_segments[band_idx].size());
        GpuEntropySegmentDescriptor* pinned_slice = host_pinned_segments + band_idx * kMaxSegmentsPerBand;
        GpuEntropySegmentDescriptor* d_seg_dst = d_segments + band_idx * kMaxSegmentsPerBand;
        GpuSegmentDecodeResult* d_res_dst = d_seg_results + band_idx * kMaxSegmentsPerBand;

        if (num_segs > 0) {
            std::memcpy(pinned_slice, band_segments[band_idx].data(), num_segs * sizeof(GpuEntropySegmentDescriptor));
            cudaMemcpyAsync(
                d_seg_dst,
                pinned_slice,
                num_segs * sizeof(GpuEntropySegmentDescriptor),
                cudaMemcpyHostToDevice,
                s
            );
        }

        // 4. Zero the destination buffer in VRAM for this band
        LaunchZeroVlcBandsGpu(d_vlc_descriptors + band_idx, 1, s);

        // 5. Replay VLC segments on GPU
        if (num_segs > 0) {
            LaunchDecodeEntropyParallelGpu(
                d_compressed_sample,
                current_compressed_size,
                d_vlc_descriptors,
                kMaxVlcBands,
                d_seg_dst,
                num_segs,
                d_fsm_tables,
                d_res_dst,
                s, nullptr, ErrorGuard()
            );
        }

        if (vlc_descs[band_idx].has_peaks && vlc_descs[band_idx].peak_level > 0)
            LaunchRestoreDecodePeaks(d_compressed_sample,current_compressed_size,d_vlc_descriptors+band_idx,1,s,ErrorGuard());
        // 6. Cooperative Difference Coding if band requires it
        if (vlc_descs[band_idx].difference_coding) {
            LaunchCooperativeDifferenceCodingGpu(d_vlc_descriptors + band_idx, 1, s, ErrorGuard());
        }

        // 7. Record band GPU done event
        cudaEventRecord(ev_band_done[band_idx], s);
        band_state[band_idx] = BandState::GpuQueued;

        // 8. In FullOverlap or Hybrid mode, check if inverse DAG can advance
        if (pipeline_mode == Phase2EPipelineMode::Phase2EFullOverlap ||
            pipeline_mode == Phase2EPipelineMode::Phase2FCHybridFullFrame) {
            CheckAdvanceInverseDag(vlc_descs[band_idx].channel);
        }
        host_dispatch_cpu_ms += ThreadCpuMs() - dispatch_cpu_start;
    }

    void DispatchBandHybrid(int band_idx, uint32_t marker_nibble, uint32_t checkpoint_count) {
        if (has_error.load(std::memory_order_relaxed)) return;

        std::lock_guard<std::mutex> lock(dispatch_mutex);
        if (has_error.load(std::memory_order_relaxed)) return;
        const double dispatch_cpu_start = ThreadCpuMs();

        int s_idx = next_replay_stream % num_replay_streams;
        next_replay_stream++;
        cudaStream_t s = replay_streams[s_idx];
        band_assigned_stream[band_idx] = s_idx;

        const auto& d = vlc_descs[band_idx];
        size_t c_offset = band_chunk_offset[band_idx];

        if (checkpoint_count > 0) {
            cudaMemcpyAsync(
                d_fc_checkpoints + c_offset,
                host_pinned_fc_checkpoints + c_offset,
                checkpoint_count * sizeof(HyperCine::HybridEntropy::Restart),
                cudaMemcpyHostToDevice,
                s
            );
        }

        LaunchZeroVlcBandsGpu(d_vlc_descriptors + band_idx, 1, s);
        cudaEventRecord(ev_band_start[band_idx], s);

        if (checkpoint_count > 0) {
            size_t codebook_offset = (d.codebook_id == 1) ? CS17_BASE_OFFSET :
                                    ((d.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);
            const GpuFsmEntry* d_table = d_fsm_tables + codebook_offset;
            LaunchHybridReplayGpu(
                d_compressed_sample + d.payload_offset,
                d_table,
                d_fc_checkpoints + c_offset,
                checkpoint_count,
                marker_nibble,
                static_cast<uint32_t>(d.coefficient_count),
                d.quantization,
                d.d_destination,
                d_fc_errors + band_idx,
                s,
                d.width,
                d.pitch_samples
            );
        }

        if (d.has_peaks && d.peak_level > 0)
            LaunchRestoreDecodePeaks(d_compressed_sample,current_compressed_size,d_vlc_descriptors+band_idx,1,s,ErrorGuard());
        if (d.difference_coding) {
            LaunchHybridDifferenceGpu(
                d.d_destination,
                d.width,
                d.height,
                d.pitch_samples,
                s
            );
        }

        cudaEventRecord(ev_band_done[band_idx], s);
        band_state[band_idx] = BandState::GpuQueued;

        CheckAdvanceInverseDag(d.channel);
        host_dispatch_cpu_ms += ThreadCpuMs() - dispatch_cpu_start;
    }
};

} // anonymous namespace

struct CudaDecoderPipelinePhase2E::Impl {
    int max_width = 0;
    int max_height = 0;
    int precision = 10;
    int cuda_device = 0;
    int num_slots = 3;
    bool initialized = false;

    Phase2EPipelineMode pipeline_mode = Phase2EPipelineMode::Phase2EFullOverlap;
    Phase2ESchedulingPolicy scheduling_policy = Phase2ESchedulingPolicy::PolicyA_LargestFirst;
    GpuEntropyMode entropy_mode = GpuEntropyMode::ParallelAdaptive;
    int fixed_segments_per_band = 32;
    int configured_workers = 0;
    int num_replay_streams = 4;

    DecoderWorkerPool worker_pool;
    GpuFsmTableManager fsm_table_mgr;
    std::vector<GpuFsmEntry> host_fsm_tables;

    // Phase 2F-C parameters
    Phase2FCSelectorPolicy fc_selector_policy = Phase2FCSelectorPolicy::StaticHeuristic;
    uint32_t fc_chunk_size = 256;
    std::vector<BandEntropyMethod> fc_oracle_decisions;

    std::vector<std::unique_ptr<DecodeSlot2E>> slots;
    std::mutex mutex;
    uint64_t next_ticket = 1;

    DelayHookFn delay_hook = nullptr;

    void AbandonSlot(DecodeSlot2E* s) {
        if (!s) return;
        if (s->copy_stream) cudaStreamSynchronize(s->copy_stream);
        if (s->main_stream) cudaStreamSynchronize(s->main_stream);
        for (int i = 0; i < s->num_replay_streams; ++i) {
            if (s->replay_streams[i]) cudaStreamSynchronize(s->replay_streams[i]);
        }
        for (int ch = 0; ch < 3; ++ch) {
            if (s->channel_streams[ch]) cudaStreamSynchronize(s->channel_streams[ch]);
        }
        std::lock_guard<std::mutex> lock(mutex);
        s->status = DecodeTicketStatus::Failed;
        s->in_use = false;
        s->ticket = 0;
    }

    DecodeSlot2E* AcquireSlot() {
        for (auto& s : slots) {
            if (!s->in_use) {
                s->in_use = true;
                s->ticket = next_ticket++;
                s->status = DecodeTicketStatus::Pending;
                s->fieldplus_mode = false;
                s->frame_index = 0;
                s->interlaced_gop1 = false;
                s->production_guard = false;
                s->pipeline_mode = pipeline_mode;
                s->scheduling_policy = scheduling_policy;
                s->entropy_mode = entropy_mode;
                s->fixed_segments_per_band = fixed_segments_per_band;
                s->num_replay_streams = num_replay_streams;
                s->delay_hook = delay_hook;
                s->d_fsm_tables = fsm_table_mgr.GetDeviceTable();
                s->fc_selector_policy = fc_selector_policy;
                s->fc_chunk_size = fc_chunk_size;
                s->fc_oracle_decisions = fc_oracle_decisions;
                s->has_error.store(false);
                s->active_index_workers = 0; s->peak_index_workers = 0;
                s->host_dispatch_cpu_ms = 0;
                s->error_msg.clear();
                s->next_replay_stream = 0;
                s->color_enqueued = false;
                for (int b = 0; b < kMaxVlcBands; ++b) {
                    s->band_state[b] = BandState::Unindexed;
                    s->band_assigned_stream[b] = -1;
                    s->task_stats[b] = TaskStat{};
                    s->band_method[b] = BandEntropyMethod::MethodE_HostIndex;
                    s->fc_band_records[b] = Phase2FCBandRecord{};
                    s->band_chunk_offset[b] = 0;
                    s->band_chunk_count[b] = 0;
                }
                for (int ch = 0; ch < 3; ++ch) {
                    s->ch_state[ch] = ChannelInverseState{};
                }
                return s.get();
            }
        }
        return nullptr;
    }

    DecodeSlot2E* FindSlot(uint64_t ticket) {
        for (auto& s : slots) {
            if (s->in_use && s->ticket == ticket) return s.get();
        }
        return nullptr;
    }
};

CudaDecoderPipelinePhase2E::CudaDecoderPipelinePhase2E() : impl_(std::make_unique<Impl>()) {}

CudaDecoderPipelinePhase2E::~CudaDecoderPipelinePhase2E() {
    Release();
}

CudaDecoderPipelinePhase2E::CudaDecoderPipelinePhase2E(CudaDecoderPipelinePhase2E&&) noexcept = default;
CudaDecoderPipelinePhase2E& CudaDecoderPipelinePhase2E::operator=(CudaDecoderPipelinePhase2E&&) noexcept = default;

bool CudaDecoderPipelinePhase2E::Initialize(
    int max_width,
    int max_height,
    int num_slots,
    int precision,
    int num_host_workers,
    int num_replay_streams,
    int cuda_device
) {
    Release();

    if (max_width <= 0 || max_height <= 0 || num_slots <= 0) return false;
    if (num_replay_streams <= 0) num_replay_streams = 4;
    if (num_replay_streams > kMaxReplayStreams) num_replay_streams = kMaxReplayStreams;

    cudaError_t err = cudaSetDevice(cuda_device);
    if (err != cudaSuccess) return false;

    impl_->max_width = max_width;
    impl_->max_height = max_height;
    impl_->precision = precision;
    impl_->cuda_device = cuda_device;
    impl_->num_slots = num_slots;
    impl_->configured_workers = num_host_workers;
    impl_->num_replay_streams = num_replay_streams;

    // 1. Initialize persistent FSM tables
    if (!ExtractCineFormBaseFsmTables(impl_->host_fsm_tables)) {
        std::cerr << "Phase 2E Error: Failed to extract CineForm base FSM tables" << std::endl;
        return false;
    }

    if (!impl_->fsm_table_mgr.Initialize(cuda_device)) {
        std::cerr << "Phase 2E Error: Failed to initialize GPU FSM Table Manager" << std::endl;
        return false;
    }

    // 2. Initialize persistent worker pool
    if (!impl_->worker_pool.Initialize(num_host_workers)) {
        std::cerr << "Phase 2E Error: Failed to initialize DecoderWorkerPool" << std::endl;
        return false;
    }

    // 3. Allocate slot ring buffers
    impl_->slots.reserve(num_slots);
    for (int i = 0; i < num_slots; ++i) {
        auto slot = std::make_unique<DecodeSlot2E>();
        slot->slot_index = i;
        slot->max_width = max_width;
        slot->max_height = max_height;
        slot->precision = precision;
        slot->cuda_device = cuda_device;
        slot->num_replay_streams = num_replay_streams;

        for (int b = 0; b < kMaxBandSegmentScratch; ++b) {
            slot->band_segments[b].reserve(kMaxSegmentsPerBand);
        }
        slot->host_segments.reserve(kTotalSegmentCapacity);
        slot->host_bands.reserve(30);
        slot->ll_descs.reserve(3);
        slot->vlc_descs.reserve(kMaxVlcBands);
        slot->fieldplus.ll_descriptors.reserve(3);
        slot->fieldplus.vlc_descriptors.reserve(kFieldPlusVlcBands);
        slot->fieldplus.segments.reserve(kFieldPlusSegmentCapacity);

        for (int ch = 0; ch < 3; ++ch) {
            auto& c = slot->channels[ch];
            int ch_max_w = (ch == 0) ? max_width : (max_width / 2);
            int ch_max_h = max_height;

            int w_l2 = ch_max_w / 8;
            int h_l2 = ch_max_h / 8;
            int w_l1 = ch_max_w / 4;
            int h_l1 = ch_max_h / 4;
            int w_l0 = ch_max_w / 2;
            int h_l0 = ch_max_h / 2;

            c.l2_pitch = (static_cast<size_t>(w_l2) * sizeof(int16_t) + 15) & ~size_t(15);
            c.l1_pitch = (static_cast<size_t>(w_l1) * sizeof(int16_t) + 15) & ~size_t(15);
            c.l0_pitch = (static_cast<size_t>(w_l0) * sizeof(int16_t) + 15) & ~size_t(15);

            for (int b = 0; b < 4; ++b) {
                err = cudaMalloc(&c.d_l2_band[b], c.l2_pitch * h_l2);
                if (err != cudaSuccess) return false;
            }

            for (int b = 1; b < 4; ++b) {
                err = cudaMalloc(&c.d_l1_band[b], c.l1_pitch * h_l1);
                if (err != cudaSuccess) return false;
                err = cudaMalloc(&c.d_l0_band[b], c.l0_pitch * h_l0);
                if (err != cudaSuccess) return false;
            }

            c.ll1_pitch = c.l1_pitch;
            err = cudaMalloc(&c.d_ll1, c.ll1_pitch * h_l1);
            if (err != cudaSuccess) return false;
            c.d_l1_band[0] = c.d_ll1;

            c.ll0_pitch = c.l0_pitch;
            err = cudaMalloc(&c.d_ll0, c.ll0_pitch * h_l0);
            if (err != cudaSuccess) return false;
            c.d_l0_band[0] = c.d_ll0;

            // Full channel width so the interlaced horizontal inverse can park
            // temporal low/high here (full width, half height) without a second
            // frame buffer. Progressive vertical writes only the left half.
            // Height stays the full frame, which the spatial vertical kernel needs.
            err = cudaMallocPitch(&c.d_vert_low, &c.vert_low_pitch, ch_max_w * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;
            err = cudaMallocPitch(&c.d_vert_high, &c.vert_high_pitch, ch_max_w * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;

            err = cudaMallocPitch(&c.d_final_plane, &c.final_plane_pitch, ch_max_w * sizeof(int16_t), ch_max_h);
            if (err != cudaSuccess) return false;

        }

        err = cudaMallocPitch(&slot->d_bgra, &slot->bgra_pitch, max_width * 4, max_height);
        if (err != cudaSuccess) return false;

        if (cudaMallocHost(&slot->host_pinned_compressed, slot->compressed_capacity) != cudaSuccess ||
            cudaMallocHost(&slot->host_pinned_segments, kTotalSegmentCapacity * sizeof(GpuEntropySegmentDescriptor)) != cudaSuccess ||
            cudaMalloc(&slot->d_compressed_sample, slot->compressed_capacity) != cudaSuccess ||
            cudaMalloc(&slot->d_ll_descriptors, 3 * sizeof(GpuEntropyBandDescriptor)) != cudaSuccess ||
            cudaMalloc(&slot->d_vlc_descriptors, kMaxVlcBands * sizeof(GpuEntropyBandDescriptor)) != cudaSuccess ||
            cudaMalloc(&slot->d_vlc_results, kMaxVlcBands * sizeof(GpuBandDecodeResult)) != cudaSuccess ||
            cudaMalloc(&slot->d_segments, kTotalSegmentCapacity * sizeof(GpuEntropySegmentDescriptor)) != cudaSuccess ||
            cudaMalloc(&slot->d_seg_results, kTotalSegmentCapacity * sizeof(GpuSegmentDecodeResult)) != cudaSuccess) return false;

        if (cudaMalloc(&slot->d_index_results, kMaxVlcBands * sizeof(GpuCheckpointIndexResult)) != cudaSuccess ||
            cudaMalloc(&slot->d_frame_error, sizeof(uint32_t)) != cudaSuccess ||
            cudaMallocHost(&slot->host_frame_error, sizeof(uint32_t)) != cudaSuccess) return false;

        const size_t kMaxFcChunks = 131072;
        if (cudaMalloc(&slot->d_fc_summaries, kMaxFcChunks * sizeof(HyperCine::HybridEntropy::Summary)) != cudaSuccess ||
            cudaMallocHost(&slot->host_pinned_fc_summaries, kMaxFcChunks * sizeof(HyperCine::HybridEntropy::Summary)) != cudaSuccess ||
            cudaMalloc(&slot->d_fc_checkpoints, kMaxFcChunks * sizeof(HyperCine::HybridEntropy::Restart)) != cudaSuccess ||
            cudaMallocHost(&slot->host_pinned_fc_checkpoints, kMaxFcChunks * sizeof(HyperCine::HybridEntropy::Restart)) != cudaSuccess ||
            cudaMalloc(&slot->d_fc_errors, kMaxVlcBands * sizeof(uint32_t)) != cudaSuccess ||
            cudaMallocHost(&slot->host_pinned_fc_errors, kMaxVlcBands * sizeof(uint32_t)) != cudaSuccess) return false;

        // Create streams
        cudaStreamCreateWithFlags(&slot->copy_stream, cudaStreamNonBlocking);
        for (int s = 0; s < kMaxReplayStreams; ++s) {
            cudaStreamCreateWithFlags(&slot->replay_streams[s], cudaStreamNonBlocking);
        }
        for (int ch = 0; ch < 3; ++ch) {
            cudaStreamCreateWithFlags(&slot->channel_streams[ch], cudaStreamNonBlocking);
            cudaEventCreateWithFlags(&slot->ev_ch_done[ch], cudaEventDisableTiming);
            cudaEventCreate(&slot->ev_inv_l2_start[ch]);
            cudaEventCreate(&slot->ev_inv_l2_stop[ch]);
            cudaEventCreate(&slot->ev_inv_l1_start[ch]);
            cudaEventCreate(&slot->ev_inv_l1_stop[ch]);
            cudaEventCreate(&slot->ev_inv_l0_start[ch]);
            cudaEventCreate(&slot->ev_inv_l0_stop[ch]);
            cudaEventCreate(&slot->ev_field_start[ch]);
            cudaEventCreate(&slot->ev_field_stop[ch]);
        }
        cudaStreamCreateWithFlags(&slot->main_stream, cudaStreamNonBlocking);

        // Create timing events
        cudaEventCreate(&slot->ev_frame_start);
        cudaEventCreate(&slot->ev_h2d_start);
        cudaEventCreate(&slot->ev_h2d_stop);
        cudaEventCreate(&slot->ev_compressed_h2d_ready);
        cudaEventCreate(&slot->ev_ll_ready);

        for (int b = 0; b < kMaxVlcBands; ++b) {
            cudaEventCreate(&slot->ev_index_start[b]);
            cudaEventCreate(&slot->ev_index_done[b]);
            cudaEventCreate(&slot->ev_band_start[b]);
            cudaEventCreate(&slot->ev_band_done[b]);
            cudaEventCreate(&slot->ev_summary_start[b]);
            cudaEventCreate(&slot->ev_summary_done[b]);
        }

        cudaEventCreate(&slot->ev_color_start);
        cudaEventCreate(&slot->ev_color_stop);
        cudaEventCreate(&slot->ev_ready);
        // Failed creation leaves the initialized-null handle untouched. The
        // slot destructor drains and releases every successfully owned object.
        if (!slot->copy_stream || !slot->main_stream || !slot->ev_frame_start ||
            !slot->ev_h2d_start || !slot->ev_h2d_stop || !slot->ev_compressed_h2d_ready ||
            !slot->ev_ll_ready || !slot->ev_color_start || !slot->ev_color_stop || !slot->ev_ready) return false;
        for (auto stream : slot->replay_streams) if (!stream) return false;
        for (int ch = 0; ch < 3; ++ch)
            if (!slot->channel_streams[ch] || !slot->ev_ch_done[ch] ||
                !slot->ev_inv_l2_start[ch] || !slot->ev_inv_l2_stop[ch] ||
                !slot->ev_inv_l1_start[ch] || !slot->ev_inv_l1_stop[ch] ||
                !slot->ev_inv_l0_start[ch] || !slot->ev_inv_l0_stop[ch] ||
                !slot->ev_field_start[ch] || !slot->ev_field_stop[ch]) return false;
        for (int b = 0; b < kMaxVlcBands; ++b)
            if (!slot->ev_index_start[b] || !slot->ev_index_done[b] || !slot->ev_band_start[b] ||
                !slot->ev_band_done[b] || !slot->ev_summary_start[b] || !slot->ev_summary_done[b]) return false;

        size_t device_bytes = slot->compressed_capacity + 30 * sizeof(GpuEntropyBandDescriptor) +
            kMaxVlcBands * (sizeof(GpuBandDecodeResult) + sizeof(GpuCheckpointIndexResult) + sizeof(uint32_t)) +
            kTotalSegmentCapacity * (sizeof(GpuEntropySegmentDescriptor) + sizeof(GpuSegmentDecodeResult)) +
            sizeof(uint32_t) + kMaxFcChunks * (sizeof(Summary) + sizeof(Restart)) + slot->bgra_pitch * max_height;
        for (int ch = 0; ch < 3; ++ch) {
            const auto& c = slot->channels[ch];
            device_bytes += 4 * c.l2_pitch * (max_height / 8) +
                4 * c.l1_pitch * (max_height / 4) + 4 * c.l0_pitch * (max_height / 2) +
                (c.vert_low_pitch + c.vert_high_pitch + c.final_plane_pitch) * max_height;
        }
        slot->allocated_device_bytes = device_bytes;
        slot->allocated_pinned_bytes = slot->compressed_capacity +
            kTotalSegmentCapacity * sizeof(GpuEntropySegmentDescriptor) + sizeof(uint32_t) +
            kMaxFcChunks * (sizeof(Summary) + sizeof(Restart)) + kMaxVlcBands * sizeof(uint32_t);

        impl_->slots.push_back(std::move(slot));
    }

    impl_->initialized = true;
    return true;
}

void CudaDecoderPipelinePhase2E::Release() {
    if (!impl_) return;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->worker_pool.Shutdown();
        impl_->slots.clear();
        impl_->fsm_table_mgr.Release();
        impl_->host_fsm_tables.clear();
        impl_->initialized = false;
    }
}

bool CudaDecoderPipelinePhase2E::IsInitialized() const {
    return impl_ && impl_->initialized;
}

void CudaDecoderPipelinePhase2E::PrepareProductionScratch() {
    for (auto& slot : impl_->slots)
        for (auto& resolved : slot->reusable_resolved) resolved.checkpoints.reserve(131072);
}

DecoderMemoryStats CudaDecoderPipelinePhase2E::GetMemoryStats() const {
    DecoderMemoryStats out;
    if (!impl_ || !impl_->initialized) return out;
    out.slot_count = int(impl_->slots.size());
    out.immutable_device_bytes = 18752 * sizeof(GpuFsmEntry);
    out.immutable_host_bytes = impl_->host_fsm_tables.capacity() * sizeof(GpuFsmEntry);
    out.device_bytes = out.immutable_device_bytes;
    out.pageable_capacity_bytes = sizeof(Impl) + out.immutable_host_bytes;
    out.cuda_allocation_calls = 1 + uint64_t(out.slot_count) * 63;
    for (const auto& s : impl_->slots) {
        out.device_bytes += s->allocated_device_bytes;
        out.pinned_host_bytes += s->allocated_pinned_bytes;
        out.cuda_allocation_calls += s->fieldplus.allocation_calls;
        out.pageable_capacity_bytes += sizeof(DecodeSlot2E) +
            (s->host_bands.capacity() + s->vlc_descs.capacity() + s->ll_descs.capacity()) * sizeof(GpuEntropyBandDescriptor) +
            s->host_segments.capacity() * sizeof(GpuEntropySegmentDescriptor) +
            (s->fieldplus.ll_descriptors.capacity() + s->fieldplus.vlc_descriptors.capacity()) * sizeof(GpuEntropyBandDescriptor) +
            s->fieldplus.segments.capacity() * sizeof(GpuEntropySegmentDescriptor);
        for (int b = 0; b < kMaxBandSegmentScratch; ++b) {
            out.pageable_capacity_bytes += s->band_segments[b].capacity() * sizeof(GpuEntropySegmentDescriptor);
        }
        for (int b = 0; b < kMaxVlcBands; ++b)
            out.pageable_capacity_bytes += s->reusable_resolved[b].checkpoints.capacity() * sizeof(Restart);
    }
    return out;
}

void CudaDecoderPipelinePhase2E::SetPipelineMode(Phase2EPipelineMode mode) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pipeline_mode = mode;
    }
}

Phase2EPipelineMode CudaDecoderPipelinePhase2E::GetPipelineMode() const {
    return impl_ ? impl_->pipeline_mode : Phase2EPipelineMode::Phase2EFullOverlap;
}

void CudaDecoderPipelinePhase2E::SetSchedulingPolicy(Phase2ESchedulingPolicy policy) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->scheduling_policy = policy;
    }
}

Phase2ESchedulingPolicy CudaDecoderPipelinePhase2E::GetSchedulingPolicy() const {
    return impl_ ? impl_->scheduling_policy : Phase2ESchedulingPolicy::PolicyA_LargestFirst;
}

bool CudaDecoderPipelinePhase2E::SetWorkerCount(int num_workers) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->configured_workers = num_workers;
    return impl_->worker_pool.Initialize(num_workers);
}

int CudaDecoderPipelinePhase2E::GetWorkerCount() const {
    return impl_ ? impl_->worker_pool.WorkerCount() : 0;
}

bool CudaDecoderPipelinePhase2E::SetReplayStreamCount(int count) {
    if (!impl_ || count <= 0) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (count > kMaxReplayStreams) count = kMaxReplayStreams;
    impl_->num_replay_streams = count;
    for (auto& s : impl_->slots) {
        s->num_replay_streams = count;
    }
    return true;
}

int CudaDecoderPipelinePhase2E::GetReplayStreamCount() const {
    return impl_ ? impl_->num_replay_streams : 4;
}

void CudaDecoderPipelinePhase2E::SetEntropyMode(GpuEntropyMode mode, int fixed_segments_per_band) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->entropy_mode = mode;
        impl_->fixed_segments_per_band = fixed_segments_per_band;
    }
}

GpuEntropyMode CudaDecoderPipelinePhase2E::GetEntropyMode() const {
    return impl_ ? impl_->entropy_mode : GpuEntropyMode::ParallelAdaptive;
}

int CudaDecoderPipelinePhase2E::GetFixedSegmentsPerBand() const {
    return impl_ ? impl_->fixed_segments_per_band : 32;
}

void CudaDecoderPipelinePhase2E::SetTestDelayHook(DelayHookFn hook) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->delay_hook = hook;
    }
}

uint64_t CudaDecoderPipelinePhase2E::QueueDecode(
    const uint8_t* compressed_sample,
    size_t compressed_size,
    DeviceSurfaceType requested_surface,
    ColorMatrix matrix,
    ColorRange range,
    RowOrientation orientation,
    cudaStream_t caller_stream
) {
    if (!impl_ || !impl_->initialized || !compressed_sample || compressed_size == 0) return 0;

    auto t_frame_start = Clock::now();

    int ch_offset = (requested_surface == DeviceSurfaceType::BGRA8) ? 24 : 4;
    std::vector<GpuEntropyBandDescriptor> parsed_bands;
    std::string parse_err;

    if (!ParseCineFormSample(compressed_sample, compressed_size, ch_offset, parsed_bands, &parse_err)) {
        std::cerr << "Phase 2E Parse Error: " << parse_err << std::endl;
        return 0;
    }

    if (parsed_bands.size() != 30) {
        std::cerr << "Phase 2E Parse Error: expected 30 bands for YUV422 progressive, got "
                  << parsed_bands.size() << std::endl;
        return 0;
    }

    auto t_parse_end = Clock::now();
    float host_parse_ms = std::chrono::duration<float, std::milli>(t_parse_end - t_frame_start).count();

    auto result = QueueDecodeParsed(compressed_sample, compressed_size, parsed_bands,
        requested_surface, matrix, range, orientation, false, host_parse_ms);
    if (result && caller_stream) StreamWait(result, caller_stream);
    return result;
}

uint64_t CudaDecoderPipelinePhase2E::QueueDecodeParsed(const uint8_t* compressed_sample,
    size_t compressed_size, const std::vector<GpuEntropyBandDescriptor>& input_bands,
    DeviceSurfaceType requested_surface, ColorMatrix matrix, ColorRange range,
    RowOrientation orientation, bool production_guard, float host_parse_ms, bool interlaced_gop1) {
    if (!impl_ || !impl_->initialized || input_bands.size() != 30) return 0;
    auto t_parse_end = Clock::now();
    auto t_frame_start = t_parse_end - std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<float, std::milli>(host_parse_ms));
    cudaStream_t caller_stream = nullptr;

    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->AcquireSlot();
    }

    if (!slot) {
        std::cerr << "Phase 2E Error: No available slot in decoder pool" << std::endl;
        return 0;
    }

    auto& parsed_bands = slot->host_bands;
    parsed_bands = input_bands;

    slot->timings = Phase2eSlotStageTimings{};
    slot->production_guard = production_guard;
    slot->timings.host_parse_ms = host_parse_ms;
    slot->surface_type = requested_surface;
    slot->matrix = matrix;
    slot->range = range;
    slot->orientation = orientation;
    slot->interlaced_gop1 = interlaced_gop1;
    slot->t_frame_start = t_frame_start;
    slot->t_parse_end = t_parse_end;

    int frame_width = parsed_bands[7].width * 2;
    int frame_height = parsed_bands[7].height * 2;
    if (frame_width > slot->max_width || frame_height > slot->max_height) {
        impl_->AbandonSlot(slot);
        return 0;
    }
    slot->width = frame_width;
    slot->height = frame_height;

    // Reuse maximum-size allocations with this frame's compact band strides.
    for (int ch = 0; ch < 3; ++ch) {
        auto& c = slot->channels[ch];
        int w = ch == 0 ? frame_width : frame_width / 2;
        c.l2_pitch = (size_t(w / 8) * 2 + 15) & ~size_t(15);
        c.l1_pitch = (size_t(w / 4) * 2 + 15) & ~size_t(15);
        c.l0_pitch = (size_t(w / 2) * 2 + 15) & ~size_t(15);
        c.ll1_pitch = c.l1_pitch; c.ll0_pitch = c.l0_pitch;
    }
    // Partition into 3 LL bands and 27 VLC bands and bind destination pointers
    slot->ll_descs.clear();
    slot->vlc_descs.clear();
    slot->ll_descs.reserve(3);
    slot->vlc_descs.reserve(kMaxVlcBands);

    for (auto& b : parsed_bands) {
        int ch = b.channel;
        auto& c = slot->channels[ch];

        if (b.subband == 0) {
            b.d_destination = c.d_l2_band[0];
            b.pitch_bytes = c.l2_pitch;
            b.pitch_samples = c.l2_pitch / sizeof(int16_t);
            slot->ll_descs.push_back(b);
        } else {
            int lvl = b.level;
            int bidx = b.band;
            if (lvl == 2) {
                b.d_destination = c.d_l2_band[bidx];
                b.pitch_bytes = c.l2_pitch;
                b.pitch_samples = c.l2_pitch / sizeof(int16_t);
            } else if (lvl == 1) {
                b.d_destination = c.d_l1_band[bidx];
                b.pitch_bytes = c.l1_pitch;
                b.pitch_samples = c.l1_pitch / sizeof(int16_t);
            } else {
                b.d_destination = c.d_l0_band[bidx];
                b.pitch_bytes = c.l0_pitch;
                b.pitch_samples = c.l0_pitch / sizeof(int16_t);
            }
            slot->vlc_descs.push_back(b);
        }
    }

    slot->host_bands = parsed_bands;

    if (compressed_size > slot->compressed_capacity) {
        impl_->AbandonSlot(slot);
        return 0;
    }

    if (slot->ll_descs.size() != 3 || slot->vlc_descs.size() != kMaxVlcBands) {
        std::cerr << "Phase 2E Parse Error: expected 3 LL + 27 VLC bands" << std::endl;
        impl_->AbandonSlot(slot);
        return 0;
    }

    // Task Preparation & Scheduling Policy Sorting
    slot->t_prep_start = Clock::now();

    for (int i = 0; i < kMaxVlcBands; ++i) {
        const auto& d = slot->vlc_descs[i];
        slot->band_map[d.channel][d.level][d.band] = i;
        slot->band_segments[i].clear();
    }

    int largest_idx = 0;
    uint32_t largest_bytes = 0;
    for (int i = 0; i < kMaxVlcBands; ++i) {
        if (slot->vlc_descs[i].payload_bytes > largest_bytes) {
            largest_bytes = slot->vlc_descs[i].payload_bytes;
            largest_idx = i;
        }
    }
    slot->timings.largest_band_idx = largest_idx;
    slot->timings.largest_band_bytes = largest_bytes;

    int task_indices[kMaxVlcBands];
    for (int i = 0; i < kMaxVlcBands; ++i) task_indices[i] = i;

    if (slot->scheduling_policy == Phase2ESchedulingPolicy::PolicyA_LargestFirst) {
        std::sort(task_indices, task_indices + kMaxVlcBands, [&](int a, int b) {
            return slot->vlc_descs[a].payload_bytes > slot->vlc_descs[b].payload_bytes;
        });
    } else if (slot->scheduling_policy == Phase2ESchedulingPolicy::PolicyB_InverseCritical) {
        // Deepest level first (Level 2 -> Level 1 -> Level 0), channel ascending (Y first)
        std::sort(task_indices, task_indices + kMaxVlcBands, [&](int a, int b) {
            const auto& da = slot->vlc_descs[a];
            const auto& db = slot->vlc_descs[b];
            if (da.level != db.level) return da.level > db.level;
            if (da.channel != db.channel) return da.channel < db.channel;
            return da.payload_bytes > db.payload_bytes;
        });
    } else if (slot->scheduling_policy == Phase2ESchedulingPolicy::PolicyC_Hybrid) {
        // Depth-weighted payload bytes
        auto score = [&](int idx) {
            const auto& d = slot->vlc_descs[idx];
            uint64_t weight = (d.level == 2) ? 3 : ((d.level == 1) ? 2 : 1);
            return static_cast<uint64_t>(d.payload_bytes) * weight;
        };
        std::sort(task_indices, task_indices + kMaxVlcBands, [&](int a, int b) {
            return score(a) > score(b);
        });
    }

    slot->t_prep_end = Clock::now();
    slot->timings.task_prep_ms = std::chrono::duration<float, std::milli>(slot->t_prep_end - slot->t_prep_start).count();

    // 1. Launch Compressed Sample H2D Transfer & LL Decode Asynchronously (Section 8)
    std::memcpy(slot->host_pinned_compressed, compressed_sample, compressed_size);
    slot->current_compressed_size = compressed_size;

    cudaEventRecord(slot->ev_frame_start, slot->copy_stream);
    cudaEventRecord(slot->ev_h2d_start, slot->copy_stream);
    if (production_guard) {
        *slot->host_frame_error = 0;
        cudaMemsetAsync(slot->d_frame_error, 0, sizeof(uint32_t), slot->copy_stream);
    }

    cudaMemcpyAsync(
        slot->d_compressed_sample,
        slot->host_pinned_compressed,
        compressed_size,
        cudaMemcpyHostToDevice,
        slot->copy_stream
    );

    cudaMemcpyAsync(
        slot->d_vlc_descriptors,
        slot->vlc_descs.data(),
        slot->vlc_descs.size() * sizeof(GpuEntropyBandDescriptor),
        cudaMemcpyHostToDevice,
        slot->copy_stream
    );

    cudaMemcpyAsync(
        slot->d_ll_descriptors,
        slot->ll_descs.data(),
        slot->ll_descs.size() * sizeof(GpuEntropyBandDescriptor),
        cudaMemcpyHostToDevice,
        slot->copy_stream
    );

    cudaEventRecord(slot->ev_h2d_stop, slot->copy_stream);
    cudaEventRecord(slot->ev_compressed_h2d_ready, slot->copy_stream);

    // Launch LL decode on copy_stream immediately
    LaunchDecodeLowpassGpu(
        slot->d_compressed_sample,
        compressed_size,
        slot->d_ll_descriptors,
        static_cast<int>(slot->ll_descs.size()),
        slot->copy_stream
    );
    cudaEventRecord(slot->ev_ll_ready, slot->copy_stream);

    if (slot->GpuIndex()) {
        *slot->host_frame_error = 0;
        if (cudaMemsetAsync(slot->d_frame_error, 0, sizeof(uint32_t), slot->copy_stream) != cudaSuccess) {
            impl_->AbandonSlot(slot); return 0;
        }
        // Re-record compressed dependency after frame error reset.
        cudaEventRecord(slot->ev_compressed_h2d_ready, slot->copy_stream);
        slot->t_batch_submit = Clock::now();
        const double schedule_cpu_start = ThreadCpuMs();
        slot->timings.active_workers = 0;
        slot->timings.active_streams = slot->num_replay_streams;
        for (int i = 0; i < kMaxVlcBands; ++i) {
            int b = task_indices[i];
            if (slot->delay_hook) {
                const auto& d = slot->vlc_descs[b];
                slot->delay_hook(b, d.channel, d.level, d.band);
            }
            if (!slot->DispatchGpuIndex(b)) { impl_->AbandonSlot(slot); return 0; }
        }
        slot->t_batch_return = Clock::now();
        slot->host_dispatch_cpu_ms = ThreadCpuMs() - schedule_cpu_start;
        if (caller_stream) cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
        return slot->ticket;
    }

    // Phase 2F-C Per-Band Selection & Async Summary Generation (Sections 7, 8, 15, 18, 21)
    if (slot->pipeline_mode == Phase2EPipelineMode::Phase2FCHybridFullFrame) {
        size_t total_fc_chunks = 0;
        const size_t kMaxFcChunks = 131072;

        for (int i = 0; i < kMaxVlcBands; ++i) {
            const auto& d = slot->vlc_descs[i];
            BandEntropyMethod method = BandEntropyMethod::MethodE_HostIndex;

            if (slot->fc_selector_policy == Phase2FCSelectorPolicy::ForceHybridBF) {
                method = BandEntropyMethod::MethodBF_Hybrid;
            } else if (slot->fc_selector_policy == Phase2FCSelectorPolicy::ForcePhase2E) {
                method = BandEntropyMethod::MethodE_HostIndex;
            } else if (slot->fc_selector_policy == Phase2FCSelectorPolicy::OraclePerBand) {
                if (i < static_cast<int>(slot->fc_oracle_decisions.size())) {
                    method = slot->fc_oracle_decisions[i];
                } else {
                    method = BandEntropyMethod::MethodE_HostIndex;
                }
            } else if (slot->fc_selector_policy == Phase2FCSelectorPolicy::AdaptiveDynamic) {
                if (d.payload_bytes >= 16384) {
                    method = BandEntropyMethod::MethodBF_Hybrid;
                } else {
                    method = BandEntropyMethod::MethodE_HostIndex;
                }
            } else {
                // StaticHeuristic:
                // Rule: dense noise has bytes_per_coeff >= 1.0 (on 4K noise ~2.42)
                // Texture has bytes_per_coeff ~0.81 (dominated by CPU correction, Method E is faster)
                float bytes_per_coeff = (d.coefficient_count > 0) ? (float(d.payload_bytes) / float(d.coefficient_count)) : 0.0f;
                if (d.payload_bytes >= 32768 && bytes_per_coeff >= 1.05f) {
                    method = BandEntropyMethod::MethodBF_Hybrid;
                } else {
                    method = BandEntropyMethod::MethodE_HostIndex;
                }
            }

            if (method == BandEntropyMethod::MethodBF_Hybrid) {
                if (d.payload_bytes == 0 || d.codebook_id < 0 || d.codebook_id > 2) {
                    method = BandEntropyMethod::MethodE_HostIndex;
                } else {
                    size_t chunks = (d.payload_bytes + slot->fc_chunk_size - 1) / slot->fc_chunk_size;
                    if (total_fc_chunks + chunks > kMaxFcChunks) {
                        method = BandEntropyMethod::MethodE_HostIndex;
                    } else {
                        slot->band_chunk_offset[i] = total_fc_chunks;
                        slot->band_chunk_count[i] = chunks;
                        total_fc_chunks += chunks;
                    }
                }
            }
            slot->band_method[i] = method;
        }

        // Reset errors and launch summaries on copy_stream
        cudaMemsetAsync(slot->d_fc_errors, 0, kMaxVlcBands * sizeof(uint32_t), slot->copy_stream);
        for (int i = 0; i < kMaxVlcBands; ++i) {
            if (slot->band_method[i] == BandEntropyMethod::MethodBF_Hybrid) {
                const auto& d = slot->vlc_descs[i];
                size_t c_offset = slot->band_chunk_offset[i];
                size_t c_count = slot->band_chunk_count[i];
                size_t codebook_offset = (d.codebook_id == 1) ? CS17_BASE_OFFSET :
                                        ((d.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);
                const GpuFsmEntry* d_table = impl_->fsm_table_mgr.GetDeviceTable() + codebook_offset;
                unsigned states = (d.codebook_id == 0) ? 136 : 518;

                cudaEventRecord(slot->ev_summary_start[i], slot->copy_stream);
                LaunchHybridSummariesGpu(
                    slot->d_compressed_sample + d.payload_offset,
                    d.payload_bytes,
                    d_table,
                    states,
                    slot->fc_chunk_size,
                    static_cast<uint32_t>(c_count),
                    slot->d_fc_summaries + c_offset,
                    slot->copy_stream
                );
                cudaMemcpyAsync(
                    slot->host_pinned_fc_summaries + c_offset,
                    slot->d_fc_summaries + c_offset,
                    c_count * sizeof(HyperCine::HybridEntropy::Summary),
                    cudaMemcpyDeviceToHost,
                    slot->copy_stream
                );
                cudaEventRecord(slot->ev_summary_done[i], slot->copy_stream);
            }
        }
    }

    // 2. Parallel Host Checkpoint Indexing & Streaming GPU Replay (Section 6, 9, 11, 14, 24, 25)
    slot->timings.active_workers = impl_->worker_pool.WorkerCount();
    slot->timings.active_streams = slot->num_replay_streams;
    std::atomic<bool> abort_flag{false};
    std::mutex error_mutex;
    std::string first_error;

    auto task_fn = [&](int task_idx, int worker_id) {
        if (production_guard) cudaSetDevice(slot->cuda_device);
        const int band_idx = task_indices[task_idx];
        const auto t0 = Clock::now();
        const double cpu0 = ThreadCpuMs();
        const int active = slot->active_index_workers.fetch_add(1) + 1;
        int peak = slot->peak_index_workers.load();
        while (active > peak && !slot->peak_index_workers.compare_exchange_weak(peak, active)) {}

        std::string err;
        bool ok = false;

        if (slot->pipeline_mode == Phase2EPipelineMode::Phase2FCHybridFullFrame &&
            slot->band_method[band_idx] == BandEntropyMethod::MethodBF_Hybrid)
        {
            // Method B/F Hybrid Path
            cudaEventSynchronize(slot->ev_summary_done[band_idx]);

            const auto& d = slot->vlc_descs[band_idx];
            const uint8_t* p = compressed_sample + d.payload_offset;
            uint32_t bytes = d.payload_bytes;
            size_t codebook_offset = (d.codebook_id == 1) ? CS17_BASE_OFFSET :
                                    ((d.codebook_id == 2) ? CS18_BASE_OFFSET : CS9_BASE_OFFSET);
            const GpuFsmEntry* host_table = impl_->host_fsm_tables.data() + codebook_offset;
            unsigned states = (d.codebook_id == 0) ? 136 : 518;
            uint16_t initial_state = d.initial_fsm_state;
            uint32_t cap = static_cast<uint32_t>(d.coefficient_count);
            size_t c_offset = slot->band_chunk_offset[band_idx];
            size_t c_count = slot->band_chunk_count[band_idx];
            auto* summaries = slot->host_pinned_fc_summaries + c_offset;

            auto& resolved = slot->reusable_resolved[band_idx];
            bool fallback = false;
            try {
                float threshold = (slot->fc_selector_policy == Phase2FCSelectorPolicy::AdaptiveDynamic) ? 0.05f : 1.0f;
                resolved = HyperCine::HybridEntropy::Resolve(
                    p, bytes, host_table, initial_state, cap, slot->fc_chunk_size, summaries, c_count, threshold,
                    1.0f, nullptr, &resolved
                );
                if (resolved.fallback_triggered) {
                    fallback = true;
                }
            } catch (const std::exception& e) {
                fallback = true;
            }

            if (fallback) {
                // Adaptive Fallback to Method E
                auto& rec = slot->fc_band_records[band_idx];
                rec.band_index = band_idx;
                rec.channel = d.channel;
                rec.level = d.level;
                rec.orientation = d.band;
                rec.payload_bytes = d.payload_bytes;
                rec.coefficient_count = d.coefficient_count;
                rec.chosen_method = BandEntropyMethod::MethodE_HostIndex;
                rec.fell_back = true;

                slot->band_method[band_idx] = BandEntropyMethod::MethodE_HostIndex;
                try {
                    ok = (production_guard ? GenerateSingleBandCheckpointsValidated : GenerateSingleBandCheckpointsCpu)(
                        compressed_sample, compressed_size, slot->vlc_descs[band_idx], band_idx,
                        impl_->host_fsm_tables.data(), impl_->host_fsm_tables.size(),
                        slot->entropy_mode, slot->fixed_segments_per_band,
                        slot->band_segments[band_idx], &err
                    );
                } catch (const std::exception& e) {
                    err = std::string("band ") + std::to_string(band_idx) + " fallback: " + e.what();
                }
                if (ok) {
                    if (slot->delay_hook) slot->delay_hook(band_idx, d.channel, d.level, d.band);
                    slot->DispatchBandGpu(band_idx, worker_id);
                }
            } else {
                // Hybrid B/F Succeeded
                ok = true;
                if (!resolved.checkpoints.empty()) {
                    std::memcpy(
                        slot->host_pinned_fc_checkpoints + c_offset,
                        resolved.checkpoints.data(),
                        resolved.checkpoints.size() * sizeof(HyperCine::HybridEntropy::Restart)
                    );
                }

                auto& rec = slot->fc_band_records[band_idx];
                rec.band_index = band_idx;
                rec.channel = d.channel;
                rec.level = d.level;
                rec.orientation = d.band;
                rec.payload_bytes = d.payload_bytes;
                rec.coefficient_count = d.coefficient_count;
                rec.chosen_method = BandEntropyMethod::MethodBF_Hybrid;
                rec.chunk_size = slot->fc_chunk_size;
                rec.chunk_count = static_cast<uint32_t>(c_count);
                rec.checkpoint_count = static_cast<uint32_t>(resolved.checkpoints.size());
                rec.corrected_nibbles = resolved.corrections;
                rec.correction_ratio = (c_count > 0 && slot->fc_chunk_size > 0) ?
                    float(resolved.corrections) / float(c_count * slot->fc_chunk_size * 2) : 0.0f;
                rec.full_chunk_corrections = resolved.full_chunks;
                rec.max_correction_nibbles = resolved.longest;
                rec.cpu_resolve_ms = resolved.cpu_resolve_ms;
                rec.fell_back = false;

                if (slot->delay_hook) slot->delay_hook(band_idx, d.channel, d.level, d.band);
                slot->DispatchBandHybrid(band_idx, resolved.marker, static_cast<uint32_t>(resolved.checkpoints.size()));
            }
        } else {
            // Method E Host Index Path
            const auto& d = slot->vlc_descs[band_idx];
            auto& rec = slot->fc_band_records[band_idx];
            rec.band_index = band_idx;
            rec.channel = d.channel;
            rec.level = d.level;
            rec.orientation = d.band;
            rec.payload_bytes = d.payload_bytes;
            rec.coefficient_count = d.coefficient_count;
            rec.chosen_method = BandEntropyMethod::MethodE_HostIndex;
            rec.fell_back = false;

            try {
                ok = (production_guard ? GenerateSingleBandCheckpointsValidated : GenerateSingleBandCheckpointsCpu)(
                    compressed_sample, compressed_size, slot->vlc_descs[band_idx], band_idx,
                    impl_->host_fsm_tables.data(), impl_->host_fsm_tables.size(),
                    slot->entropy_mode, slot->fixed_segments_per_band,
                    slot->band_segments[band_idx], &err
                );
            } catch (const std::exception& e) {
                err = std::string("band ") + std::to_string(band_idx) + ": " + e.what();
            }

            if (ok) {
                if (slot->delay_hook) slot->delay_hook(band_idx, d.channel, d.level, d.band);
                if (slot->pipeline_mode != Phase2EPipelineMode::Phase2DFrameBarrier) {
                    slot->DispatchBandGpu(band_idx, worker_id);
                }
            }
        }

        const auto t1 = Clock::now();
        if (production_guard && cudaGetLastError() != cudaSuccess) {
            ok = false;
            err = "CUDA worker dispatch failure";
        }
        slot->active_index_workers.fetch_sub(1);
        const double cpu1 = ThreadCpuMs();
        TaskStat& ts = slot->task_stats[band_idx];
        ts.start = t0;
        ts.end = t1;
        ts.duration_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        ts.worker_id = worker_id;
        ts.thread_cpu_ms = float(cpu1 - cpu0);

        if (!ok) {
            bool expected = false;
            if (abort_flag.compare_exchange_strong(expected, true)) {
                std::lock_guard<std::mutex> lk(error_mutex);
                first_error = err;
                slot->has_error.store(true);
            }
            return;
        }
    };

    slot->t_batch_submit = Clock::now();
    bool pool_threw = false;
    try {
        impl_->worker_pool.ExecuteBatch(kMaxVlcBands,
            [&task_fn](int i, int worker) { task_fn(i, worker); }, &abort_flag);
    } catch (const std::exception& e) {
        pool_threw = true;
        first_error = std::string("worker exception: ") + e.what();
    } catch (...) {
        pool_threw = true;
        first_error = "worker exception: unknown";
    }
    slot->t_batch_return = Clock::now();

    if (pool_threw || abort_flag.load(std::memory_order_relaxed) || slot->has_error.load(std::memory_order_relaxed)) {
        if (!production_guard) std::cerr << "Phase 2E Indexing Failure: " << first_error << std::endl;
        impl_->AbandonSlot(slot);
        return 0;
    }

    // 3. Complete Staging / Barriers Depending on Pipeline Mode
    if (slot->pipeline_mode == Phase2EPipelineMode::Phase2DFrameBarrier) {
        // Phase 2D Baseline Mode: Canonical Gather -> Full H2D -> Replay All Bands -> Diff -> Inverse -> Color
        slot->host_segments.clear();
        for (int b = 0; b < kMaxVlcBands; ++b) {
            slot->host_segments.insert(
                slot->host_segments.end(),
                slot->band_segments[b].begin(),
                slot->band_segments[b].end()
            );
        }

        int num_segments = static_cast<int>(slot->host_segments.size());
        if (num_segments > 0) {
            cudaMemcpyAsync(
                slot->d_segments,
                slot->host_segments.data(),
                num_segments * sizeof(GpuEntropySegmentDescriptor),
                cudaMemcpyHostToDevice,
                slot->main_stream
            );
        }

        cudaStreamWaitEvent(slot->main_stream, slot->ev_compressed_h2d_ready, 0);

        LaunchZeroVlcBandsGpu(slot->d_vlc_descriptors, kMaxVlcBands, slot->main_stream);
        if (num_segments > 0) {
            LaunchDecodeEntropyParallelGpu(
                slot->d_compressed_sample,
                compressed_size,
                slot->d_vlc_descriptors,
                kMaxVlcBands,
                slot->d_segments,
                num_segments,
                impl_->fsm_table_mgr.GetDeviceTable(),
                slot->d_seg_results,
                slot->main_stream, nullptr, slot->ErrorGuard()
            );
        }
        if (std::any_of(slot->vlc_descs.begin(),slot->vlc_descs.end(),[](const auto& d){return d.has_peaks && d.peak_level > 0;}))
            LaunchRestoreDecodePeaks(slot->d_compressed_sample,compressed_size,slot->d_vlc_descriptors,kMaxVlcBands,slot->main_stream,slot->ErrorGuard());
        LaunchCooperativeDifferenceCodingGpu(slot->d_vlc_descriptors, kMaxVlcBands, slot->main_stream, slot->ErrorGuard());

        for (int b = 0; b < kMaxVlcBands; ++b) {
            cudaEventRecord(slot->ev_band_done[b], slot->main_stream);
        }

        for (int ch = 0; ch < 3; ++ch) {
            cudaStream_t s = slot->channel_streams[ch];
            cudaStreamWaitEvent(s, slot->ev_ll_ready, 0);
            cudaStreamWaitEvent(s, slot->ev_band_done[0], 0);

            cudaEventRecord(slot->ev_inv_l2_start[ch], s);
            slot->LaunchLevel2Inverse(ch, s);
            cudaEventRecord(slot->ev_inv_l2_stop[ch], s);

            cudaEventRecord(slot->ev_inv_l1_start[ch], s);
            slot->LaunchLevel1Inverse(ch, s);
            cudaEventRecord(slot->ev_inv_l1_stop[ch], s);

            cudaEventRecord(slot->ev_inv_l0_start[ch], s);
            slot->LaunchLevel0Inverse(ch, s);
            cudaEventRecord(slot->ev_inv_l0_stop[ch], s);

            cudaEventRecord(slot->ev_ch_done[ch], s);
        }

        for (int ch = 0; ch < 3; ++ch) {
            cudaStreamWaitEvent(slot->main_stream, slot->ev_ch_done[ch], 0);
        }

        if (slot->surface_type == DeviceSurfaceType::BGRA8) {
            cudaEventRecord(slot->ev_color_start, slot->main_stream);
            LaunchYuv422ToBgra8(
                slot->channels[0].d_final_plane, slot->channels[0].final_plane_pitch,
                slot->channels[1].d_final_plane, slot->channels[1].final_plane_pitch,
                slot->channels[2].d_final_plane, slot->channels[2].final_plane_pitch,
                slot->d_bgra, slot->bgra_pitch,
                frame_width, frame_height,
                matrix, range, orientation,
                slot->main_stream, slot->ErrorGuard(), slot->interlaced_gop1
            );
            cudaEventRecord(slot->ev_color_stop, slot->main_stream);
        }

        cudaEventRecord(slot->ev_ready, slot->main_stream);
    } else if (slot->pipeline_mode == Phase2EPipelineMode::Phase2EEntropyOverlap) {
        // Variant A: All bands were streamed to GPU as soon as indexed.
        // Retain inverse barrier: wait for all 27 bands before starting inverse transforms.
        for (int ch = 0; ch < 3; ++ch) {
            cudaStream_t s = slot->channel_streams[ch];
            cudaStreamWaitEvent(s, slot->ev_ll_ready, 0);
            for (int b = 0; b < kMaxVlcBands; ++b) {
                if (slot->vlc_descs[b].channel == ch) {
                    cudaStreamWaitEvent(s, slot->ev_band_done[b], 0);
                }
            }

            cudaEventRecord(slot->ev_inv_l2_start[ch], s);
            slot->LaunchLevel2Inverse(ch, s);
            cudaEventRecord(slot->ev_inv_l2_stop[ch], s);

            cudaEventRecord(slot->ev_inv_l1_start[ch], s);
            slot->LaunchLevel1Inverse(ch, s);
            cudaEventRecord(slot->ev_inv_l1_stop[ch], s);

            cudaEventRecord(slot->ev_inv_l0_start[ch], s);
            slot->LaunchLevel0Inverse(ch, s);
            cudaEventRecord(slot->ev_inv_l0_stop[ch], s);

            cudaEventRecord(slot->ev_ch_done[ch], s);
        }

        for (int ch = 0; ch < 3; ++ch) {
            cudaStreamWaitEvent(slot->main_stream, slot->ev_ch_done[ch], 0);
        }

        if (slot->surface_type == DeviceSurfaceType::BGRA8) {
            cudaEventRecord(slot->ev_color_start, slot->main_stream);
            LaunchYuv422ToBgra8(
                slot->channels[0].d_final_plane, slot->channels[0].final_plane_pitch,
                slot->channels[1].d_final_plane, slot->channels[1].final_plane_pitch,
                slot->channels[2].d_final_plane, slot->channels[2].final_plane_pitch,
                slot->d_bgra, slot->bgra_pitch,
                frame_width, frame_height,
                matrix, range, orientation,
                slot->main_stream, nullptr, slot->interlaced_gop1
            );
            cudaEventRecord(slot->ev_color_stop, slot->main_stream);
        }

        cudaEventRecord(slot->ev_ready, slot->main_stream);
    } else {
        // Variant B (Phase2EFullOverlap):
        // Inverse DAG advanced automatically as dependencies became complete.
        // Ensure color conversion and ev_ready are recorded.
        std::lock_guard<std::mutex> lk(slot->dispatch_mutex);
        for (int ch = 0; ch < 3; ++ch) {
            slot->CheckAdvanceInverseDag(ch);
        }
    }

    if (production_guard) {
        cudaMemcpyAsync(slot->host_frame_error, slot->d_frame_error, sizeof(uint32_t),
                        cudaMemcpyDeviceToHost, slot->main_stream);
        if (slot->pipeline_mode == Phase2EPipelineMode::Phase2FCHybridFullFrame)
            cudaMemcpyAsync(slot->host_pinned_fc_errors, slot->d_fc_errors,
                kMaxVlcBands * sizeof(uint32_t), cudaMemcpyDeviceToHost, slot->main_stream);
        cudaEventRecord(slot->ev_ready, slot->main_stream);
    }
    if (caller_stream) {
        cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    }

    return slot->ticket;
}

uint64_t CudaDecoderPipelinePhase2E::QueueDecodeFieldPlusParsed(const uint8_t* compressed_sample,
    size_t compressed_size, const FieldPlusSampleDescriptor& parsed, int frame_index,
    DeviceSurfaceType requested_surface, ColorMatrix matrix, ColorRange range,
    RowOrientation orientation, bool production_guard) {
    if (!impl_ || !impl_->initialized || !compressed_sample || compressed_size == 0 ||
        parsed.frame_count != 2 || parsed.width <= 0 || parsed.height <= 0 ||
        (frame_index != 0 && frame_index != 1)) return 0;

    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->AcquireSlot();
    }
    if (!slot) return 0;
    if (compressed_size > slot->compressed_capacity || parsed.width > slot->max_width ||
        parsed.height > slot->max_height || !slot->EnsureFieldPlusBuffers()) {
        impl_->AbandonSlot(slot);
        return 0;
    }

    const auto parse_end = Clock::now();
    slot->fieldplus_mode = true;
    slot->frame_index = frame_index;
    slot->production_guard = production_guard;
    slot->interlaced_gop1 = false;
    slot->width = parsed.width;
    slot->height = parsed.height;
    slot->precision = parsed.coded_precision;
    slot->surface_type = requested_surface;
    slot->matrix = matrix;
    slot->range = range;
    slot->orientation = orientation;
    slot->timings = Phase2eSlotStageTimings{};
    slot->t_frame_start = parse_end;
    slot->t_parse_end = parse_end;
    slot->current_compressed_size = compressed_size;

    auto& fp = slot->fieldplus;
    fp.ll_descriptors.clear();
    fp.vlc_descriptors.clear();
    fp.segments.clear();
    fp.ll_descriptors.reserve(3);
    fp.vlc_descriptors.reserve(kFieldPlusVlcBands);
    fp.segments.reserve(kFieldPlusSegmentCapacity);

    for (int ch = 0; ch < kFieldPlusChannelCount; ++ch) {
        const int channel_width = ch == 0 ? parsed.width : parsed.width / 2;
        auto& c = slot->channels[ch];
        c.l2_pitch = (size_t(channel_width / 8) * sizeof(int16_t) + 15) & ~size_t(15);
        c.l1_pitch = (size_t(channel_width / 4) * sizeof(int16_t) + 15) & ~size_t(15);
        c.l0_pitch = (size_t(channel_width / 2) * sizeof(int16_t) + 15) & ~size_t(15);
        c.ll1_pitch = c.l1_pitch;
        c.ll0_pitch = c.l0_pitch;
        fp.w3_pitch[ch] = c.l1_pitch;
    }

    auto bind_destination = [&](GpuEntropyBandDescriptor& d, int logical) {
        auto& c = slot->channels[d.channel];
        if (logical < 4) {
            d.d_destination = c.d_l2_band[logical];
            d.pitch_bytes = c.l2_pitch;
        } else if (logical < 7) {
            const int band = logical - 3;
            d.d_destination = c.d_l1_band[band];
            d.pitch_bytes = c.l1_pitch;
        } else if (logical < 11) {
            const int band = logical - 7;
            d.d_destination = fp.d_w3_band[d.channel][band];
            d.pitch_bytes = fp.w3_pitch[d.channel];
        } else {
            const int band = logical <= 13 ? logical - 10 : logical - 13;
            d.d_destination = c.d_l0_band[band];
            d.pitch_bytes = c.l0_pitch;
        }
        d.pitch_samples = d.pitch_bytes / sizeof(int16_t);
        d.coefficient_count = size_t(d.height) * d.pitch_samples;
    };

    for (int ch = 0; ch < kFieldPlusChannelCount; ++ch) {
        for (int logical = 0; logical < kFieldPlusLogicalBandsPerChannel; ++logical) {
            auto d = parsed.bands[size_t(ch * kFieldPlusLogicalBandsPerChannel + logical)].entropy;
            if (logical == 0) {
                // The upstream FIELDPLUS base inverse applies format-specific
                // lowpass biases beyond the values used by the GOP-1 parser:
                // +24 for BGRA and +10 for YU64, measured against the frozen
                // progressive fixture's direct coefficient references.
                d.channel_offset += requested_surface == DeviceSurfaceType::BGRA8 ? 24 : 10;
            }
            bind_destination(d, logical);
            fp.bands[size_t(ch * kFieldPlusLogicalBandsPerChannel + logical)] = d;
            if (logical == 0) {
                fp.ll_descriptors.push_back(d);
            } else if (logical == 7) {
                // Raw16 temporal-high LL is copied by its dedicated bounded kernel.
            } else if (logical < 11 || (frame_index == 1 && logical >= 11 && logical <= 13) ||
                       (frame_index == 0 && logical >= 14 && logical <= 16)) {
                fp.vlc_descriptors.push_back(d);
            }
        }
    }
    if (fp.ll_descriptors.size() != 3 || fp.vlc_descriptors.size() != kFieldPlusVlcBands) {
        impl_->AbandonSlot(slot);
        return 0;
    }

    std::string checkpoint_error;
    for (size_t band = 0; band < fp.vlc_descriptors.size(); ++band) {
        // Reuse the slot's pre-reserved per-band checkpoint scratch. A local
        // vector here allocated on every FIELDPLUS request after warm-up.
        auto& band_segments = slot->band_segments[band];
        band_segments.clear();
        const auto& d = fp.vlc_descriptors[band];
        const bool indexed = (production_guard ? GenerateSingleBandCheckpointsValidated : GenerateSingleBandCheckpointsCpu)(
            compressed_sample, compressed_size, d, static_cast<int>(band),
            impl_->host_fsm_tables.data(), impl_->host_fsm_tables.size(),
            slot->entropy_mode, slot->fixed_segments_per_band, band_segments, &checkpoint_error);
        if (!indexed || band_segments.empty() || band_segments.size() > kFieldPlusMaxSegmentsPerBand ||
            fp.segments.size() + band_segments.size() > kFieldPlusSegmentCapacity) {
            impl_->AbandonSlot(slot);
            return 0;
        }
        fp.segments.insert(fp.segments.end(), band_segments.begin(), band_segments.end());
    }

    std::memcpy(slot->host_pinned_compressed, compressed_sample, compressed_size);
    std::memcpy(fp.host_pinned_ll_descriptors, fp.ll_descriptors.data(),
        fp.ll_descriptors.size() * sizeof(GpuEntropyBandDescriptor));
    std::memcpy(fp.host_pinned_vlc_descriptors, fp.vlc_descriptors.data(),
        fp.vlc_descriptors.size() * sizeof(GpuEntropyBandDescriptor));
    if (!fp.segments.empty())
        std::memcpy(fp.host_pinned_segments, fp.segments.data(),
            fp.segments.size() * sizeof(GpuEntropySegmentDescriptor));

    slot->timings.active_workers = 0;
    slot->timings.active_streams = 1;
    slot->t_batch_submit = slot->t_batch_return = Clock::now();
    *slot->host_frame_error = 0;
    auto fail_gpu = [&]() {
        impl_->AbandonSlot(slot);
        return uint64_t(0);
    };
    if (cudaMemsetAsync(slot->d_frame_error, 0, sizeof(uint32_t), slot->main_stream) != cudaSuccess)
        return fail_gpu();
    cudaEventRecord(slot->ev_frame_start, slot->main_stream);
    cudaEventRecord(slot->ev_h2d_start, slot->main_stream);
    if (cudaMemcpyAsync(slot->d_compressed_sample, slot->host_pinned_compressed, compressed_size,
            cudaMemcpyHostToDevice, slot->main_stream) != cudaSuccess ||
        cudaMemcpyAsync(fp.d_ll_descriptors, fp.host_pinned_ll_descriptors,
            fp.ll_descriptors.size() * sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice, slot->main_stream) != cudaSuccess ||
        cudaMemcpyAsync(fp.d_vlc_descriptors, fp.host_pinned_vlc_descriptors,
            fp.vlc_descriptors.size() * sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice, slot->main_stream) != cudaSuccess ||
        (!fp.segments.empty() && cudaMemcpyAsync(fp.d_segments, fp.host_pinned_segments,
            fp.segments.size() * sizeof(GpuEntropySegmentDescriptor), cudaMemcpyHostToDevice, slot->main_stream) != cudaSuccess))
        return fail_gpu();
    cudaEventRecord(slot->ev_h2d_stop, slot->main_stream);
    cudaEventRecord(slot->ev_compressed_h2d_ready, slot->main_stream);

    if (LaunchDecodeLowpassGpu(slot->d_compressed_sample, compressed_size, fp.d_ll_descriptors,
            static_cast<int>(fp.ll_descriptors.size()), slot->main_stream) != cudaSuccess)
        return fail_gpu();
    for (int ch = 0; ch < kFieldPlusChannelCount; ++ch) {
        const auto& raw = parsed.bands[size_t(ch * kFieldPlusLogicalBandsPerChannel + 7)].entropy;
        const int raw_pitch = int(fp.w3_pitch[ch] / sizeof(int16_t));
        const dim3 block(32, 8);
        const dim3 grid((raw.width + int(block.x) - 1) / int(block.x),
                        (raw.height + int(block.y) - 1) / int(block.y));
        DecodeFieldPlusRaw16Kernel<<<grid, block, 0, slot->main_stream>>>(
            slot->d_compressed_sample, compressed_size, raw.payload_offset,
            fp.d_w3_band[ch][0], raw_pitch, raw.width, raw.height, slot->ErrorGuard());
        if (cudaGetLastError() != cudaSuccess) return fail_gpu();
    }
    cudaEventRecord(slot->ev_ll_ready, slot->main_stream);

    if (LaunchZeroVlcBandsGpu(fp.d_vlc_descriptors, kFieldPlusVlcBands, slot->main_stream) != cudaSuccess ||
        LaunchDecodeEntropyParallelGpu(slot->d_compressed_sample, compressed_size,
            fp.d_vlc_descriptors, kFieldPlusVlcBands, fp.d_segments, static_cast<int>(fp.segments.size()),
            impl_->fsm_table_mgr.GetDeviceTable(), fp.d_segment_results, slot->main_stream,
            nullptr, slot->ErrorGuard()) != cudaSuccess)
        return fail_gpu();
    if (std::any_of(fp.vlc_descriptors.begin(), fp.vlc_descriptors.end(),
            [](const auto& d) { return d.has_peaks && d.peak_level > 0; }) &&
        LaunchRestoreDecodePeaks(slot->d_compressed_sample, compressed_size, fp.d_vlc_descriptors,
            kFieldPlusVlcBands, slot->main_stream, slot->ErrorGuard()) != cudaSuccess)
        return fail_gpu();
    if (LaunchCooperativeDifferenceCodingGpu(fp.d_vlc_descriptors, kFieldPlusVlcBands,
            slot->main_stream, slot->ErrorGuard()) != cudaSuccess)
        return fail_gpu();

    for (int ch = 0; ch < kFieldPlusChannelCount; ++ch) {
        auto& c = slot->channels[ch];
        const int channel_width = ch == 0 ? parsed.width : parsed.width / 2;
        const int width_l1 = channel_width / 4;
        const int height_l1 = parsed.height / 4;
        const dim3 block_v(16, 16);
        const dim3 grid_v((width_l1 + int(block_v.x) - 1) / int(block_v.x),
                          (height_l1 + int(block_v.y) - 1) / int(block_v.y));
        VerticalInverseKernel<<<grid_v, block_v, 0, slot->main_stream>>>(
            fp.d_w3_band[ch][0], int(fp.w3_pitch[ch]),
            fp.d_w3_band[ch][1], int(fp.w3_pitch[ch]),
            fp.d_w3_band[ch][2], int(fp.w3_pitch[ch]),
            fp.d_w3_band[ch][3], int(fp.w3_pitch[ch]),
            c.d_vert_low, int(c.vert_low_pitch), c.d_vert_high, int(c.vert_high_pitch),
            width_l1, height_l1, 3, slot->ErrorGuard());
        const dim3 block_h(64, 4);
        const dim3 grid_h((width_l1 + int(block_h.x) - 1) / int(block_h.x),
                          (2 * height_l1 + int(block_h.y) - 1) / int(block_h.y));
        HorizontalInverseKernel<<<grid_h, block_h, 0, slot->main_stream>>>(
            c.d_vert_low, int(c.vert_low_pitch), c.d_vert_high, int(c.vert_high_pitch),
            fp.d_gop_high[ch], int(fp.gop_high_pitch[ch]), width_l1, 2 * height_l1, 4, slot->ErrorGuard());

        slot->LaunchLevel2Inverse(ch, slot->main_stream, true); // w5 -> wavelet 4 lowpass
        slot->LaunchLevel1Inverse(ch, slot->main_stream, true); // w4 -> GOP temporal low
        if (cudaMemcpy2DAsync(fp.d_gop_low[ch], fp.gop_high_pitch[ch], c.d_ll0, c.ll0_pitch,
                size_t(channel_width / 2) * sizeof(int16_t), size_t(parsed.height / 2),
                cudaMemcpyDeviceToDevice, slot->main_stream) != cudaSuccess)
            return fail_gpu();

        const int temporal_width = channel_width / 2;
        const int temporal_height = parsed.height / 2;
        const dim3 temporal_block(128, 1);
        const dim3 temporal_grid((temporal_width + int(temporal_block.x) - 1) / int(temporal_block.x),
                                 (temporal_height + int(temporal_block.y) - 1) / int(temporal_block.y));
        FieldPlusTemporalInverseKernel<<<temporal_grid, temporal_block, 0, slot->main_stream>>>(
            c.d_ll0, int(c.ll0_pitch), fp.d_gop_high[ch], int(fp.gop_high_pitch[ch]),
            c.d_ll0, int(c.ll0_pitch), temporal_width, temporal_height, frame_index, 40, slot->ErrorGuard());
        slot->LaunchLevel0Inverse(ch, slot->main_stream, true); // selected w0/w1 base frame
        if (cudaGetLastError() != cudaSuccess) return fail_gpu();
    }

    if (requested_surface == DeviceSurfaceType::BGRA8) {
        cudaEventRecord(slot->ev_color_start, slot->main_stream);
        LaunchYuv422ToBgra8(slot->channels[0].d_final_plane, slot->channels[0].final_plane_pitch,
            slot->channels[1].d_final_plane, slot->channels[1].final_plane_pitch,
            slot->channels[2].d_final_plane, slot->channels[2].final_plane_pitch,
            slot->d_bgra, slot->bgra_pitch, parsed.width, parsed.height, matrix, range,
            orientation, slot->main_stream, slot->ErrorGuard(), false);
        cudaEventRecord(slot->ev_color_stop, slot->main_stream);
    }
    if (production_guard && cudaMemcpyAsync(slot->host_frame_error, slot->d_frame_error,
            sizeof(uint32_t), cudaMemcpyDeviceToHost, slot->main_stream) != cudaSuccess)
        return fail_gpu();
    cudaEventRecord(slot->ev_ready, slot->main_stream);
    return slot->ticket;
}

DecodeTicketStatus CudaDecoderPipelinePhase2E::PollDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return DecodeTicketStatus::Invalid;
    std::lock_guard<std::mutex> lock(impl_->mutex);

    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot) return DecodeTicketStatus::Invalid;

    if (slot->status == DecodeTicketStatus::Complete) return DecodeTicketStatus::Complete;
    if (slot->status == DecodeTicketStatus::Failed) return DecodeTicketStatus::Failed;

    cudaError_t err = cudaEventQuery(slot->ev_ready);
    if (err == cudaSuccess) {
        if ((slot->GpuIndex() || slot->production_guard) && *slot->host_frame_error) {
            slot->status = DecodeTicketStatus::Failed;
        } else if (slot->pipeline_mode == Phase2EPipelineMode::Phase2FCHybridFullFrame) {
            if (!slot->production_guard)
                cudaMemcpy(slot->host_pinned_fc_errors, slot->d_fc_errors, kMaxVlcBands * sizeof(uint32_t), cudaMemcpyDeviceToHost);
            bool fc_err = false;
            for (int b = 0; b < kMaxVlcBands; ++b) {
                if (slot->host_pinned_fc_errors[b] != 0) { fc_err = true; break; }
            }
            slot->status = fc_err ? DecodeTicketStatus::Failed : DecodeTicketStatus::Complete;
        } else {
            slot->status = DecodeTicketStatus::Complete;
        }
        return slot->status;
    } else if (err == cudaErrorNotReady) {
        return DecodeTicketStatus::Pending;
    } else {
        slot->status = DecodeTicketStatus::Failed;
        return DecodeTicketStatus::Failed;
    }
}

bool CudaDecoderPipelinePhase2E::WaitForDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
        if (!slot) return false;
        if (slot->status == DecodeTicketStatus::Complete) return true;
        if (slot->status == DecodeTicketStatus::Failed) return false;
    }

    auto t0 = Clock::now();
    cudaError_t err = cudaEventSynchronize(slot->ev_ready);
    auto t1 = Clock::now();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    slot->timings.ready_overhead_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    if (err == cudaSuccess) {
        if ((slot->GpuIndex() || slot->production_guard) && *slot->host_frame_error) {
            slot->status = DecodeTicketStatus::Failed;
        } else if (slot->pipeline_mode == Phase2EPipelineMode::Phase2FCHybridFullFrame) {
            if (!slot->production_guard)
                cudaMemcpy(slot->host_pinned_fc_errors, slot->d_fc_errors, kMaxVlcBands * sizeof(uint32_t), cudaMemcpyDeviceToHost);
            bool fc_err = false;
            for (int b = 0; b < kMaxVlcBands; ++b) {
                if (slot->host_pinned_fc_errors[b] != 0) { fc_err = true; break; }
            }
            slot->status = fc_err ? DecodeTicketStatus::Failed : DecodeTicketStatus::Complete;
        } else {
            slot->status = DecodeTicketStatus::Complete;
        }
        return slot->status == DecodeTicketStatus::Complete;
    }
    slot->status = DecodeTicketStatus::Failed;
    return false;
}

bool CudaDecoderPipelinePhase2E::StreamWait(uint64_t ticket, cudaStream_t caller_stream) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !caller_stream) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot || !slot->ev_ready) return false;

    cudaError_t err = cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2E::GetDeviceBgraOutput(uint64_t ticket, DeviceBgraSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot || slot->surface_type != DeviceSurfaceType::BGRA8 || slot->status != DecodeTicketStatus::Complete) {
        return false;
    }

    out_desc->d_bgra = slot->d_bgra;
    out_desc->width = slot->width;
    out_desc->height = slot->height;
    out_desc->padded_width = slot->max_width;
    out_desc->padded_height = slot->max_height;
    out_desc->pitch = slot->bgra_pitch;
    out_desc->orientation = slot->orientation;
    out_desc->matrix = slot->matrix;
    out_desc->range = slot->range;
    out_desc->cuda_device = slot->cuda_device;
    out_desc->ready_event = slot->ev_ready;
    out_desc->ticket = slot->ticket;
    out_desc->slot_index = slot->slot_index;

    return true;
}

bool CudaDecoderPipelinePhase2E::GetDevicePlanarOutput(uint64_t ticket, DevicePlanarSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot || slot->surface_type != DeviceSurfaceType::PlanarYUV422_16S || slot->status != DecodeTicketStatus::Complete) {
        return false;
    }

    out_desc->d_y = slot->channels[0].d_final_plane;
    out_desc->d_v = slot->channels[1].d_final_plane;
    out_desc->d_u = slot->channels[2].d_final_plane;
    out_desc->width = slot->width;
    out_desc->height = slot->height;
    out_desc->padded_width = slot->max_width;
    out_desc->padded_height = slot->max_height;
    out_desc->y_pitch = slot->channels[0].final_plane_pitch;
    out_desc->v_pitch = slot->channels[1].final_plane_pitch;
    out_desc->u_pitch = slot->channels[2].final_plane_pitch;
    out_desc->is_signed = true;
    out_desc->precision = slot->precision;
    out_desc->matrix = slot->matrix;
    out_desc->range = slot->range;
    out_desc->chroma_siting = ChromaSiting::CoSited;
    out_desc->cuda_device = slot->cuda_device;
    out_desc->ready_event = slot->ev_ready;
    out_desc->ticket = slot->ticket;
    out_desc->slot_index = slot->slot_index;

    return true;
}

bool CudaDecoderPipelinePhase2E::InspectReconstruction(uint64_t ticket, DecoderReconstruction* out) {
    if (!impl_ || !out) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* slot = impl_->FindSlot(ticket);
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;
    *out = {};
    out->slot_index = slot->slot_index;
    for (int ch = 0; ch < 3; ++ch) {
        const int native = ch == 1 ? 2 : ch == 2 ? 1 : 0;
        const auto& c = slot->channels[native];
        out->planes[ch] = {c.d_final_plane, c.final_plane_pitch, ch ? slot->width / 2 : slot->width, slot->height};
        out->allocation_bytes[ch] = c.final_plane_pitch * slot->max_height;
    }
    return true;
}

bool CudaDecoderPipelinePhase2E::ReleaseOutput(uint64_t ticket) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    if (slot->status == DecodeTicketStatus::Pending) cudaEventSynchronize(slot->ev_ready);
    slot->in_use = false;
    slot->ticket = 0;
    slot->status = DecodeTicketStatus::Invalid;
    return true;
}

bool CudaDecoderPipelinePhase2E::DownloadBgraToHost(uint64_t ticket, uint8_t* host_bgra, size_t host_pitch) {
    if (!impl_ || !host_bgra) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    cudaError_t err = cudaMemcpy2D(
        host_bgra, host_pitch,
        slot->d_bgra, slot->bgra_pitch,
        slot->width * 4, slot->height,
        cudaMemcpyDeviceToHost
    );
    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2E::DownloadPlanarToHost(
    uint64_t ticket,
    int16_t* host_y, size_t y_pitch,
    int16_t* host_v, size_t v_pitch,
    int16_t* host_u, size_t u_pitch
) {
    if (!impl_) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    cudaError_t err = cudaSuccess;
    if (host_y) {
        err = cudaMemcpy2D(
            host_y, y_pitch,
            slot->channels[0].d_final_plane, slot->channels[0].final_plane_pitch,
            slot->width * sizeof(int16_t), slot->height,
            cudaMemcpyDeviceToHost
        );
        if (err != cudaSuccess) return false;
    }

    if (host_v) {
        err = cudaMemcpy2D(
            host_v, v_pitch,
            slot->channels[1].d_final_plane, slot->channels[1].final_plane_pitch,
            (slot->width / 2) * sizeof(int16_t), slot->height,
            cudaMemcpyDeviceToHost
        );
        if (err != cudaSuccess) return false;
    }

    if (host_u) {
        err = cudaMemcpy2D(
            host_u, u_pitch,
            slot->channels[2].d_final_plane, slot->channels[2].final_plane_pitch,
            (slot->width / 2) * sizeof(int16_t), slot->height,
            cudaMemcpyDeviceToHost
        );
        if (err != cudaSuccess) return false;
    }
    return true;
}

bool CudaDecoderPipelinePhase2E::GetStageTimings(uint64_t ticket, Phase2eSlotStageTimings* timings) {
    if (!impl_ || !timings) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    *timings = Phase2eSlotStageTimings{};
    auto ms_rel = [&](Clock::time_point tp) {
        return std::chrono::duration<float, std::milli>(tp - slot->t_frame_start).count();
    };

    timings->host_parse_ms = slot->timings.host_parse_ms;
    timings->task_prep_ms = slot->timings.task_prep_ms;
    timings->t_frame_start_ms = 0.0f;
    timings->t_parse_end_ms = ms_rel(slot->t_parse_end);

    // H2D Timings
    float h2d_ms = 0.0f;
    cudaEventElapsedTime(&h2d_ms, slot->ev_h2d_start, slot->ev_h2d_stop);
    timings->compressed_h2d_ms = h2d_ms;

    float t_h2d_start = 0.0f, t_h2d_end = 0.0f;
    cudaEventElapsedTime(&t_h2d_start, slot->ev_frame_start, slot->ev_h2d_start);
    cudaEventElapsedTime(&t_h2d_end, slot->ev_frame_start, slot->ev_h2d_stop);
    timings->t_h2d_start_ms = t_h2d_start;
    timings->t_h2d_end_ms = t_h2d_end;

    // LL Decode
    float ll_ms = 0.0f;
    cudaEventElapsedTime(&ll_ms, slot->ev_h2d_stop, slot->ev_ll_ready);
    timings->gpu_ll_ms = ll_ms;

    // Host Indexing Statistics
    Clock::time_point first_idx_start = slot->t_batch_return;
    Clock::time_point last_idx_end = slot->t_batch_submit;
    float sum_index_work = 0.0f;
    float slowest_task = 0.0f;
    int slowest_worker = -1;

    for (int i = 0; i < kMaxVlcBands; ++i) {
        const TaskStat& ts = slot->task_stats[i];
        if (ts.worker_id < 0) continue;
        if (ts.start < first_idx_start) first_idx_start = ts.start;
        if (ts.end > last_idx_end) last_idx_end = ts.end;
        sum_index_work += ts.duration_ms;
        if (ts.duration_ms > slowest_task) {
            slowest_task = ts.duration_ms;
            slowest_worker = ts.worker_id;
        }
    }

    timings->host_schedule_cpu_ms = float(slot->host_dispatch_cpu_ms);
    timings->host_index_thread_cpu_ms = 0;
    for (const auto& ts : slot->task_stats) timings->host_index_thread_cpu_ms += ts.thread_cpu_ms;
    timings->peak_index_workers = slot->peak_index_workers.load();
    timings->host_index_cpu_work_ms = sum_index_work;
    timings->slowest_task_ms = slowest_task;
    timings->slowest_worker_id = slowest_worker;
    timings->active_workers = slot->timings.active_workers;
    timings->active_streams = slot->timings.active_streams;
    timings->largest_band_idx = slot->timings.largest_band_idx;
    timings->largest_band_bytes = slot->timings.largest_band_bytes;

    timings->t_first_index_start_ms = ms_rel(first_idx_start);
    timings->t_final_index_end_ms = ms_rel(last_idx_end);
    timings->host_index_wall_ms = std::chrono::duration<float, std::milli>(last_idx_end - first_idx_start).count();
    timings->queue_overhead_ms = first_idx_start > slot->t_batch_submit ?
        std::chrono::duration<float, std::milli>(first_idx_start - slot->t_batch_submit).count() : 0.0f;
    timings->barrier_wait_ms = slot->t_batch_return > last_idx_end ?
        std::chrono::duration<float, std::milli>(slot->t_batch_return - last_idx_end).count() : 0.0f;

    if (slot->GpuIndex()) {
        timings->host_index_cpu_work_ms = timings->host_index_wall_ms = 0;
        timings->t_first_index_start_ms = timings->t_final_index_end_ms = 0;
        timings->queue_overhead_ms = timings->barrier_wait_ms = 0;
        timings->host_schedule_wall_ms = std::chrono::duration<float, std::milli>(
            slot->t_batch_return - slot->t_batch_submit).count();
    }
    std::vector<GpuCheckpointIndexResult> gpu_index;
    if (slot->GpuIndex() && !GetGpuIndexResults(ticket, gpu_index)) return false;
    float earliest_index = 1e9f, latest_index = 0;
    // Per-Band Replay Timings
    timings->band_records.clear();
    timings->band_records.reserve(kMaxVlcBands);
    float earliest_gpu_start = 1e9f;
    float latest_gpu_end = 0.0f;
    float sum_replay_work = 0.0f;

    for (int b = 0; b < kMaxVlcBands; ++b) {
        BandTimelineRecord rec{};
        rec.band_index = b;
        rec.channel = slot->vlc_descs[b].channel;
        rec.level = slot->vlc_descs[b].level;
        rec.band = slot->vlc_descs[b].band;
        rec.payload_bytes = slot->vlc_descs[b].payload_bytes;
        rec.segment_count = static_cast<int>(slot->band_segments[b].size());

        // Find which task index processed this band
        for (int ti = 0; ti < kMaxVlcBands; ++ti) {
            if (ti == b && slot->task_stats[ti].worker_id >= 0)
            {
                rec.host_index_start_ms = ms_rel(slot->task_stats[ti].start);
                rec.host_index_end_ms = ms_rel(slot->task_stats[ti].end);
                rec.host_index_duration_ms = slot->task_stats[ti].duration_ms;
                rec.host_worker_id = slot->task_stats[ti].worker_id;
                break;
            }
        }

        rec.gpu_stream_id = slot->band_assigned_stream[b];
        if (slot->GpuIndex()) {
            cudaEventElapsedTime(&rec.gpu_index_start_ms, slot->ev_frame_start, slot->ev_index_start[b]);
            cudaEventElapsedTime(&rec.gpu_index_end_ms, slot->ev_frame_start, slot->ev_index_done[b]);
            cudaEventElapsedTime(&rec.gpu_index_duration_ms, slot->ev_index_start[b], slot->ev_index_done[b]);
            rec.gpu_index_work_ms = float(gpu_index[b].total_ns * 1e-6);
            rec.max_index_kernel_ms = float(gpu_index[b].max_launch_ns * 1e-6);
            rec.segment_count = gpu_index[b].segment_count;
            timings->gpu_index_work_ms += rec.gpu_index_work_ms;
            timings->max_index_kernel_ms = std::max(timings->max_index_kernel_ms, rec.max_index_kernel_ms);
            earliest_index = std::min(earliest_index, rec.gpu_index_start_ms);
            latest_index = std::max(latest_index, rec.gpu_index_end_ms);
        }

        float g_start = 0.0f, g_end = 0.0f, g_dur = 0.0f;
        cudaEventElapsedTime(&g_start, slot->ev_frame_start, slot->ev_band_start[b]);
        cudaEventElapsedTime(&g_end, slot->ev_frame_start, slot->ev_band_done[b]);
        cudaEventElapsedTime(&g_dur, slot->ev_band_start[b], slot->ev_band_done[b]);

        rec.gpu_replay_start_ms = g_start;
        rec.gpu_replay_end_ms = g_end;
        rec.gpu_replay_duration_ms = g_dur;

        if (g_start < earliest_gpu_start) earliest_gpu_start = g_start;
        if (g_end > latest_gpu_end) latest_gpu_end = g_end;
        sum_replay_work += g_dur;

        timings->band_records.push_back(rec);
    }

    if (slot->GpuIndex()) {
        timings->gpu_index_wall_ms = latest_index - earliest_index;
        timings->t_first_index_start_ms = earliest_index;
        timings->t_final_index_end_ms = latest_index;
    }
    timings->t_first_gpu_launch_ms = (earliest_gpu_start < 1e8f) ? earliest_gpu_start : 0.0f;
    timings->t_final_replay_end_ms = latest_gpu_end;
    timings->gpu_replay_work_ms = sum_replay_work;
    timings->gpu_replay_wall_ms = (latest_gpu_end > earliest_gpu_start) ? (latest_gpu_end - earliest_gpu_start) : 0.0f;

    // Per-Channel Inverse Wavelet Timings
    timings->inverse_records.clear();
    timings->inverse_records.reserve(9);
    float earliest_inv_start = 1e9f;
    float latest_inv_end = 0.0f;
    float sum_inv_work = 0.0f;

    for (int ch = 0; ch < 3; ++ch) {
        // Level 2
        float l2_s = 0.0f, l2_e = 0.0f, l2_d = 0.0f;
        cudaEventElapsedTime(&l2_s, slot->ev_frame_start, slot->ev_inv_l2_start[ch]);
        cudaEventElapsedTime(&l2_e, slot->ev_frame_start, slot->ev_inv_l2_stop[ch]);
        cudaEventElapsedTime(&l2_d, slot->ev_inv_l2_start[ch], slot->ev_inv_l2_stop[ch]);
        timings->inverse_records.push_back({ch, 2, l2_s, l2_e, l2_d});
        if (l2_s < earliest_inv_start) earliest_inv_start = l2_s;
        if (l2_e > latest_inv_end) latest_inv_end = l2_e;
        sum_inv_work += l2_d;

        // Level 1
        float l1_s = 0.0f, l1_e = 0.0f, l1_d = 0.0f;
        cudaEventElapsedTime(&l1_s, slot->ev_frame_start, slot->ev_inv_l1_start[ch]);
        cudaEventElapsedTime(&l1_e, slot->ev_frame_start, slot->ev_inv_l1_stop[ch]);
        cudaEventElapsedTime(&l1_d, slot->ev_inv_l1_start[ch], slot->ev_inv_l1_stop[ch]);
        timings->inverse_records.push_back({ch, 1, l1_s, l1_e, l1_d});
        if (l1_s < earliest_inv_start) earliest_inv_start = l1_s;
        if (l1_e > latest_inv_end) latest_inv_end = l1_e;
        sum_inv_work += l1_d;

        // Level 0
        float l0_s = 0.0f, l0_e = 0.0f, l0_d = 0.0f;
        cudaEventElapsedTime(&l0_s, slot->ev_frame_start, slot->ev_inv_l0_start[ch]);
        cudaEventElapsedTime(&l0_e, slot->ev_frame_start, slot->ev_inv_l0_stop[ch]);
        cudaEventElapsedTime(&l0_d, slot->ev_inv_l0_start[ch], slot->ev_inv_l0_stop[ch]);
        timings->inverse_records.push_back({ch, 0, l0_s, l0_e, l0_d});
        if (l0_s < earliest_inv_start) earliest_inv_start = l0_s;
        if (l0_e > latest_inv_end) latest_inv_end = l0_e;
        sum_inv_work += l0_d;
    }

    timings->t_first_inverse_start_ms = (earliest_inv_start < 1e8f) ? earliest_inv_start : 0.0f;
    timings->inverse_wavelet_work_ms = sum_inv_work;
    timings->inverse_wavelet_wall_ms = (latest_inv_end > earliest_inv_start) ? (latest_inv_end - earliest_inv_start) : 0.0f;
    timings->field_combine_ms = 0.0f;
    if (slot->interlaced_gop1) {
        for (int ch = 0; ch < 3; ++ch) {
            float field_ms = 0.0f;
            cudaEventElapsedTime(&field_ms, slot->ev_field_start[ch], slot->ev_field_stop[ch]);
            timings->field_combine_ms += field_ms;
        }
    }

    // Color Conversion
    float color_s = 0.0f, color_e = 0.0f, color_d = 0.0f;
    if (slot->surface_type == DeviceSurfaceType::BGRA8) {
        cudaEventElapsedTime(&color_s, slot->ev_frame_start, slot->ev_color_start);
        cudaEventElapsedTime(&color_e, slot->ev_frame_start, slot->ev_color_stop);
        cudaEventElapsedTime(&color_d, slot->ev_color_start, slot->ev_color_stop);
    }
    timings->t_color_start_ms = color_s;
    timings->color_convert_ms = color_d;

    // Final Ready
    float total_crit_ms = 0.0f;
    cudaEventElapsedTime(&total_crit_ms, slot->ev_frame_start, slot->ev_ready);
    timings->t_ready_ms = total_crit_ms;
    timings->total_critical_path_ms = total_crit_ms;
    timings->ready_overhead_ms = slot->timings.ready_overhead_ms;

    timings->total_work_sum_ms = timings->host_parse_ms + timings->task_prep_ms +
                                 timings->compressed_h2d_ms + timings->gpu_ll_ms +
                                 timings->host_index_cpu_work_ms + timings->gpu_replay_work_ms +
                                 timings->inverse_wavelet_work_ms + timings->color_convert_ms;

    timings->total_work_sum_ms += timings->gpu_index_work_ms;
    // Overlap Metrics (Section 29)
    float serialized_work = timings->host_index_wall_ms + timings->gpu_replay_wall_ms;
    float comb_end = std::max(timings->t_final_replay_end_ms, timings->t_final_index_end_ms);
    float comb_start = timings->t_first_index_start_ms;
    float actual_combined_wall = (comb_end > comb_start) ? (comb_end - comb_start) : 0.0f;

    float overlap_saved = (serialized_work > actual_combined_wall) ? (serialized_work - actual_combined_wall) : 0.0f;
    float min_stage = std::min(timings->host_index_wall_ms, timings->gpu_replay_wall_ms);
    float overlap_eff = (min_stage > 0.001f) ? (overlap_saved / min_stage) : 0.0f;

    timings->overlap_saved_ms = overlap_saved;
    timings->overlap_efficiency = overlap_eff;

    if (slot->pipeline_mode == Phase2EPipelineMode::Phase2FCHybridFullFrame) {
        timings->fc_band_records.assign(slot->fc_band_records, slot->fc_band_records + kMaxVlcBands);
        float fc_summary_work = 0.0f;
        float fc_cpu_resolve_work = 0.0f;
        int hybrid_bands = 0, method_e_bands = 0, fallback_bands = 0;

        for (int b = 0; b < kMaxVlcBands; ++b) {
            const auto& rec = slot->fc_band_records[b];
            if (rec.fell_back) {
                fallback_bands++;
                method_e_bands++;
            } else if (rec.chosen_method == BandEntropyMethod::MethodBF_Hybrid) {
                hybrid_bands++;
                float s_dur = 0.0f;
                cudaEventElapsedTime(&s_dur, slot->ev_summary_start[b], slot->ev_summary_done[b]);
                fc_summary_work += s_dur;
                fc_cpu_resolve_work += rec.cpu_resolve_ms;
            } else {
                method_e_bands++;
            }
        }
        timings->fc_summary_work_ms = fc_summary_work;
        timings->fc_cpu_resolve_ms = fc_cpu_resolve_work;
        timings->fc_bf_band_count = hybrid_bands;
        timings->fc_phase2e_band_count = method_e_bands;
        timings->fc_fallback_band_count = fallback_bands;
    }

    return true;
}

bool CudaDecoderPipelinePhase2E::GetBandDecodeResults(uint64_t ticket, std::vector<GpuBandDecodeResult>& results) {
    if (!impl_) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    results.clear();
    if (slot->GpuIndex()) {
        std::vector<GpuCheckpointIndexResult> index;
        if (!GetGpuIndexResults(ticket, index)) return false;
        for (const auto& i : index) {
            GpuBandDecodeResult r{};
            r.status = i.status; r.consumed_bytes = (i.next_nibble + 1) / 2;
            r.final_fsm_state = i.state; r.final_coeff_pos = i.out_coeff_pos;
            results.push_back(r);
        }
        return true;
    }
    for (int b = 0; b < kMaxVlcBands; ++b) {
        GpuBandDecodeResult r{};
        r.status = GpuBandDecodeStatus::Complete;
        results.push_back(r);
    }
    return true;
}

bool CudaDecoderPipelinePhase2E::GetSegmentDecodeResults(uint64_t ticket, std::vector<GpuSegmentDecodeResult>& results) {
    if (!impl_) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    std::vector<GpuCheckpointIndexResult> index;
    if (slot->GpuIndex() && !GetGpuIndexResults(ticket, index)) return false;
    auto count = [&](int b) -> size_t { return slot->GpuIndex() ? index[b].segment_count : slot->band_segments[b].size(); };
    size_t total_segs = 0;
    for (int b = 0; b < kMaxVlcBands; ++b) total_segs += count(b);
    if (total_segs == 0) {
        results.clear();
        return true;
    }

    results.resize(total_segs);
    size_t cur = 0;
    for (int b = 0; b < kMaxVlcBands; ++b) {
        size_t n = count(b);
        if (n > 0) {
            cudaMemcpy(
                results.data() + cur,
                slot->d_seg_results + b * kMaxSegmentsPerBand,
                n * sizeof(GpuSegmentDecodeResult),
                cudaMemcpyDeviceToHost
            );
            cur += n;
        }
    }
    return true;
}

bool CudaDecoderPipelinePhase2E::GetHostCheckpoints(uint64_t ticket, std::vector<GpuEntropySegmentDescriptor>& out_segments) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    if (slot->GpuIndex()) return false;
    out_segments.clear();
    for (int b = 0; b < kMaxVlcBands; ++b) {
        out_segments.insert(out_segments.end(), slot->band_segments[b].begin(), slot->band_segments[b].end());
    }
    return true;
}

bool CudaDecoderPipelinePhase2E::DownloadBandCoefficientsToHost(
    uint64_t ticket,
    int band_index,
    std::vector<int16_t>& out_coeffs,
    int* out_w,
    int* out_h,
    int* out_pitch
) {
    if (!impl_) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;
    if (slot->fieldplus_mode) {
        if (band_index < 0 || band_index >= kFieldPlusBandCount) return false;
        const auto& desc = slot->fieldplus.bands[size_t(band_index)];
        if (!desc.d_destination || desc.width <= 0 || desc.height <= 0 || desc.pitch_bytes == 0) return false;
        if (out_w) *out_w = desc.width;
        if (out_h) *out_h = desc.height;
        if (out_pitch) *out_pitch = static_cast<int>(desc.pitch_bytes);
        out_coeffs.resize((desc.pitch_bytes / sizeof(int16_t)) * size_t(desc.height));
        return cudaMemcpy(out_coeffs.data(), desc.d_destination, desc.pitch_bytes * size_t(desc.height),
            cudaMemcpyDeviceToHost) == cudaSuccess;
    }
    if (band_index < 0 || band_index >= static_cast<int>(slot->host_bands.size())) return false;

    const auto& desc = slot->host_bands[band_index];
    if (out_w) *out_w = desc.width;
    if (out_h) *out_h = desc.height;
    if (out_pitch) *out_pitch = static_cast<int>(desc.pitch_bytes);

    size_t total_samples = (desc.pitch_bytes / sizeof(int16_t)) * desc.height;
    out_coeffs.resize(total_samples);

    cudaError_t err = cudaMemcpy(
        out_coeffs.data(),
        desc.d_destination,
        desc.pitch_bytes * desc.height,
        cudaMemcpyDeviceToHost
    );

    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2E::DownloadFieldPlusTemporalStages(uint64_t ticket, int channel,
    std::vector<int16_t>& temporal_low, std::vector<int16_t>& temporal_high,
    std::vector<int16_t>& wavelet4_ll, std::vector<int16_t>& frame_base_ll,
    int* out_width, int* out_height) {
    if (!impl_ || channel < 0 || channel >= 3) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* slot = impl_->FindSlot(ticket);
    if (!slot || slot->status != DecodeTicketStatus::Complete || !slot->fieldplus_mode) return false;
    const int channel_width = channel == 0 ? slot->width : slot->width / 2;
    const int width = channel_width / 2;
    const int height = slot->height / 2;
    const size_t row_bytes = size_t(width) * sizeof(int16_t);
    temporal_low.resize(size_t(width) * size_t(height));
    temporal_high.resize(size_t(width) * size_t(height));
    wavelet4_ll.resize(size_t(channel_width / 4) * size_t(slot->height / 4));
    frame_base_ll.resize(size_t(width) * size_t(height));
    const auto& c = slot->channels[channel];
    const auto& fp = slot->fieldplus;
    if (cudaMemcpy2D(temporal_low.data(), row_bytes, fp.d_gop_low[channel], fp.gop_high_pitch[channel],
            row_bytes, height, cudaMemcpyDeviceToHost) != cudaSuccess ||
        cudaMemcpy2D(temporal_high.data(), row_bytes, fp.d_gop_high[channel], fp.gop_high_pitch[channel],
            row_bytes, height, cudaMemcpyDeviceToHost) != cudaSuccess ||
        cudaMemcpy2D(wavelet4_ll.data(), size_t(channel == 0 ? slot->width / 4 : slot->width / 8) * sizeof(int16_t),
            c.d_ll1, c.ll1_pitch,
            size_t(channel == 0 ? slot->width / 4 : slot->width / 8) * sizeof(int16_t),
            slot->height / 4, cudaMemcpyDeviceToHost) != cudaSuccess ||
        cudaMemcpy2D(frame_base_ll.data(), row_bytes, c.d_ll0, c.ll0_pitch,
            row_bytes, height, cudaMemcpyDeviceToHost) != cudaSuccess) return false;
    if (out_width) *out_width = width;
    if (out_height) *out_height = height;
    return true;
}

bool CudaDecoderPipelinePhase2E::DownloadFieldPlusSpatialStages(uint64_t ticket, int channel,
    std::vector<int16_t>& vertical_low, std::vector<int16_t>& vertical_high,
    int* out_width, int* out_height) {
    if (!impl_ || channel < 0 || channel >= 3) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* slot = impl_->FindSlot(ticket);
    if (!slot || slot->status != DecodeTicketStatus::Complete || !slot->fieldplus_mode) return false;
    const int channel_width = channel == 0 ? slot->width : slot->width / 2;
    const int width = channel_width / 2;
    const int height = slot->height;
    const size_t row_bytes = size_t(width) * sizeof(int16_t);
    vertical_low.resize(size_t(width) * size_t(height));
    vertical_high.resize(size_t(width) * size_t(height));
    const auto& c = slot->channels[channel];
    if (cudaMemcpy2D(vertical_low.data(), row_bytes, c.d_vert_low, c.vert_low_pitch,
            row_bytes, height, cudaMemcpyDeviceToHost) != cudaSuccess ||
        cudaMemcpy2D(vertical_high.data(), row_bytes, c.d_vert_high, c.vert_high_pitch,
            row_bytes, height, cudaMemcpyDeviceToHost) != cudaSuccess) return false;
    if (out_width) *out_width = width;
    if (out_height) *out_height = height;
    return true;
}

bool CudaDecoderPipelinePhase2E::GetGpuIndexResults(uint64_t ticket, std::vector<GpuCheckpointIndexResult>& out) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto slot = impl_->FindSlot(ticket);
    if (!slot || !slot->GpuIndex() || slot->status == DecodeTicketStatus::Pending) return false;
    out.resize(kMaxVlcBands);
    return cudaMemcpy(out.data(), slot->d_index_results, out.size() * sizeof(out[0]), cudaMemcpyDeviceToHost) == cudaSuccess;
}
bool CudaDecoderPipelinePhase2E::GetGpuCheckpoints(uint64_t ticket, std::vector<GpuEntropySegmentDescriptor>& out) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto slot = impl_->FindSlot(ticket);
    if (!slot || !slot->GpuIndex() || slot->status != DecodeTicketStatus::Complete) return false;
    GpuCheckpointIndexResult results[kMaxVlcBands];
    if (cudaMemcpy(results, slot->d_index_results, sizeof(results), cudaMemcpyDeviceToHost) != cudaSuccess) return false;
    out.clear();
    for (int b = 0; b < kMaxVlcBands; ++b) {
        size_t n = results[b].segment_count;
        if (n > kMaxSegmentsPerBand || results[b].status != GpuBandDecodeStatus::Complete) return false;
        size_t cur = out.size();
        out.resize(cur + n);
        if (n && cudaMemcpy(out.data() + cur, slot->d_segments + b * kMaxSegmentsPerBand,
            n * sizeof(out[0]), cudaMemcpyDeviceToHost) != cudaSuccess) return false;
    }
    return true;
}
bool CudaDecoderPipelinePhase2E::DownloadIntermediateLl(uint64_t ticket, int ch, int level, std::vector<int16_t>& out) {
    if (!impl_ || ch < 0 || ch >= 3 || level < 0 || level > 1) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto slot = impl_->FindSlot(ticket);
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;
    auto& c = slot->channels[ch];
    int w = (ch == 0 ? slot->width : slot->width / 2) / (level ? 4 : 2);
    int h = slot->height / (level ? 4 : 2);
    out.resize(size_t(w) * h);
    return cudaMemcpy2D(out.data(), w * sizeof(int16_t), level ? c.d_ll1 : c.d_ll0,
        level ? c.ll1_pitch : c.ll0_pitch, w * sizeof(int16_t), h, cudaMemcpyDeviceToHost) == cudaSuccess;
}

bool CudaDecoderPipelinePhase2E::DownloadInterlacedFieldStage(
    uint64_t ticket, int channel,
    std::vector<int16_t>& temporal_low, std::vector<int16_t>& temporal_high,
    std::vector<int16_t>& reconstructed, int* width, int* field_rows,
    int* low_pitch, int* out_pitch) {
    if (!impl_ || channel < 0 || channel > 2) return false;
    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || !slot->interlaced_gop1 || slot->status != DecodeTicketStatus::Complete) return false;
    const int ch_w = channel == 0 ? slot->width : slot->width / 2;
    const int rows = slot->height / 2;
    if (width) *width = ch_w;
    if (field_rows) *field_rows = rows;
    const auto& c = slot->channels[channel];
    if (low_pitch) *low_pitch = int(c.vert_low_pitch);
    if (out_pitch) *out_pitch = int(c.final_plane_pitch);
    temporal_low.resize(size_t(ch_w) * rows);
    temporal_high.resize(size_t(ch_w) * rows);
    reconstructed.resize(size_t(ch_w) * slot->height);
    const size_t row_bytes = size_t(ch_w) * sizeof(int16_t);
    if (cudaMemcpy2D(temporal_low.data(), row_bytes, c.d_vert_low, c.vert_low_pitch,
            row_bytes, rows, cudaMemcpyDeviceToHost) != cudaSuccess) return false;
    if (cudaMemcpy2D(temporal_high.data(), row_bytes, c.d_vert_high, c.vert_high_pitch,
            row_bytes, rows, cudaMemcpyDeviceToHost) != cudaSuccess) return false;
    return cudaMemcpy2D(reconstructed.data(), row_bytes, c.d_final_plane, c.final_plane_pitch,
        row_bytes, slot->height, cudaMemcpyDeviceToHost) == cudaSuccess;
}

bool LaunchInterlacedFieldCombine(const int16_t* low, size_t low_pitch,
    const int16_t* high, size_t high_pitch, int16_t* output, size_t output_pitch,
    int width, int field_rows, cudaStream_t stream, uint32_t* frame_error) {
    if (!low || !high || !output || width <= 0 || field_rows <= 0) return false;
    dim3 block(32, 8);
    dim3 grid((width + block.x - 1) / block.x, (field_rows + block.y - 1) / block.y);
    FieldCombineKernel<<<grid, block, 0, stream>>>(
        low, int(low_pitch), high, int(high_pitch), output, int(output_pitch),
        width, field_rows, frame_error);
    return cudaGetLastError() == cudaSuccess;
}

std::string CudaDecoderPipelinePhase2E::ExportTimelineJson(uint64_t ticket) {
    Phase2eSlotStageTimings t{};
    if (!GetStageTimings(ticket, &t)) return "{}";

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"total_critical_path_ms\": " << t.total_critical_path_ms << ",\n";
    ss << "  \"total_work_sum_ms\": " << t.total_work_sum_ms << ",\n";
    ss << "  \"gpu_index_wall_ms\": " << t.gpu_index_wall_ms << ",\n";
    ss << "  \"gpu_index_work_ms\": " << t.gpu_index_work_ms << ",\n";
    ss << "  \"max_index_kernel_ms\": " << t.max_index_kernel_ms << ",\n";
    ss << "  \"host_schedule_wall_ms\": " << t.host_schedule_wall_ms << ",\n";
    ss << "  \"host_parse_ms\": " << t.host_parse_ms << ",\n";
    ss << "  \"task_prep_ms\": " << t.task_prep_ms << ",\n";
    ss << "  \"compressed_h2d_ms\": " << t.compressed_h2d_ms << ",\n";
    ss << "  \"gpu_ll_ms\": " << t.gpu_ll_ms << ",\n";
    ss << "  \"host_index_wall_ms\": " << t.host_index_wall_ms << ",\n";
    ss << "  \"host_index_cpu_work_ms\": " << t.host_index_cpu_work_ms << ",\n";
    ss << "  \"gpu_replay_wall_ms\": " << t.gpu_replay_wall_ms << ",\n";
    ss << "  \"gpu_replay_work_ms\": " << t.gpu_replay_work_ms << ",\n";
    ss << "  \"inverse_wavelet_wall_ms\": " << t.inverse_wavelet_wall_ms << ",\n";
    ss << "  \"inverse_wavelet_work_ms\": " << t.inverse_wavelet_work_ms << ",\n";
    ss << "  \"field_combine_ms\": " << t.field_combine_ms << ",\n";
    ss << "  \"color_convert_ms\": " << t.color_convert_ms << ",\n";
    ss << "  \"overlap_saved_ms\": " << t.overlap_saved_ms << ",\n";
    ss << "  \"overlap_efficiency\": " << t.overlap_efficiency << ",\n";
    ss << "  \"active_workers\": " << t.active_workers << ",\n";
    ss << "  \"active_streams\": " << t.active_streams << ",\n";
    ss << "  \"markers\": {\n";
    ss << "    \"parse_complete_ms\": " << t.t_parse_end_ms << ",\n";
    ss << "    \"h2d_start_ms\": " << t.t_h2d_start_ms << ",\n";
    ss << "    \"h2d_end_ms\": " << t.t_h2d_end_ms << ",\n";
    ss << "    \"first_index_start_ms\": " << t.t_first_index_start_ms << ",\n";
    ss << "    \"first_gpu_launch_ms\": " << t.t_first_gpu_launch_ms << ",\n";
    ss << "    \"final_index_end_ms\": " << t.t_final_index_end_ms << ",\n";
    ss << "    \"first_inverse_start_ms\": " << t.t_first_inverse_start_ms << ",\n";
    ss << "    \"final_replay_end_ms\": " << t.t_final_replay_end_ms << ",\n";
    ss << "    \"color_start_ms\": " << t.t_color_start_ms << ",\n";
    ss << "    \"ready_ms\": " << t.t_ready_ms << "\n";
    ss << "  },\n";
    ss << "  \"bands\": [\n";
    for (size_t i = 0; i < t.band_records.size(); ++i) {
        const auto& b = t.band_records[i];
        ss << "    {"
           << "\"band_idx\": " << b.band_index << ", "
           << "\"channel\": " << b.channel << ", "
           << "\"level\": " << b.level << ", "
           << "\"band\": " << b.band << ", "
           << "\"bytes\": " << b.payload_bytes << ", "
           << "\"segments\": " << b.segment_count << ", "
           << "\"worker\": " << b.host_worker_id << ", "
           << "\"stream\": " << b.gpu_stream_id << ", "
           << "\"idx_start\": " << b.host_index_start_ms << ", "
           << "\"idx_end\": " << b.host_index_end_ms << ", "
           << "\"gpu_index_start\": " << b.gpu_index_start_ms << ", "
           << "\"gpu_index_end\": " << b.gpu_index_end_ms << ", "
           << "\"gpu_index_work_ms\": " << b.gpu_index_work_ms << ", "
           << "\"max_index_kernel_ms\": " << b.max_index_kernel_ms << ", "
           << "\"gpu_start\": " << b.gpu_replay_start_ms << ", "
           << "\"gpu_end\": " << b.gpu_replay_end_ms
           << "}" << (i + 1 < t.band_records.size() ? ",\n" : "\n");
    }
    ss << "  ],\n";
    ss << "  \"inverse_levels\": [\n";
    for (size_t i = 0; i < t.inverse_records.size(); ++i) {
        const auto& inv = t.inverse_records[i];
        ss << "    {"
           << "\"channel\": " << inv.channel << ", "
           << "\"level\": " << inv.level << ", "
           << "\"start_ms\": " << inv.start_ms << ", "
           << "\"end_ms\": " << inv.end_ms << ", "
           << "\"duration_ms\": " << inv.duration_ms
           << "}" << (i + 1 < t.inverse_records.size() ? ",\n" : "\n");
    }
    ss << "  ]\n";
    ss << "}\n";

    return ss.str();
}

void CudaDecoderPipelinePhase2E::SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy policy) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->fc_selector_policy = policy;
    }
}

Phase2FCSelectorPolicy CudaDecoderPipelinePhase2E::GetPhase2FCSelectorPolicy() const {
    return impl_ ? impl_->fc_selector_policy : Phase2FCSelectorPolicy::StaticHeuristic;
}

void CudaDecoderPipelinePhase2E::SetPhase2FCChunkSize(uint32_t chunk_size) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->fc_chunk_size = chunk_size ? chunk_size : 256;
    }
}

uint32_t CudaDecoderPipelinePhase2E::GetPhase2FCChunkSize() const {
    return impl_ ? impl_->fc_chunk_size : 256;
}

void CudaDecoderPipelinePhase2E::SetPhase2FCOracleDecisions(const std::vector<BandEntropyMethod>& decisions) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->fc_oracle_decisions = decisions;
    }
}

bool CudaDecoderPipelinePhase2E::GetPhase2FCBandRecords(uint64_t ticket, std::vector<Phase2FCBandRecord>& out_records) const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;
    out_records.assign(slot->fc_band_records, slot->fc_band_records + kMaxVlcBands);
    return true;
}

} // namespace HyperCine
