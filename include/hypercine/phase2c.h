// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE2C_H
#define HYPERCINE_PHASE2C_H

#include "hypercine/entropy_gpu.h"
#include "hypercine/phase2a_gpu.h"

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace hypercine {

// Pure C++17 FIPS 180-4 SHA-256 hex digest (64 lowercase hex chars).
std::string Sha256Hex(const void* data, size_t size);

// Section 1: Exact Phase 2B / 2C 30-band coverage audit.
struct Phase2BSubbandAuditEntry {
    int index = 0;
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    std::string name;
    bool lowpass = false;
    int width = 0;
    int height = 0;
    int coeff_count = 0;
    bool gpu_entropy = false;
    bool cpu_entropy = false;
    size_t raw_coeff_d2h_bytes = 0;
    size_t compressed_payload_d2h_bytes = 0;
    size_t final_band_stream_bytes = 0;
    double gpu_entropy_ms = 0.0;
    double cpu_encode_or_wrap_ms = 0.0;
};

struct Phase2BCoverageAudit {
    bool ok = false;
    int width = 0;
    int height = 0;
    int total_bands = 0;
    int gpu_entropy_bands = 0;
    int cpu_lowpass_bands = 0;
    int64_t lowpass_total_coeffs = 0;
    size_t lowpass_raw_d2h_bytes = 0;
    size_t lowpass_encoded_bytes = 0;
    int64_t highpass_total_coeffs = 0;
    size_t highpass_raw_equiv_bytes = 0;
    size_t highpass_compressed_d2h_bytes = 0;
    size_t highpass_encoded_bytes = 0;
    size_t total_d2h_bytes = 0;
    double lowpass_cpu_ms = 0.0;
    double highpass_header_cpu_ms = 0.0;
    double assembly_cpu_ms = 0.0;
    double gpu_wavelet_ms = 0.0;
    double gpu_entropy_ms = 0.0;
    double d2h_ms = 0.0;
    double total_frame_ms = 0.0;
    double lowpass_pct_of_frame = 0.0;
    std::vector<Phase2BSubbandAuditEntry> bands;
    std::string error;
};

Phase2BCoverageAudit AuditPhase2BCoverage(int width, int height, uint32_t seed = 0x1337BEEFu);

// Section 2: Explicit 5-level bitstream identity & SHA-256 verification.
struct Phase2CBandIdentity {
    int index = 0;
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    std::string name;
    bool lowpass = false;
    int width = 0;
    int height = 0;
    bool coeff_equal = false;
    bool payload_len_equal = false;
    bool payload_byte_equal = false;
    size_t cpu_payload_bytes = 0;
    size_t gpu_payload_bytes = 0;
    std::string coeff_sha256;
    std::string cpu_payload_sha256;
    std::string gpu_payload_sha256;
};

struct Phase2CIdentityReport {
    bool ok = false;
    int width = 0;
    int height = 0;
    bool pre_entropy_coeff_equal = false;
    bool per_band_payload_byte_equal = false;
    bool per_band_payload_len_equal = false;
    bool complete_sample_byte_equal = false;
    bool streamed_d2h_sample_byte_equal = false;
    bool multistream_sample_byte_equal = false;
    bool gpu_lowpass_sample_byte_equal = false;
    bool upstream_decoder_pixel_equal = false;
    int mismatch_offset = -1;
    size_t sample_bytes = 0;
    int decoded_max_abs_diff = 0;
    double psnr_vs_source_db = 0.0;
    std::string cpu_sample_sha256;
    std::string gpu_consolidated_sample_sha256;
    std::string gpu_streamed_sample_sha256;
    std::string gpu_lowpass_sample_sha256;
    std::string cpu_decoded_pixels_sha256;
    std::string gpu_decoded_pixels_sha256;
    std::vector<Phase2CBandIdentity> bands;
    std::string error;
};

Phase2CIdentityReport VerifyPhase2CIdentity(int width, int height, uint32_t seed = 0x1337BEEFu);

// Section 4: Isolated GPU entropy concurrency benchmark entry.
struct Phase2CConcurrencyPoint {
    std::string label;
    int streams = 1;
    CompressedD2HMode d2h_mode = CompressedD2HMode::FrameConsolidated;
    double mean_wall_ms = 0.0;
    double median_wall_ms = 0.0;
    double p95_wall_ms = 0.0;
    double gpu_entropy_ms = 0.0;
    double gpu_consolidate_ms = 0.0;
    double compressed_d2h_ms = 0.0;
    double speedup_vs_1stream = 1.0;
    int max_active_blocks_per_sm = 0;
    double effective_gbps = 0.0;
    EntropyTiming substage_sum;
    bool byte_match = false;
};

std::vector<Phase2CConcurrencyPoint> BenchmarkPhase2CEntropyConcurrency(int width, int height,
                                                                        int iterations = 8,
                                                                        uint32_t seed = 0x1337BEEFu);

// Sections 3, 5, 6, 8, 9, 10, 11: Multi-frame pipelined GPU entropy encoder.
enum class Phase2CInputMode {
    HostInput = 0,    // HOST_INPUT_GPU_ENTROPY_PIPE
    DeviceInput = 1   // DEVICE_INPUT_GPU_ENTROPY_PIPE
};

struct Phase2CConfig {
    int width = 1920;
    int height = 1080;
    int frames = 12;
    int warmup_frames = 2;
    int frame_slots = 3;
    int entropy_streams = 4;
    CompressedD2HMode d2h_mode = CompressedD2HMode::FrameConsolidated;
    Phase2CInputMode input_mode = Phase2CInputMode::DeviceInput;
    bool gpu_pack_lowpass = false;
    bool record_substage_events = false;
    bool verify_bitstream = true;
    bool async_cpu_assembly = true;
    bool deterministic_test_metadata = false;
    uint32_t seed = 0x1337BEEFu;
};

struct Phase2CResult {
    bool ok = false;
    Phase2CConfig config;
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

    // Per-frame stage breakdown (averages over measured frames).
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

    double cpu_lowpass_ms = 0.0;
    double cpu_header_wrap_ms = 0.0;
    double cpu_sample_concat_ms = 0.0;
    double cpu_assembly_total_ms = 0.0;
    double cpu_dispatch_poll_ms = 0.0;
    double total_cpu_ms = 0.0;

    EntropyTiming entropy_substage_sum;

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
    double cpu_utilization_pct = 0.0;
    double avg_frames_in_flight = 0.0;

    // Quality & verification.
    bool bitstream_byte_match = false;
    bool decode_ok = false;
    double psnr_db = 0.0;
    std::string sample_sha256;
    std::vector<uint8_t> last_bitstream;
    std::string error;
};

Phase2CResult RunPhase2C(const Phase2CConfig& cfg);

}  // namespace hypercine

#endif
