// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE3A_H
#define HYPERCINE_PHASE3A_H

#include "hypercine/phase2d.h"

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace hypercine {

// ============================================================================
// SECTION 1 & 2: Multi-Frame Input Modes, Quantizer State Modes, & Frame Types
// ============================================================================

enum class Phase3AInputMode {
    // True end-to-end host pageable input: caller provides distinct pageable host
    // BGRA frames; timed path includes host-to-pinned staging + H2D DMA + GPU
    // encode + D2H DMA + CPU bitstream assembly.
    HostPageableEndToEnd = 0,

    // Pinned host API input: caller provides distinct pre-allocated pinned host
    // BGRA frames; timed path includes H2D DMA + GPU encode + D2H DMA + CPU
    // bitstream assembly.
    HostPinnedApi = 1,

    // Device-resident input: caller provides distinct GPU-resident BGRA frames;
    // timed path includes GPU color/wavelet/entropy + D2H DMA + CPU assembly.
    DeviceInput = 2
};

const char* Phase3AInputModeName(Phase3AInputMode mode);

enum class Phase3AQuantMode {
    // Default production & sequence mode: tracks encoder->lastgopbitcount from
    // the previous frame's encoded sample bits and updates q->FSratelimiter and
    // transform quant[36] before every frame k, matching pristine CFHD_EncodeSample.
    DynamicUpstreamState = 0,

    // Unit-test-only fixed mode: forces lastgopbitcount = 0 (FSratelimiter = 4).
    FixedFrame1Initial = 1,

    // Unit-test-only fixed mode: pre-conditions to steady-state FSratelimiter.
    FixedSteadyState = 2
};

const char* Phase3AQuantModeName(Phase3AQuantMode mode);

enum class Phase3APattern {
    FlatBlack = 0,
    FlatMidGray = 1,
    FlatWhite = 2,
    HorizontalGradient = 3,
    VerticalGradient = 4,
    Checkerboard = 5,
    HardEdges = 6,
    DeterministicNoise = 7,
    HighFrequencyPattern = 8,
    LowDetailPhotographicSynthetic = 9,
    HighDetailPhotographicSynthetic = 10
};

constexpr int kPhase3APatternCount = 11;

const char* Phase3APatternName(Phase3APattern pattern);
Phase3APattern SelectSequencePattern(int frame_index);

// Generate a deterministic BGRA frame with embedded binary macro-block identity
// stamp for `frame_index` and `pattern`.
void GeneratePhase3AFrame(int width, int height, int frame_index, Phase3APattern pattern,
                          uint8_t* bgra_out, int pitch, uint32_t base_seed = 0x1337BEEFu);

void StampFrameIdentityBlocks(uint8_t* bgra, int width, int height, int pitch,
                              int frame_index, Phase3APattern pattern);

bool ExtractStampedFrameIdentity(const uint8_t* decoded_bgra, int width, int height, int pitch,
                                 int* out_frame_index, int* out_pattern = nullptr);

struct Phase3AFrameInput {
    int frame_index = 0;
    Phase3APattern pattern = Phase3APattern::HighDetailPhotographicSynthetic;
    const uint8_t* bgra_ptr = nullptr;
    // Host copy used for PSNR when bgra_ptr is device memory.
    const uint8_t* host_ref = nullptr;
    int pitch = 0;
    int width = 0;
    int height = 0;
};

struct Phase3AFrameRecord {
    int frame_index = 0;
    Phase3APattern pattern = Phase3APattern::HighDetailPhotographicSynthetic;
    size_t prev_encoded_bytes = 0;
    int64_t prev_lastgopbitcount = 0;
    int current_fs_rate_limiter = 0;
    int quant_table[36] = {};
    int scale_table[36] = {};
    size_t sample_bytes = 0;
    std::string sample_sha256;
    std::string masked_sample_sha256;
    double submit_ms = 0.0;
    double complete_ms = 0.0;
    double latency_ms = 0.0;
};

// ============================================================================
// SECTION 3: 120-Frame Sequence Equivalence Verification
//            (A: Pristine Upstream, B: HyperCine CPU/Ref, C: HyperCine GPU)
// ============================================================================

struct Phase3AFrameEquivalenceItem {
    int frame_index = 0;
    std::string pattern_name;
    size_t a_prev_bytes = 0;
    size_t b_prev_bytes = 0;
    size_t c_prev_bytes = 0;
    int64_t a_prev_bits = 0;
    int64_t b_prev_bits = 0;
    int64_t c_prev_bits = 0;
    int a_fs_rate_limiter = 0;
    int b_fs_rate_limiter = 0;
    int c_fs_rate_limiter = 0;
    int a_quant[36] = {};
    int b_quant[36] = {};
    int c_quant[36] = {};
    bool quant_tables_match = false;
    bool all_30_coeff_hashes_match = false;
    bool all_30_payload_sizes_match = false;
    bool all_30_payload_hashes_match = false;
    size_t a_sample_bytes = 0;
    size_t b_sample_bytes = 0;
    size_t c_sample_bytes = 0;
    std::string a_masked_sha256;
    std::string b_masked_sha256;
    std::string c_masked_sha256;
    bool sample_masked_match = false;
    bool b_eq_c_unmasked = false;
    bool decoded_match = false;
    int max_decoded_pixel_diff = 0;
    double psnr_db = 0.0;
    int extracted_frame_index = -1;
};

struct Phase3ABandReference {
    int channel = 0;
    int wavelet = 0;
    int band = 0;
    int subband = 0;
    int width = 0;
    int height = 0;
    int quant = 0;
    int scale = 0;
    std::string sdk_coeff_fnv64;
    std::string cuda_coeff_fnv64;
    std::string sdk_payload_sha256;
    std::string cuda_payload_sha256;
};

struct Phase3AReferenceFrameArtifact {
    int frame_index = 0;
    std::string pattern_name;
    int width = 0;
    int height = 0;
    int pitch = 0;
    int fs_rate_limiter = 0;
    int previous_sample_bytes = 0;
    int quant[36] = {};
    int scale[36] = {};
    std::vector<uint8_t> input_bgra;
    std::vector<uint8_t> upstream_cfhd;
    std::vector<uint8_t> cuda_cfhd;
    std::vector<uint8_t> normalized_upstream_cfhd;
    std::vector<uint8_t> normalized_cuda_cfhd;
    std::vector<Phase2DMetadataDiffRange> metadata_mask_ranges;
    std::vector<Phase3ABandReference> bands;
    std::string input_sha256;
    std::string upstream_sha256;
    std::string cuda_sha256;
    std::string normalized_upstream_sha256;
    std::string normalized_cuda_sha256;
    size_t first_unmasked_difference = static_cast<size_t>(-1);
};

using Phase3AReferenceFrameCallback =
    std::function<bool(const Phase3AReferenceFrameArtifact&, std::string*)>;

struct Phase3ASequence120Report {
    bool ok = false;
    int width = 0;
    int height = 0;
    int total_frames = 0;
    int matched_frames = 0;
    int distinct_fs_rate_limiters_observed = 0;
    int quant_transitions_observed = 0;
    std::vector<int> fs_rate_limiter_trajectory;
    std::vector<Phase3AFrameEquivalenceItem> frames;
    std::string error;
};

Phase3ASequence120Report VerifyPhase3ASequence120(
    int width = 1920, int height = 1080, int num_frames = 120,
    uint32_t seed = 0x1337BEEFu, const Phase3AReferenceFrameCallback& artifact_callback = {},
    EncodingQuality quality = EncodingQuality::FilmScan3);

// ============================================================================
// SECTION 4: 300-Frame Distinct-Frame Slot Wrap & Ordering Verification
// ============================================================================

struct Phase3ADistinctFrames300Report {
    bool ok = false;
    int width = 0;
    int height = 0;
    int num_slots = 0;
    Phase3AInputMode input_mode = Phase3AInputMode::HostPageableEndToEnd;
    int submitted_frames = 0;
    int completed_frames = 0;
    bool strict_monotonic_indices = false;
    bool no_duplicates_or_missing = false;
    bool no_stale_slot_reuse = false;
    int decoded_checked_count = 0;
    int decoded_stamp_matched_count = 0;
    double min_psnr_db = 0.0;
    double mean_psnr_db = 0.0;
    std::vector<int> verified_decode_indices;
    std::string error;
};

Phase3ADistinctFrames300Report VerifyPhase3ADistinctFrames300(
    int width, int height, int num_frames = 300,
    Phase3AInputMode input_mode = Phase3AInputMode::HostPageableEndToEnd,
    Phase2DSlotPolicy slot_policy = Phase2DSlotPolicy::DefaultAuto);

// ============================================================================
// SECTION 5: Adversarial GPU Entropy Edge-Case Suite (17 Pathological Patterns)
// ============================================================================

enum class Phase3AAdversarialCase {
    AllZeros = 0,
    SingleNonZeroFirst = 1,
    SingleNonZeroLast = 2,
    AlternatingNonZeroZero = 3,
    ZeroRunsAroundBoundaries = 4,
    VeryLongZeroRuns = 5,
    CodebookTransitionValues = 6,
    MaxMinSupportedCoeffs = 7,
    EscapeLargeMagnitude = 8,
    ByteBoundaryCrossings = 9,
    Word16BoundaryCrossings = 10,
    Word32BoundaryCrossings = 11,
    CudaBlockBoundaryCrossings = 12,
    EndOfBandTerminationEdge = 13,
    SparseRandomKnownSeed = 14,
    DenseHighEntropyKnownSeed = 15,
    RowGapStrideEdge = 16
};

constexpr int kPhase3AAdversarialCaseCount = 17;
const char* Phase3AAdversarialCaseName(Phase3AAdversarialCase c);

struct Phase3AAdversarialCaseResult {
    std::string case_name;
    int width = 0;
    int height = 0;
    int pitch_px = 0;
    int subband = 0;
    size_t upstream_payload_bytes = 0;
    int total_tokens = 0;
    int total_bits = 0;
    bool cpu_stages_match = false;
    bool gpu_ref_match = false;
    bool gpu_fused_match = false;
    bool gpu_scan_opt_match = false;
    bool gpu_graph_match = false;
    std::string upstream_sha256;
    std::string gpu_sha256;
    std::string error;
};

struct Phase3AAdversarialEntropyReport {
    bool ok = false;
    int total_cases_run = 0;
    int total_cases_passed = 0;
    std::vector<Phase3AAdversarialCaseResult> results;
    std::string error;
};

Phase3AAdversarialEntropyReport VerifyPhase3AAdversarialEntropy();

// ============================================================================
// SECTION 13: Multi-Frame Production Metadata Sequence Verification
// ============================================================================

struct Phase3AMetadataFrameInfo {
    int frame_index = 0;
    std::string clip_guid_hex;
    std::string encode_date;
    std::string encode_time;
    std::string timecode;
    int32_t unique_framenum = -1;
    std::string sample_sha256;
    std::string masked_sha256;
    bool matches_upstream_masked = false;
};

struct Phase3AMetadataSequenceReport {
    bool ok = false;
    int num_frames = 0;
    bool clip1_guid_non_zero = false;
    bool clip1_guid_constant_across_frames = false;
    bool clip2_guid_distinct_from_clip1 = false;
    bool dates_and_times_valid = false;
    bool unique_framenum_monotonic_0_to_n = false;
    bool timecode_progresses = false;
    bool all_frames_match_upstream_masked = false;
    std::string clip1_guid_hex;
    std::string clip2_guid_hex;
    std::vector<Phase3AMetadataFrameInfo> clip1_frames;
    std::string error;
};

Phase3AMetadataSequenceReport VerifyPhase3AMetadataSequence(
    int width = 1920, int height = 1080, int num_frames = 16);

// ============================================================================
// SECTIONS 6, 7, 8, 9, 10: Hardened Sequence Benchmark & NVML Telemetry
// ============================================================================

struct Phase3ANvmlTelemetry {
    bool nvml_available = false;
    int sample_count = 0;
    double hardware_gpu_utilization_pct_mean = 0.0;
    double hardware_gpu_utilization_pct_max = 0.0;
    double hardware_mem_utilization_pct_mean = 0.0;
    double hardware_mem_utilization_pct_max = 0.0;
    double graphics_clock_mhz_mean = 0.0;
    int graphics_clock_mhz_min = 0;
    int graphics_clock_mhz_max = 0;
    double mem_clock_mhz_mean = 0.0;
    int mem_clock_mhz_min = 0;
    int mem_clock_mhz_max = 0;
    int temperature_c_start = 0;
    int temperature_c_end = 0;
    int temperature_c_max = 0;
    double power_w_mean = 0.0;
    double power_w_max = 0.0;
    uint64_t throttle_reasons_bitmask = 0;
    std::string throttle_reasons_str = "None";
};

struct Phase3AConfig {
    int width = 1920;
    int height = 1080;
    int frames = 24;
    int warmup_frames = 4;
    Phase3AInputMode input_mode = Phase3AInputMode::HostPageableEndToEnd;
    Phase3AQuantMode quant_mode = Phase3AQuantMode::DynamicUpstreamState;
    Phase2DSlotPolicy slot_policy = Phase2DSlotPolicy::DefaultAuto;
    int user_override_slots = 0;
    int entropy_streams = 4;
    Phase2DD2HPolicy d2h_policy = Phase2DD2HPolicy::Throughput;
    Phase2DSyncMode sync_mode = Phase2DSyncMode::Blocking;
    bool use_cuda_graph = true;
    GpuEntropyKernelMode kernel_mode = GpuEntropyKernelMode::FusedScanOptimized;
    bool gpu_pack_lowpass = false;
    bool record_substage_events = false;
    bool async_cpu_assembly = true;
    bool deterministic_test_metadata = false;
    bool verify_sequence = true;
    bool retain_frame_records = true;
    // Keep existing HyperCine output stable; SDK default FilmScan1 is separate.
    EncodingQuality encoding_quality = EncodingQuality::FilmScan3;
    uint32_t seed = 0x1337BEEFu;
};

struct Phase3AResult {
    bool ok = false;
    Phase3AConfig config;
    Phase2DSlotSelection slot_selection;
    int submitted_frames = 0;
    int completed_frames = 0;
    double wall_ms = 0.0;
    double sustained_fps = 0.0;
    double frame_period_ms = 0.0;
    double first_quarter_fps = 0.0;
    double last_quarter_fps = 0.0;

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
    double p99_latency_ms = 0.0;
    double first_frame_latency_ms = 0.0;

    // ProductionEndToEnd accounting. sustained_fps is completed-frame throughput
    // of the post-warmup window, not a submission rate and not 1000/mean_latency.
    double steady_state_fps = 0.0;
    int steady_state_frames = 0;
    double warmup_excluded_ms = 0.0;
    double drain_ms = 0.0;
    double device_sync_ms = 0.0;
    bool device_drained = false;
    bool watchdog_tripped = false;
    double rate_control_ms = 0.0;
    std::string throughput_definition;

    // Per-frame GPU & CPU stage breakdown (averages over measured frames).
    double host_pageable_stage_ms = 0.0;
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
    int graph_captures_count = 0;
    double cpu_lowpass_ms = 0.0;
    double cpu_header_wrap_ms = 0.0;
    double cpu_sample_concat_ms = 0.0;
    double cpu_assembly_total_ms = 0.0;
    double cpu_dispatch_poll_ms = 0.0;
    double total_cpu_ms = 0.0;

    EntropyTiming entropy_substage_sum;

    // Launch count & CUDA Graph metrics.
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
    size_t mean_bitstream_bytes = 0;
    size_t min_bitstream_bytes = 0;
    size_t max_bitstream_bytes = 0;

    size_t vram_before_bytes = 0;
    size_t vram_after_init_bytes = 0;
    size_t vram_mid_run_bytes = 0;
    size_t vram_end_run_bytes = 0;
    size_t vram_after_destroy_bytes = 0;
    size_t vram_used_bytes = 0;
    size_t slot_vram_bytes = 0;
    size_t slot_pinned_host_bytes = 0;

    size_t host_rss_before_bytes = 0;
    size_t host_rss_mid_bytes = 0;
    size_t host_rss_end_bytes = 0;
    size_t host_rss_after_destroy_bytes = 0;

    // Utilization and telemetry (Section 7).
    // Renamed from misleading `gpu_utilization_pct` to explicitly reflect its definition:
    // 100.0 * min(1.0, total_gpu_ms / frame_period_ms).
    double pipeline_gpu_stage_occupancy_estimate_pct = 0.0;
    Phase3ANvmlTelemetry nvml;
    Phase2DCpuMetrics cpu;
    double avg_frames_in_flight = 0.0;

    // Sequence & quality verification.
    // decode_ok / stamp_verify_ok are meaningful only when verification_performed
    // is true. A skipped verify is not a pass.
    bool verification_performed = false;
    bool sequence_order_ok = false;
    bool decode_ok = false;
    bool stamp_verify_ok = false;
    double min_psnr_db = 0.0;
    double mean_psnr_db = 0.0;
    int distinct_fs_rate_limiters = 0;
    std::vector<Phase3AFrameRecord> frame_records;
    std::string error;
};

Phase3AResult RunPhase3A(const Phase3AConfig& cfg,
                         const std::vector<Phase3AFrameInput>* explicit_inputs = nullptr);

// Section 8: Multi-trial 8K Variance & Thermal/Clock Study.
struct Phase3AVarianceStudySummary {
    int width = 7680;
    int height = 4320;
    Phase3AInputMode input_mode = Phase3AInputMode::DeviceInput;
    int trials = 0;
    double mean_fps = 0.0;
    double median_fps = 0.0;
    double stddev_fps = 0.0;
    double min_fps = 0.0;
    double max_fps = 0.0;
    double mean_clock_mhz = 0.0;
    int min_clock_mhz = 0;
    int max_clock_mhz = 0;
    double mean_power_w = 0.0;
    double max_power_w = 0.0;
    int max_temp_c = 0;
    std::string combined_throttle_reasons;
    std::vector<Phase3AResult> trial_results;
};

Phase3AVarianceStudySummary RunPhase3AVarianceStudy(
    int width, int height, Phase3AInputMode input_mode,
    int trials = 5, int frames_per_trial = 24);

// Section 9: CUDA Graph A/B Study on Changing-Complexity Sequence with Dynamic Quantizer.
struct Phase3ACudaGraphABReport {
    bool ok = false;
    int width = 0;
    int height = 0;
    int frames_per_trial = 0;
    int trials = 0;
    bool all_frames_byte_identical = false;
    int graph_captures_per_run = 0;
    int distinct_quant_states_exercised = 0;
    double graph_off_mean_fps = 0.0;
    double graph_off_median_fps = 0.0;
    double graph_off_stddev_fps = 0.0;
    double graph_off_enqueue_ms = 0.0;
    double graph_off_entropy_ms = 0.0;
    double graph_off_cpu_ms_per_frame = 0.0;
    double graph_on_mean_fps = 0.0;
    double graph_on_median_fps = 0.0;
    double graph_on_stddev_fps = 0.0;
    double graph_on_enqueue_ms = 0.0;
    double graph_on_entropy_ms = 0.0;
    double graph_on_cpu_ms_per_frame = 0.0;
    double fps_delta_pct = 0.0;
    double enqueue_reduction_pct = 0.0;
    std::string error;
};

Phase3ACudaGraphABReport RunPhase3ACudaGraphABStudy(
    int width, int height, int frames_per_trial = 24, int trials = 3);

}  // namespace hypercine

#endif
