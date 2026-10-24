// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "gpu_diagnostic_report.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using hypercine::diag::BuildGpuDiagnosticJson;
using hypercine::diag::CudaMemInfoSnapshot;
using hypercine::diag::GpuDiagnosticReport;
using hypercine::diag::ReportWritePolicy;
using hypercine::diag::UnavailableMetric;
using hypercine::diag::VramUsedSnapshot;
using hypercine::diag::WriteGpuDiagnosticReportFile;

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

GpuDiagnosticReport PassingReport() {
    GpuDiagnosticReport report;
    report.status = "PASSED";
    report.ok = true;
    report.source_commit = "0123456789abcdef";
    report.compiler = "MSVC 19.44";
    report.build_type = "Release";
    report.cuda_compiler_version = "12.8.93";
    report.cuda_runtime_version = 12080;
    report.cuda_driver_version = 13000;
    report.device_available = true;
    report.device_name = "Tesla M4";
    report.compute_major = 5;
    report.compute_minor = 2;
    report.multiprocessor_count = 6;
    report.device_total_vram_bytes = 4028690432ull;
    report.width = 1920;
    report.height = 1080;
    report.frames = 4;
    report.warmup_frames = 2;
    report.input_mode = "HostPageableEndToEnd";
    report.encoding_quality = "filmscan3";
    report.slot_count = 2;

    CudaMemInfoSnapshot available;
    available.label = "after_probe_context_init";
    available.available = true;
    available.free_bytes = 100;
    available.total_bytes = 300;
    available.used_bytes = 200;
    report.cuda_mem_get_info.push_back(available);

    CudaMemInfoSnapshot unavailable;
    unavailable.label = "internal_boundary";
    unavailable.available = false;
    unavailable.unavailable_reason = "sampling skipped";
    report.cuda_mem_get_info.push_back(unavailable);

    report.result_vram_used_snapshots.push_back({"after_init", 50});
    report.result_vram_used_snapshots.push_back({"after_destroy", 0});
    report.measured_vram_delta_available = true;
    report.measured_vram_delta_bytes = 50;
    report.measured_peak_device_delta_available = true;
    report.measured_peak_device_delta_bytes = 250;
    report.per_slot_estimated_vram_bytes = 10;
    report.session_workspace_estimated_vram_bytes = 20;
    report.total_estimated_vram_bytes = 30;
    report.unavailable_metrics.push_back({"allocator_high_water_bytes", "no hooks"});
    return report;
}

void PassingReportContract() {
    const std::string json = BuildGpuDiagnosticJson(PassingReport());
    Check(Contains(json, "\"schema\": \"hypercine.gpu_diagnostic\""), "schema name");
    Check(Contains(json, "\"schema_version\": 1"), "schema version");
    Check(Contains(json, "\"status\": \"PASSED\""), "status");
    Check(Contains(json, "\"available\": true"), "available snapshot");
    Check(Contains(json, "\"free_bytes\": 100"), "free bytes");
    Check(Contains(json, "\"used_bytes\": 200"), "used bytes");
    Check(Contains(json, "\"available\": false"), "unavailable snapshot");
    Check(Contains(json, "\"free_bytes\": null"), "unavailable bytes are null");
    Check(Contains(json, "\"unavailable_reason\": \"sampling skipped\""), "reason");
    Check(Contains(json, "\"measured_vram_delta_bytes\": 50"), "measured delta");
    Check(Contains(json, "\"measured_peak_device_delta_bytes\": 250"), "measured peak delta");
    Check(Contains(json, "\"per_slot_vram_bytes\": 10"), "estimated per slot");
    Check(Contains(json, "\"session_workspace_vram_bytes\": 20"), "estimated workspace");
    Check(Contains(json, "\"total_vram_bytes\": 30"), "estimated total");
    Check(Contains(json, "\"message\": null"), "no failure message");
    Check(Contains(json, "\"code\": null"), "no CUDA error code");
    Check(Contains(json, "\"failing_operation\": null"), "no failing operation");
    Check(Contains(json, "\"metric\": \"allocator_high_water_bytes\""), "unavailable metric");
    Check(!Contains(json, "\"free_bytes\": 0"), "unavailable bytes must not be zero");
    Check(BuildGpuDiagnosticJson(PassingReport()) == json, "deterministic output");
}

void FailureReportContract() {
    GpuDiagnosticReport report = PassingReport();
    report.ok = false;
    report.status = "FAILED";
    report.error_message = "run failed at frame \"3\"\\path";
    report.cuda_error_available = true;
    report.cuda_error_code = 2;
    report.cuda_error_name = "cudaErrorMemoryAllocation";
    report.cuda_error_description = "out of memory";
    report.unavailable_metrics.push_back({"failing_operation", "not exposed"});
    const std::string json = BuildGpuDiagnosticJson(report);
    Check(Contains(json, "\"status\": \"FAILED\""), "failed status");
    Check(Contains(json, "\"code\": 2"), "CUDA error code");
    Check(Contains(json, "\"name\": \"cudaErrorMemoryAllocation\""), "CUDA error name");
    Check(Contains(json, "\"description\": \"out of memory\""), "CUDA error description");
    Check(Contains(json, "run failed at frame \\\"3\\\"\\\\path"), "message escaping");
    Check(Contains(json, "\"metric\": \"failing_operation\""), "failing operation metric");
    Check(Contains(json, "\"available\": true"), "partial memory evidence retained");
}

void UnavailableMeasuredDeltaContract() {
    GpuDiagnosticReport report = PassingReport();
    report.measured_vram_delta_available = false;
    report.measured_peak_device_delta_available = false;
    const std::string json = BuildGpuDiagnosticJson(report);
    Check(Contains(json, "\"measured_vram_delta_bytes\": null"),
          "unavailable measured delta must be null");
    Check(Contains(json, "\"measured_peak_device_delta_bytes\": null"),
          "unavailable measured peak must be null");
}

void InvalidArgumentsContract() {
    GpuDiagnosticReport report;
    report.status = "INVALID_ARGUMENTS";
    report.error_message = "invalid --width";
    const std::string json = BuildGpuDiagnosticJson(report);
    Check(Contains(json, "\"status\": \"INVALID_ARGUMENTS\""), "invalid status");
    Check(Contains(json, "\"message\": \"invalid --width\""), "invalid message");
    Check(Contains(json, "\"code\": null"), "invalid run has no CUDA code");
}

class TempDirectory {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("hypercine_gpu_diagnostic_" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    std::string File(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteSentinel(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

constexpr const char* kSentinel = "sentinel bytes that must survive\n";

void CreateNewRefusesToOverwrite() {
    TempDirectory dir;
    const std::string path = dir.File("report.json");
    WriteSentinel(path, kSentinel);
    std::string error;
    Check(!WriteGpuDiagnosticReportFile(PassingReport(), path, ReportWritePolicy::kCreateNew,
                                        &error),
          "create-new must refuse an existing path");
    Check(Contains(error, "already exists"), "refusal must name the existing path");
    Check(ReadFile(path) == kSentinel, "existing artifact must stay byte-identical");
}

void CreateNewWritesFreshFile() {
    TempDirectory dir;
    const std::string path = dir.File("report.json");
    std::string error;
    Check(WriteGpuDiagnosticReportFile(PassingReport(), path, ReportWritePolicy::kCreateNew,
                                       &error),
          "create-new must write a fresh path");
    Check(ReadFile(path) == BuildGpuDiagnosticJson(PassingReport()),
          "written payload must equal the built JSON");
}

void OverwriteReplacesExisting() {
    TempDirectory dir;
    const std::string path = dir.File("report.json");
    WriteSentinel(path, kSentinel);
    std::string error;
    Check(WriteGpuDiagnosticReportFile(PassingReport(), path, ReportWritePolicy::kOverwrite,
                                       &error),
          "explicit overwrite must succeed");
    const std::string written = ReadFile(path);
    Check(written != kSentinel, "explicit overwrite must replace the sentinel");
    Check(Contains(written, "\"schema\": \"hypercine.gpu_diagnostic\""),
          "overwritten payload must be a diagnostic report");
}

void EmptyPathIsRejected() {
    std::string error;
    Check(!WriteGpuDiagnosticReportFile(PassingReport(), "", ReportWritePolicy::kCreateNew,
                                        &error),
          "empty output path must be rejected");
    Check(Contains(error, "empty"), "empty-path error");
}

void MissingParentLeavesNoFile() {
    TempDirectory dir;
    const std::string path = dir.File("missing/report.json");
    std::string error;
    Check(!WriteGpuDiagnosticReportFile(PassingReport(), path, ReportWritePolicy::kCreateNew,
                                        &error),
          "missing parent directory must fail");
    Check(!std::filesystem::exists(path), "no partial artifact may be created");
}

void DirectoryIsNeverReplaced() {
    TempDirectory dir;
    const std::string path = dir.File("existing-directory");
    std::filesystem::create_directories(path);
    std::string error;
    Check(!WriteGpuDiagnosticReportFile(PassingReport(), path, ReportWritePolicy::kOverwrite,
                                        &error),
          "a directory must never be replaced by a report");
    Check(std::filesystem::is_directory(path), "existing directory must survive");
}

}  // namespace

int main() {
    try {
        PassingReportContract();
        FailureReportContract();
        UnavailableMeasuredDeltaContract();
        InvalidArgumentsContract();
        CreateNewRefusesToOverwrite();
        CreateNewWritesFreshFile();
        OverwriteReplacesExisting();
        EmptyPathIsRejected();
        MissingParentLeavesNoFile();
        DirectoryIsNeverReplaced();
    } catch (const std::exception& error) {
        std::cerr << "gpu diagnostic report FAIL: " << error.what() << "\n";
        return 1;
    }
    std::cout << "gpu diagnostic report PASS\n";
    return 0;
}
