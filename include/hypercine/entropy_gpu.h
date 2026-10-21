// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_ENTROPY_GPU_H
#define HYPERCINE_ENTROPY_GPU_H

#include "hypercine/entropy_stage.h"

#include <stdint.h>

namespace hypercine {

struct GpuEntropySession;

enum class GpuEntropyDispatch {
    OneStream = 0,
    MultiStream = 1,
    Batched = 2
};

// Full packs on the device. Tokens stops after the run/value list and finishes
// codebook, prefix, and packing on the host. Codewords stops after the codebook
// and finishes the prefix and packing on the host.
enum class GpuEntropyStop {
    Full = 0,
    Tokens = 1,
    Codewords = 2
};

struct GpuBandRequest {
    const int16_t* device_coeff = nullptr;
    int width = 0;
    int height = 0;
    int pitch_px = 0;
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
};

struct GpuBandResult {
    bool ok = false;
    int tokens = 0;
    int total_bits = 0;
    int coeff_bytes = 0;
    EntropyTiming timing;
    std::vector<uint8_t> coeff_payload;
    std::vector<uint32_t> tokens_packed;
    std::vector<uint32_t> codeword;
    std::vector<uint16_t> bit_length;
    std::vector<uint32_t> bit_offset;
    std::string error;
};

enum class CompressedD2HMode {
    FrameConsolidated = 0,
    BandStreamed = 1
};

enum class GpuEntropyKernelMode {
    ReferencePhase2C = 0,
    Fused = 1,
    FusedScanOptimized = 2
};

struct GpuEntropyFrameSlot;

struct GpuEntropyFrameConfig {
    int entropy_streams = 4;
    CompressedD2HMode d2h_mode = CompressedD2HMode::FrameConsolidated;
    bool record_substage_events = false;
    bool lpt_schedule = true;
    bool use_cuda_graph = false;
    GpuEntropyKernelMode kernel_mode = GpuEntropyKernelMode::ReferencePhase2C;
};

struct GpuBandCompressedView {
    const uint8_t* bytes = nullptr;
    int byte_count = 0;
    int token_count = 0;
    int total_bits = 0;
    EntropyTiming timing;
};

struct GpuFrameEntropyStats {
    double gpu_entropy_wall_ms = 0.0;
    double gpu_consolidate_ms = 0.0;
    double compressed_d2h_ms = 0.0;
    size_t compressed_d2h_bytes = 0;
    int active_streams = 0;
    int max_active_blocks_per_sm = 0;
    int total_kernel_launches = 0;
    int graph_kernel_nodes = 0;
    int graph_total_nodes = 0;
    bool graph_used = false;
    bool graph_captured_this_frame = false;
    double host_enqueue_ms = 0.0;
    double graph_capture_ms = 0.0;
    EntropyTiming substage_sum;
    EntropyTiming band_timings[32];
};

GpuEntropySession* CreateGpuEntropySession();
void DestroyGpuEntropySession(GpuEntropySession* session);
int GpuEntropyScanOccupancy();

// One highpass band already resident on the device. coeff_payload matches the
// CPU stage coefficient bytes, including the band-end code and 32-bit pad.
bool EncodeHighpassOnGpu(GpuEntropySession* session, const int16_t* device_coeff, int width, int height,
                         int pitch_px, const EntropyTables& tables, bool keep_symbols, GpuBandResult* result,
                         GpuEntropyStop stop = GpuEntropyStop::Full);

// OneStream finishes each band before the next launch. MultiStream keeps up to
// `streams` bands in flight on separate streams and workspaces (capped at 8).
// Batched queues a wave on one stream, one workspace per band, and synchronizes
// once per wave. Summed stage times can exceed the wall time when bands overlap.
bool EncodeHighpassBatch(GpuEntropySession* session, const GpuBandRequest* requests, GpuBandResult* results,
                         int count, const EntropyTables* tables, GpuEntropyDispatch dispatch, int streams,
                         EntropyTiming* wall, GpuEntropyStop stop = GpuEntropyStop::Full);

// Phase 2C/2D multi-frame non-blocking GPU entropy slot API.
GpuEntropyFrameSlot* CreateGpuEntropyFrameSlot(int max_width, int max_height);
void DestroyGpuEntropyFrameSlot(GpuEntropyFrameSlot* slot);

// Queue all 27 highpass bands for one frame without blocking the host thread.
// `wait_wavelet_event` (optional cudaEvent_t) gates the start of GPU entropy.
// `wait_band_events` (optional array of `count` cudaEvent_t) gates each band individually.
bool QueueGpuEntropyFrame(GpuEntropySession* session, GpuEntropyFrameSlot* slot,
                          const GpuBandRequest* requests, int count,
                          const EntropyTables* tables, const GpuEntropyFrameConfig& cfg,
                          void* wait_wavelet_event = nullptr,
                          void* const* wait_band_events = nullptr);

// Poll results are three-way. Pending means the stage is still running.
// Failed is a sticky error (CUDA failure or entropy word overflow) and must
// not be treated as "not finished yet".
enum class GpuStageStatus {
    Pending = 0,
    Complete = 1,
    Failed = 2
};

// Non-blocking progress for FrameConsolidated mode: starts the single consolidated
// D2H DMA copy as soon as GPU entropy + compaction completes.
// Complete means the copy has been queued, not that the copy has finished.
// IsGpuEntropyFrameComplete waits for the copy as well.
GpuStageStatus PollGpuEntropyFrameD2HStatus(GpuEntropyFrameSlot* slot);
bool PollGpuEntropyFrameD2H(GpuEntropyFrameSlot* slot);

const char* GpuEntropyFrameError(const GpuEntropyFrameSlot* slot);
bool GpuEntropyFrameHasFailed(const GpuEntropyFrameSlot* slot);

// Thread-local reason for the most recent failed CreateGpuEntropyFrameSlot call:
// "UNSUPPORTED_RESOURCE_BOUND: ... requested N B; free N B of N B; CUDA ...".
// Empty when the last call succeeded. Capacity exhaustion stays a hard failure.
const char* GpuEntropySlotCreationError();

// Test hook. A non-zero cap replaces the proven allocation for the next queue
// so overflow returns Failed instead of writing past the buffer.
void SetGpuEntropyCapacityOverrideForTest(GpuEntropyFrameSlot* slot,
                                          uint32_t per_band_words,
                                          uint32_t consolidated_words);

// Test hook. Makes the next device allocation of at least `min_bytes` in a
// session workspace or codebook upload fail once, so the fail-closed capacity
// diagnostic can be exercised deterministically without exhausting the device.
void SetGpuEntropyAllocationFailureInjectionForTest(size_t min_bytes);

// Blocking progress for FrameConsolidated mode: waits on blocking event for GPU
// entropy + compaction to complete, then queues the consolidated D2H DMA copy.
bool WaitAndStartGpuEntropyFrameD2H(GpuEntropyFrameSlot* slot);

// Non-blocking progress for BandStreamed mode: queues per-band async D2H as each
// band finishes packing on GPU, and returns the index of the next band whose D2H
// has completed on host (or -1 if none newly ready).
int PollNextStreamedBandReady(GpuEntropyFrameSlot* slot);

// Returns true when all 27 highpass bands have completed GPU entropy and D2H transfer.
bool IsGpuEntropyFrameComplete(GpuEntropyFrameSlot* slot);

// Blocks on OS-backed CUDA completion event until all 27 highpass bands have completed
// GPU entropy and D2H transfer.
bool WaitGpuEntropyFrameComplete(GpuEntropyFrameSlot* slot);

// Returns true when the GPU entropy compute kernels for `slot` have finished
// (even if D2H is still in progress), allowing the next frame to reuse session workspaces.
bool IsGpuEntropyComputeDone(GpuEntropyFrameSlot* slot);

// Blocks on OS-backed CUDA completion event until GPU entropy compute finishes on `slot`.
bool WaitGpuEntropyComputeDone(GpuEntropyFrameSlot* slot);

// Blocks on any cudaEvent_t created with cudaEventBlockingSync.
bool WaitGpuEvent(void* cuda_event);

// Returns the cudaEvent_t recorded when GPU entropy compute finishes on `slot`.
void* GetGpuEntropyComputeDoneEvent(GpuEntropyFrameSlot* slot);

// Retrieve a zero-copy pinned-host view of band `band_index`'s compressed VLC words.
bool GetGpuBandCompressedView(GpuEntropyFrameSlot* slot, int band_index, GpuBandCompressedView* view);

// Collect GPU event timings and byte counts after the frame completes.
bool CollectGpuFrameEntropyStats(GpuEntropyFrameSlot* slot, GpuFrameEntropyStats* stats);

}  // namespace hypercine

#endif
