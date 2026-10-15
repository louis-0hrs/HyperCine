// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE2D_H
#define HYPERCINE_PHASE2D_H

#include "hypercine/entropy_gpu.h"
#include "hypercine/phase2a_gpu.h"
#include "hypercine/phase2c.h"

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace hypercine {

// SDK-identical regular CFHD quality values. KEYING and UNCOMPRESSED modes
// are deliberately excluded because they describe separate format families.
enum class EncodingQuality : int {
    Low = 1,
    Medium = 2,
    High = 3,
    FilmScan1 = 4,
    FilmScan2 = 5,
    FilmScan3 = 6
};

inline const char* EncodingQualityName(EncodingQuality quality) noexcept {
    switch (quality) {
    case EncodingQuality::Low: return "low";
    case EncodingQuality::Medium: return "medium";
    case EncodingQuality::High: return "high";
    case EncodingQuality::FilmScan1: return "filmscan1";
    case EncodingQuality::FilmScan2: return "filmscan2";
    case EncodingQuality::FilmScan3: return "filmscan3";
    }
    return "invalid";
}

inline const char* EncodingQualitySdkName(EncodingQuality quality) noexcept {
    switch (quality) {
    case EncodingQuality::Low: return "CFHD_ENCODING_QUALITY_LOW";
    case EncodingQuality::Medium: return "CFHD_ENCODING_QUALITY_MEDIUM";
    case EncodingQuality::High: return "CFHD_ENCODING_QUALITY_HIGH";
    case EncodingQuality::FilmScan1: return "CFHD_ENCODING_QUALITY_FILMSCAN1";
    case EncodingQuality::FilmScan2: return "CFHD_ENCODING_QUALITY_FILMSCAN2";
    case EncodingQuality::FilmScan3: return "CFHD_ENCODING_QUALITY_FILMSCAN3";
    }
    return "invalid";
}

inline bool IsValidEncodingQuality(EncodingQuality quality) noexcept {
    switch (quality) {
    case EncodingQuality::Low:
    case EncodingQuality::Medium:
    case EncodingQuality::High:
    case EncodingQuality::FilmScan1:
    case EncodingQuality::FilmScan2:
    case EncodingQuality::FilmScan3: return true;
    }
    return false;
}

// ============================================================================
// PART 1: Clarify & Verify "Byte-Identical" (A: Pristine Upstream, B: HyperCine
//         CPU/Reference, C: HyperCine GPU) across Quantizer Rate-Limiter States
// ============================================================================

enum class Phase2DQuantStateMode {
    // Frame 1 of a freshly opened CFHD_EncoderRef:
    // encoder->lastgopbitcount == 0 -> q->FSratelimiter == 4 (scale == 12).
    // Produces 900,396 B at 1080p, 3,563,684 B at 4K, 14,009,028 B at 8K.
    Frame1Initial = 0,

    // Steady-state Frame 2+ of CFHD_EncoderRef for FilmScan 3:
    // QuantizationSetQuality adapts q->FSratelimiter: 4 -> 0 (scale == 8)
    // once compression ratio is evaluated against FilmScan 3 target [3.0 .. 4.5].
    // Produces 1,181,024 B at 1080p, 4,668,864 B at 4K, 18,393,596 B at 8K.
    SteadyState = 1
};

struct Phase2DMetadataDiffRange {
    uint32_t tag_fourcc = 0;
    std::string tag_name;
    size_t byte_offset = 0;
    size_t byte_length = 0;
};

struct Phase2DSubbandComparison {
    int index = 0;
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    std::string name;
    bool lowpass = false;
    int width = 0;
    int height = 0;
    int quant = 0;
    int scale = 0;
    size_t a_upstream_bytes = 0;
    size_t b_cpu_ref_bytes = 0;
    size_t c_gpu_bytes = 0;
    bool a_eq_b = false;
    bool b_eq_c = false;
    bool a_eq_c = false;
    std::string a_sha256;
    std::string b_sha256;
    std::string c_sha256;
};

struct Phase2DUpstreamComparisonReport {
    bool ok = false;
    int width = 0;
    int height = 0;
    Phase2DQuantStateMode quant_state_mode = Phase2DQuantStateMode::SteadyState;

    // Exact encoder settings & state captured at encode time.
    std::string pixel_format = "CFHD_PIXEL_FORMAT_BGRA";
    std::string encoded_format = "CFHD_ENCODED_FORMAT_YUV_422";
    std::string encoding_quality = EncodingQualitySdkName(EncodingQuality::FilmScan3);
    uint32_t encoding_flags = 0;
    int fs_rate_limiter = 0;
    int last_gop_bytes = 0;
    double target_compression_ratio = 0.0;
    int prescale[3] = {};
    int quant_table[36] = {};
    int scale_table[36] = {};

    // Sample sizes and SHA-256 digests for A, B, and C.
    size_t a_upstream_sample_bytes = 0;
    size_t b_cpu_ref_sample_bytes = 0;
    size_t c_gpu_ref_sample_bytes = 0;
    size_t c_gpu_fused_sample_bytes = 0;
    size_t c_gpu_scan_opt_sample_bytes = 0;
    size_t c_gpu_graph_sample_bytes = 0;

    std::string a_upstream_sha256;
    std::string b_cpu_ref_sha256;
    std::string c_gpu_ref_sha256;
    std::string c_gpu_fused_sha256;
    std::string c_gpu_scan_opt_sha256;
    std::string c_gpu_graph_sha256;

    std::string a_masked_sha256;
    std::string b_masked_sha256;
    std::string c_masked_sha256;

    // Unmasked and metadata-masked comparisons.
    bool b_eq_c_unmasked = false;
    bool c_all_modes_byte_identical = false;
    bool a_eq_b_same_handle_unmasked = false;
    bool a_eq_b_independent_handle_masked = false;
    bool a_eq_c_independent_handle_masked = false;
    bool all_30_subbands_a_eq_b_eq_c = false;
    size_t unmasked_independent_diff_bytes = 0;
    size_t masked_independent_diff_bytes = 0;
    std::vector<Phase2DMetadataDiffRange> volatile_metadata_ranges;
    std::vector<Phase2DSubbandComparison> bands;

    // Decoded frame verification.
    bool decoded_a_eq_b = false;
    bool decoded_b_eq_c = false;
    int max_abs_pixel_diff = 0;
    double psnr_db = 0.0;
    std::string decoded_pixels_sha256;
    std::string error;
};

Phase2DUpstreamComparisonReport VerifyPhase2DUpstreamComparison(
    int width, int height,
    Phase2DQuantStateMode state_mode = Phase2DQuantStateMode::SteadyState,
    uint32_t seed = 0x1337BEEFu,
    EncodingQuality quality = EncodingQuality::FilmScan3);

// Mask volatile metadata fields (TAG_CLIP_GUID, TAG_ENCODE_DATE, TAG_ENCODE_TIME,
// TAG_TIMECODE) in a CFHD bitstream copy for deterministic cross-instance comparison.
std::vector<uint8_t> MaskCfhdVolatileMetadata(
    const std::vector<uint8_t>& bitstream,
    std::vector<Phase2DMetadataDiffRange>* out_ranges = nullptr);

// ============================================================================
// PART 2: Production Metadata Safety Verification
// ============================================================================

struct Phase2DMetadataSafetyReport {
    bool ok = false;
    bool normal_mode_guids_distinct = false;
    bool normal_mode_unmasked_distinct = false;
    bool normal_mode_masked_identical = false;
    bool deterministic_mode_unmasked_identical = false;
    std::string clip1_guid_hex;
    std::string clip2_guid_hex;
    std::string clip1_date;
    std::string clip1_time;
    std::string clip1_sha256;
    std::string clip2_sha256;
    std::string clip1_masked_sha256;
    std::string clip2_masked_sha256;
    std::string det1_sha256;
    std::string det2_sha256;
    std::string error;
};

Phase2DMetadataSafetyReport VerifyPhase2DMetadataSafety(int width = 1920, int height = 1080);

// ============================================================================
// PART 4 & PART 5: Slot Selection & Compressed D2H Mode Policies
// ============================================================================

enum class Phase2DSlotPolicy {
    DefaultAuto = 0,    // 1080p -> 2 slots, 4K -> 3 slots, 8K -> 2 slots (VRAM-bounded)
    LowLatency = 1,     // 1 slot across all resolutions (minimum latency & VRAM)
    MaxThroughput = 2,  // 1080p -> 2 slots, 4K -> 3 slots, 8K -> 3 slots (VRAM-bounded)
    UserOverride = 3    // Explicit user_override_slots (clamped to VRAM bound)
};

enum class Phase2DD2HPolicy {
    Throughput = 0,             // FrameConsolidated for all resolutions (default)
    LowLatency = 1,             // BandStreamed for 1080p/4K, FrameConsolidated for 8K
    ForceFrameConsolidated = 2,
    ForceBandStreamed = 3
};

enum class Phase2DSyncMode {
    Blocking = 0,  // Default: OS-backed blocking CUDA events + condition variables (no busy spin)
    Polling = 1    // Legacy Phase 2C tight cudaEventQuery polling loop (for comparison)
};

struct Phase2DSlotSelection {
    int width = 0;
    int height = 0;
    Phase2DSlotPolicy policy = Phase2DSlotPolicy::DefaultAuto;
    int requested_override_slots = 0;
    int policy_target_slots = 0;
    int vram_max_slots = 0;
    int selected_slots = 0;
    size_t per_slot_vram_bytes = 0;
    size_t session_workspace_vram_bytes = 0;
    size_t total_estimated_vram_bytes = 0;
    size_t available_vram_bytes = 0;
    CompressedD2HMode selected_d2h_mode = CompressedD2HMode::FrameConsolidated;
};

Phase2DSlotSelection SelectPhase2DFrameSlots(
    int width, int height,
    Phase2DSlotPolicy slot_policy = Phase2DSlotPolicy::DefaultAuto,
    int user_override_slots = 0,
    Phase2DD2HPolicy d2h_policy = Phase2DD2HPolicy::Throughput,
    size_t override_available_vram_bytes = 0);

// ============================================================================
// PARTS 3, 6, 7, 8, 9, 10: Phase 2D Pipeline Configuration & Result
// ============================================================================

struct Phase2DConfig {
    int width = 1920;
    int height = 1080;
    int frames = 12;
    int warmup_frames = 2;
    Phase2DSlotPolicy slot_policy = Phase2DSlotPolicy::DefaultAuto;
    int user_override_slots = 0;  // > 0 overrides slot_policy
    int entropy_streams = 4;
    Phase2DD2HPolicy d2h_policy = Phase2DD2HPolicy::Throughput;
    Phase2CInputMode input_mode = Phase2CInputMode::DeviceInput;
    Phase2DSyncMode sync_mode = Phase2DSyncMode::Blocking;
    Phase2DQuantStateMode quant_state_mode = Phase2DQuantStateMode::SteadyState;
    bool use_cuda_graph = true;
    GpuEntropyKernelMode kernel_mode = GpuEntropyKernelMode::FusedScanOptimized;
    bool gpu_pack_lowpass = false;
    bool record_substage_events = false;
    bool verify_bitstream = true;
    bool async_cpu_assembly = true;
    bool deterministic_test_metadata = false;
    uint32_t seed = 0x1337BEEFu;
};

struct Phase2DCpuMetrics {
    int logical_processors = 1;
    int active_host_threads = 1;
    double wall_ms = 0.0;
    double process_cpu_ms = 0.0;
    double main_thread_cpu_ms = 0.0;
    double worker_thread_cpu_ms = 0.0;

    // Normalized across all logical processors (0..100% of total machine CPU).
    double normalized_process_cpu_pct = 0.0;
    // Single-core equivalent (100% = 1.0 full logical processor core).
    double single_core_equiv_pct = 0.0;
    double main_thread_single_core_pct = 0.0;
    double worker_thread_single_core_pct = 0.0;

    // Host thread state breakdown (ms per frame and total ms).
    double main_thread_running_ms = 0.0;
    double main_thread_blocked_ms = 0.0;
    double main_thread_spin_ms = 0.0;
    int64_t poll_iterations = 0;
    int64_t blocking_waits = 0;

    // Efficiency metrics.
    double cpu_core_sec_per_wall_sec = 0.0;
    double cpu_core_ms_per_frame = 0.0;
    double cpu_core_sec_per_60fps_video_sec = 0.0;
};

struct Phase2DResult {
    bool ok = false;
    Phase2DConfig config;
    Phase2DSlotSelection slot_selection;
    int encoded_frames = 0;
    double wall_ms = 0.0;
    double sustained_fps = 0.0;
    double frame_period_ms = 0.0;

    // Submit-to-complete per-frame latency statistics.
    double mean_latency_ms = 0.0;
    double median_latency_ms = 0.0;
    double p95_latency_ms = 0.0;
    double min_latency_ms = 0.0;
    double max_latency_ms = 0.0;

    // Inter-frame completion interval statistics.
    double mean_period_ms = 0.0;
    double median_period_ms = 0.0;
    double p95_period_ms = 0.0;

    // Per-frame GPU & CPU stage breakdown (averages over measured frames).
    double h2d_ms = 0.0;
    double color_ms = 0.0;
    double wavelet_ms = 0.0;
    double gpu_frontend_wavelet_ms = 0.0;
    double gpu_entropy_ms = 0.0;
    double gpu_consolidate_ms = 0.0;
    double compressed_d2h_ms = 0.0;
    double lowpass_d2h_ms = 0.0;
    double total_d2h_ms = 0.0;
    double total_gpu_ms = 0.0;

    double host_enqueue_ms = 0.0;
    double graph_capture_ms = 0.0;
    double cpu_lowpass_ms = 0.0;
    double cpu_header_wrap_ms = 0.0;
    double cpu_sample_concat_ms = 0.0;
    double cpu_assembly_total_ms = 0.0;
    double cpu_dispatch_poll_ms = 0.0;
    double total_cpu_ms = 0.0;

    EntropyTiming entropy_substage_sum;

    // Launch count & CUDA Graph metrics (Part 6 & Part 9).
    int kernel_launches_per_band = 0;
    int kernel_launches_per_frame = 0;
    int host_launch_calls_per_frame = 0;
    int graph_kernel_nodes = 0;
    int graph_total_nodes = 0;
    bool graph_used = false;

    // Transfer & memory footprint metrics.
    size_t h2d_bytes_per_frame = 0;
    size_t lowpass_d2h_bytes_per_frame = 0;
    size_t compressed_highpass_d2h_bytes_per_frame = 0;
    size_t total_d2h_bytes_per_frame = 0;
    size_t bitstream_bytes = 0;
    size_t vram_used_bytes = 0;
    size_t slot_vram_bytes = 0;
    size_t slot_pinned_host_bytes = 0;

    // Utilization and queue depth.
    double gpu_utilization_pct = 0.0;
    Phase2DCpuMetrics cpu;
    double avg_frames_in_flight = 0.0;

    // Quality & verification.
    bool bitstream_byte_match = false;
    bool decode_ok = false;
    double psnr_db = 0.0;
    std::string sample_sha256;
    std::vector<uint8_t> last_bitstream;
    std::string error;
};

Phase2DResult RunPhase2D(const Phase2DConfig& cfg);

// Isolated GPU entropy study for Phase 2D-A (Graph), 2D-B (Fused), 2D-C (Scan Optimized).
struct Phase2DKernelStudyPoint {
    std::string label;
    bool use_cuda_graph = false;
    GpuEntropyKernelMode kernel_mode = GpuEntropyKernelMode::ReferencePhase2C;
    int streams = 4;
    int kernel_launches_per_band = 0;
    int kernel_launches_per_frame = 0;
    int host_launch_calls_per_frame = 0;
    int graph_kernel_nodes = 0;
    int graph_total_nodes = 0;
    double graph_capture_ms = 0.0;
    double host_enqueue_ms = 0.0;
    double mean_wall_ms = 0.0;
    double median_wall_ms = 0.0;
    double p95_wall_ms = 0.0;
    double gpu_entropy_ms = 0.0;
    double gpu_consolidate_ms = 0.0;
    double compressed_d2h_ms = 0.0;
    double speedup_vs_ref_no_graph = 1.0;
    EntropyTiming substage_sum;
    size_t sample_bytes = 0;
    std::string sample_sha256;
    bool byte_match_reference = false;
};

std::vector<Phase2DKernelStudyPoint> BenchmarkPhase2DKernelEvolution(
    int width, int height, int iterations = 10,
    Phase2DQuantStateMode quant_state = Phase2DQuantStateMode::SteadyState,
    uint32_t seed = 0x1337BEEFu);

}  // namespace hypercine

#endif
