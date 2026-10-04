// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#if defined(HYPERCINE_CUDA_AVAILABLE) || defined(__CUDACC__)
#include <cuda_runtime.h>
#else
typedef struct CUstream_st* cudaStream_t;
typedef struct CUevent_st* cudaEvent_t;
typedef int cudaError_t;
#endif
#include "hypercine/decode_inverse_cuda.h"
#include "hypercine/decode_color_cuda.h"

namespace HyperCine {

enum class DeviceSurfaceType : int {
    BGRA8 = 0,
    PlanarYUV422_16S = 1
};

enum class ChromaSiting : int {
    CoSited = 0,
    Centered = 1
};

enum class DecodeTicketStatus : int {
    Invalid = 0,
    Pending = 1,
    Complete = 2,
    Failed = 3
};

struct DevicePlanarSurfaceDescriptor {
    int16_t* d_y = nullptr;
    int16_t* d_u = nullptr;
    int16_t* d_v = nullptr;

    int width = 0;              // Visible width
    int height = 0;             // Visible height
    int padded_width = 0;       // Padded allocated width
    int padded_height = 0;      // Padded allocated height

    size_t y_pitch = 0;         // Line stride in bytes
    size_t u_pitch = 0;
    size_t v_pitch = 0;

    bool is_signed = true;      // Signed 16-bit
    int precision = 10;         // Effective precision (10-bit)

    ColorMatrix matrix = ColorMatrix::BT709;
    ColorRange range = ColorRange::CG;
    ChromaSiting chroma_siting = ChromaSiting::CoSited;

    int cuda_device = 0;
    cudaEvent_t ready_event = nullptr;
    uint64_t ticket = 0;
    int slot_index = -1;
};

struct DeviceBgraSurfaceDescriptor {
    uint8_t* d_bgra = nullptr;

    int width = 0;              // Visible width
    int height = 0;             // Visible height
    int padded_width = 0;
    int padded_height = 0;
    size_t pitch = 0;           // Line stride in bytes

    RowOrientation orientation = RowOrientation::BottomUp;
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorRange range = ColorRange::CG;

    int cuda_device = 0;
    cudaEvent_t ready_event = nullptr;
    uint64_t ticket = 0;
    int slot_index = -1;
};

struct SlotStageTimings {
    float h2d_ms = 0.0f;
    float inverse_ms = 0.0f;
    float color_ms = 0.0f;
    float total_gpu_ms = 0.0f;
};

class CudaDecoderPipelinePhase2A {
public:
    CudaDecoderPipelinePhase2A();
    ~CudaDecoderPipelinePhase2A();

    CudaDecoderPipelinePhase2A(const CudaDecoderPipelinePhase2A&) = delete;
    CudaDecoderPipelinePhase2A& operator=(const CudaDecoderPipelinePhase2A&) = delete;

    CudaDecoderPipelinePhase2A(CudaDecoderPipelinePhase2A&&) noexcept;
    CudaDecoderPipelinePhase2A& operator=(CudaDecoderPipelinePhase2A&&) noexcept;

    // Initialize the pipeline with slot pool for up to max_width x max_height
    bool Initialize(int max_width, int max_height, int num_slots = 3, int precision = 10, int cuda_device = 0);
    void Release();
    bool IsInitialized() const;

    // Asynchronously queue decode work onto an available slot.
    // If caller_stream is non-null, work is synchronized with caller stream via events.
    uint64_t QueueDecode(
        const std::vector<DecodeBandDescriptor>& bands,
        DeviceSurfaceType requested_surface,
        ColorMatrix matrix = ColorMatrix::BT709,
        ColorRange range = ColorRange::CG,
        RowOrientation orientation = RowOrientation::BottomUp,
        cudaStream_t caller_stream = nullptr
    );

    // Poll status of ticket (non-blocking)
    DecodeTicketStatus PollDecode(uint64_t ticket);

    // Block CPU thread until decode ticket is complete
    bool WaitForDecode(uint64_t ticket);

    // Record an event wait on caller_stream so caller can consume device surface without host sync
    bool StreamWait(uint64_t ticket, cudaStream_t caller_stream);

    // Retrieve published DeviceOutput surface descriptors
    bool GetDeviceBgraOutput(uint64_t ticket, DeviceBgraSurfaceDescriptor* out_desc);
    bool GetDevicePlanarOutput(uint64_t ticket, DevicePlanarSurfaceDescriptor* out_desc);

    // Release ticket ownership, returning slot to reusable pool
    bool ReleaseOutput(uint64_t ticket);

    // Host validation helper: copy DeviceOutput BGRA to host buffer
    bool DownloadBgraToHost(uint64_t ticket, uint8_t* host_bgra, size_t host_pitch);

    // Host validation helper: copy DeviceOutput Planar YUV to host buffers
    bool DownloadPlanarToHost(
        uint64_t ticket,
        int16_t* host_y, size_t y_pitch,
        int16_t* host_v, size_t v_pitch,
        int16_t* host_u, size_t u_pitch
    );

    // Query execution timings for a ticket
    bool GetStageTimings(uint64_t ticket, SlotStageTimings* timings);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace HyperCine
