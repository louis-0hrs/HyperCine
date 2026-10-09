// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
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

// Host checkpoint indexing execution mode (Section 2)
enum class HostIndexExecutionMode : uint32_t {
    SingleThreadReference = 0, // Phase 2C single-threaded indexing reference
    ParallelWorkerPool = 1     // Phase 2D parallel host checkpoint indexing (default)
};

// Host checkpoint scheduling strategy (Section 7 & 8)
enum class HostIndexSchedulingPolicy : uint32_t {
    DescriptorOrder = 0,       // FIFO / original subband descriptor order (0..26)
    LargestFirst = 1           // Descending compressed payload bytes (LPT)
};

// Detailed stage-by-stage timings for Phase 2D (Section 14 & 24)
struct Phase2dSlotStageTimings {
    float host_parse_ms = 0.0f;
    float task_prep_ms = 0.0f;            // Task list build + optional largest-first sort
    float queue_overhead_ms = 0.0f;       // Measured: batch submit -> first task start (wake/dispatch latency)
    float checkpoint_traversal_ms = 0.0f; // Sum of per-band CPU traversal time across all workers
    float barrier_wait_ms = 0.0f;         // Measured: last task end -> ExecuteBatch return (join latency)
    float index_parallel_wall_ms = 0.0f;  // Wall time of the parallel batch (submit -> return)
    float segment_gather_ms = 0.0f;       // Canonical-order gather of per-band checkpoints
    float host_index_ms = 0.0f;           // Wall-clock host indexing time (prep excluded; batch + gather)
    float compressed_h2d_ms = 0.0f;
    float gpu_ll_ms = 0.0f;
    float gpu_vlc_ms = 0.0f;            // Parallel replay time (Pass A2)
    float gpu_diff_coding_ms = 0.0f;    // Cooperative difference coding time (Rule 10)
    float gpu_entropy_ms = 0.0f;        // gpu_ll_ms + gpu_vlc_ms + gpu_diff_coding_ms
    float inverse_wavelet_ms = 0.0f;
    float color_convert_ms = 0.0f;
    float total_gpu_ms = 0.0f;
    float ready_overhead_ms = 0.0f;       // Host wait latency for ev_ready
    float total_decode_ms = 0.0f;       // Sum of stages: parse + prep + index + H2D + GPU (benchmarks measure true wall time externally)

    // Critical-path analysis metrics (Section 24)
    int largest_band_idx = -1;
    uint32_t largest_band_bytes = 0;
    float slowest_task_ms = 0.0f;
    int slowest_worker_id = -1;
    int active_workers = 0;
};

// Persistent bounded worker pool for host-parallel indexing (Section 5 & 6)
class DecoderWorkerPool {
public:
    DecoderWorkerPool();
    ~DecoderWorkerPool();

    DecoderWorkerPool(const DecoderWorkerPool&) = delete;
    DecoderWorkerPool& operator=(const DecoderWorkerPool&) = delete;

    DecoderWorkerPool(DecoderWorkerPool&&) noexcept;
    DecoderWorkerPool& operator=(DecoderWorkerPool&&) noexcept;

    // Initialize or resize persistent worker pool with num_workers threads.
    // If num_workers <= 0, defaults to min(std::thread::hardware_concurrency(), 16).
    bool Initialize(int num_workers = 0);
    void Shutdown();
    bool IsRunning() const;
    int WorkerCount() const;

    // Dispatches a batch of tasks to the worker pool and blocks caller until all tasks complete
    // or abort_flag is asserted.
    void ExecuteBatch(
        int num_tasks,
        const std::function<void(int task_idx, int worker_id)>& task_fn,
        std::atomic<bool>* abort_flag = nullptr
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Phase 2D Accelerated CUDA Decoder Pipeline with Parallel Host Checkpoint Indexing
class CudaDecoderPipelinePhase2D {
public:
    CudaDecoderPipelinePhase2D();
    ~CudaDecoderPipelinePhase2D();

    CudaDecoderPipelinePhase2D(const CudaDecoderPipelinePhase2D&) = delete;
    CudaDecoderPipelinePhase2D& operator=(const CudaDecoderPipelinePhase2D&) = delete;

    CudaDecoderPipelinePhase2D(CudaDecoderPipelinePhase2D&&) noexcept;
    CudaDecoderPipelinePhase2D& operator=(CudaDecoderPipelinePhase2D&&) noexcept;

    // Initialize the pipeline with slot pool and persistent worker pool
    // Default worker count: 0 (auto-detect based on hardware, clamped to 16)
    bool Initialize(
        int max_width,
        int max_height,
        int num_slots = 3,
        int precision = 10,
        int num_host_workers = 0,
        int cuda_device = 0
    );
    void Release();
    bool IsInitialized() const;

    // Configure host checkpoint indexing execution mode (SingleThreadReference vs ParallelWorkerPool)
    void SetHostIndexMode(HostIndexExecutionMode mode);
    HostIndexExecutionMode GetHostIndexMode() const;

    // Configure task scheduling policy (DescriptorOrder vs LargestFirst)
    void SetSchedulingPolicy(HostIndexSchedulingPolicy policy);
    HostIndexSchedulingPolicy GetSchedulingPolicy() const;

    // Configure worker thread count (e.g. 1, 2, 4, 6, 8, 12, 16)
    bool SetWorkerCount(int num_workers);
    int GetWorkerCount() const;

    // Configure GPU entropy mode (ParallelAdaptive vs ParallelConfigurable)
    void SetEntropyMode(GpuEntropyMode mode, int fixed_segments_per_band = 32);
    GpuEntropyMode GetEntropyMode() const;
    int GetFixedSegmentsPerBand() const;

    // Asynchronously queue compressed sample decode onto an available slot.
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
    bool GetStageTimings(uint64_t ticket, Phase2dSlotStageTimings* timings);

    // Query per-band GPU decode results (diagnostics)
    bool GetBandDecodeResults(uint64_t ticket, std::vector<GpuBandDecodeResult>& results);

    // Query per-segment GPU decode results (diagnostics)
    bool GetSegmentDecodeResults(uint64_t ticket, std::vector<GpuSegmentDecodeResult>& results);

    // Diagnostics: canonical-order (band 0..26) host checkpoint records used for GPU replay
    bool GetHostCheckpoints(uint64_t ticket, std::vector<GpuEntropySegmentDescriptor>& out_segments);

    // Direct access to decoded signed-16 coefficient bands in VRAM (for bit-exact verification)
    bool DownloadBandCoefficientsToHost(
        uint64_t ticket,
        int band_index,
        std::vector<int16_t>& out_coeffs,
        int* out_w = nullptr,
        int* out_h = nullptr,
        int* out_pitch = nullptr
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace HyperCine
