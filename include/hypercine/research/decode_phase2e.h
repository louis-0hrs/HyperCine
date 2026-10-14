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
#include "hypercine/decode_phase2d.h"
#include "hypercine/decode_gpu_index.h"
#include "hypercine/decoder.h"

namespace HyperCine {

struct FieldPlusSampleDescriptor;
struct DecoderReconstruction;

// Phase 2E Pipeline Overlap Execution Mode (Section 2, 24, 25, 26)
enum class Phase2EPipelineMode : uint32_t {
    Phase2DFrameBarrier = 0,   // Existing Phase 2D barrier baseline (parallel index -> barrier -> GPU replay)
    Phase2EEntropyOverlap = 1, // Variant A: Host index + GPU replay overlap, inverse barrier retained
    Phase2EFullOverlap = 2,    // Variant B: Host index + GPU replay + inverse DAG overlap
    Phase2FAGpuIndexBatch = 3, // Complete-band GPU checkpoint scan -> device replay -> inverse DAG
    Phase2FCHybridFullFrame = 4, // Full-frame adaptive hybrid entropy (Phase 2F-C)
    Phase2DBarrier = Phase2DFrameBarrier,
    Phase2EHostStreaming = Phase2EFullOverlap,
    Phase2FAGpuIndex = Phase2FAGpuIndexBatch,
    Phase2FCHybrid = Phase2FCHybridFullFrame
};

// Phase 2F-C Selector Policies (Section 7, 8, 10, 11, 35)
enum class Phase2FCSelectorPolicy : uint32_t {
    StaticHeuristic = 0,    // Deterministic rule: payload size + density
    AdaptiveDynamic = 1,    // Static selection + runtime early convergence check & fallback
    ForceHybridBF = 2,      // Force Method B/F on all eligible highpass bands
    ForcePhase2E = 3,       // Force Method E on all bands (Phase 2E baseline)
    OraclePerBand = 4       // Offline oracle: chooses fastest measured method per band
};

// Method selected per highpass band (Section 7)
enum class BandEntropyMethod : uint32_t {
    MethodE_HostIndex = 0,  // Existing Phase 2E host checkpoint scan + GPU replay
    MethodBF_Hybrid = 1,    // Phase 2F-B GPU summaries + CPU exact correction + GPU replay
    MethodTrivial = 2       // Small-band / trivial single segment
};

// Phase 2F-C per-band diagnostic and exactness record (Section 9, 27)
struct Phase2FCBandRecord {
    int band_index = -1;
    int channel = -1;
    int level = -1;
    int orientation = -1;
    uint32_t payload_bytes = 0;
    uint32_t coefficient_count = 0;
    BandEntropyMethod chosen_method = BandEntropyMethod::MethodE_HostIndex;
    uint32_t chunk_size = 256;
    uint32_t chunk_count = 0;
    uint32_t checkpoint_count = 0;
    uint64_t coefficient_hash = 0;
    float correction_ratio = 0.0f;
    uint32_t corrected_nibbles = 0;
    uint32_t max_correction_nibbles = 0;
    uint32_t full_chunk_corrections = 0;
    float gpu_summary_ms = 0.0f;
    float cpu_resolve_ms = 0.0f;
    float gpu_replay_ms = 0.0f;
    size_t scratch_bytes = 0;
    uint32_t final_error = 0;
    bool fell_back = false;
};

// Phase 2E Scheduling Policies (Section 12, 13)
enum class Phase2ESchedulingPolicy : uint32_t {
    PolicyA_LargestFirst = 0,       // Descending compressed payload bytes (LPT)
    PolicyB_InverseCritical = 1,    // Deepest level first: Level 2 -> Level 1 -> Level 0 (prioritizing Y)
    PolicyC_Hybrid = 2,             // Depth-weighted payload: payload_bytes * (level + 1) descending
    DescriptorOrder = 3             // FIFO / original subband descriptor order (0..26)
};

// Detailed per-band timeline measurement record (Section 21 & 23)
struct BandTimelineRecord {
    int band_index = -1;
    int channel = -1;
    int level = -1;
    int band = -1;
    uint32_t payload_bytes = 0;
    int segment_count = 0;
    float host_index_start_ms = 0.0f;
    float host_index_end_ms = 0.0f;
    float host_index_duration_ms = 0.0f;
    int host_worker_id = -1;
    float gpu_index_start_ms = 0.0f, gpu_index_end_ms = 0.0f;
    float gpu_index_duration_ms = 0.0f;
    float gpu_index_work_ms = 0.0f, max_index_kernel_ms = 0.0f;
    float gpu_replay_start_ms = 0.0f;
    float gpu_replay_end_ms = 0.0f;
    float gpu_replay_duration_ms = 0.0f;
    int gpu_stream_id = -1;
};

// Detailed per-channel/level inverse wavelet timeline record (Section 21 & 23)
struct InverseTimelineRecord {
    int channel = -1; // 0: Y, 1: V, 2: U
    int level = -1;   // 2 (deepest), 1, 0 (full)
    float start_ms = 0.0f;
    float end_ms = 0.0f;
    float duration_ms = 0.0f;
};

// Detailed stage-by-stage timings & critical-path analysis for Phase 2E (Section 21, 22, 28, 29)
struct Phase2eSlotStageTimings {
    float host_parse_ms = 0.0f;
    float task_prep_ms = 0.0f;
    float compressed_h2d_ms = 0.0f;
    float gpu_ll_ms = 0.0f;

    // Host indexing metrics
    float host_index_wall_ms = 0.0f;        // Earliest index start to latest index end
    float host_index_cpu_work_ms = 0.0f;    // Sum of per-band host index durations
    float queue_overhead_ms = 0.0f;         // Batch submit -> first task start
    float barrier_wait_ms = 0.0f;           // Join latency (if barrier mode)

    float gpu_index_wall_ms = 0.0f, gpu_index_work_ms = 0.0f;
    float max_index_kernel_ms = 0.0f;
    float host_schedule_wall_ms = 0.0f;
    float host_index_thread_cpu_ms = 0.0f;
    float host_schedule_cpu_ms = 0.0f;
    int peak_index_workers = 0;
    // GPU VLC replay metrics
    float gpu_replay_wall_ms = 0.0f;        // Earliest GPU replay start to latest replay end
    float gpu_replay_work_ms = 0.0f;        // Sum of per-band GPU replay durations
    float gpu_diff_coding_ms = 0.0f;        // Sum of difference coding durations

    // Inverse wavelet metrics
    float inverse_wavelet_wall_ms = 0.0f;   // Earliest inverse start to latest inverse end
    float inverse_wavelet_work_ms = 0.0f;   // Sum of inverse level durations
    // GOP-1 field combine, summed across channels. Nested inside level 0, so it
    // is not added again to total_work_sum_ms. Zero on a progressive frame.
    float field_combine_ms = 0.0f;

    // Final color & completion metrics
    float color_convert_ms = 0.0f;
    float ready_overhead_ms = 0.0f;         // Host wait latency for ev_ready
    float total_critical_path_ms = 0.0f;    // Frame start to ev_ready completion (true wall latency)
    float total_work_sum_ms = 0.0f;         // Sum of all individual stage durations

    // Timeline event markers relative to frame start (ms)
    float t_frame_start_ms = 0.0f;
    float t_parse_end_ms = 0.0f;
    float t_h2d_start_ms = 0.0f;
    float t_h2d_end_ms = 0.0f;
    float t_first_index_start_ms = 0.0f;
    float t_first_gpu_launch_ms = 0.0f;
    float t_final_index_end_ms = 0.0f;
    float t_first_inverse_start_ms = 0.0f;
    float t_final_replay_end_ms = 0.0f;
    float t_color_start_ms = 0.0f;
    float t_ready_ms = 0.0f;

    // Overlap metrics (Section 29)
    float overlap_saved_ms = 0.0f;          // serialized_work - actual_combined_wall
    float overlap_efficiency = 0.0f;        // overlap_saved / min(index_wall, replay_wall)

    // Diagnostics & details
    int active_workers = 0;
    int active_streams = 0;
    int largest_band_idx = -1;
    uint32_t largest_band_bytes = 0;
    float slowest_task_ms = 0.0f;
    int slowest_worker_id = -1;

    std::vector<BandTimelineRecord> band_records;
    std::vector<InverseTimelineRecord> inverse_records;

    // Phase 2F-C Hybrid metrics (Section 9, 17, 27, 40)
    float fc_selector_ms = 0.0f;
    float fc_summary_wall_ms = 0.0f;
    float fc_summary_work_ms = 0.0f;
    float fc_cpu_resolve_ms = 0.0f;
    int fc_bf_band_count = 0;
    int fc_phase2e_band_count = 0;
    int fc_trivial_band_count = 0;
    int fc_fallback_band_count = 0;
    size_t fc_peak_scratch_bytes = 0;
    std::vector<Phase2FCBandRecord> fc_band_records;
};

// Phase 2E Accelerated CUDA Decoder Pipeline with Asynchronous Band Streaming & Overlap
class CudaDecoderPipelinePhase2E {
public:
    CudaDecoderPipelinePhase2E();
    ~CudaDecoderPipelinePhase2E();

    CudaDecoderPipelinePhase2E(const CudaDecoderPipelinePhase2E&) = delete;
    CudaDecoderPipelinePhase2E& operator=(const CudaDecoderPipelinePhase2E&) = delete;

    CudaDecoderPipelinePhase2E(CudaDecoderPipelinePhase2E&&) noexcept;
    CudaDecoderPipelinePhase2E& operator=(CudaDecoderPipelinePhase2E&&) noexcept;

    // Initialize pipeline with slot pool, persistent worker pool, and GPU replay stream pool.
    // Default worker count: 0 (auto-detect based on hardware, clamped to 16).
    // Default replay streams: 4.
    bool Initialize(
        int max_width,
        int max_height,
        int num_slots = 3,
        int precision = 10,
        int num_host_workers = 0,
        int num_replay_streams = 4,
        int cuda_device = 0
    );
    void Release();
    bool IsInitialized() const;

    // Configure pipeline overlap mode (Phase2DFrameBarrier, Phase2EEntropyOverlap, Phase2EFullOverlap)
    void SetPipelineMode(Phase2EPipelineMode mode);
    Phase2EPipelineMode GetPipelineMode() const;

    // Configure task scheduling policy (PolicyA_LargestFirst, PolicyB_InverseCritical, PolicyC_Hybrid, DescriptorOrder)
    void SetSchedulingPolicy(Phase2ESchedulingPolicy policy);
    Phase2ESchedulingPolicy GetSchedulingPolicy() const;

    // Configure worker thread count (e.g. 1, 2, 4, 8, 16)
    bool SetWorkerCount(int num_workers);
    int GetWorkerCount() const;

    // Configure GPU replay stream pool size (e.g. 1, 2, 4, 8)
    bool SetReplayStreamCount(int count);
    int GetReplayStreamCount() const;

    // Configure GPU entropy mode (ParallelAdaptive vs ParallelConfigurable)
    void SetEntropyMode(GpuEntropyMode mode, int fixed_segments_per_band = 32);
    GpuEntropyMode GetEntropyMode() const;
    int GetFixedSegmentsPerBand() const;

    // Synthetic race testing hook (Section 36)
    using DelayHookFn = std::function<void(int band_idx, int channel, int level, int band)>;
    void SetTestDelayHook(DelayHookFn hook);

    // Asynchronously queue compressed sample decode onto an available slot
    uint64_t QueueDecode(
        const uint8_t* compressed_sample,
        size_t compressed_size,
        DeviceSurfaceType requested_surface = DeviceSurfaceType::BGRA8,
        ColorMatrix matrix = ColorMatrix::BT709,
        ColorRange range = ColorRange::CG,
        RowOrientation orientation = RowOrientation::BottomUp,
        cudaStream_t caller_stream = nullptr
    );

    // Internal production adapter: descriptors have already passed strict validation.
    uint64_t QueueDecodeParsed(const uint8_t*, size_t,
        const std::vector<GpuEntropyBandDescriptor>&,
        DeviceSurfaceType, ColorMatrix, ColorRange, RowOrientation,
        bool production_guard, float host_parse_ms = 0, bool interlaced_gop1 = false);
    uint64_t QueueDecodeFieldPlusParsed(const uint8_t*, size_t,
        const FieldPlusSampleDescriptor&, int frame_index,
        DeviceSurfaceType, ColorMatrix, ColorRange, RowOrientation,
        bool production_guard = true);
    DecoderMemoryStats GetMemoryStats() const;
    void PrepareProductionScratch();

    // Poll status of ticket (non-blocking)
    DecodeTicketStatus PollDecode(uint64_t ticket);

    // Block CPU thread until decode ticket is complete
    bool WaitForDecode(uint64_t ticket);

    // Record an event wait on caller_stream so caller can consume device surface without host sync
    bool StreamWait(uint64_t ticket, cudaStream_t caller_stream);

    // Retrieve published DeviceOutput surface descriptors
    bool GetDeviceBgraOutput(uint64_t ticket, DeviceBgraSurfaceDescriptor* out_desc);
    bool GetDevicePlanarOutput(uint64_t ticket, DevicePlanarSurfaceDescriptor* out_desc);
    bool InspectReconstruction(uint64_t ticket, DecoderReconstruction* out);

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
    bool GetStageTimings(uint64_t ticket, Phase2eSlotStageTimings* timings);

    // Query per-band GPU decode results (diagnostics)
    bool GetBandDecodeResults(uint64_t ticket, std::vector<GpuBandDecodeResult>& results);

    // Query per-segment GPU decode results (diagnostics)
    bool GetSegmentDecodeResults(uint64_t ticket, std::vector<GpuSegmentDecodeResult>& results);

    // Diagnostics: canonical-order (band 0..26) host checkpoint records used for GPU replay
    bool GetHostCheckpoints(uint64_t ticket, std::vector<GpuEntropySegmentDescriptor>& out_segments);

    // Diagnostics only: D2H after completion, never part of replay dispatch.
    bool GetGpuCheckpoints(uint64_t ticket, std::vector<GpuEntropySegmentDescriptor>& out);
    bool GetGpuIndexResults(uint64_t ticket, std::vector<GpuCheckpointIndexResult>& out);
    bool DownloadIntermediateLl(uint64_t ticket, int channel, int level, std::vector<int16_t>& out);
    // FIELDPLUS stage oracle hook: tight signed-16 temporal low/high planes and
    // the selected output frame's base LL, after the GOP temporal inverse.
    bool DownloadFieldPlusTemporalStages(uint64_t ticket, int channel,
        std::vector<int16_t>& temporal_low, std::vector<int16_t>& temporal_high,
        std::vector<int16_t>& wavelet4_ll, std::vector<int16_t>& frame_base_ll,
        int* width = nullptr, int* height = nullptr);
    bool DownloadFieldPlusSpatialStages(uint64_t ticket, int channel,
        std::vector<int16_t>& vertical_low, std::vector<int16_t>& vertical_high,
        int* width = nullptr, int* height = nullptr);
    // Test/research only. Tight host rows of the temporal low/high pair (the
    // horizontal inverse immediately before the field kernel) and the
    // reconstructed plane. Wavelet channel 0=Y, 1=V, 2=U. Device pitches are
    // returned in bytes. Fails unless this ticket took the GOP-1 path.
    bool DownloadInterlacedFieldStage(uint64_t ticket, int channel,
        std::vector<int16_t>& temporal_low, std::vector<int16_t>& temporal_high,
        std::vector<int16_t>& reconstructed, int* width = nullptr, int* field_rows = nullptr,
        int* low_pitch = nullptr, int* out_pitch = nullptr);
    // Direct access to decoded signed-16 coefficient bands in VRAM (for bit-exact verification)
    bool DownloadBandCoefficientsToHost(
        uint64_t ticket,
        int band_index,
        std::vector<int16_t>& out_coeffs,
        int* out_w = nullptr,
        int* out_h = nullptr,
        int* out_pitch = nullptr
    );

    // Export machine-readable timeline artifact in JSON format (Section 23)
    std::string ExportTimelineJson(uint64_t ticket);

    // Phase 2F-C Configuration & Diagnostics (Section 7, 8, 13, 27, 35)
    void SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy policy);
    Phase2FCSelectorPolicy GetPhase2FCSelectorPolicy() const;

    void SetPhase2FCChunkSize(uint32_t chunk_size);
    uint32_t GetPhase2FCChunkSize() const;

    void SetPhase2FCOracleDecisions(const std::vector<BandEntropyMethod>& decisions);

    bool GetPhase2FCBandRecords(uint64_t ticket, std::vector<Phase2FCBandRecord>& out_records) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience alias for Phase 2F-C pipeline
using CudaDecoderPipelinePhase2FC = CudaDecoderPipelinePhase2E;

#if defined(HYPERCINE_CUDA_AVAILABLE) || defined(__CUDACC__)
// Device pointers. Runs only the field combine, for the kernel unit test.
bool LaunchInterlacedFieldCombine(const int16_t* low, size_t low_pitch,
    const int16_t* high, size_t high_pitch, int16_t* output, size_t output_pitch,
    int width, int field_rows, cudaStream_t stream, uint32_t* frame_error = nullptr);
#endif

} // namespace HyperCine
