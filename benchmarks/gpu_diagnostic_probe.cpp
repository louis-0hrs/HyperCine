// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Bounded opt-in diagnostic probe for the Phase3A GPU path. It records
// provenance, cudaMemGetInfo snapshots and exact CUDA error context around a
// short RunPhase3A call. It is not part of the benchmark headline contract and
// does not change codec behavior.

#include "gpu_diagnostic_report.h"

#include "hypercine/phase3a.h"
#include "hypercine/quality_state.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using hypercine::diag::CudaMemInfoSnapshot;
using hypercine::diag::GpuDiagnosticReport;
using hypercine::diag::UnavailableMetric;
using hypercine::diag::VramUsedSnapshot;

struct Options {
    int width = 1920;
    int height = 1080;
    int frames = 4;
    int warmup_frames = 2;
    int requested_slots = 0;
    hypercine::Phase3AInputMode input_mode = hypercine::Phase3AInputMode::HostPageableEndToEnd;
    hypercine::EncodingQuality quality = hypercine::EncodingQuality::FilmScan3;
    std::string out_path;
};

bool ParseInt(const std::string& text, int* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    *value = static_cast<int>(parsed);
    return true;
}

bool ParseInputMode(const std::string& text, hypercine::Phase3AInputMode* mode) {
    if (text == "HostPageableEndToEnd") {
        *mode = hypercine::Phase3AInputMode::HostPageableEndToEnd;
    } else if (text == "HostPinnedApi") {
        *mode = hypercine::Phase3AInputMode::HostPinnedApi;
    } else if (text == "DeviceInput") {
        *mode = hypercine::Phase3AInputMode::DeviceInput;
    } else {
        return false;
    }
    return true;
}

bool ParseQuality(const std::string& text, hypercine::EncodingQuality* quality) {
    for (hypercine::EncodingQuality candidate :
         {hypercine::EncodingQuality::Low, hypercine::EncodingQuality::Medium,
          hypercine::EncodingQuality::High, hypercine::EncodingQuality::FilmScan1,
          hypercine::EncodingQuality::FilmScan2, hypercine::EncodingQuality::FilmScan3}) {
        if (text == hypercine::EncodingQualityName(candidate)) {
            *quality = candidate;
            return true;
        }
    }
    return false;
}

CudaMemInfoSnapshot TakeMemInfo(const std::string& label) {
    CudaMemInfoSnapshot snap;
    snap.label = label;
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    const cudaError_t status = cudaMemGetInfo(&free_bytes, &total_bytes);
    if (status != cudaSuccess) {
        snap.available = false;
        snap.unavailable_reason =
            std::string("cudaMemGetInfo failed: ") + cudaGetErrorName(status);
        return snap;
    }
    snap.available = true;
    snap.free_bytes = static_cast<uint64_t>(free_bytes);
    snap.total_bytes = static_cast<uint64_t>(total_bytes);
    snap.used_bytes = (snap.total_bytes >= snap.free_bytes)
                          ? (snap.total_bytes - snap.free_bytes)
                          : 0;
    return snap;
}

void AddResultSnapshot(GpuDiagnosticReport* report, const char* label, size_t used_bytes,
                       bool run_ok) {
    if (run_ok || used_bytes != 0) {
        report->result_vram_used_snapshots.push_back({label, static_cast<uint64_t>(used_bytes)});
    }
}

void FillProvenance(GpuDiagnosticReport* report) {
    report->source_commit = HYPERCINE_SOURCE_COMMIT;
    report->source_dirty = HYPERCINE_SOURCE_DIRTY;
    report->compiler =
        std::string(HYPERCINE_COMPILER_ID) + " " + std::string(HYPERCINE_COMPILER_VERSION);
    report->build_type = HYPERCINE_BUILD_TYPE;
    report->cuda_compiler_version = HYPERCINE_CUDA_COMPILER_VERSION;
    int runtime_version = 0;
    int driver_version = 0;
    if (cudaRuntimeGetVersion(&runtime_version) == cudaSuccess) {
        report->cuda_runtime_version = runtime_version;
    }
    if (cudaDriverGetVersion(&driver_version) == cudaSuccess) {
        report->cuda_driver_version = driver_version;
    }
}

bool WriteReport(const GpuDiagnosticReport& report, const std::string& out_path,
                 std::string* error) {
    if (out_path.empty()) {
        *error = "--out is required";
        return false;
    }
    std::ofstream file(out_path, std::ios::binary);
    if (!file) {
        *error = "failed to open output path";
        return false;
    }
    file << hypercine::diag::BuildGpuDiagnosticJson(report);
    if (!file) {
        *error = "failed to write output file";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string parse_error;

    for (int i = 1; i < argc && parse_error.empty(); ++i) {
        const std::string arg = argv[i];
        auto value_of = [&](const char* name) -> std::string {
            const std::string prefix = std::string(name) + "=";
            if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
            if (arg == name && i + 1 < argc) return std::string(argv[++i]);
            return std::string();
        };
        const std::string out = value_of("--out");
        if (!out.empty()) {
            opt.out_path = out;
        } else if (arg.rfind("--width", 0) == 0) {
            int v = 0;
            if (!ParseInt(value_of("--width"), &v)) parse_error = "invalid --width";
            else opt.width = v;
        } else if (arg.rfind("--height", 0) == 0) {
            int v = 0;
            if (!ParseInt(value_of("--height"), &v)) parse_error = "invalid --height";
            else opt.height = v;
        } else if (arg.rfind("--frames", 0) == 0) {
            int v = 0;
            if (!ParseInt(value_of("--frames"), &v)) parse_error = "invalid --frames";
            else opt.frames = v;
        } else if (arg.rfind("--warmup", 0) == 0) {
            int v = 0;
            if (!ParseInt(value_of("--warmup"), &v)) parse_error = "invalid --warmup";
            else opt.warmup_frames = v;
        } else if (arg.rfind("--slots", 0) == 0) {
            int v = 0;
            if (!ParseInt(value_of("--slots"), &v)) parse_error = "invalid --slots";
            else opt.requested_slots = v;
        } else if (arg.rfind("--input-mode", 0) == 0) {
            if (!ParseInputMode(value_of("--input-mode"), &opt.input_mode)) {
                parse_error = "invalid --input-mode";
            }
        } else if (arg.rfind("--quality", 0) == 0) {
            if (!ParseQuality(value_of("--quality"), &opt.quality)) {
                parse_error = "invalid --quality";
            }
        } else if (arg.rfind("--out", 0) == 0) {
            parse_error = "missing value for --out";
        } else {
            parse_error = "unknown argument";
        }
    }

    GpuDiagnosticReport report;
    report.width = opt.width;
    report.height = opt.height;
    report.frames = opt.frames;
    report.warmup_frames = opt.warmup_frames;
    report.requested_slots = opt.requested_slots;
    report.input_mode = hypercine::Phase3AInputModeName(opt.input_mode);
    report.encoding_quality = hypercine::EncodingQualityName(opt.quality);
    FillProvenance(&report);

    auto finish = [&](int exit_code) {
        std::string write_error;
        if (!WriteReport(report, opt.out_path, &write_error)) {
            std::cerr << "gpu_diagnostic_probe: " << write_error << "\n";
            return 3;
        }
        std::cout << "gpu_diagnostic_probe: status=" << report.status
                  << " out=" << opt.out_path << "\n";
        return exit_code;
    };

    if (parse_error.empty() && (opt.width <= 0 || opt.height <= 0 || opt.frames <= 0 ||
                                opt.warmup_frames < 0 || opt.requested_slots < 0 ||
                                opt.requested_slots > 4)) {
        parse_error = "invalid geometry, frame count, or slot request";
    }
    if (!parse_error.empty()) {
        report.status = "INVALID_ARGUMENTS";
        report.error_message = parse_error;
        return finish(2);
    }

    int device_count = 0;
    const cudaError_t count_status = cudaGetDeviceCount(&device_count);
    if (count_status != cudaSuccess || device_count <= 0) {
        report.status = "FAILED";
        report.error_message = (count_status != cudaSuccess)
                                   ? "cudaGetDeviceCount failed"
                                   : "no CUDA device is available";
        if (count_status != cudaSuccess) {
            report.cuda_error_available = true;
            report.cuda_error_code = static_cast<int>(count_status);
            report.cuda_error_name = cudaGetErrorName(count_status);
            report.cuda_error_description = cudaGetErrorString(count_status);
        }
        report.unavailable_metrics.push_back(
            {"device", "cudaGetDeviceCount returned no usable device"});
        return finish(3);
    }

    const cudaError_t select_status = cudaSetDevice(0);
    if (select_status != cudaSuccess) {
        report.status = "FAILED";
        report.error_message = "cudaSetDevice(0) failed";
        report.cuda_error_available = true;
        report.cuda_error_code = static_cast<int>(select_status);
        report.cuda_error_name = cudaGetErrorName(select_status);
        report.cuda_error_description = cudaGetErrorString(select_status);
        return finish(3);
    }

    // Context creation is attributed explicitly: the first snapshot is taken
    // after the probe's own context initialization, not before it.
    const cudaError_t init_status = cudaFree(nullptr);
    report.cuda_mem_get_info.push_back(TakeMemInfo("after_probe_context_init"));
    if (init_status != cudaSuccess) {
        report.status = "FAILED";
        report.error_message = "probe CUDA context initialization failed";
        report.cuda_error_available = true;
        report.cuda_error_code = static_cast<int>(init_status);
        report.cuda_error_name = cudaGetErrorName(init_status);
        report.cuda_error_description = cudaGetErrorString(init_status);
        return finish(3);
    }

    cudaDeviceProp properties{};
    const cudaError_t prop_status = cudaGetDeviceProperties(&properties, 0);
    if (prop_status == cudaSuccess) {
        report.device_available = true;
        report.device_name = properties.name;
        report.compute_major = properties.major;
        report.compute_minor = properties.minor;
        report.multiprocessor_count = properties.multiProcessorCount;
        report.device_total_vram_bytes = static_cast<uint64_t>(properties.totalGlobalMem);
    } else {
        report.unavailable_metrics.push_back(
            {"device_properties", std::string("cudaGetDeviceProperties failed: ") +
                                      cudaGetErrorName(prop_status)});
    }

    hypercine::Phase3AConfig config;
    config.width = opt.width;
    config.height = opt.height;
    config.frames = opt.frames;
    config.warmup_frames = opt.warmup_frames;
    config.input_mode = opt.input_mode;
    config.encoding_quality = opt.quality;
    config.user_override_slots = opt.requested_slots;
    if (opt.requested_slots > 0) {
        config.slot_policy = hypercine::Phase2DSlotPolicy::UserOverride;
    }

    const hypercine::Phase3AResult result = hypercine::RunPhase3A(config);
    report.cuda_mem_get_info.push_back(TakeMemInfo("after_run"));
    report.ok = result.ok;
    report.status = result.ok ? "PASSED" : "FAILED";
    report.slot_count = result.slot_selection.selected_slots;
    report.per_slot_estimated_vram_bytes = result.slot_selection.per_slot_vram_bytes;
    report.session_workspace_estimated_vram_bytes =
        result.slot_selection.session_workspace_vram_bytes;
    report.total_estimated_vram_bytes = result.slot_selection.total_estimated_vram_bytes;
    report.slot_vram_bytes = result.slot_vram_bytes;
    report.slot_pinned_host_bytes = result.slot_pinned_host_bytes;
    report.host_rss_before_bytes = result.host_rss_before_bytes;
    report.host_rss_peak_observed_bytes =
        std::max({result.host_rss_before_bytes, result.host_rss_mid_bytes,
                  result.host_rss_end_bytes, result.host_rss_after_destroy_bytes});

    AddResultSnapshot(&report, "before_alloc", result.vram_before_bytes, result.ok);
    AddResultSnapshot(&report, "after_init", result.vram_after_init_bytes, result.ok);
    AddResultSnapshot(&report, "mid_run", result.vram_mid_run_bytes, result.ok);
    AddResultSnapshot(&report, "end_run", result.vram_end_run_bytes, result.ok);
    AddResultSnapshot(&report, "after_destroy", result.vram_after_destroy_bytes, result.ok);
    if (result.ok && result.vram_after_init_bytes > 0) {
        report.measured_vram_delta_available = true;
        report.measured_vram_delta_bytes = result.vram_used_bytes;
    }
    if (result.ok && !report.result_vram_used_snapshots.empty()) {
        const uint64_t baseline = report.result_vram_used_snapshots.front().used_bytes;
        uint64_t peak = baseline;
        for (const VramUsedSnapshot& snap : report.result_vram_used_snapshots) {
            peak = std::max(peak, snap.used_bytes);
        }
        if (baseline > 0 && peak >= baseline) {
            report.measured_peak_device_delta_available = true;
            report.measured_peak_device_delta_bytes = peak - baseline;
        }
    }

    report.unavailable_metrics.push_back(
        {"process_owned_high_water_bytes",
         "cudaMemGetInfo reports device-wide usage; allocator hooks are required "
         "for process-owned attribution"});
    report.unavailable_metrics.push_back(
        {"internal_boundary_free_total_bytes",
         "cudaMemGetInfo is sampled before and after the run; internal Phase3A "
         "boundaries record used bytes only"});
    report.unavailable_metrics.push_back(
        {"nvml_per_counter_validity",
         "this probe does not collect NVML; per-counter availability is owned by "
         "the telemetry adapter"});

    if (!result.ok) {
        report.error_message = result.error;
        report.unavailable_metrics.push_back(
            {"failing_operation",
             "RunPhase3A does not expose the failing CUDA call site"});
        const cudaError_t sync_status = cudaDeviceSynchronize();
        const cudaError_t last_status = cudaGetLastError();
        const cudaError_t cuda_status =
            (last_status != cudaSuccess) ? last_status : sync_status;
        if (cuda_status != cudaSuccess) {
            report.cuda_error_available = true;
            report.cuda_error_code = static_cast<int>(cuda_status);
            report.cuda_error_name = cudaGetErrorName(cuda_status);
            report.cuda_error_description = cudaGetErrorString(cuda_status);
        }
    }

    return finish(result.ok ? 0 : 3);
}
