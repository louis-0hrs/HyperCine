// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "gpu_diagnostic_report.h"

#include <cerrno>
#include <cstdio>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hypercine {
namespace diag {
namespace {

std::string Escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(static_cast<unsigned char>(ch));
                    out += escaped.str();
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

void WriteNullableU64(std::ostringstream& json, bool available, uint64_t value) {
    if (available) {
        json << value;
    } else {
        json << "null";
    }
}

// A device path ("NUL" on Windows, "/dev/null" on POSIX) accepts writes and
// discards them, so a successful fopen/fwrite does not prove an artifact
// exists. Only a regular file counts as evidence.
bool StreamIsRegularFile(std::FILE* file) {
#if defined(_WIN32)
    struct _stat64 info {};
    if (_fstat64(_fileno(file), &info) != 0) return false;
    return (info.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat info {};
    if (fstat(fileno(file), &info) != 0) return false;
    return S_ISREG(info.st_mode);
#endif
}

}  // namespace

std::string BuildGpuDiagnosticJson(const GpuDiagnosticReport& report) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"schema\": \"hypercine.gpu_diagnostic\",\n";
    json << "  \"schema_version\": " << report.schema_version << ",\n";
    json << "  \"status\": \"" << Escape(report.status) << "\",\n";
    json << "  \"ok\": " << (report.ok ? "true" : "false") << ",\n";

    json << "  \"provenance\": {\n";
    json << "    \"source_commit\": \"" << Escape(report.source_commit) << "\",\n";
    json << "    \"source_dirty\": " << (report.source_dirty ? "true" : "false") << ",\n";
    json << "    \"compiler\": \"" << Escape(report.compiler) << "\",\n";
    json << "    \"build_type\": \"" << Escape(report.build_type) << "\",\n";
    json << "    \"cuda_compiler_version\": \"" << Escape(report.cuda_compiler_version) << "\",\n";
    json << "    \"cuda_runtime_version\": " << report.cuda_runtime_version << ",\n";
    json << "    \"cuda_driver_version\": " << report.cuda_driver_version << ",\n";
    json << "    \"backend\": \"" << Escape(report.backend) << "\"\n";
    json << "  },\n";

    json << "  \"device\": {\n";
    json << "    \"available\": " << (report.device_available ? "true" : "false") << ",\n";
    json << "    \"name\": ";
    if (report.device_available) {
        json << "\"" << Escape(report.device_name) << "\",\n";
    } else {
        json << "null,\n";
    }
    json << "    \"compute_capability\": ";
    if (report.device_available) {
        json << "\"" << report.compute_major << "." << report.compute_minor << "\",\n";
    } else {
        json << "null,\n";
    }
    json << "    \"multiprocessor_count\": ";
    WriteNullableU64(json, report.device_available,
                     static_cast<uint64_t>(report.multiprocessor_count));
    json << ",\n";
    json << "    \"total_vram_bytes\": ";
    WriteNullableU64(json, report.device_available, report.device_total_vram_bytes);
    json << "\n";
    json << "  },\n";

    json << "  \"workload\": {\n";
    json << "    \"width\": " << report.width << ",\n";
    json << "    \"height\": " << report.height << ",\n";
    json << "    \"frames\": " << report.frames << ",\n";
    json << "    \"warmup_frames\": " << report.warmup_frames << ",\n";
    json << "    \"input_mode\": \"" << Escape(report.input_mode) << "\",\n";
    json << "    \"encoding_quality\": \"" << Escape(report.encoding_quality) << "\",\n";
    json << "    \"requested_slots\": " << report.requested_slots << ",\n";
    json << "    \"slot_count\": " << report.slot_count << "\n";
    json << "  },\n";

    json << "  \"memory\": {\n";
    json << "    \"cuda_mem_get_info\": [\n";
    for (size_t i = 0; i < report.cuda_mem_get_info.size(); ++i) {
        const CudaMemInfoSnapshot& snap = report.cuda_mem_get_info[i];
        json << "      {\n";
        json << "        \"label\": \"" << Escape(snap.label) << "\",\n";
        json << "        \"available\": " << (snap.available ? "true" : "false") << ",\n";
        json << "        \"free_bytes\": ";
        WriteNullableU64(json, snap.available, snap.free_bytes);
        json << ",\n";
        json << "        \"total_bytes\": ";
        WriteNullableU64(json, snap.available, snap.total_bytes);
        json << ",\n";
        json << "        \"used_bytes\": ";
        WriteNullableU64(json, snap.available, snap.used_bytes);
        json << ",\n";
        json << "        \"unavailable_reason\": ";
        if (snap.available) {
            json << "null\n";
        } else {
            json << "\"" << Escape(snap.unavailable_reason) << "\"\n";
        }
        json << "      }";
        if (i + 1 < report.cuda_mem_get_info.size()) json << ",";
        json << "\n";
    }
    json << "    ],\n";

    json << "    \"result_vram_used_snapshots\": [\n";
    for (size_t i = 0; i < report.result_vram_used_snapshots.size(); ++i) {
        const VramUsedSnapshot& snap = report.result_vram_used_snapshots[i];
        json << "      {\"label\": \"" << Escape(snap.label)
             << "\", \"used_bytes\": " << snap.used_bytes << "}";
        if (i + 1 < report.result_vram_used_snapshots.size()) json << ",";
        json << "\n";
    }
    json << "    ],\n";
    json << "    \"measured_vram_delta_bytes\": ";
    WriteNullableU64(json, report.measured_vram_delta_available,
                     report.measured_vram_delta_bytes);
    json << ",\n";
    json << "    \"measured_peak_device_delta_bytes\": ";
    WriteNullableU64(json, report.measured_peak_device_delta_available,
                     report.measured_peak_device_delta_bytes);
    json << ",\n";
    json << "    \"estimated\": {\n";
    json << "      \"per_slot_vram_bytes\": " << report.per_slot_estimated_vram_bytes << ",\n";
    json << "      \"session_workspace_vram_bytes\": "
         << report.session_workspace_estimated_vram_bytes << ",\n";
    json << "      \"total_vram_bytes\": " << report.total_estimated_vram_bytes << "\n";
    json << "    },\n";
    json << "    \"slot_vram_bytes\": " << report.slot_vram_bytes << ",\n";
    json << "    \"slot_pinned_host_bytes\": " << report.slot_pinned_host_bytes << ",\n";
    json << "    \"host_rss_before_bytes\": " << report.host_rss_before_bytes << ",\n";
    json << "    \"host_rss_peak_observed_bytes\": " << report.host_rss_peak_observed_bytes
         << "\n";
    json << "  },\n";

    json << "  \"failure\": {\n";
    json << "    \"message\": ";
    if (report.status == "PASSED") {
        json << "null,\n";
    } else {
        json << "\"" << Escape(report.error_message) << "\",\n";
    }
    json << "    \"failing_operation\": ";
    if (report.failing_operation.empty()) {
        json << "null,\n";
    } else {
        json << "\"" << Escape(report.failing_operation) << "\",\n";
    }
    json << "    \"cuda_error\": {\n";
    json << "      \"code\": ";
    WriteNullableU64(json, report.cuda_error_available,
                     static_cast<uint64_t>(report.cuda_error_code));
    json << ",\n";
    json << "      \"name\": ";
    if (!report.cuda_error_available || report.cuda_error_name.empty()) {
        json << "null,\n";
    } else {
        json << "\"" << Escape(report.cuda_error_name) << "\",\n";
    }
    json << "      \"description\": ";
    if (!report.cuda_error_available || report.cuda_error_description.empty()) {
        json << "null\n";
    } else {
        json << "\"" << Escape(report.cuda_error_description) << "\"\n";
    }
    json << "    }\n";
    json << "  },\n";

    json << "  \"unavailable_metrics\": [\n";
    for (size_t i = 0; i < report.unavailable_metrics.size(); ++i) {
        json << "    {\"metric\": \"" << Escape(report.unavailable_metrics[i].metric)
             << "\", \"reason\": \"" << Escape(report.unavailable_metrics[i].reason) << "\"}";
        if (i + 1 < report.unavailable_metrics.size()) json << ",";
        json << "\n";
    }
    json << "  ]\n";
    json << "}\n";
    return json.str();
}

bool WriteGpuDiagnosticReportFile(const GpuDiagnosticReport& report,
                                  const std::string& out_path,
                                  ReportWritePolicy policy, std::string* error) {
    if (out_path.empty()) {
        *error = "output path is empty";
        return false;
    }

    // "x" is the C11 exclusive-create mode: the call fails instead of
    // truncating an existing file. Both the UCRT and POSIX fopen support it.
    const char* const mode =
        (policy == ReportWritePolicy::kCreateNew) ? "wbx" : "wb";
    errno = 0;
    std::FILE* file = std::fopen(out_path.c_str(), mode);
    if (file == nullptr) {
        if (policy == ReportWritePolicy::kCreateNew && errno == EEXIST) {
            *error = "output path already exists; pass --overwrite to replace it";
        } else {
            *error = "failed to open output path";
        }
        return false;
    }

    if (!StreamIsRegularFile(file)) {
        std::fclose(file);
        // Never remove here: with kOverwrite the path may be an existing
        // device node, and the caller did not ask for it to be deleted.
        *error = "output path is not a regular file";
        return false;
    }

    const std::string json = BuildGpuDiagnosticJson(report);
    bool ok = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    if (ok && std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        *error = "failed to write output file";
        // Never leave a truncated sidecar behind: a partial report is not
        // evidence, and a caller could mistake it for one.
        std::remove(out_path.c_str());
        return false;
    }
    return true;
}

}  // namespace diag
}  // namespace hypercine
