// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#if defined(HYPERCINE_CUDA_AVAILABLE) || defined(__CUDACC__)
#include <cuda_runtime.h>
#else
typedef struct CUstream_st* cudaStream_t;
typedef struct CUevent_st* cudaEvent_t;
typedef int cudaError_t;
#endif

#include "hypercine/decode_phase2a.h"
#include "hypercine/decode_entropy_cuda.h"

namespace HyperCine {

struct Phase2bSlotStageTimings {
    float host_parse_ms = 0.0f;
    float compressed_h2d_ms = 0.0f;
    float gpu_ll_ms = 0.0f;
    float gpu_vlc_ms = 0.0f;
    float gpu_entropy_ms = 0.0f;
    float inverse_wavelet_ms = 0.0f;
    float color_convert_ms = 0.0f;
    float total_gpu_ms = 0.0f;
};

class CudaDecoderPipelinePhase2B {
public:
    CudaDecoderPipelinePhase2B();
    ~CudaDecoderPipelinePhase2B();

    CudaDecoderPipelinePhase2B(const CudaDecoderPipelinePhase2B&) = delete;
    CudaDecoderPipelinePhase2B& operator=(const CudaDecoderPipelinePhase2B&) = delete;

    CudaDecoderPipelinePhase2B(CudaDecoderPipelinePhase2B&&) noexcept;
    CudaDecoderPipelinePhase2B& operator=(CudaDecoderPipelinePhase2B&&) noexcept;

    // Initialize the pipeline with slot pool for up to max_width x max_height
    bool Initialize(int max_width, int max_height, int num_slots = 3, int precision = 10, int cuda_device = 0);
    void Release();
    bool IsInitialized() const;

    // Asynchronously queue compressed sample decode onto an available slot.
    // Host parse builds immutable band descriptors; compressed sample is copied via CompressedH2D,
    // followed by GPU LL & VLC entropy decode, inverse wavelet, and optional color conversion.
    uint64_t QueueDecode(
        const uint8_t* compressed_sample,
        size_t compressed_size,
        DeviceSurfaceType requested_surface = DeviceSurfaceType::BGRA8,
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
    bool GetStageTimings(uint64_t ticket, Phase2bSlotStageTimings* timings);

    // Query per-band GPU decode results (diagnostics)
    bool GetBandDecodeResults(uint64_t ticket, std::vector<GpuBandDecodeResult>& results);

    // Direct access to decoded signed-16 coefficient bands in VRAM (for bit-exact verification)
    bool DownloadBandCoefficientsToHost(uint64_t ticket, int band_index, std::vector<int16_t>& out_coeffs, int* out_w = nullptr, int* out_h = nullptr, int* out_pitch = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace HyperCine
