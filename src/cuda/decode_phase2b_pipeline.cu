// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2b.h"
#include "hypercine/decode_color_cuda.h"
#include "decode_inverse_kernels.cuh"

#include <cuda_runtime.h>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <iostream>

namespace HyperCine {

namespace {

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

struct DecodeSlot2B {
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

    std::vector<GpuEntropyBandDescriptor> host_bands;
    GpuBandDecodeResult host_vlc_results[27];

    cudaStream_t main_stream = nullptr;
    cudaStream_t channel_streams[3] = {nullptr, nullptr, nullptr};

    cudaEvent_t ev_h2d_start = nullptr;
    cudaEvent_t ev_h2d_stop = nullptr;
    cudaEvent_t ev_ll_stop = nullptr;
    cudaEvent_t ev_vlc_stop = nullptr;
    cudaEvent_t ev_inv_start = nullptr;
    cudaEvent_t ev_inv_stop = nullptr;
    cudaEvent_t ev_color_start = nullptr;
    cudaEvent_t ev_color_stop = nullptr;
    cudaEvent_t ev_ch_done[3] = {nullptr, nullptr, nullptr};
    cudaEvent_t ev_ready = nullptr;

    Phase2bSlotStageTimings timings{};

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

        if (main_stream) { cudaStreamDestroy(main_stream); main_stream = nullptr; }
        for (int ch = 0; ch < 3; ++ch) {
            if (channel_streams[ch]) { cudaStreamDestroy(channel_streams[ch]); channel_streams[ch] = nullptr; }
            if (ev_ch_done[ch]) { cudaEventDestroy(ev_ch_done[ch]); ev_ch_done[ch] = nullptr; }
        }
        if (ev_h2d_start)   { cudaEventDestroy(ev_h2d_start);   ev_h2d_start = nullptr; }
        if (ev_h2d_stop)    { cudaEventDestroy(ev_h2d_stop);    ev_h2d_stop = nullptr; }
        if (ev_ll_stop)     { cudaEventDestroy(ev_ll_stop);     ev_ll_stop = nullptr; }
        if (ev_vlc_stop)    { cudaEventDestroy(ev_vlc_stop);    ev_vlc_stop = nullptr; }
        if (ev_inv_start)   { cudaEventDestroy(ev_inv_start);   ev_inv_start = nullptr; }
        if (ev_inv_stop)    { cudaEventDestroy(ev_inv_stop);    ev_inv_stop = nullptr; }
        if (ev_color_start) { cudaEventDestroy(ev_color_start); ev_color_start = nullptr; }
        if (ev_color_stop)  { cudaEventDestroy(ev_color_stop);  ev_color_stop = nullptr; }
        if (ev_ready)       { cudaEventDestroy(ev_ready);       ev_ready = nullptr; }

        in_use = false;
        ticket = 0;
        status = DecodeTicketStatus::Invalid;
    }

    ~DecodeSlot2B() {
        FreeBuffers();
    }
};

} // anonymous namespace

struct CudaDecoderPipelinePhase2B::Impl {
    int max_width = 0;
    int max_height = 0;
    int precision = 10;
    int cuda_device = 0;
    int num_slots = 3;
    bool initialized = false;

    GpuFsmTableManager fsm_table_mgr;

    std::vector<std::unique_ptr<DecodeSlot2B>> slots;
    std::mutex mutex;
    uint64_t next_ticket = 1;

    DecodeSlot2B* AcquireSlot() {
        for (auto& s : slots) {
            if (!s->in_use) {
                s->in_use = true;
                s->ticket = next_ticket++;
                s->status = DecodeTicketStatus::Pending;
                return s.get();
            }
        }
        return nullptr;
    }

    DecodeSlot2B* FindSlot(uint64_t ticket) {
        if (ticket == 0) return nullptr;
        for (auto& s : slots) {
            if (s->in_use && s->ticket == ticket) return s.get();
        }
        return nullptr;
    }

    void FreeAll() {
        std::lock_guard<std::mutex> lock(mutex);
        slots.clear();
        fsm_table_mgr.Release();
        initialized = false;
    }
};

CudaDecoderPipelinePhase2B::CudaDecoderPipelinePhase2B() : impl_(std::make_unique<Impl>()) {}
CudaDecoderPipelinePhase2B::~CudaDecoderPipelinePhase2B() = default;
CudaDecoderPipelinePhase2B::CudaDecoderPipelinePhase2B(CudaDecoderPipelinePhase2B&&) noexcept = default;
CudaDecoderPipelinePhase2B& CudaDecoderPipelinePhase2B::operator=(CudaDecoderPipelinePhase2B&&) noexcept = default;

bool CudaDecoderPipelinePhase2B::IsInitialized() const {
    return impl_ && impl_->initialized;
}

bool CudaDecoderPipelinePhase2B::Initialize(int max_width, int max_height, int num_slots, int precision, int cuda_device) {
    if (!impl_ || max_width <= 0 || max_height <= 0 || num_slots <= 0) return false;

    Release();

    std::lock_guard<std::mutex> lock(impl_->mutex);

    impl_->max_width = max_width;
    impl_->max_height = max_height;
    impl_->num_slots = num_slots;
    impl_->precision = precision;
    impl_->cuda_device = cuda_device;

    cudaError_t err = cudaSetDevice(cuda_device);
    if (err != cudaSuccess) return false;

    // Initialize global GPU FSM tables
    if (!impl_->fsm_table_mgr.Initialize(cuda_device)) {
        return false;
    }

    for (int i = 0; i < num_slots; ++i) {
        auto slot = std::make_unique<DecodeSlot2B>();
        slot->slot_index = i;
        slot->max_width = max_width;
        slot->max_height = max_height;
        slot->precision = precision;
        slot->cuda_device = cuda_device;

        err = cudaStreamCreateWithFlags(&slot->main_stream, cudaStreamNonBlocking);
        if (err != cudaSuccess) return false;

        for (int ch = 0; ch < 3; ++ch) {
            err = cudaStreamCreateWithFlags(&slot->channel_streams[ch], cudaStreamNonBlocking);
            if (err != cudaSuccess) return false;
            err = cudaEventCreateWithFlags(&slot->ev_ch_done[ch], cudaEventDisableTiming);
            if (err != cudaSuccess) return false;
        }

        err = cudaEventCreate(&slot->ev_h2d_start);   if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_h2d_stop);    if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_ll_stop);     if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_vlc_stop);    if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_inv_start);   if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_inv_stop);    if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_color_start); if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_color_stop);  if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_ready);       if (err != cudaSuccess) return false;

        // Allocate pinned host and device compressed sample staging
        slot->compressed_capacity = 32 * 1024 * 1024;
        err = cudaMallocHost(&slot->host_pinned_compressed, slot->compressed_capacity);
        if (err != cudaSuccess) return false;

        err = cudaMalloc(&slot->d_compressed_sample, slot->compressed_capacity);
        if (err != cudaSuccess) return false;

        // Allocate descriptor and result arrays
        err = cudaMalloc(&slot->d_ll_descriptors, 3 * sizeof(GpuEntropyBandDescriptor));
        if (err != cudaSuccess) return false;

        err = cudaMalloc(&slot->d_vlc_descriptors, 27 * sizeof(GpuEntropyBandDescriptor));
        if (err != cudaSuccess) return false;

        err = cudaMalloc(&slot->d_vlc_results, 27 * sizeof(GpuBandDecodeResult));
        if (err != cudaSuccess) return false;

        // Allocate channel band and intermediate buffers
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

            // Scratch buffers for vertical pass (max dimensions: w_l0 x (2 * h_l0))
            err = cudaMallocPitch(&c.d_vert_low, &c.vert_low_pitch, w_l0 * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;
            err = cudaMallocPitch(&c.d_vert_high, &c.vert_high_pitch, w_l0 * sizeof(int16_t), 2 * h_l0);
            if (err != cudaSuccess) return false;

            // Final reconstructed plane
            err = cudaMallocPitch(&c.d_final_plane, &c.final_plane_pitch, ch_max_w * sizeof(int16_t), ch_max_h);
            if (err != cudaSuccess) return false;
        }

        // Allocate BGRA surface
        err = cudaMallocPitch(&slot->d_bgra, &slot->bgra_pitch, max_width * 4, max_height);
        if (err != cudaSuccess) return false;

        impl_->slots.push_back(std::move(slot));
    }

    impl_->initialized = true;
    return true;
}

void CudaDecoderPipelinePhase2B::Release() {
    if (impl_) {
        impl_->FreeAll();
    }
}

uint64_t CudaDecoderPipelinePhase2B::QueueDecode(
    const uint8_t* compressed_sample,
    size_t compressed_size,
    DeviceSurfaceType requested_surface,
    ColorMatrix matrix,
    ColorRange range,
    RowOrientation orientation,
    cudaStream_t caller_stream
) {
    if (!impl_ || !impl_->initialized || !compressed_sample || compressed_size == 0) return 0;

    auto parse_start = std::chrono::high_resolution_clock::now();

    int ch_offset = (requested_surface == DeviceSurfaceType::BGRA8) ? 24 : 4;
    std::vector<GpuEntropyBandDescriptor> parsed_bands;
    std::string parse_err;

    if (!ParseCineFormSample(compressed_sample, compressed_size, ch_offset, parsed_bands, &parse_err)) {
        std::cerr << "Phase 2B Parse Error: " << parse_err << std::endl;
        return 0;
    }

    if (parsed_bands.size() != 30) {
        return 0;
    }

    auto parse_end = std::chrono::high_resolution_clock::now();
    float host_parse_ms = std::chrono::duration<float, std::milli>(parse_end - parse_start).count();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2B* slot = impl_->AcquireSlot();
    if (!slot) return 0;

    uint64_t ticket = slot->ticket;
    slot->surface_type = requested_surface;
    slot->matrix = matrix;
    slot->range = range;
    slot->orientation = orientation;
    slot->host_bands = parsed_bands;
    slot->timings.host_parse_ms = host_parse_ms;

    int frame_width = parsed_bands[7].width * 2;
    int frame_height = parsed_bands[7].height * 2;
    slot->width = frame_width;
    slot->height = frame_height;

    // 1. Assign GPU VRAM destination pointers for all 30 bands
    std::vector<GpuEntropyBandDescriptor> ll_descs;
    std::vector<GpuEntropyBandDescriptor> vlc_descs;
    ll_descs.reserve(3);
    vlc_descs.reserve(27);

    for (size_t i = 0; i < parsed_bands.size(); ++i) {
        auto& b = parsed_bands[i];
        int ch = b.channel;
        auto& c = slot->channels[ch];

        if (b.subband == 0) {
            b.d_destination = c.d_l2_band[0];
            ll_descs.push_back(b);
        } else {
            if (b.level == 2) {
                b.d_destination = c.d_l2_band[b.band];
            } else if (b.level == 1) {
                b.d_destination = c.d_l1_band[b.band];
            } else {
                b.d_destination = c.d_l0_band[b.band];
            }
            vlc_descs.push_back(b);
        }
    }

    slot->host_bands = parsed_bands;

    // Check compressed buffer capacity
    if (compressed_size > slot->compressed_capacity) {
        slot->status = DecodeTicketStatus::Failed;
        return 0;
    }

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

    // Upload band descriptors
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

    cudaEventRecord(slot->ev_h2d_stop, slot->main_stream);

    // 3. GPU Entropy Decode
    // Launch LL decode kernel
    LaunchDecodeLowpassGpu(
        slot->d_compressed_sample,
        compressed_size,
        slot->d_ll_descriptors,
        static_cast<int>(ll_descs.size()),
        slot->main_stream
    );
    cudaEventRecord(slot->ev_ll_stop, slot->main_stream);

    // Launch Highpass VLC decode kernel
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

    // 4. CUDA Inverse Wavelet Transform
    cudaEventRecord(slot->ev_inv_start, slot->main_stream);

    for (int ch = 0; ch < 3; ++ch) {
        auto& c = slot->channels[ch];
        cudaStream_t s = slot->channel_streams[ch];

        // Channel stream waits for VLC decode completion
        cudaStreamWaitEvent(s, slot->ev_vlc_stop, 0);

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

        // LEVEL 0 -> Final reconstructed plane
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

    // Main stream waits for channels to finish reconstruction
    cudaStreamWaitEvent(slot->main_stream, slot->ev_ch_done[0], 0);
    cudaStreamWaitEvent(slot->main_stream, slot->ev_ch_done[1], 0);
    cudaStreamWaitEvent(slot->main_stream, slot->ev_ch_done[2], 0);
    cudaEventRecord(slot->ev_inv_stop, slot->main_stream);

    // 5. CUDA Color Conversion
    if (requested_surface == DeviceSurfaceType::BGRA8) {
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
            slot->status = DecodeTicketStatus::Failed;
            return 0;
        }

        cudaEventRecord(slot->ev_color_stop, slot->main_stream);
    }

    // 6. Record ready event
    cudaEventRecord(slot->ev_ready, slot->main_stream);

    if (caller_stream) {
        cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    }

    return ticket;
}

DecodeTicketStatus CudaDecoderPipelinePhase2B::PollDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return DecodeTicketStatus::Invalid;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2B* slot = impl_->FindSlot(ticket);
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

bool CudaDecoderPipelinePhase2B::WaitForDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;

    DecodeSlot2B* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
        if (!slot) return false;
    }

    cudaError_t err = cudaEventSynchronize(slot->ev_ready);
    if (err == cudaSuccess) {
        slot->status = DecodeTicketStatus::Complete;
        return true;
    } else {
        slot->status = DecodeTicketStatus::Failed;
        return false;
    }
}

bool CudaDecoderPipelinePhase2B::StreamWait(uint64_t ticket, cudaStream_t caller_stream) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !caller_stream) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2B* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    cudaError_t err = cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2B::GetDeviceBgraOutput(uint64_t ticket, DeviceBgraSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2B* slot = impl_->FindSlot(ticket);
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

bool CudaDecoderPipelinePhase2B::GetDevicePlanarOutput(uint64_t ticket, DevicePlanarSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2B* slot = impl_->FindSlot(ticket);
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

bool CudaDecoderPipelinePhase2B::ReleaseOutput(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot2B* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    slot->in_use = false;
    slot->ticket = 0;
    slot->status = DecodeTicketStatus::Invalid;
    return true;
}

bool CudaDecoderPipelinePhase2B::DownloadBgraToHost(uint64_t ticket, uint8_t* host_bgra, size_t host_pitch) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !host_bgra) return false;

    DecodeSlot2B* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
        if (!slot || slot->surface_type != DeviceSurfaceType::BGRA8) return false;
    }

    if (!WaitForDecode(ticket)) return false;

    cudaError_t err = cudaMemcpy2D(
        host_bgra, host_pitch,
        slot->d_bgra, slot->bgra_pitch,
        slot->width * 4, slot->height,
        cudaMemcpyDeviceToHost
    );

    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2B::DownloadPlanarToHost(
    uint64_t ticket,
    int16_t* host_y, size_t y_pitch,
    int16_t* host_v, size_t v_pitch,
    int16_t* host_u, size_t u_pitch
) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;

    DecodeSlot2B* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
        if (!slot || slot->surface_type != DeviceSurfaceType::PlanarYUV422_16S) return false;
    }

    if (!WaitForDecode(ticket)) return false;

    cudaError_t err;
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

bool CudaDecoderPipelinePhase2B::GetStageTimings(uint64_t ticket, Phase2bSlotStageTimings* timings) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !timings) return false;

    DecodeSlot2B* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
        if (!slot) return false;
    }

    if (!WaitForDecode(ticket)) return false;

    timings->host_parse_ms = slot->timings.host_parse_ms;

    float h2d_ms = 0.0f;
    float ll_ms = 0.0f;
    float vlc_ms = 0.0f;
    float inv_ms = 0.0f;
    float col_ms = 0.0f;
    float tot_ms = 0.0f;

    cudaEventElapsedTime(&h2d_ms, slot->ev_h2d_start, slot->ev_h2d_stop);
    cudaEventElapsedTime(&ll_ms, slot->ev_h2d_stop, slot->ev_ll_stop);
    cudaEventElapsedTime(&vlc_ms, slot->ev_ll_stop, slot->ev_vlc_stop);
    cudaEventElapsedTime(&inv_ms, slot->ev_inv_start, slot->ev_inv_stop);

    if (slot->surface_type == DeviceSurfaceType::BGRA8) {
        cudaEventElapsedTime(&col_ms, slot->ev_color_start, slot->ev_color_stop);
    }
    cudaEventElapsedTime(&tot_ms, slot->ev_h2d_start, slot->ev_ready);

    timings->compressed_h2d_ms = h2d_ms;
    timings->gpu_ll_ms = ll_ms;
    timings->gpu_vlc_ms = vlc_ms;
    timings->gpu_entropy_ms = ll_ms + vlc_ms;
    timings->inverse_wavelet_ms = inv_ms;
    timings->color_convert_ms = col_ms;
    timings->total_gpu_ms = tot_ms;

    return true;
}

bool CudaDecoderPipelinePhase2B::GetBandDecodeResults(uint64_t ticket, std::vector<GpuBandDecodeResult>& results) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;

    DecodeSlot2B* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
        if (!slot) return false;
    }

    if (!WaitForDecode(ticket)) return false;

    results.resize(27);
    cudaError_t err = cudaMemcpy(
        results.data(),
        slot->d_vlc_results,
        27 * sizeof(GpuBandDecodeResult),
        cudaMemcpyDeviceToHost
    );

    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2B::DownloadBandCoefficientsToHost(
    uint64_t ticket,
    int band_index,
    std::vector<int16_t>& out_coeffs,
    int* out_w,
    int* out_h,
    int* out_pitch
) {
    if (!impl_ || !impl_->initialized || ticket == 0 || band_index < 0 || band_index >= 30) return false;

    DecodeSlot2B* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
        if (!slot) return false;
    }

    if (!WaitForDecode(ticket)) return false;

    const auto& desc = slot->host_bands[band_index];
    int w = desc.width;
    int h = desc.height;
    size_t pitch_bytes = desc.pitch_bytes;
    size_t total_samples = desc.coefficient_count;

    out_coeffs.resize(total_samples, 0);

    cudaError_t err = cudaMemcpy(
        out_coeffs.data(),
        desc.d_destination,
        pitch_bytes * h,
        cudaMemcpyDeviceToHost
    );

    if (err != cudaSuccess) return false;

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_pitch) *out_pitch = static_cast<int>(pitch_bytes);

    return true;
}

} // namespace HyperCine
