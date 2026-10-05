// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2d.h"
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

namespace HyperCine {

// External declaration from decode_fsm_tables.cpp
bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>& out_tables);

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kMaxVlcBands = 27;

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

struct DecodeSlot2D {
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

    GpuEntropyMode entropy_mode = GpuEntropyMode::ParallelAdaptive;
    int fixed_segments_per_band = 32;

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

    // Phase 2C/2D segment descriptors and results
    GpuEntropySegmentDescriptor* d_segments = nullptr;
    GpuSegmentDecodeResult* d_seg_results = nullptr;
    size_t max_segment_capacity = 27 * 64; // up to 1,728 segments per frame

    // Reusable per-band checkpoint buffers (Section 10 & 11)
    std::vector<GpuEntropySegmentDescriptor> band_segments[27];

    std::vector<GpuEntropyBandDescriptor> host_bands;
    std::vector<GpuEntropySegmentDescriptor> host_segments;
    GpuBandDecodeResult host_vlc_results[27];
    std::vector<GpuSegmentDecodeResult> host_seg_results;

    cudaStream_t main_stream = nullptr;
    cudaStream_t channel_streams[3] = {nullptr, nullptr, nullptr};

    cudaEvent_t ev_h2d_start = nullptr;
    cudaEvent_t ev_h2d_stop = nullptr;
    cudaEvent_t ev_ll_stop = nullptr;
    cudaEvent_t ev_vlc_stop = nullptr;
    cudaEvent_t ev_diff_stop = nullptr;
    cudaEvent_t ev_inv_start = nullptr;
    cudaEvent_t ev_inv_stop = nullptr;
    cudaEvent_t ev_color_start = nullptr;
    cudaEvent_t ev_color_stop = nullptr;
    cudaEvent_t ev_ch_done[3] = {nullptr, nullptr, nullptr};
    cudaEvent_t ev_ready = nullptr;

    Phase2dSlotStageTimings timings{};

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
        if (d_compressed_sample) { cudaFree(d_compressed_sample); d_compressed_sample = nullptr; }
        if (d_ll_descriptors) { cudaFree(d_ll_descriptors); d_ll_descriptors = nullptr; }
        if (d_vlc_descriptors) { cudaFree(d_vlc_descriptors); d_vlc_descriptors = nullptr; }
        if (d_vlc_results) { cudaFree(d_vlc_results); d_vlc_results = nullptr; }
        if (d_segments) { cudaFree(d_segments); d_segments = nullptr; }
        if (d_seg_results) { cudaFree(d_seg_results); d_seg_results = nullptr; }

        if (main_stream) { cudaStreamDestroy(main_stream); main_stream = nullptr; }
        for (int ch = 0; ch < 3; ++ch) {
            if (channel_streams[ch]) { cudaStreamDestroy(channel_streams[ch]); channel_streams[ch] = nullptr; }
            if (ev_ch_done[ch]) { cudaEventDestroy(ev_ch_done[ch]); ev_ch_done[ch] = nullptr; }
        }
        if (ev_h2d_start)   { cudaEventDestroy(ev_h2d_start);   ev_h2d_start = nullptr; }
        if (ev_h2d_stop)    { cudaEventDestroy(ev_h2d_stop);    ev_h2d_stop = nullptr; }
        if (ev_ll_stop)     { cudaEventDestroy(ev_ll_stop);     ev_ll_stop = nullptr; }
        if (ev_vlc_stop)    { cudaEventDestroy(ev_vlc_stop);    ev_vlc_stop = nullptr; }
        if (ev_diff_stop)   { cudaEventDestroy(ev_diff_stop);   ev_diff_stop = nullptr; }
        if (ev_inv_start)   { cudaEventDestroy(ev_inv_start);   ev_inv_start = nullptr; }
        if (ev_inv_stop)    { cudaEventDestroy(ev_inv_stop);    ev_inv_stop = nullptr; }
        if (ev_color_start) { cudaEventDestroy(ev_color_start); ev_color_start = nullptr; }
        if (ev_color_stop)  { cudaEventDestroy(ev_color_stop);  ev_color_stop = nullptr; }
        if (ev_ready)       { cudaEventDestroy(ev_ready);       ev_ready = nullptr; }

        for (int b = 0; b < 27; ++b) {
            band_segments[b].clear();
        }

        in_use = false;
        ticket = 0;
        status = DecodeTicketStatus::Invalid;
    }

    ~DecodeSlot2D() {
        FreeBuffers();
    }
};

} // anonymous namespace

struct CudaDecoderPipelinePhase2D::Impl {
    int max_width = 0;
    int max_height = 0;
    int precision = 10;
    int cuda_device = 0;
    int num_slots = 3;
    bool initialized = false;

    GpuEntropyMode entropy_mode = GpuEntropyMode::ParallelAdaptive;
    int fixed_segments_per_band = 32;

    HostIndexExecutionMode host_index_mode = HostIndexExecutionMode::ParallelWorkerPool;
    HostIndexSchedulingPolicy scheduling_policy = HostIndexSchedulingPolicy::LargestFirst;
    int configured_workers = 0;

    DecoderWorkerPool worker_pool;

    GpuFsmTableManager fsm_table_mgr;
    std::vector<GpuFsmEntry> host_fsm_tables;

    std::vector<std::unique_ptr<DecodeSlot2D>> slots;
    std::mutex mutex;
    uint64_t next_ticket = 1;

    // Return a slot acquired inside QueueDecode to the pool after a failure.
    // The caller never received a ticket, so nobody else can release it.
    // Drains the slot stream first so no in-flight work still references
    // the slot's pinned/device buffers when it is reused.
    void AbandonSlot(DecodeSlot2D* s) {
        if (!s) return;
        if (s->main_stream) cudaStreamSynchronize(s->main_stream);
        std::lock_guard<std::mutex> lock(mutex);
        s->status = DecodeTicketStatus::Failed;
        s->in_use = false;
        s->ticket = 0;
    }

    DecodeSlot2D* AcquireSlot() {
        for (auto& s : slots) {
            if (!s->in_use) {
                s->in_use = true;
                s->ticket = next_ticket++;
                s->status = DecodeTicketStatus::Pending;
                s->entropy_mode = entropy_mode;
                s->fixed_segments_per_band = fixed_segments_per_band;
                return s.get();
            }
        }
        return nullptr;
    }

    DecodeSlot2D* FindSlot(uint64_t ticket) {
        for (auto& s : slots) {
            if (s->in_use && s->ticket == ticket) return s.get();
        }
        return nullptr;
    }
};

CudaDecoderPipelinePhase2D::CudaDecoderPipelinePhase2D() : impl_(std::make_unique<Impl>()) {}

CudaDecoderPipelinePhase2D::~CudaDecoderPipelinePhase2D() {
    Release();
}

CudaDecoderPipelinePhase2D::CudaDecoderPipelinePhase2D(CudaDecoderPipelinePhase2D&&) noexcept = default;
CudaDecoderPipelinePhase2D& CudaDecoderPipelinePhase2D::operator=(CudaDecoderPipelinePhase2D&&) noexcept = default;

bool CudaDecoderPipelinePhase2D::Initialize(
    int max_width,
    int max_height,
    int num_slots,
    int precision,
    int num_host_workers,
    int cuda_device
) {
    Release();

    if (max_width <= 0 || max_height <= 0 || num_slots <= 0) return false;

    cudaError_t err = cudaSetDevice(cuda_device);
    if (err != cudaSuccess) return false;

    impl_->max_width = max_width;
    impl_->max_height = max_height;
    impl_->precision = precision;
    impl_->cuda_device = cuda_device;
    impl_->num_slots = num_slots;
    impl_->configured_workers = num_host_workers;

    // 1. Initialize persistent FSM tables
    if (!ExtractCineFormBaseFsmTables(impl_->host_fsm_tables)) {
        std::cerr << "Phase 2D Error: Failed to extract CineForm base FSM tables" << std::endl;
        return false;
    }

    if (!impl_->fsm_table_mgr.Initialize(cuda_device)) {
        std::cerr << "Phase 2D Error: Failed to initialize GPU FSM Table Manager" << std::endl;
        return false;
    }

    // 2. Initialize persistent worker pool (Section 5 & 6)
    if (!impl_->worker_pool.Initialize(num_host_workers)) {
        std::cerr << "Phase 2D Error: Failed to initialize DecoderWorkerPool" << std::endl;
        return false;
    }

    // 3. Allocate slot ring buffers
    impl_->slots.reserve(num_slots);
    for (int i = 0; i < num_slots; ++i) {
        auto slot = std::make_unique<DecodeSlot2D>();
        slot->slot_index = i;
        slot->max_width = max_width;
        slot->max_height = max_height;
        slot->precision = precision;
        slot->cuda_device = cuda_device;

        // Pre-reserve per-band checkpoint vectors to eliminate hot-path allocation (Section 10 & 11)
        for (int b = 0; b < 27; ++b) {
            slot->band_segments[b].reserve(64);
        }
        slot->host_segments.reserve(27 * 64);

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

            // Scratch buffers for vertical pass
            err = cudaMallocPitch(&c.d_vert_low, &c.vert_low_pitch, w_l0 * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;
            err = cudaMallocPitch(&c.d_vert_high, &c.vert_high_pitch, w_l0 * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;

            // Final reconstructed plane
            err = cudaMallocPitch(&c.d_final_plane, &c.final_plane_pitch, ch_max_w * sizeof(int16_t), ch_max_h);
            if (err != cudaSuccess) return false;
        }

        err = cudaMallocPitch(&slot->d_bgra, &slot->bgra_pitch, max_width * 4, max_height);
        if (err != cudaSuccess) return false;

        cudaMallocHost(&slot->host_pinned_compressed, slot->compressed_capacity);
        cudaMalloc(&slot->d_compressed_sample, slot->compressed_capacity);

        cudaMalloc(&slot->d_ll_descriptors, 3 * sizeof(GpuEntropyBandDescriptor));
        cudaMalloc(&slot->d_vlc_descriptors, 27 * sizeof(GpuEntropyBandDescriptor));
        cudaMalloc(&slot->d_vlc_results, 27 * sizeof(GpuBandDecodeResult));

        cudaMalloc(&slot->d_segments, slot->max_segment_capacity * sizeof(GpuEntropySegmentDescriptor));
        cudaMalloc(&slot->d_seg_results, slot->max_segment_capacity * sizeof(GpuSegmentDecodeResult));

        cudaStreamCreateWithFlags(&slot->main_stream, cudaStreamNonBlocking);
        for (int ch = 0; ch < 3; ++ch) {
            cudaStreamCreateWithFlags(&slot->channel_streams[ch], cudaStreamNonBlocking);
            cudaEventCreateWithFlags(&slot->ev_ch_done[ch], cudaEventDisableTiming);
        }

        cudaEventCreate(&slot->ev_h2d_start);
        cudaEventCreate(&slot->ev_h2d_stop);
        cudaEventCreate(&slot->ev_ll_stop);
        cudaEventCreate(&slot->ev_vlc_stop);
        cudaEventCreate(&slot->ev_diff_stop);
        cudaEventCreate(&slot->ev_inv_start);
        cudaEventCreate(&slot->ev_inv_stop);
        cudaEventCreate(&slot->ev_color_start);
        cudaEventCreate(&slot->ev_color_stop);
        cudaEventCreate(&slot->ev_ready);

        impl_->slots.push_back(std::move(slot));
    }

    impl_->initialized = true;
    return true;
}

void CudaDecoderPipelinePhase2D::Release() {
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

bool CudaDecoderPipelinePhase2D::IsInitialized() const {
    return impl_ && impl_->initialized;
}

void CudaDecoderPipelinePhase2D::SetHostIndexMode(HostIndexExecutionMode mode) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->host_index_mode = mode;
    }
}

HostIndexExecutionMode CudaDecoderPipelinePhase2D::GetHostIndexMode() const {
    return impl_ ? impl_->host_index_mode : HostIndexExecutionMode::ParallelWorkerPool;
}

void CudaDecoderPipelinePhase2D::SetSchedulingPolicy(HostIndexSchedulingPolicy policy) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->scheduling_policy = policy;
    }
}

HostIndexSchedulingPolicy CudaDecoderPipelinePhase2D::GetSchedulingPolicy() const {
    return impl_ ? impl_->scheduling_policy : HostIndexSchedulingPolicy::LargestFirst;
}

bool CudaDecoderPipelinePhase2D::SetWorkerCount(int num_workers) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->configured_workers = num_workers;
    return impl_->worker_pool.Initialize(num_workers);
}

int CudaDecoderPipelinePhase2D::GetWorkerCount() const {
    return impl_ ? impl_->worker_pool.WorkerCount() : 0;
}

void CudaDecoderPipelinePhase2D::SetEntropyMode(GpuEntropyMode mode, int fixed_segments_per_band) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->entropy_mode = mode;
        impl_->fixed_segments_per_band = fixed_segments_per_band;
    }
}

GpuEntropyMode CudaDecoderPipelinePhase2D::GetEntropyMode() const {
    return impl_ ? impl_->entropy_mode : GpuEntropyMode::ParallelAdaptive;
}

int CudaDecoderPipelinePhase2D::GetFixedSegmentsPerBand() const {
    return impl_ ? impl_->fixed_segments_per_band : 32;
}

uint64_t CudaDecoderPipelinePhase2D::QueueDecode(
    const uint8_t* compressed_sample,
    size_t compressed_size,
    DeviceSurfaceType requested_surface,
    ColorMatrix matrix,
    ColorRange range,
    RowOrientation orientation,
    cudaStream_t caller_stream
) {
    if (!impl_ || !impl_->initialized || !compressed_sample || compressed_size == 0) return 0;

    auto parse_start = Clock::now();

    int ch_offset = (requested_surface == DeviceSurfaceType::BGRA8) ? 24 : 4;
    std::vector<GpuEntropyBandDescriptor> parsed_bands;
    std::string parse_err;

    if (!ParseCineFormSample(compressed_sample, compressed_size, ch_offset, parsed_bands, &parse_err)) {
        std::cerr << "Phase 2D Parse Error: " << parse_err << std::endl;
        return 0;
    }

    if (parsed_bands.size() != 30) {
        std::cerr << "Phase 2D Parse Error: expected 30 bands for YUV422 progressive, got "
                  << parsed_bands.size() << std::endl;
        return 0;
    }

    auto parse_end = Clock::now();
    float host_parse_ms = std::chrono::duration<float, std::milli>(parse_end - parse_start).count();

    DecodeSlot2D* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->AcquireSlot();
    }

    if (!slot) {
        std::cerr << "Phase 2D Error: No available slot in decoder pool" << std::endl;
        return 0;
    }

    slot->timings = Phase2dSlotStageTimings{};
    slot->timings.host_parse_ms = host_parse_ms;
    slot->surface_type = requested_surface;
    slot->matrix = matrix;
    slot->range = range;
    slot->orientation = orientation;

    int frame_width = parsed_bands[7].width * 2;
    int frame_height = parsed_bands[7].height * 2;
    slot->width = frame_width;
    slot->height = frame_height;

    // Partition into 3 LL bands and 27 VLC bands and bind destination pointers
    std::vector<GpuEntropyBandDescriptor> ll_descs;
    std::vector<GpuEntropyBandDescriptor> vlc_descs;
    ll_descs.reserve(3);
    vlc_descs.reserve(27);

    for (auto& b : parsed_bands) {
        int ch = b.channel;
        auto& c = slot->channels[ch];

        if (b.subband == 0) {
            b.d_destination = c.d_l2_band[0];
            b.pitch_bytes = c.l2_pitch;
            b.pitch_samples = c.l2_pitch / sizeof(int16_t);
            ll_descs.push_back(b);
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
            vlc_descs.push_back(b);
        }
    }

    slot->host_bands = parsed_bands;

    // Check compressed buffer capacity
    if (compressed_size > slot->compressed_capacity) {
        impl_->AbandonSlot(slot);
        return 0;
    }

    int num_vlc_bands = static_cast<int>(vlc_descs.size());
    if (ll_descs.size() != 3 || num_vlc_bands != kMaxVlcBands) {
        std::cerr << "Phase 2D Parse Error: expected 3 LL + 27 VLC bands, got "
                  << ll_descs.size() << " LL + " << num_vlc_bands << " VLC" << std::endl;
        impl_->AbandonSlot(slot);
        return 0;
    }

    // Task Preparation & Scheduling Policy (Section 7, 8, 14, 24)
    auto prep_start = Clock::now();

    int task_indices[27];
    for (int i = 0; i < num_vlc_bands; ++i) {
        task_indices[i] = i;
        slot->band_segments[i].clear();
    }

    int largest_idx = 0;
    uint32_t largest_bytes = 0;
    for (int i = 0; i < num_vlc_bands; ++i) {
        if (vlc_descs[i].payload_bytes > largest_bytes) {
            largest_bytes = vlc_descs[i].payload_bytes;
            largest_idx = i;
        }
    }
    slot->timings.largest_band_idx = largest_idx;
    slot->timings.largest_band_bytes = largest_bytes;

    if (impl_->scheduling_policy == HostIndexSchedulingPolicy::LargestFirst) {
        std::sort(task_indices, task_indices + num_vlc_bands, [&](int a, int b) {
            return vlc_descs[a].payload_bytes > vlc_descs[b].payload_bytes;
        });
    }

    auto prep_end = Clock::now();
    slot->timings.task_prep_ms = std::chrono::duration<float, std::milli>(prep_end - prep_start).count();

    // Checkpoint Indexing (SingleThread vs ParallelWorkerPool)
    auto index_start = Clock::now();
    slot->host_segments.clear();

    if (slot->entropy_mode != GpuEntropyMode::SerialReference) {
        if (impl_->host_index_mode == HostIndexExecutionMode::SingleThreadReference) {
            slot->timings.active_workers = 1;
            float sum_traversal_ms = 0.0f;
            float max_task_ms = 0.0f;
            bool has_error = false;
            std::string first_error;

            for (int i = 0; i < num_vlc_bands; ++i) {
                int b = task_indices[i];
                auto t0 = Clock::now();
                std::string err;
                bool ok = GenerateSingleBandCheckpointsCpu(
                    compressed_sample, compressed_size, vlc_descs[b], b,
                    impl_->host_fsm_tables.data(), impl_->host_fsm_tables.size(),
                    slot->entropy_mode, slot->fixed_segments_per_band,
                    slot->band_segments[b], &err
                );
                auto t1 = Clock::now();
                float dur = std::chrono::duration<float, std::milli>(t1 - t0).count();
                sum_traversal_ms += dur;
                if (dur > max_task_ms) max_task_ms = dur;
                if (!ok) {
                    has_error = true;
                    first_error = err;
                    break;
                }
            }

            slot->timings.slowest_task_ms = max_task_ms;
            slot->timings.slowest_worker_id = 0;
            slot->timings.checkpoint_traversal_ms = sum_traversal_ms;
            slot->timings.index_parallel_wall_ms = sum_traversal_ms; // serial: wall == work
            slot->timings.barrier_wait_ms = 0.0f;
            slot->timings.queue_overhead_ms = 0.0f;

            if (has_error) {
                std::cerr << "Phase 2D SingleThread Indexing Failure: " << first_error << std::endl;
                impl_->AbandonSlot(slot);
                return 0;
            }
        } else {
            // Phase 2D Parallel Worker Pool Execution (Section 5, 7, 10, 11)
            slot->timings.active_workers = impl_->worker_pool.WorkerCount();
            std::atomic<bool> abort_flag{false};
            std::mutex error_mutex;
            std::string first_error;

            struct TaskStat {
                Clock::time_point start{};
                Clock::time_point end{};
                float duration_ms = 0.0f;
                int worker_id = -1;
            };
            TaskStat task_stats[kMaxVlcBands];

            // Each task writes ONLY to its own band_segments[band_idx] and
            // task_stats[task_idx]; no shared append structure, no locks on
            // the success path.
            auto task_fn = [&](int task_idx, int worker_id) {
                const int band_idx = task_indices[task_idx];
                const auto t0 = Clock::now();

                std::string err;
                bool ok = false;
                try {
                    ok = GenerateSingleBandCheckpointsCpu(
                        compressed_sample, compressed_size, vlc_descs[band_idx], band_idx,
                        impl_->host_fsm_tables.data(), impl_->host_fsm_tables.size(),
                        slot->entropy_mode, slot->fixed_segments_per_band,
                        slot->band_segments[band_idx], &err
                    );
                } catch (const std::exception& e) {
                    err = std::string("band ") + std::to_string(band_idx) + ": " + e.what();
                }

                const auto t1 = Clock::now();
                TaskStat& ts = task_stats[task_idx];
                ts.start = t0;
                ts.end = t1;
                ts.duration_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
                ts.worker_id = worker_id;

                if (!ok) {
                    bool expected = false;
                    if (abort_flag.compare_exchange_strong(expected, true)) {
                        std::lock_guard<std::mutex> lk(error_mutex);
                        first_error = err;
                    }
                }
            };

            auto q_start = Clock::now();
            bool pool_threw = false;
            try {
                impl_->worker_pool.ExecuteBatch(num_vlc_bands, task_fn, &abort_flag);
            } catch (const std::exception& e) {
                pool_threw = true;
                first_error = std::string("worker exception: ") + e.what();
            } catch (...) {
                pool_threw = true;
                first_error = "worker exception: unknown";
            }
            auto q_end = Clock::now();

            // Measured scheduling overheads (no estimated constants):
            //   queue_overhead = submit -> earliest task start (wake + dispatch latency)
            //   barrier_wait   = latest task end -> batch return (join latency)
            Clock::time_point first_start = q_end, last_end = q_start;
            float sum_traversal_ms = 0.0f;
            float max_task_ms = 0.0f;
            int slowest_worker = -1;
            for (int i = 0; i < num_vlc_bands; ++i) {
                const TaskStat& ts = task_stats[i];
                if (ts.worker_id < 0) continue; // skipped after abort
                if (ts.start < first_start) first_start = ts.start;
                if (ts.end > last_end) last_end = ts.end;
                sum_traversal_ms += ts.duration_ms;
                if (ts.duration_ms > max_task_ms) {
                    max_task_ms = ts.duration_ms;
                    slowest_worker = ts.worker_id;
                }
            }
            auto ms = [](Clock::duration d) { return std::chrono::duration<float, std::milli>(d).count(); };
            slot->timings.index_parallel_wall_ms = ms(q_end - q_start);
            slot->timings.queue_overhead_ms = first_start > q_start ? ms(first_start - q_start) : 0.0f;
            slot->timings.barrier_wait_ms = q_end > last_end ? ms(q_end - last_end) : 0.0f;
            slot->timings.slowest_task_ms = max_task_ms;
            slot->timings.slowest_worker_id = slowest_worker;
            slot->timings.checkpoint_traversal_ms = sum_traversal_ms;

            if (pool_threw || abort_flag.load(std::memory_order_relaxed)) {
                std::cerr << "Phase 2D Parallel Indexing Failure: " << first_error << std::endl;
                impl_->AbandonSlot(slot);
                return 0;
            }
        }

        // Canonical-order gather (0..26) into reserved contiguous storage.
        // Ordering is independent of worker count and scheduling policy.
        auto gather_start = Clock::now();
        slot->host_segments.clear();
        for (int b = 0; b < num_vlc_bands; ++b) {
            slot->host_segments.insert(
                slot->host_segments.end(),
                slot->band_segments[b].begin(),
                slot->band_segments[b].end()
            );
        }
        slot->timings.segment_gather_ms =
            std::chrono::duration<float, std::milli>(Clock::now() - gather_start).count();
    }

    auto index_end = Clock::now();
    slot->timings.host_index_ms = std::chrono::duration<float, std::milli>(index_end - index_start).count();

    // 2. Asynchronous Compressed H2D transfer
    std::memcpy(slot->host_pinned_compressed, compressed_sample, compressed_size);
    slot->current_compressed_size = compressed_size;

    cudaEventRecord(slot->ev_h2d_start, slot->main_stream);

    cudaMemcpyAsync(
        slot->d_compressed_sample,
        slot->host_pinned_compressed,
        compressed_size,
        cudaMemcpyHostToDevice,
        slot->main_stream
    );

    cudaMemcpyAsync(
        slot->d_ll_descriptors,
        ll_descs.data(),
        ll_descs.size() * sizeof(GpuEntropyBandDescriptor),
        cudaMemcpyHostToDevice,
        slot->main_stream
    );

    cudaMemcpyAsync(
        slot->d_vlc_descriptors,
        vlc_descs.data(),
        vlc_descs.size() * sizeof(GpuEntropyBandDescriptor),
        cudaMemcpyHostToDevice,
        slot->main_stream
    );

    int num_segments = static_cast<int>(slot->host_segments.size());
    if (num_segments > 0) {
        if (static_cast<size_t>(num_segments) > slot->max_segment_capacity) {
            impl_->AbandonSlot(slot);
            return 0;
        }
        cudaMemcpyAsync(
            slot->d_segments,
            slot->host_segments.data(),
            num_segments * sizeof(GpuEntropySegmentDescriptor),
            cudaMemcpyHostToDevice,
            slot->main_stream
        );
    }

    cudaEventRecord(slot->ev_h2d_stop, slot->main_stream);

    // 3. GPU LowPass Decode
    LaunchDecodeLowpassGpu(
        slot->d_compressed_sample,
        compressed_size,
        slot->d_ll_descriptors,
        static_cast<int>(ll_descs.size()),
        slot->main_stream
    );
    cudaEventRecord(slot->ev_ll_stop, slot->main_stream);

    // 4. GPU HighPass VLC Replay
    if (slot->entropy_mode == GpuEntropyMode::SerialReference) {
        LaunchDecodeEntropyGpu(
            slot->d_compressed_sample,
            compressed_size,
            slot->d_vlc_descriptors,
            static_cast<int>(vlc_descs.size()),
            impl_->fsm_table_mgr.GetDeviceTable(),
            slot->d_vlc_results,
            slot->main_stream
        );
        cudaEventRecord(slot->ev_vlc_stop, slot->main_stream);
        cudaEventRecord(slot->ev_diff_stop, slot->main_stream);
    } else {
        LaunchZeroVlcBandsGpu(
            slot->d_vlc_descriptors,
            static_cast<int>(vlc_descs.size()),
            slot->main_stream
        );

        LaunchDecodeEntropyParallelGpu(
            slot->d_compressed_sample,
            compressed_size,
            slot->d_vlc_descriptors,
            static_cast<int>(vlc_descs.size()),
            slot->d_segments,
            num_segments,
            impl_->fsm_table_mgr.GetDeviceTable(),
            slot->d_seg_results,
            slot->main_stream
        );
        cudaEventRecord(slot->ev_vlc_stop, slot->main_stream);

        LaunchCooperativeDifferenceCodingGpu(
            slot->d_vlc_descriptors,
            static_cast<int>(vlc_descs.size()),
            slot->main_stream
        );
        cudaEventRecord(slot->ev_diff_stop, slot->main_stream);
    }

    // 5. CUDA Inverse Wavelet Transform
    cudaEventRecord(slot->ev_inv_start, slot->main_stream);

    for (int ch = 0; ch < 3; ++ch) {
        auto& c = slot->channels[ch];
        cudaStream_t s = slot->channel_streams[ch];

        cudaStreamWaitEvent(s, slot->ev_diff_stop, 0);

        int ch_w = (ch == 0) ? frame_width : (frame_width / 2);
        int ch_h = frame_height;

        int w_l2 = ch_w / 8;
        int h_l2 = ch_h / 8;
        int w_l1 = ch_w / 4;
        int h_l1 = ch_h / 4;
        int w_l0 = ch_w / 2;
        int h_l0 = ch_h / 2;

        dim3 block(16, 16);

        // LEVEL 2 -> LL1
        {
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

        // LEVEL 1 -> LL0
        {
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

        // LEVEL 0 -> Final Reconstructed Plane
        {
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

        cudaEventRecord(slot->ev_ch_done[ch], s);
    }

    for (int ch = 0; ch < 3; ++ch) {
        cudaStreamWaitEvent(slot->main_stream, slot->ev_ch_done[ch], 0);
    }
    cudaEventRecord(slot->ev_inv_stop, slot->main_stream);

    // 6. CUDA Color Conversion (YUV422 -> BGRA8)
    if (slot->surface_type == DeviceSurfaceType::BGRA8) {
        cudaEventRecord(slot->ev_color_start, slot->main_stream);

        bool color_ok = LaunchYuv422ToBgra8(
            slot->channels[0].d_final_plane, slot->channels[0].final_plane_pitch,
            slot->channels[1].d_final_plane, slot->channels[1].final_plane_pitch,
            slot->channels[2].d_final_plane, slot->channels[2].final_plane_pitch,
            slot->d_bgra, slot->bgra_pitch,
            frame_width, frame_height,
            matrix, range, orientation,
            slot->main_stream
        );

        if (!color_ok) {
            impl_->AbandonSlot(slot);
            return 0;
        }

        cudaEventRecord(slot->ev_color_stop, slot->main_stream);
    }

    // 7. Record ready event
    cudaEventRecord(slot->ev_ready, slot->main_stream);

    if (caller_stream) {
        cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    }

    return slot->ticket;
}

DecodeTicketStatus CudaDecoderPipelinePhase2D::PollDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return DecodeTicketStatus::Invalid;
    std::lock_guard<std::mutex> lock(impl_->mutex);

    DecodeSlot2D* slot = impl_->FindSlot(ticket);
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

bool CudaDecoderPipelinePhase2D::WaitForDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;
    DecodeSlot2D* slot = nullptr;
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

bool CudaDecoderPipelinePhase2D::StreamWait(uint64_t ticket, cudaStream_t caller_stream) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !caller_stream) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2D* slot = impl_->FindSlot(ticket);
    if (!slot || !slot->ev_ready) return false;

    cudaError_t err = cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2D::GetDeviceBgraOutput(uint64_t ticket, DeviceBgraSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2D* slot = impl_->FindSlot(ticket);
    if (!slot || slot->surface_type != DeviceSurfaceType::BGRA8) return false;

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

bool CudaDecoderPipelinePhase2D::GetDevicePlanarOutput(uint64_t ticket, DevicePlanarSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2D* slot = impl_->FindSlot(ticket);
    if (!slot || slot->surface_type != DeviceSurfaceType::PlanarYUV422_16S) return false;

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

bool CudaDecoderPipelinePhase2D::ReleaseOutput(uint64_t ticket) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2D* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    slot->in_use = false;
    slot->ticket = 0;
    slot->status = DecodeTicketStatus::Invalid;
    return true;
}

bool CudaDecoderPipelinePhase2D::DownloadBgraToHost(uint64_t ticket, uint8_t* host_bgra, size_t host_pitch) {
    if (!impl_ || !host_bgra) return false;
    DecodeSlot2D* slot = nullptr;
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

bool CudaDecoderPipelinePhase2D::DownloadPlanarToHost(
    uint64_t ticket,
    int16_t* host_y, size_t y_pitch,
    int16_t* host_v, size_t v_pitch,
    int16_t* host_u, size_t u_pitch
) {
    if (!impl_) return false;
    DecodeSlot2D* slot = nullptr;
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

bool CudaDecoderPipelinePhase2D::GetStageTimings(uint64_t ticket, Phase2dSlotStageTimings* timings) {
    if (!impl_ || !timings) return false;
    DecodeSlot2D* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    float h2d_ms = 0.0f, ll_ms = 0.0f, vlc_ms = 0.0f, diff_ms = 0.0f, inv_ms = 0.0f, color_ms = 0.0f;
    cudaEventElapsedTime(&h2d_ms, slot->ev_h2d_start, slot->ev_h2d_stop);
    cudaEventElapsedTime(&ll_ms, slot->ev_h2d_stop, slot->ev_ll_stop);
    cudaEventElapsedTime(&vlc_ms, slot->ev_ll_stop, slot->ev_vlc_stop);
    cudaEventElapsedTime(&diff_ms, slot->ev_vlc_stop, slot->ev_diff_stop);
    cudaEventElapsedTime(&inv_ms, slot->ev_inv_start, slot->ev_inv_stop);
    cudaEventElapsedTime(&color_ms, slot->ev_color_start, slot->ev_color_stop);

    timings->host_parse_ms = slot->timings.host_parse_ms;
    timings->task_prep_ms = slot->timings.task_prep_ms;
    timings->queue_overhead_ms = slot->timings.queue_overhead_ms;
    timings->checkpoint_traversal_ms = slot->timings.checkpoint_traversal_ms;
    timings->barrier_wait_ms = slot->timings.barrier_wait_ms;
    timings->index_parallel_wall_ms = slot->timings.index_parallel_wall_ms;
    timings->segment_gather_ms = slot->timings.segment_gather_ms;
    timings->host_index_ms = slot->timings.host_index_ms;
    timings->compressed_h2d_ms = h2d_ms;
    timings->gpu_ll_ms = ll_ms;
    timings->gpu_vlc_ms = vlc_ms;
    timings->gpu_diff_coding_ms = diff_ms;
    timings->gpu_entropy_ms = ll_ms + vlc_ms + diff_ms;
    timings->inverse_wavelet_ms = inv_ms;
    timings->color_convert_ms = color_ms;
    timings->total_gpu_ms = timings->gpu_entropy_ms + inv_ms + color_ms;
    timings->ready_overhead_ms = slot->timings.ready_overhead_ms;
    timings->total_decode_ms = timings->host_parse_ms + timings->task_prep_ms + timings->host_index_ms +
                               h2d_ms + timings->total_gpu_ms;

    timings->largest_band_idx = slot->timings.largest_band_idx;
    timings->largest_band_bytes = slot->timings.largest_band_bytes;
    timings->slowest_task_ms = slot->timings.slowest_task_ms;
    timings->slowest_worker_id = slot->timings.slowest_worker_id;
    timings->active_workers = slot->timings.active_workers;

    return true;
}

bool CudaDecoderPipelinePhase2D::GetBandDecodeResults(uint64_t ticket, std::vector<GpuBandDecodeResult>& results) {
    if (!impl_) return false;
    DecodeSlot2D* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    if (slot->entropy_mode == GpuEntropyMode::SerialReference) {
        cudaMemcpy(slot->host_vlc_results, slot->d_vlc_results, 27 * sizeof(GpuBandDecodeResult), cudaMemcpyDeviceToHost);
        results.assign(slot->host_vlc_results, slot->host_vlc_results + 27);
    } else {
        results.clear();
        for (int b = 0; b < 27; ++b) {
            GpuBandDecodeResult r{};
            r.status = GpuBandDecodeStatus::Complete;
            results.push_back(r);
        }
    }
    return true;
}

bool CudaDecoderPipelinePhase2D::GetSegmentDecodeResults(uint64_t ticket, std::vector<GpuSegmentDecodeResult>& results) {
    if (!impl_) return false;
    DecodeSlot2D* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;

    size_t num_segs = slot->host_segments.size();
    if (num_segs == 0) {
        results.clear();
        return true;
    }

    slot->host_seg_results.resize(num_segs);
    cudaMemcpy(slot->host_seg_results.data(), slot->d_seg_results, num_segs * sizeof(GpuSegmentDecodeResult), cudaMemcpyDeviceToHost);
    results = slot->host_seg_results;
    return true;
}

bool CudaDecoderPipelinePhase2D::GetHostCheckpoints(uint64_t ticket, std::vector<GpuEntropySegmentDescriptor>& out_segments) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2D* slot = impl_->FindSlot(ticket);
    if (!slot || slot->status != DecodeTicketStatus::Complete) return false;
    out_segments = slot->host_segments;
    return true;
}

bool CudaDecoderPipelinePhase2D::DownloadBandCoefficientsToHost(
    uint64_t ticket,
    int band_index,
    std::vector<int16_t>& out_coeffs,
    int* out_w,
    int* out_h,
    int* out_pitch
) {
    if (!impl_) return false;
    DecodeSlot2D* slot = nullptr;
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

} // namespace HyperCine
