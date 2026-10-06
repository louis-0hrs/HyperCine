// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2e.h"
#include "hypercine/decode_color_cuda.h"
#include "decode_inverse_kernels.cuh"

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

namespace HyperCine {

// External declaration from decode_fsm_tables.cpp
bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>& out_tables);

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kMaxVlcBands = 27;
constexpr int kMaxReplayStreams = 8;
constexpr int kMaxSegmentsPerBand = 64;
constexpr int kTotalSegmentCapacity = kMaxVlcBands * kMaxSegmentsPerBand; // 1,728

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
};

struct DecodeSlot2E {
    int slot_index = -1;
    uint64_t ticket = 0;
    bool in_use = false;
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

    // Reusable per-band host checkpoint buffers
    std::vector<GpuEntropySegmentDescriptor> band_segments[kMaxVlcBands];
    std::vector<GpuEntropyBandDescriptor> host_bands;
    std::vector<GpuEntropyBandDescriptor> vlc_descs;
    std::vector<GpuEntropyBandDescriptor> ll_descs;
    std::vector<GpuEntropySegmentDescriptor> host_segments; // canonical gathered
    GpuBandDecodeResult host_vlc_results[kMaxVlcBands];
    std::vector<GpuSegmentDecodeResult> host_seg_results;

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
    cudaEvent_t ev_ch_done[3] = {};

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
            band_segments[b].clear();
        }

        if (ev_color_start) { cudaEventDestroy(ev_color_start); ev_color_start = nullptr; }
        if (ev_color_stop)  { cudaEventDestroy(ev_color_stop);  ev_color_stop = nullptr; }
        if (ev_ready)       { cudaEventDestroy(ev_ready);       ev_ready = nullptr; }

        in_use = false;
        ticket = 0;
        status = DecodeTicketStatus::Invalid;
    }

    ~DecodeSlot2E() {
        FreeBuffers();
    }

    void LaunchLevel2Inverse(int ch, cudaStream_t s) {
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
            w_l2, h_l2, 0
        );

        dim3 block_h(64, 4);
        dim3 grid_h((w_l2 + block_h.x - 1) / block_h.x, (2 * h_l2 + block_h.y - 1) / block_h.y);
        HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            c.d_ll1, int(c.ll1_pitch),
            w_l2, 2 * h_l2, 0
        );
    }

    void LaunchLevel1Inverse(int ch, cudaStream_t s) {
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
            w_l1, h_l1, 2
        );

        dim3 block_h(64, 4);
        dim3 grid_h((w_l1 + block_h.x - 1) / block_h.x, (2 * h_l1 + block_h.y - 1) / block_h.y);
        HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            c.d_ll0, int(c.ll0_pitch),
            w_l1, 2 * h_l1, 2
        );
    }

    void LaunchLevel0Inverse(int ch, cudaStream_t s) {
        auto& c = channels[ch];
        int ch_w = (ch == 0) ? width : (width / 2);
        int ch_h = height;
        int w_l0 = ch_w / 2;
        int h_l0 = ch_h / 2;

        dim3 block(16, 16);
        dim3 grid_v((w_l0 + block.x - 1) / block.x, (h_l0 + block.y - 1) / block.y);
        VerticalInverseKernel<<<grid_v, block, 0, s>>>(
            c.d_ll0, int(c.ll0_pitch),
            c.d_l0_band[1], int(c.l0_pitch),
            c.d_l0_band[2], int(c.l0_pitch),
            c.d_l0_band[3], int(c.l0_pitch),
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            w_l0, h_l0, 0
        );

        dim3 block_h(64, 4);
        dim3 grid_h((w_l0 + block_h.x - 1) / block_h.x, (2 * h_l0 + block_h.y - 1) / block_h.y);
        HorizontalInverseKernel<<<grid_h, block_h, 0, s>>>(
            c.d_vert_low, int(c.vert_low_pitch),
            c.d_vert_high, int(c.vert_high_pitch),
            c.d_final_plane, int(c.final_plane_pitch),
            w_l0, 2 * h_l0, 0
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
                    main_stream
                );
                cudaEventRecord(ev_color_stop, main_stream);
            }

            cudaEventRecord(ev_ready, main_stream);
            color_enqueued = true;
        }
    }

    void DispatchBandGpu(int band_idx, int worker_id) {
        (void)worker_id;
        if (has_error.load(std::memory_order_relaxed)) return;

        std::lock_guard<std::mutex> lock(dispatch_mutex);
        if (has_error.load(std::memory_order_relaxed)) return;

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
                s
            );
        }

        // 6. Cooperative Difference Coding if band requires it
        if (vlc_descs[band_idx].difference_coding) {
            LaunchCooperativeDifferenceCodingGpu(d_vlc_descriptors + band_idx, 1, s);
        }

        // 7. Record band GPU done event
        cudaEventRecord(ev_band_done[band_idx], s);
        band_state[band_idx] = BandState::GpuQueued;

        // 8. In FullOverlap mode, check if inverse DAG can advance
        if (pipeline_mode == Phase2EPipelineMode::Phase2EFullOverlap) {
            CheckAdvanceInverseDag(vlc_descs[band_idx].channel);
        }
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

    std::vector<std::unique_ptr<DecodeSlot2E>> slots;
    std::mutex mutex;
    uint64_t next_ticket = 1;

    DelayHookFn delay_hook = nullptr;

    void AbandonSlot(DecodeSlot2E* s) {
        if (!s) return;
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
                s->pipeline_mode = pipeline_mode;
                s->scheduling_policy = scheduling_policy;
                s->entropy_mode = entropy_mode;
                s->fixed_segments_per_band = fixed_segments_per_band;
                s->num_replay_streams = num_replay_streams;
                s->delay_hook = delay_hook;
                s->d_fsm_tables = fsm_table_mgr.GetDeviceTable();
                s->has_error.store(false);
                s->error_msg.clear();
                s->next_replay_stream = 0;
                s->color_enqueued = false;
                for (int b = 0; b < kMaxVlcBands; ++b) {
                    s->band_state[b] = BandState::Unindexed;
                    s->band_assigned_stream[b] = -1;
                    s->task_stats[b] = TaskStat{};
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

        for (int b = 0; b < kMaxVlcBands; ++b) {
            slot->band_segments[b].reserve(kMaxSegmentsPerBand);
        }
        slot->host_segments.reserve(kTotalSegmentCapacity);

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

            err = cudaMallocPitch(&c.d_vert_low, &c.vert_low_pitch, w_l0 * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;
            err = cudaMallocPitch(&c.d_vert_high, &c.vert_high_pitch, w_l0 * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;

            err = cudaMallocPitch(&c.d_final_plane, &c.final_plane_pitch, ch_max_w * sizeof(int16_t), ch_max_h);
            if (err != cudaSuccess) return false;
        }

        err = cudaMallocPitch(&slot->d_bgra, &slot->bgra_pitch, max_width * 4, max_height);
        if (err != cudaSuccess) return false;

        cudaMallocHost(&slot->host_pinned_compressed, slot->compressed_capacity);
        cudaMallocHost(&slot->host_pinned_segments, kTotalSegmentCapacity * sizeof(GpuEntropySegmentDescriptor));
        cudaMalloc(&slot->d_compressed_sample, slot->compressed_capacity);

        cudaMalloc(&slot->d_ll_descriptors, 3 * sizeof(GpuEntropyBandDescriptor));
        cudaMalloc(&slot->d_vlc_descriptors, kMaxVlcBands * sizeof(GpuEntropyBandDescriptor));
        cudaMalloc(&slot->d_vlc_results, kMaxVlcBands * sizeof(GpuBandDecodeResult));

        cudaMalloc(&slot->d_segments, kTotalSegmentCapacity * sizeof(GpuEntropySegmentDescriptor));
        cudaMalloc(&slot->d_seg_results, kTotalSegmentCapacity * sizeof(GpuSegmentDecodeResult));

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
        }
        cudaStreamCreateWithFlags(&slot->main_stream, cudaStreamNonBlocking);

        // Create timing events
        cudaEventCreate(&slot->ev_frame_start);
        cudaEventCreate(&slot->ev_h2d_start);
        cudaEventCreate(&slot->ev_h2d_stop);
        cudaEventCreate(&slot->ev_compressed_h2d_ready);
        cudaEventCreate(&slot->ev_ll_ready);

        for (int b = 0; b < kMaxVlcBands; ++b) {
            cudaEventCreate(&slot->ev_band_start[b]);
            cudaEventCreate(&slot->ev_band_done[b]);
        }

        cudaEventCreate(&slot->ev_color_start);
        cudaEventCreate(&slot->ev_color_stop);
        cudaEventCreate(&slot->ev_ready);

        impl_->slots.push_back(std::move(slot));
    }

    impl_->initialized = true;
    return true;
}

void CudaDecoderPipelinePhase2E::Release() {
    if (!impl_ || !impl_->initialized) return;

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

    DecodeSlot2E* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->AcquireSlot();
    }

    if (!slot) {
        std::cerr << "Phase 2E Error: No available slot in decoder pool" << std::endl;
        return 0;
    }

    slot->timings = Phase2eSlotStageTimings{};
    slot->timings.host_parse_ms = host_parse_ms;
    slot->surface_type = requested_surface;
    slot->matrix = matrix;
    slot->range = range;
    slot->orientation = orientation;
    slot->t_frame_start = t_frame_start;
    slot->t_parse_end = t_parse_end;

    int frame_width = parsed_bands[7].width * 2;
    int frame_height = parsed_bands[7].height * 2;
    slot->width = frame_width;
    slot->height = frame_height;

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

    // 2. Parallel Host Checkpoint Indexing & Streaming GPU Replay (Section 6, 9, 11, 14, 24, 25)
    slot->timings.active_workers = impl_->worker_pool.WorkerCount();
    slot->timings.active_streams = slot->num_replay_streams;
    std::atomic<bool> abort_flag{false};
    std::mutex error_mutex;
    std::string first_error;

    auto task_fn = [&](int task_idx, int worker_id) {
        const int band_idx = task_indices[task_idx];
        const auto t0 = Clock::now();

        std::string err;
        bool ok = false;
        try {
            ok = GenerateSingleBandCheckpointsCpu(
                compressed_sample, compressed_size, slot->vlc_descs[band_idx], band_idx,
                impl_->host_fsm_tables.data(), impl_->host_fsm_tables.size(),
                slot->entropy_mode, slot->fixed_segments_per_band,
                slot->band_segments[band_idx], &err
            );
        } catch (const std::exception& e) {
            err = std::string("band ") + std::to_string(band_idx) + ": " + e.what();
        }

        const auto t1 = Clock::now();
        TaskStat& ts = slot->task_stats[task_idx];
        ts.start = t0;
        ts.end = t1;
        ts.duration_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        ts.worker_id = worker_id;

        if (!ok) {
            bool expected = false;
            if (abort_flag.compare_exchange_strong(expected, true)) {
                std::lock_guard<std::mutex> lk(error_mutex);
                first_error = err;
                slot->has_error.store(true);
            }
            return;
        }

        if (slot->delay_hook) {
            const auto& d = slot->vlc_descs[band_idx];
            slot->delay_hook(band_idx, d.channel, d.level, d.band);
        }

        // As soon as this band's checkpoints are generated, stream to GPU (Variants A & B)
        if (slot->pipeline_mode != Phase2EPipelineMode::Phase2DFrameBarrier) {
            slot->DispatchBandGpu(band_idx, worker_id);
        }
    };

    slot->t_batch_submit = Clock::now();
    bool pool_threw = false;
    try {
        impl_->worker_pool.ExecuteBatch(kMaxVlcBands, task_fn, &abort_flag);
    } catch (const std::exception& e) {
        pool_threw = true;
        first_error = std::string("worker exception: ") + e.what();
    } catch (...) {
        pool_threw = true;
        first_error = "worker exception: unknown";
    }
    slot->t_batch_return = Clock::now();

    if (pool_threw || abort_flag.load(std::memory_order_relaxed) || slot->has_error.load(std::memory_order_relaxed)) {
        std::cerr << "Phase 2E Indexing Failure: " << first_error << std::endl;
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
                slot->main_stream
            );
        }
        LaunchCooperativeDifferenceCodingGpu(slot->d_vlc_descriptors, kMaxVlcBands, slot->main_stream);

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
                slot->main_stream
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
                slot->main_stream
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

    if (caller_stream) {
        cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    }

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
        slot->status = DecodeTicketStatus::Complete;
        return DecodeTicketStatus::Complete;
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
        slot->status = DecodeTicketStatus::Complete;
        return true;
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

bool CudaDecoderPipelinePhase2E::ReleaseOutput(uint64_t ticket) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2E* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

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
            if (slot->task_stats[ti].worker_id >= 0 &&
                slot->vlc_descs[slot->band_map[rec.channel][rec.level][rec.band]].payload_bytes == slot->vlc_descs[b].payload_bytes &&
                slot->band_assigned_stream[b] >= 0)
            {
                rec.host_index_start_ms = ms_rel(slot->task_stats[ti].start);
                rec.host_index_end_ms = ms_rel(slot->task_stats[ti].end);
                rec.host_index_duration_ms = slot->task_stats[ti].duration_ms;
                rec.host_worker_id = slot->task_stats[ti].worker_id;
                break;
            }
        }

        rec.gpu_stream_id = slot->band_assigned_stream[b];

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

    size_t total_segs = 0;
    for (int b = 0; b < kMaxVlcBands; ++b) total_segs += slot->band_segments[b].size();
    if (total_segs == 0) {
        results.clear();
        return true;
    }

    results.resize(total_segs);
    size_t cur = 0;
    for (int b = 0; b < kMaxVlcBands; ++b) {
        size_t n = slot->band_segments[b].size();
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

std::string CudaDecoderPipelinePhase2E::ExportTimelineJson(uint64_t ticket) {
    Phase2eSlotStageTimings t{};
    if (!GetStageTimings(ticket, &t)) return "{}";

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"total_critical_path_ms\": " << t.total_critical_path_ms << ",\n";
    ss << "  \"total_work_sum_ms\": " << t.total_work_sum_ms << ",\n";
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
    ss << "  \"color_convert_ms\": " << t.color_convert_ms << ",\n";
    ss << "  \"overlap_saved_ms\": " << t.overlap_saved_ms << ",\n";
    ss << "  \"overlap_efficiency\": " << t.overlap_efficiency << ",\n";
    ss << "  \"active_workers\": " << t.active_workers << ",\n";
    ss << "  \"active_streams\": " << t.active_streams << ",\n";
    ss << "  \"markers\": {\n";
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

} // namespace HyperCine
