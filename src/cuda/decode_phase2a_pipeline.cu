// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2a.h"
#include "hypercine/decode_color_cuda.h"
#include "decode_inverse_kernels.cuh"

#include <cuda_runtime.h>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
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

struct DecodeSlot {
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

    cudaStream_t streams[3] = {nullptr, nullptr, nullptr};

    cudaEvent_t ev_h2d_start = nullptr;
    cudaEvent_t ev_h2d_stop = nullptr;
    cudaEvent_t ev_inv_start = nullptr;
    cudaEvent_t ev_inv_stop = nullptr;
    cudaEvent_t ev_color_start = nullptr;
    cudaEvent_t ev_color_stop = nullptr;
    cudaEvent_t ev_ch_done[3] = {nullptr, nullptr, nullptr};
    cudaEvent_t ev_ready = nullptr;

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

        for (int ch = 0; ch < 3; ++ch) {
            if (streams[ch]) { cudaStreamDestroy(streams[ch]); streams[ch] = nullptr; }
            if (ev_ch_done[ch]) { cudaEventDestroy(ev_ch_done[ch]); ev_ch_done[ch] = nullptr; }
        }
        if (ev_h2d_start) { cudaEventDestroy(ev_h2d_start); ev_h2d_start = nullptr; }
        if (ev_h2d_stop)  { cudaEventDestroy(ev_h2d_stop);  ev_h2d_stop = nullptr; }
        if (ev_inv_start) { cudaEventDestroy(ev_inv_start); ev_inv_start = nullptr; }
        if (ev_inv_stop)  { cudaEventDestroy(ev_inv_stop);  ev_inv_stop = nullptr; }
        if (ev_color_start) { cudaEventDestroy(ev_color_start); ev_color_start = nullptr; }
        if (ev_color_stop)  { cudaEventDestroy(ev_color_stop);  ev_color_stop = nullptr; }
        if (ev_ready) { cudaEventDestroy(ev_ready); ev_ready = nullptr; }

        in_use = false;
        ticket = 0;
        status = DecodeTicketStatus::Invalid;
    }

    ~DecodeSlot() {
        FreeBuffers();
    }
};

} // anonymous namespace

struct CudaDecoderPipelinePhase2A::Impl {
    int max_width = 0;
    int max_height = 0;
    int precision = 10;
    int cuda_device = 0;
    int num_slots = 0;
    bool initialized = false;
    uint64_t next_ticket = 1;
    mutable std::mutex mutex;

    std::vector<std::unique_ptr<DecodeSlot>> slots;

    DecodeSlot* FindSlot(uint64_t ticket) {
        if (ticket == 0) return nullptr;
        for (auto& s : slots) {
            if (s->in_use && s->ticket == ticket) return s.get();
        }
        return nullptr;
    }

    void FreeAll() {
        std::lock_guard<std::mutex> lock(mutex);
        slots.clear();
        initialized = false;
    }
};

CudaDecoderPipelinePhase2A::CudaDecoderPipelinePhase2A() : impl_(std::make_unique<Impl>()) {}
CudaDecoderPipelinePhase2A::~CudaDecoderPipelinePhase2A() = default;
CudaDecoderPipelinePhase2A::CudaDecoderPipelinePhase2A(CudaDecoderPipelinePhase2A&&) noexcept = default;
CudaDecoderPipelinePhase2A& CudaDecoderPipelinePhase2A::operator=(CudaDecoderPipelinePhase2A&&) noexcept = default;

bool CudaDecoderPipelinePhase2A::IsInitialized() const {
    return impl_ && impl_->initialized;
}

bool CudaDecoderPipelinePhase2A::Initialize(int max_width, int max_height, int num_slots, int precision, int cuda_device) {
    if (!impl_ || max_width <= 0 || max_height <= 0 || num_slots <= 0) return false;

    Release();

    std::lock_guard<std::mutex> lock(impl_->mutex);

    impl_->max_width = max_width;
    impl_->max_height = max_height;
    impl_->num_slots = num_slots;
    impl_->precision = precision;
    impl_->cuda_device = cuda_device;

    cudaSetDevice(cuda_device);

    for (int i = 0; i < num_slots; ++i) {
        auto slot = std::make_unique<DecodeSlot>();
        slot->slot_index = i;
        slot->max_width = max_width;
        slot->max_height = max_height;
        slot->precision = precision;
        slot->cuda_device = cuda_device;

        cudaError_t err;
        for (int ch = 0; ch < 3; ++ch) {
            err = cudaStreamCreateWithFlags(&slot->streams[ch], cudaStreamNonBlocking);
            if (err != cudaSuccess) return false;
            err = cudaEventCreateWithFlags(&slot->ev_ch_done[ch], cudaEventDisableTiming);
            if (err != cudaSuccess) return false;
        }

        err = cudaEventCreate(&slot->ev_h2d_start);   if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_h2d_stop);    if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_inv_start);   if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_inv_stop);    if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_color_start); if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_color_stop);  if (err != cudaSuccess) return false;
        err = cudaEventCreate(&slot->ev_ready);       if (err != cudaSuccess) return false;

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

            for (int b = 0; b < 4; ++b) {
                err = cudaMallocPitch(&c.d_l2_band[b], &c.l2_pitch, w_l2 * sizeof(int16_t), h_l2);
                if (err != cudaSuccess) return false;
            }

            for (int b = 1; b < 4; ++b) {
                err = cudaMallocPitch(&c.d_l1_band[b], &c.l1_pitch, w_l1 * sizeof(int16_t), h_l1);
                if (err != cudaSuccess) return false;
                err = cudaMallocPitch(&c.d_l0_band[b], &c.l0_pitch, w_l0 * sizeof(int16_t), h_l0);
                if (err != cudaSuccess) return false;
            }

            err = cudaMallocPitch(&c.d_ll1, &c.ll1_pitch, w_l1 * sizeof(int16_t), h_l1);
            if (err != cudaSuccess) return false;
            c.d_l1_band[0] = c.d_ll1;

            err = cudaMallocPitch(&c.d_ll0, &c.ll0_pitch, w_l0 * sizeof(int16_t), h_l0);
            if (err != cudaSuccess) return false;
            c.d_l0_band[0] = c.d_ll0;

            err = cudaMallocPitch(&c.d_vert_low, &c.vert_low_pitch, w_l0 * sizeof(int16_t), ch_max_h);
            if (err != cudaSuccess) return false;

            err = cudaMallocPitch(&c.d_vert_high, &c.vert_high_pitch, w_l0 * sizeof(int16_t), ch_max_h);
            if (err != cudaSuccess) return false;

            err = cudaMallocPitch(&c.d_final_plane, &c.final_plane_pitch, ch_max_w * sizeof(int16_t), ch_max_h);
            if (err != cudaSuccess) return false;
        }

        // BGRA surface buffer: 4 bytes per pixel
        err = cudaMallocPitch(&slot->d_bgra, &slot->bgra_pitch, max_width * 4, max_height);
        if (err != cudaSuccess) return false;

        impl_->slots.push_back(std::move(slot));
    }

    impl_->initialized = true;
    return true;
}

void CudaDecoderPipelinePhase2A::Release() {
    if (impl_) {
        impl_->FreeAll();
    }
}

uint64_t CudaDecoderPipelinePhase2A::QueueDecode(
    const std::vector<DecodeBandDescriptor>& bands,
    DeviceSurfaceType requested_surface,
    ColorMatrix matrix,
    ColorRange range,
    RowOrientation orientation,
    cudaStream_t caller_stream)
{
    if (!impl_ || !impl_->initialized || bands.empty()) return 0;

    std::lock_guard<std::mutex> lock(impl_->mutex);

    // Find available slot
    DecodeSlot* slot = nullptr;
    for (auto& s : impl_->slots) {
        if (!s->in_use) {
            slot = s.get();
            break;
        }
    }
    if (!slot) {
        // No slots available
        return 0;
    }

    // Determine frame dimensions from bands
    int frame_width = 0, frame_height = 0;
    for (const auto& b : bands) {
        if (b.channel == 0 && b.level == 0 && b.band == 1) {
            frame_width = b.width * 2;
            frame_height = b.height * 2;
            break;
        }
    }
    if (frame_width == 0 || frame_height == 0) {
        for (const auto& b : bands) {
            if (b.channel == 0 && b.level == 2 && b.band == 0) {
                frame_width = b.width * 8;
                frame_height = b.height * 8;
                break;
            }
        }
    }

    if (frame_width <= 0 || frame_height <= 0 ||
        frame_width > impl_->max_width || frame_height > impl_->max_height) {
        return 0;
    }

    uint64_t ticket = impl_->next_ticket++;
    slot->ticket = ticket;
    slot->in_use = true;
    slot->status = DecodeTicketStatus::Pending;
    slot->width = frame_width;
    slot->height = frame_height;
    slot->surface_type = requested_surface;
    slot->matrix = matrix;
    slot->range = range;
    slot->orientation = orientation;

    cudaSetDevice(slot->cuda_device);

    // If caller provided a stream, synchronize slot streams with caller stream
    if (caller_stream != nullptr) {
        cudaEvent_t ev_sync;
        cudaEventCreateWithFlags(&ev_sync, cudaEventDisableTiming);
        cudaEventRecord(ev_sync, caller_stream);
        for (int ch = 0; ch < 3; ++ch) {
            cudaStreamWaitEvent(slot->streams[ch], ev_sync, 0);
        }
        cudaEventDestroy(ev_sync);
    }

    // 1. Host-to-Device Coefficient Upload
    cudaEventRecord(slot->ev_h2d_start, slot->streams[0]);

    for (const auto& b : bands) {
        if (b.channel < 0 || b.channel >= 3) {
            slot->status = DecodeTicketStatus::Failed;
            return 0;
        }
        auto& c = slot->channels[b.channel];
        cudaStream_t s = slot->streams[b.channel];

        int16_t* dst = nullptr;
        size_t dst_pitch = 0;

        if (b.level == 2) {
            if (b.band < 0 || b.band >= 4) { slot->status = DecodeTicketStatus::Failed; return 0; }
            dst = c.d_l2_band[b.band];
            dst_pitch = c.l2_pitch;
        } else if (b.level == 1) {
            if (b.band < 1 || b.band >= 4) { slot->status = DecodeTicketStatus::Failed; return 0; }
            dst = c.d_l1_band[b.band];
            dst_pitch = c.l1_pitch;
        } else if (b.level == 0) {
            if (b.band < 1 || b.band >= 4) { slot->status = DecodeTicketStatus::Failed; return 0; }
            dst = c.d_l0_band[b.band];
            dst_pitch = c.l0_pitch;
        } else {
            slot->status = DecodeTicketStatus::Failed;
            return 0;
        }

        cudaError_t err = cudaMemcpy2DAsync(
            dst, dst_pitch,
            b.host_data, b.pitch,
            b.width * sizeof(int16_t), b.height,
            cudaMemcpyHostToDevice, s
        );
        if (err != cudaSuccess) {
            slot->status = DecodeTicketStatus::Failed;
            return 0;
        }
    }

    // Record H2D completion
    cudaEventRecord(slot->ev_ch_done[1], slot->streams[1]);
    cudaEventRecord(slot->ev_ch_done[2], slot->streams[2]);
    cudaStreamWaitEvent(slot->streams[0], slot->ev_ch_done[1], 0);
    cudaStreamWaitEvent(slot->streams[0], slot->ev_ch_done[2], 0);
    cudaEventRecord(slot->ev_h2d_stop, slot->streams[0]);

    // 2. Inverse Wavelet DAG
    cudaEventRecord(slot->ev_inv_start, slot->streams[0]);

    for (int ch = 0; ch < 3; ++ch) {
        auto& c = slot->channels[ch];
        cudaStream_t s = slot->streams[ch];

        int ch_w = (ch == 0) ? frame_width : (frame_width / 2);
        int ch_h = frame_height;

        int w_l2 = ch_w / 8;
        int h_l2 = ch_h / 8;
        int w_l1 = ch_w / 4;
        int h_l1 = ch_h / 4;
        int w_l0 = ch_w / 2;
        int h_l0 = ch_h / 2;

        dim3 block(16, 16);

        // --- LEVEL 2: w_l2 x h_l2, descale = 0 -> LL1 (w_l1 x h_l1) ---
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

        // --- LEVEL 1: w_l1 x h_l1, descale = 2 -> LL0 (w_l0 x h_l0) ---
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

        // --- LEVEL 0: w_l0 x h_l0, descale = 0 -> Final Planar Channel (ch_w x ch_h) ---
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

    // Stream 0 waits for channels 1 (V) and 2 (U) to complete reconstruction
    cudaStreamWaitEvent(slot->streams[0], slot->ev_ch_done[1], 0);
    cudaStreamWaitEvent(slot->streams[0], slot->ev_ch_done[2], 0);
    cudaEventRecord(slot->ev_inv_stop, slot->streams[0]);

    // 3. Optional CUDA Color Conversion to BGRA8
    if (requested_surface == DeviceSurfaceType::BGRA8) {
        cudaEventRecord(slot->ev_color_start, slot->streams[0]);

        bool color_ok = LaunchYuv422ToBgra8(
            slot->channels[0].d_final_plane, slot->channels[0].final_plane_pitch,
            slot->channels[1].d_final_plane, slot->channels[1].final_plane_pitch,
            slot->channels[2].d_final_plane, slot->channels[2].final_plane_pitch,
            slot->d_bgra, slot->bgra_pitch,
            frame_width, frame_height,
            matrix, range, orientation,
            slot->streams[0]
        );

        if (!color_ok) {
            slot->status = DecodeTicketStatus::Failed;
            return 0;
        }

        cudaEventRecord(slot->ev_color_stop, slot->streams[0]);
    }

    // 4. Record ready event on stream 0 signifying all writes to output surface are finished
    cudaEventRecord(slot->ev_ready, slot->streams[0]);

    return ticket;
}

DecodeTicketStatus CudaDecoderPipelinePhase2A::PollDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return DecodeTicketStatus::Invalid;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot* slot = impl_->FindSlot(ticket);
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

bool CudaDecoderPipelinePhase2A::WaitForDecode(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;

    DecodeSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot) return false;

    if (slot->status == DecodeTicketStatus::Complete) return true;
    if (slot->status == DecodeTicketStatus::Failed) return false;

    cudaError_t err = cudaEventSynchronize(slot->ev_ready);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (err == cudaSuccess) {
        slot->status = DecodeTicketStatus::Complete;
        return true;
    } else {
        slot->status = DecodeTicketStatus::Failed;
        return false;
    }
}

bool CudaDecoderPipelinePhase2A::StreamWait(uint64_t ticket, cudaStream_t caller_stream) {
    if (!impl_ || !impl_->initialized || ticket == 0 || caller_stream == nullptr) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    cudaError_t err = cudaStreamWaitEvent(caller_stream, slot->ev_ready, 0);
    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2A::GetDeviceBgraOutput(uint64_t ticket, DeviceBgraSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot* slot = impl_->FindSlot(ticket);
    if (!slot || slot->surface_type != DeviceSurfaceType::BGRA8) return false;

    if (slot->status != DecodeTicketStatus::Complete) {
        cudaError_t err = cudaEventQuery(slot->ev_ready);
        if (err == cudaSuccess) {
            slot->status = DecodeTicketStatus::Complete;
        } else {
            return false;
        }
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
    out_desc->ticket = ticket;
    out_desc->slot_index = slot->slot_index;

    return true;
}

bool CudaDecoderPipelinePhase2A::GetDevicePlanarOutput(uint64_t ticket, DevicePlanarSurfaceDescriptor* out_desc) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !out_desc) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    if (slot->status != DecodeTicketStatus::Complete) {
        cudaError_t err = cudaEventQuery(slot->ev_ready);
        if (err == cudaSuccess) {
            slot->status = DecodeTicketStatus::Complete;
        } else {
            return false;
        }
    }

    out_desc->d_y = slot->channels[0].d_final_plane;
    out_desc->d_v = slot->channels[1].d_final_plane;
    out_desc->d_u = slot->channels[2].d_final_plane;
    out_desc->y_pitch = slot->channels[0].final_plane_pitch;
    out_desc->v_pitch = slot->channels[1].final_plane_pitch;
    out_desc->u_pitch = slot->channels[2].final_plane_pitch;
    out_desc->width = slot->width;
    out_desc->height = slot->height;
    out_desc->padded_width = slot->max_width;
    out_desc->padded_height = slot->max_height;
    out_desc->is_signed = true;
    out_desc->precision = slot->precision;
    out_desc->matrix = slot->matrix;
    out_desc->range = slot->range;
    out_desc->chroma_siting = ChromaSiting::CoSited;
    out_desc->cuda_device = slot->cuda_device;
    out_desc->ready_event = slot->ev_ready;
    out_desc->ticket = ticket;
    out_desc->slot_index = slot->slot_index;

    return true;
}

bool CudaDecoderPipelinePhase2A::ReleaseOutput(uint64_t ticket) {
    if (!impl_ || !impl_->initialized || ticket == 0) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    slot->in_use = false;
    slot->ticket = 0;
    slot->status = DecodeTicketStatus::Invalid;
    return true;
}

bool CudaDecoderPipelinePhase2A::DownloadBgraToHost(uint64_t ticket, uint8_t* host_bgra, size_t host_pitch) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !host_bgra) return false;

    DecodeSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot || slot->surface_type != DeviceSurfaceType::BGRA8) return false;

    if (!WaitForDecode(ticket)) return false;

    cudaError_t err = cudaMemcpy2D(
        host_bgra, host_pitch,
        slot->d_bgra, slot->bgra_pitch,
        slot->width * 4, slot->height,
        cudaMemcpyDeviceToHost
    );
    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2A::DownloadPlanarToHost(
    uint64_t ticket,
    int16_t* host_y, size_t y_pitch,
    int16_t* host_v, size_t v_pitch,
    int16_t* host_u, size_t u_pitch)
{
    if (!impl_ || !impl_->initialized || ticket == 0 || !host_y || !host_v || !host_u) return false;

    DecodeSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot = impl_->FindSlot(ticket);
    }
    if (!slot) return false;

    if (!WaitForDecode(ticket)) return false;

    cudaError_t err;
    err = cudaMemcpy2D(
        host_y, y_pitch,
        slot->channels[0].d_final_plane, slot->channels[0].final_plane_pitch,
        slot->width * sizeof(int16_t), slot->height,
        cudaMemcpyDeviceToHost
    );
    if (err != cudaSuccess) return false;

    err = cudaMemcpy2D(
        host_v, v_pitch,
        slot->channels[1].d_final_plane, slot->channels[1].final_plane_pitch,
        (slot->width / 2) * sizeof(int16_t), slot->height,
        cudaMemcpyDeviceToHost
    );
    if (err != cudaSuccess) return false;

    err = cudaMemcpy2D(
        host_u, u_pitch,
        slot->channels[2].d_final_plane, slot->channels[2].final_plane_pitch,
        (slot->width / 2) * sizeof(int16_t), slot->height,
        cudaMemcpyDeviceToHost
    );
    return (err == cudaSuccess);
}

bool CudaDecoderPipelinePhase2A::GetStageTimings(uint64_t ticket, SlotStageTimings* timings) {
    if (!impl_ || !impl_->initialized || ticket == 0 || !timings) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    DecodeSlot* slot = impl_->FindSlot(ticket);
    if (!slot) return false;

    if (slot->status != DecodeTicketStatus::Complete) {
        cudaError_t err = cudaEventQuery(slot->ev_ready);
        if (err == cudaSuccess) {
            slot->status = DecodeTicketStatus::Complete;
        } else {
            return false;
        }
    }

    cudaEventElapsedTime(&timings->h2d_ms, slot->ev_h2d_start, slot->ev_h2d_stop);
    cudaEventElapsedTime(&timings->inverse_ms, slot->ev_inv_start, slot->ev_inv_stop);
    if (slot->surface_type == DeviceSurfaceType::BGRA8) {
        cudaEventElapsedTime(&timings->color_ms, slot->ev_color_start, slot->ev_color_stop);
    } else {
        timings->color_ms = 0.0f;
    }
    cudaEventElapsedTime(&timings->total_gpu_ms, slot->ev_h2d_start, slot->ev_ready);

    return true;
}

} // namespace HyperCine
