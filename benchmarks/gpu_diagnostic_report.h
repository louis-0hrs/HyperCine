// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_GPU_DIAGNOSTIC_REPORT_H
#define HYPERCINE_GPU_DIAGNOSTIC_REPORT_H

#include <cstdint>
#include <string>
#include <vector>

namespace hypercine {
namespace diag {

// Versioned diagnostic sidecar for a bounded Phase3A run. Field names reuse
// the existing Phase3A vocabulary; estimated values are never presented as
// measured ones.
constexpr int kGpuDiagnosticSchemaVersion = 1;

struct CudaMemInfoSnapshot {
    std::string label;
    bool available = false;
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    std::string unavailable_reason;
};

struct VramUsedSnapshot {
    std::string label;
    uint64_t used_bytes = 0;
};

struct UnavailableMetric {
    std::string metric;
    std::string reason;
};

struct GpuDiagnosticReport {
    int schema_version = kGpuDiagnosticSchemaVersion;
    bool ok = false;
    std::string status = "INVALID_ARGUMENTS";

    std::string source_commit;
    bool source_dirty = false;
    std::string compiler;
    std::string build_type;
    std::string cuda_compiler_version;
    int cuda_runtime_version = 0;
    int cuda_driver_version = 0;
    std::string backend = "cuda";

    bool device_available = false;
    std::string device_name;
    int compute_major = 0;
    int compute_minor = 0;
    int multiprocessor_count = 0;
    uint64_t device_total_vram_bytes = 0;

    int width = 0;
    int height = 0;
    int frames = 0;
    int warmup_frames = 0;
    std::string input_mode;
    std::string encoding_quality;
    int requested_slots = 0;
    int slot_count = 0;

    std::vector<CudaMemInfoSnapshot> cuda_mem_get_info;
    std::vector<VramUsedSnapshot> result_vram_used_snapshots;
    bool measured_vram_delta_available = false;
    uint64_t measured_vram_delta_bytes = 0;
    bool measured_peak_device_delta_available = false;
    uint64_t measured_peak_device_delta_bytes = 0;
    uint64_t per_slot_estimated_vram_bytes = 0;
    uint64_t session_workspace_estimated_vram_bytes = 0;
    uint64_t total_estimated_vram_bytes = 0;
    uint64_t slot_vram_bytes = 0;
    uint64_t slot_pinned_host_bytes = 0;
    uint64_t host_rss_before_bytes = 0;
    uint64_t host_rss_peak_observed_bytes = 0;

    std::string error_message;
    std::string failing_operation;
    bool cuda_error_available = false;
    int cuda_error_code = 0;
    std::string cuda_error_name;
    std::string cuda_error_description;

    std::vector<UnavailableMetric> unavailable_metrics;
};

std::string BuildGpuDiagnosticJson(const GpuDiagnosticReport& report);

enum class ReportWritePolicy {
    // Fail closed: refuse to touch a path that already exists.
    kCreateNew,
    // Explicit opt-in replacement of an existing path.
    kOverwrite,
};

// Writes the versioned JSON payload to out_path.
//
// With kCreateNew the file is created exclusively, so a pre-existing artifact
// is never truncated. If writing fails after the file was created, the file is
// removed again: a partially written report must not survive as evidence.
bool WriteGpuDiagnosticReportFile(const GpuDiagnosticReport& report,
                                  const std::string& out_path,
                                  ReportWritePolicy policy,
                                  std::string* error);

}  // namespace diag
}  // namespace hypercine

#endif  // HYPERCINE_GPU_DIAGNOSTIC_REPORT_H
