// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#ifndef _ALLOCATOR
#define _ALLOCATOR 1
#endif

#include "CFHDEncoder.h"

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
}

#include "VideoBuffers.h"
#include "SampleMetadata.h"

#define private public
#define protected public
#include "SampleEncoder.h"
#undef private
#undef protected

#include "hypercine/phase3a.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

namespace {

struct Options {
    std::string suite = "all";
    std::string out_path = "benchmarks/phase3a2_release_candidate.json";
    bool allow_dirty = false;
    int trials = 5;
    std::string resolutions = "1080,4k,8k";
};

struct ResolutionSpec {
    const char* key;
    const char* name;
    int width;
    int height;
    int headline_frames;
    int study_frames;
    int unique_frames;
};

const ResolutionSpec kSpecs[] = {
    {"1080p", "1080p", 1920, 1080, 1000, 400, 96},
    {"4k", "4K_UHD", 3840, 2160, 300, 120, 36},
    {"8k", "8K_UHD", 7680, 4320, 120, 48, 12},
};

struct GitInfo {
    std::string repo_root = ".";
    std::string commit;
    std::string branch;
    std::string status;
    std::string upstream_status;
    bool dirty = false;
};

struct HardwareInfo {
    std::string cpu;
    int logical_processors = 0;
    uint64_t ram_bytes = 0;
    std::string os;
    int ac_online = -1;
    std::string gpu_model;
    uint64_t vram_bytes = 0;
    uint64_t vram_mib = 0;
    std::string cuda_arch;
    std::string cuda_runtime;
    std::string cuda_driver_api;
    std::string nvidia_driver;
    std::string pcie_config;
    std::string gpu_list;
    std::string gpu_query;
};

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto origin = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - origin).count();
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string Capture(const std::string& command) {
    std::string output;
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) return output;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
    _pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}

std::filesystem::path FindRepoRoot() {
    std::error_code ec;
    std::filesystem::path cur = std::filesystem::current_path(ec);
    for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
        if (std::filesystem::exists(cur / ".git", ec)) return cur;
        const auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return std::filesystem::path(".");
}

GitInfo ReadGit() {
    GitInfo git;
    const std::filesystem::path root = FindRepoRoot();
    git.repo_root = root.string();
    git.commit = Capture("git -C \"" + git.repo_root + "\" rev-parse HEAD");
    git.branch = Capture("git -C \"" + git.repo_root + "\" rev-parse --abbrev-ref HEAD");
    git.status = Capture("git -C \"" + git.repo_root + "\" status --porcelain");
    git.upstream_status = Capture("git -C \"" + git.repo_root + "\" status --porcelain upstream/");
    git.dirty = !git.status.empty();
    return git;
}

std::string CpuBrand() {
#ifdef _WIN32
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        return brand;
    }
#endif
    return "unknown";
}

HardwareInfo ReadHardware() {
    HardwareInfo hw;
    hw.cpu = CpuBrand();
    hw.logical_processors = static_cast<int>(std::thread::hardware_concurrency());
#ifdef _WIN32
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) hw.ram_bytes = mem.ullTotalPhys;
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) hw.ac_online = power.ACLineStatus;
    OSVERSIONINFOEXW ver{};
    ver.dwOSVersionInfoSize = sizeof(ver);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
        if (rtl(&ver) == 0) {
            hw.os = "Windows " + std::to_string(ver.dwMajorVersion) + "." +
                    std::to_string(ver.dwMinorVersion) + " build " + std::to_string(ver.dwBuildNumber);
        }
    }
#endif
    int runtime_ver = 0;
    int driver_ver = 0;
    if (cudaRuntimeGetVersion(&runtime_ver) == cudaSuccess) {
        hw.cuda_runtime = std::to_string(runtime_ver / 1000) + "." +
                          std::to_string((runtime_ver % 1000) / 10) + " (" + std::to_string(runtime_ver) + ")";
    }
    if (cudaDriverGetVersion(&driver_ver) == cudaSuccess) {
        hw.cuda_driver_api = std::to_string(driver_ver / 1000) + "." +
                             std::to_string((driver_ver % 1000) / 10) + " (" + std::to_string(driver_ver) + ")";
    }
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess) {
        hw.gpu_model = prop.name;
        hw.vram_bytes = static_cast<uint64_t>(prop.totalGlobalMem);
        hw.vram_mib = hw.vram_bytes / (1024ull * 1024ull);
        hw.cuda_arch = "sm_" + std::to_string(prop.major) + std::to_string(prop.minor);
    }
    hw.nvidia_driver = Capture("nvidia-smi --query-gpu=driver_version --format=csv,noheader");
    hw.pcie_config = Capture(
        "nvidia-smi --query-gpu=pcie.link.gen.current,pcie.link.width.current,pcie.link.gen.max,pcie.link.width.max "
        "--format=csv,noheader");
    hw.gpu_list = Capture("nvidia-smi -L");
    hw.gpu_query = Capture(
        "nvidia-smi --query-gpu=name,driver_version,memory.total,power.limit,clocks.max.gr,clocks.max.mem,"
        "pcie.link.gen.current,pcie.link.width.current,pcie.link.gen.max,pcie.link.width.max "
        "--format=csv,noheader");
    return hw;
}

void Stats(std::vector<double> vals, double* mean, double* median, double* stddev, double* min_v, double* max_v) {
    if (vals.empty()) return;
    double sum = 0.0;
    for (double v : vals) sum += v;
    const double m = sum / static_cast<double>(vals.size());
    if (mean) *mean = m;
    double sq = 0.0;
    for (double v : vals) {
        const double d = v - m;
        sq += d * d;
    }
    if (stddev) *stddev = std::sqrt(sq / static_cast<double>(vals.size()));
    std::sort(vals.begin(), vals.end());
    if (min_v) *min_v = vals.front();
    if (max_v) *max_v = vals.back();
    if (median) {
        const size_t n = vals.size();
        *median = (n & 1u) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
    }
}

struct FramePool {
    int width = 0;
    int height = 0;
    int pitch = 0;
    int unique = 0;
    bool exact = false;
    size_t frame_bytes = 0;
    std::vector<std::vector<uint8_t>> pageable;
    std::vector<uint8_t*> pinned;
    std::vector<uint8_t*> device;
    std::string note;

    ~FramePool() { ReleaseDevice(); }

    void ReleaseDevice() {
        for (uint8_t* p : pinned) {
            if (p) cudaFreeHost(p);
        }
        pinned.clear();
        for (uint8_t* p : device) {
            if (p) cudaFree(p);
        }
        device.clear();
    }
};

bool BuildPool(const ResolutionSpec& spec, int count, int exact_warmup, FramePool* pool, std::string* error) {
    pool->width = spec.width;
    pool->height = spec.height;
    pool->pitch = spec.width * 4;
    pool->frame_bytes = static_cast<size_t>(pool->pitch) * static_cast<size_t>(spec.height);
    pool->unique = count;
    pool->exact = exact_warmup >= 0;
    pool->pageable.resize(static_cast<size_t>(pool->unique));
    std::cout << "  generating " << pool->unique << (pool->exact ? " exact-stamp " : " cycle ")
              << spec.name << " frames\n" << std::flush;
    for (int i = 0; i < pool->unique; ++i) {
        pool->pageable[static_cast<size_t>(i)].resize(pool->frame_bytes);
        const int measured = pool->exact ? (i - exact_warmup) : i;
        const int stamp = measured >= 0 ? measured : 0;
        const auto pattern = hypercine::SelectSequencePattern(measured >= 0 ? measured : i);
        hypercine::GeneratePhase3AFrame(spec.width, spec.height, stamp, pattern,
                                        pool->pageable[static_cast<size_t>(i)].data(), pool->pitch);
    }
    pool->note = pool->exact
        ? "one buffer per pipeline index; measured frame k is stamped k"
        : "unique_cycle=" + std::to_string(pool->unique) +
              "; later measured frames repeat this cycle; cycle length is greater than the slot count";
    (void)error;
    return true;
}

bool EnsurePinned(FramePool* pool, std::string* error) {
    if (!pool->pinned.empty()) return true;
    pool->pinned.assign(static_cast<size_t>(pool->unique), nullptr);
    for (int i = 0; i < pool->unique; ++i) {
        if (cudaHostAlloc(&pool->pinned[static_cast<size_t>(i)], pool->frame_bytes, cudaHostAllocDefault) != cudaSuccess) {
            if (error) *error = "cudaHostAlloc failed for pinned input pool";
            pool->ReleaseDevice();
            return false;
        }
        std::memcpy(pool->pinned[static_cast<size_t>(i)], pool->pageable[static_cast<size_t>(i)].data(), pool->frame_bytes);
    }
    return true;
}

bool EnsureDevice(FramePool* pool, std::string* error) {
    if (!pool->device.empty()) return true;
    pool->device.assign(static_cast<size_t>(pool->unique), nullptr);
    for (int i = 0; i < pool->unique; ++i) {
        if (cudaMalloc(&pool->device[static_cast<size_t>(i)], pool->frame_bytes) != cudaSuccess) {
            if (error) *error = "cudaMalloc failed for device input pool";
            pool->ReleaseDevice();
            return false;
        }
        if (cudaMemcpy(pool->device[static_cast<size_t>(i)], pool->pageable[static_cast<size_t>(i)].data(),
                       pool->frame_bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
            if (error) *error = "device input upload failed";
            pool->ReleaseDevice();
            return false;
        }
    }
    return true;
}

std::vector<hypercine::Phase3AFrameInput> MakeInputs(const FramePool& pool, int warmup, int frames,
                                                    hypercine::Phase3AInputMode mode) {
    std::vector<hypercine::Phase3AFrameInput> inputs(static_cast<size_t>(warmup + frames));
    for (int i = 0; i < warmup + frames; ++i) {
        const int src = pool.exact ? i : (i % pool.unique);
        const int measured = pool.exact ? (i - warmup) : src;
        auto& in = inputs[static_cast<size_t>(i)];
        in.frame_index = measured >= 0 ? measured : 0;
        in.pattern = hypercine::SelectSequencePattern(measured >= 0 ? measured : 0);
        in.host_ref = pool.pageable[static_cast<size_t>(src)].data();
        in.pitch = pool.pitch;
        in.width = pool.width;
        in.height = pool.height;
        if (mode == hypercine::Phase3AInputMode::HostPinnedApi) in.bgra_ptr = pool.pinned[static_cast<size_t>(src)];
        else if (mode == hypercine::Phase3AInputMode::DeviceInput) in.bgra_ptr = pool.device[static_cast<size_t>(src)];
        else in.bgra_ptr = pool.pageable[static_cast<size_t>(src)].data();
    }
    return inputs;
}

int SlotCountFor(const ResolutionSpec& spec, int override_slots) {
    const auto sel = hypercine::SelectPhase2DFrameSlots(
        spec.width, spec.height,
        override_slots > 0 ? hypercine::Phase2DSlotPolicy::UserOverride : hypercine::Phase2DSlotPolicy::DefaultAuto,
        override_slots, hypercine::Phase2DD2HPolicy::Throughput);
    return std::max(1, sel.selected_slots);
}

hypercine::Phase3AResult RunGpu(const ResolutionSpec& spec, const FramePool& pool,
                                hypercine::Phase3AInputMode mode, bool graph, int override_slots,
                                int frames, bool verify) {
    const int slots = SlotCountFor(spec, override_slots);
    hypercine::Phase3AConfig cfg{};
    cfg.width = spec.width;
    cfg.height = spec.height;
    cfg.frames = frames;
    cfg.warmup_frames = pool.exact ? std::max(0, pool.unique - frames) : std::max(slots, 2);
    cfg.input_mode = mode;
    cfg.quant_mode = hypercine::Phase3AQuantMode::DynamicUpstreamState;
    cfg.slot_policy = override_slots > 0 ? hypercine::Phase2DSlotPolicy::UserOverride
                                         : hypercine::Phase2DSlotPolicy::DefaultAuto;
    cfg.user_override_slots = override_slots;
    cfg.use_cuda_graph = graph;
    cfg.d2h_policy = hypercine::Phase2DD2HPolicy::Throughput;
    cfg.sync_mode = hypercine::Phase2DSyncMode::Blocking;
    cfg.deterministic_test_metadata = verify;
    cfg.verify_sequence = verify;
    cfg.retain_frame_records = verify;
    auto inputs = MakeInputs(pool, cfg.warmup_frames, frames, mode);
    return hypercine::RunPhase3A(cfg, &inputs);
}

struct CpuTrial {
    bool ok = false;
    double fps = 0.0;
    double wall_ms = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double first_ms = 0.0;
    size_t mean_bytes = 0;
    size_t min_bytes = 0;
    size_t max_bytes = 0;
    int frames = 0;
    std::string error;
};

CpuTrial RunUpstream(const FramePool& pool, int frames) {
    CpuTrial out;
    out.frames = frames;
    CFHD_EncoderRef ref = nullptr;
    if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) {
        out.error = "CFHD_OpenEncoder failed";
        return out;
    }
    if (CFHD_PrepareToEncode(ref, pool.width, pool.height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(ref);
        out.error = "CFHD_PrepareToEncode failed";
        return out;
    }
    for (int i = 0; i < 4; ++i) {
        const auto& frame = pool.pageable[static_cast<size_t>(i % pool.unique)];
        if (CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pool.pitch) != CFHD_ERROR_OKAY) {
            CFHD_CloseEncoder(ref);
            out.error = "upstream warmup failed";
            return out;
        }
    }
    std::vector<double> times;
    times.reserve(static_cast<size_t>(frames));
    size_t sum_bytes = 0;
    size_t min_bytes = 0;
    size_t max_bytes = 0;
    const double t0 = NowMs();
    for (int i = 0; i < frames; ++i) {
        const auto& frame = pool.pageable[static_cast<size_t>(i % pool.unique)];
        const double a = NowMs();
        if (CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pool.pitch) != CFHD_ERROR_OKAY) {
            CFHD_CloseEncoder(ref);
            out.error = "CFHD_EncodeSample failed";
            return out;
        }
        void* sample = nullptr;
        size_t nbytes = 0;
        CFHD_GetSampleData(ref, &sample, &nbytes);
        const double b = NowMs();
        times.push_back(b - a);
        sum_bytes += nbytes;
        if (min_bytes == 0 || nbytes < min_bytes) min_bytes = nbytes;
        if (nbytes > max_bytes) max_bytes = nbytes;
    }
    const double wall = NowMs() - t0;
    CFHD_CloseEncoder(ref);
    Stats(times, &out.mean_ms, &out.median_ms, nullptr, nullptr, nullptr);
    std::sort(times.begin(), times.end());
    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(times.size()))) - 1;
        if (idx >= times.size()) idx = times.size() - 1;
        return times[idx];
    };
    out.p95_ms = pct(0.95);
    out.p99_ms = pct(0.99);
    out.first_ms = times.empty() ? 0.0 : times.front();
    out.wall_ms = wall;
    out.fps = wall > 0.0 ? static_cast<double>(frames) * 1000.0 / wall : 0.0;
    out.mean_bytes = frames > 0 ? sum_bytes / static_cast<size_t>(frames) : 0;
    out.min_bytes = min_bytes;
    out.max_bytes = max_bytes;
    out.ok = true;
    return out;
}

struct TrialRecord {
    std::string suite;
    std::string resolution;
    int width = 0;
    int height = 0;
    std::string mode;
    bool graph = false;
    int requested_slots = 0;
    int trial = 0;
    int order_index = 0;
    hypercine::Phase3AResult gpu;
    CpuTrial cpu;
    bool is_cpu = false;
    std::string pool_note;
    std::string prep_error;
};

void WriteGpu(std::ostringstream& json, const TrialRecord& rec) {
    const auto& r = rec.gpu;
    const double raw_bytes = static_cast<double>(rec.width) * static_cast<double>(rec.height) * 4.0;
    const double fps = r.sustained_fps;
    const double out_bytes = static_cast<double>(r.mean_bitstream_bytes);
    json << "    {\n";
    json << "      \"suite\": \"" << JsonEscape(rec.suite) << "\",\n";
    json << "      \"resolution\": \"" << rec.resolution << "\",\n";
    json << "      \"width\": " << rec.width << ",\n";
    json << "      \"height\": " << rec.height << ",\n";
    json << "      \"input_mode\": \"" << rec.mode << "\",\n";
    json << "      \"benchmark_contract\": \"ProductionEndToEnd\",\n";
    json << "      \"throughput_kind\": \"completed_frame_sequence\",\n";
    json << "      \"graph_mode\": " << (rec.graph ? "true" : "false") << ",\n";
    json << "      \"requested_slots\": " << rec.requested_slots << ",\n";
    json << "      \"slot_count\": " << r.slot_selection.selected_slots << ",\n";
    json << "      \"trial\": " << rec.trial << ",\n";
    json << "      \"order_index\": " << rec.order_index << ",\n";
    json << "      \"ok\": " << (r.ok ? "true" : "false") << ",\n";
    json << "      \"prep_error\": \"" << JsonEscape(rec.prep_error) << "\",\n";
    json << "      \"error\": \"" << JsonEscape(r.error) << "\",\n";
    json << "      \"pool\": \"" << JsonEscape(rec.pool_note) << "\",\n";
    json << "      \"sequence\": {\n";
    json << "        \"frames\": " << r.config.frames << ",\n";
    json << "        \"warmup_frames_excluded\": " << r.config.warmup_frames << ",\n";
    json << "        \"submitted_frames\": " << r.submitted_frames << ",\n";
    json << "        \"completed_frames\": " << r.completed_frames << ",\n";
    json << "        \"wall_ms\": " << r.wall_ms << ",\n";
    json << "        \"warmup_excluded_ms\": " << r.warmup_excluded_ms << ",\n";
    json << "        \"drain_ms\": " << r.drain_ms << ",\n";
    json << "        \"device_sync_ms\": " << r.device_sync_ms << ",\n";
    json << "        \"device_drained\": " << (r.device_drained ? "true" : "false") << ",\n";
    json << "        \"watchdog_tripped\": " << (r.watchdog_tripped ? "true" : "false") << ",\n";
    json << "        \"definition\": \"" << JsonEscape(r.throughput_definition) << "\"\n";
    json << "      },\n";
    json << "      \"throughput\": {\n";
    json << "        \"sequence_fps\": " << fps << ",\n";
    json << "        \"steady_state_fps\": " << r.steady_state_fps << ",\n";
    json << "        \"steady_state_frames\": " << r.steady_state_frames << ",\n";
    json << "        \"frame_period_ms\": " << r.frame_period_ms << ",\n";
    json << "        \"mean_completion_period_ms\": " << r.mean_period_ms << "\n";
    json << "      },\n";
    json << "      \"latency\": {\n";
    json << "        \"mean_ms\": " << r.mean_latency_ms << ",\n";
    json << "        \"median_ms\": " << r.median_latency_ms << ",\n";
    json << "        \"p95_ms\": " << r.p95_latency_ms << ",\n";
    json << "        \"p99_ms\": " << r.p99_latency_ms << ",\n";
    json << "        \"first_frame_ms\": " << r.first_frame_latency_ms << ",\n";
    json << "        \"min_ms\": " << r.min_latency_ms << ",\n";
    json << "        \"max_ms\": " << r.max_latency_ms << ",\n";
    json << "        \"note\": \"submit-to-assembled-sample latency; 1000/mean_latency is not throughput when slots overlap\"\n";
    json << "      },\n";
    json << "      \"stage_times_ms\": {\n";
    json << "        \"pageable_staging\": " << r.host_pageable_stage_ms << ",\n";
    json << "        \"h2d\": " << r.h2d_ms << ",\n";
    json << "        \"color\": " << r.color_ms << ",\n";
    json << "        \"wavelet_including_quantization\": " << r.wavelet_ms << ",\n";
    json << "        \"entropy\": " << r.gpu_entropy_ms << ",\n";
    json << "        \"consolidate\": " << r.gpu_consolidate_ms << ",\n";
    json << "        \"compressed_d2h\": " << r.compressed_d2h_ms << ",\n";
    json << "        \"lowpass_d2h\": " << r.lowpass_d2h_ms << ",\n";
    json << "        \"host_assembly\": " << r.cpu_assembly_total_ms << ",\n";
    json << "        \"rate_control\": " << r.rate_control_ms << ",\n";
    json << "        \"host_enqueue\": " << r.host_enqueue_ms << ",\n";
    json << "        \"graph_capture_max\": " << r.graph_capture_ms << ",\n";
    json << "        \"graph_captures\": " << r.graph_captures_count << ",\n";
    json << "        \"overlapping_stage_sum\": " << r.total_gpu_ms << ",\n";
    json << "        \"critical_path_frame_period\": " << r.frame_period_ms << "\n";
    json << "      },\n";
    json << "      \"bandwidth\": {\n";
    json << "        \"raw_input_bytes_per_frame\": " << raw_bytes << ",\n";
    json << "        \"raw_input_gbps\": " << (raw_bytes * fps / 1.0e9) << ",\n";
    json << "        \"h2d_bytes_per_frame\": " << r.h2d_bytes_per_frame << ",\n";
    json << "        \"h2d_gbps\": " << (r.h2d_ms > 0.0 ? (static_cast<double>(r.h2d_bytes_per_frame) / (r.h2d_ms / 1.0e3) / 1.0e9) : 0.0) << ",\n";
    json << "        \"pageable_staging_gbps\": " << (r.host_pageable_stage_ms > 0.0 ? (raw_bytes / (r.host_pageable_stage_ms / 1.0e3) / 1.0e9) : 0.0) << ",\n";
    json << "        \"output_mbps\": " << (out_bytes * fps / 1.0e6) << ",\n";
    json << "        \"compression_ratio\": " << (out_bytes > 0.0 ? raw_bytes / out_bytes : 0.0) << "\n";
    json << "      },\n";
    json << "      \"compression\": {\n";
    json << "        \"encoded_format\": \"CFHD_ENCODED_FORMAT_YUV_422\",\n";
    json << "        \"quality\": \"CFHD_ENCODING_QUALITY_FILMSCAN3\",\n";
    json << "        \"gop\": 1,\n";
    json << "        \"source_format\": \"BGRA8\",\n";
    json << "        \"quant_mode\": \"DynamicUpstreamState\",\n";
    json << "        \"mean_bytes\": " << r.mean_bitstream_bytes << ",\n";
    json << "        \"min_bytes\": " << r.min_bitstream_bytes << ",\n";
    json << "        \"max_bytes\": " << r.max_bitstream_bytes << ",\n";
    json << "        \"distinct_fs_rate_limiters\": " << r.distinct_fs_rate_limiters << "\n";
    json << "      },\n";
    json << "      \"telemetry\": {\n";
    json << "        \"graphics_clock_mhz_mean\": " << r.nvml.graphics_clock_mhz_mean << ",\n";
    json << "        \"graphics_clock_mhz_min\": " << r.nvml.graphics_clock_mhz_min << ",\n";
    json << "        \"graphics_clock_mhz_max\": " << r.nvml.graphics_clock_mhz_max << ",\n";
    json << "        \"mem_clock_mhz_mean\": " << r.nvml.mem_clock_mhz_mean << ",\n";
    json << "        \"temperature_c_max\": " << r.nvml.temperature_c_max << ",\n";
    json << "        \"power_w_mean\": " << r.nvml.power_w_mean << ",\n";
    json << "        \"power_w_max\": " << r.nvml.power_w_max << ",\n";
    json << "        \"gpu_util_mean\": " << r.nvml.hardware_gpu_utilization_pct_mean << ",\n";
    json << "        \"throttle\": \"" << JsonEscape(r.nvml.throttle_reasons_str) << "\",\n";
    json << "        \"sample_count\": " << r.nvml.sample_count << "\n";
    json << "      },\n";
    json << "      \"correctness\": {\n";
    json << "        \"verification_performed\": " << (r.verification_performed ? "true" : "false") << ",\n";
    json << "        \"decode_ok\": " << (r.decode_ok ? "true" : "false") << ",\n";
    json << "        \"stamp_verify_ok\": " << (r.stamp_verify_ok ? "true" : "false") << ",\n";
    json << "        \"min_psnr_db\": " << r.min_psnr_db << ",\n";
    json << "        \"mean_psnr_db\": " << r.mean_psnr_db;
    if (!r.frame_records.empty()) {
        auto sha_at = [&](size_t index) {
            if (index >= r.frame_records.size()) return std::string();
            return r.frame_records[index].sample_sha256;
        };
        uint64_t mix = 14695981039346656037ull;
        for (const auto& rec_frame : r.frame_records) {
            for (unsigned char ch : rec_frame.sample_sha256) {
                mix ^= ch;
                mix *= 1099511628211ull;
            }
        }
        std::ostringstream mix_hex;
        mix_hex << std::hex << mix;
        json << ",\n        \"frames_hashed\": " << r.frame_records.size() << ",\n";
        json << "        \"first_sha256\": \"" << sha_at(0) << "\",\n";
        json << "        \"middle_sha256\": \"" << sha_at(r.frame_records.size() / 2) << "\",\n";
        json << "        \"last_sha256\": \"" << sha_at(r.frame_records.size() - 1) << "\",\n";
        json << "        \"sequence_fnv\": \"" << mix_hex.str() << "\"";
    }
    json << "\n      }\n";
    json << "    }";
}

void WriteCpu(std::ostringstream& json, const TrialRecord& rec) {
    const auto& c = rec.cpu;
    const double raw_bytes = static_cast<double>(rec.width) * static_cast<double>(rec.height) * 4.0;
    json << "    {\n";
    json << "      \"suite\": \"" << JsonEscape(rec.suite) << "\",\n";
    json << "      \"resolution\": \"" << rec.resolution << "\",\n";
    json << "      \"width\": " << rec.width << ",\n";
    json << "      \"height\": " << rec.height << ",\n";
    json << "      \"input_mode\": \"UpstreamCpuEncodeSample\",\n";
    json << "      \"benchmark_contract\": \"ProductionEndToEnd\",\n";
    json << "      \"throughput_kind\": \"completed_frame_sequence\",\n";
    json << "      \"baseline\": \"pristine CFHD_EncodeSample, _THREADED_ENCODER=0, one encode thread\",\n";
    json << "      \"trial\": " << rec.trial << ",\n";
    json << "      \"ok\": " << (c.ok ? "true" : "false") << ",\n";
    json << "      \"error\": \"" << JsonEscape(c.error) << "\",\n";
    json << "      \"pool\": \"" << JsonEscape(rec.pool_note) << "\",\n";
    json << "      \"throughput\": {\"sequence_fps\": " << c.fps << ", \"wall_ms\": " << c.wall_ms << "},\n";
    json << "      \"latency\": {\"mean_ms\": " << c.mean_ms << ", \"median_ms\": " << c.median_ms
         << ", \"p95_ms\": " << c.p95_ms << ", \"p99_ms\": " << c.p99_ms
         << ", \"first_frame_ms\": " << c.first_ms << "},\n";
    json << "      \"bandwidth\": {\"raw_input_gbps\": " << (raw_bytes * c.fps / 1.0e9)
         << ", \"output_mbps\": " << (static_cast<double>(c.mean_bytes) * c.fps / 1.0e6) << "},\n";
    json << "      \"compression\": {\"mean_bytes\": " << c.mean_bytes << ", \"min_bytes\": " << c.min_bytes
         << ", \"max_bytes\": " << c.max_bytes << ", \"quality\": \"CFHD_ENCODING_QUALITY_FILMSCAN3\","
         << " \"encoded_format\": \"CFHD_ENCODED_FORMAT_YUV_422\", \"source_format\": \"BGRA8\"}\n";
    json << "    }";
}

bool WantRes(const Options& opt, const char* key) {
    return opt.resolutions.find(key) != std::string::npos || opt.resolutions == "all";
}

void WarmGpu(const ResolutionSpec& spec, FramePool& pool) {
    std::cout << "  warmup " << spec.name << "\n" << std::flush;
    auto r = RunGpu(spec, pool, hypercine::Phase3AInputMode::HostPageableEndToEnd, true, 0, std::min(48, spec.study_frames), false);
    std::cout << "    warmup fps=" << r.sustained_fps << " clock=" << r.nvml.graphics_clock_mhz_mean
              << " MHz ok=" << r.ok << "\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) {
            if (i + 1 >= argc) return std::string();
            if (arg == name) return std::string(argv[++i]);
            return std::string();
        };
        if (arg == "--allow-dirty") opt.allow_dirty = true;
        else if (arg.rfind("--suite=", 0) == 0) opt.suite = arg.substr(8);
        else if (arg.rfind("--out=", 0) == 0) opt.out_path = arg.substr(6);
        else if (arg.rfind("--trials=", 0) == 0) opt.trials = std::max(1, std::atoi(arg.substr(9).c_str()));
        else if (arg.rfind("--resolutions=", 0) == 0) opt.resolutions = arg.substr(14);
        else if (const std::string v = need("--suite"); !v.empty()) opt.suite = v;
        else if (const std::string v = need("--out"); !v.empty()) opt.out_path = v;
        else if (const std::string v = need("--trials"); !v.empty()) opt.trials = std::max(1, std::atoi(v.c_str()));
        else if (const std::string v = need("--resolutions"); !v.empty()) opt.resolutions = v;
    }

    const GitInfo git = ReadGit();
    if (git.dirty && !opt.allow_dirty) {
        std::cerr << "REFUSING publication: working tree is dirty. Pass --allow-dirty to record a DIRTY result.\n";
        std::cerr << git.status << "\n";
        return 2;
    }
    const HardwareInfo hw = ReadHardware();
    std::vector<TrialRecord> records;

    auto consider = [&](const ResolutionSpec& spec) {
        if (!WantRes(opt, spec.key == std::string("1080p") ? "1080" : spec.key)) return;
        FramePool pool;
        std::string error;
        const bool speed_suite = opt.suite == "all" || opt.suite == "headline" || opt.suite == "fresh" ||
                                 opt.suite == "slots" || opt.suite == "graph";
        if (speed_suite) {
            if (!BuildPool(spec, spec.unique_frames, -1, &pool, &error)) {
                std::cerr << error << "\n";
                return;
            }
            if (opt.suite != "fresh") WarmGpu(spec, pool);
        }

        auto add_gpu = [&](const std::string& suite, hypercine::Phase3AInputMode mode, bool graph,
                           int slots, int trial, int order, int frames, bool verify) {
            TrialRecord rec;
            rec.suite = suite;
            rec.resolution = spec.name;
            rec.width = spec.width;
            rec.height = spec.height;
            rec.mode = hypercine::Phase3AInputModeName(mode);
            rec.graph = graph;
            rec.requested_slots = slots;
            rec.trial = trial;
            rec.order_index = order;
            rec.pool_note = pool.note;
            std::cout << "  " << suite << " " << spec.name << " " << rec.mode
                      << " graph=" << graph << " trial=" << trial << " frames=" << frames << "\n" << std::flush;
            if (mode == hypercine::Phase3AInputMode::HostPinnedApi && !EnsurePinned(&pool, &rec.prep_error)) {
                records.push_back(rec);
                return;
            }
            if (mode == hypercine::Phase3AInputMode::DeviceInput && !EnsureDevice(&pool, &rec.prep_error)) {
                records.push_back(rec);
                return;
            }
            rec.gpu = RunGpu(spec, pool, mode, graph, slots, frames, verify);
            std::cout << "    fps=" << rec.gpu.sustained_fps
                      << " steady=" << rec.gpu.steady_state_fps
                      << " lat=" << rec.gpu.mean_latency_ms
                      << " clock=" << rec.gpu.nvml.graphics_clock_mhz_mean
                      << " slots=" << rec.gpu.slot_selection.selected_slots
                      << " ok=" << rec.gpu.ok << " " << rec.gpu.error << "\n" << std::flush;
            records.push_back(std::move(rec));
        };

        if (opt.suite == "all" || opt.suite == "equivalence") {
            FramePool exact;
            const int slots = SlotCountFor(spec, 0);
            const int warmup = std::max(slots, 2);
            const int frames = std::min(24, spec.key == std::string("8k") ? 12 : 24);
            if (!BuildPool(spec, warmup + frames, warmup, &exact, &error)) {
                std::cerr << error << "\n";
                return;
            }
            auto add_exact = [&](hypercine::Phase3AInputMode mode) {
                TrialRecord rec;
                rec.suite = "equivalence";
                rec.resolution = spec.name;
                rec.width = spec.width;
                rec.height = spec.height;
                rec.mode = hypercine::Phase3AInputModeName(mode);
                rec.graph = true;
                rec.pool_note = exact.note;
                std::cout << "  equivalence " << spec.name << " " << rec.mode << " frames=" << frames << "\n" << std::flush;
                if (mode == hypercine::Phase3AInputMode::HostPinnedApi && !EnsurePinned(&exact, &rec.prep_error)) {
                    records.push_back(rec);
                    return;
                }
                if (mode == hypercine::Phase3AInputMode::DeviceInput && !EnsureDevice(&exact, &rec.prep_error)) {
                    records.push_back(rec);
                    return;
                }
                rec.gpu = RunGpu(spec, exact, mode, true, 0, frames, true);
                std::cout << "    fps=" << rec.gpu.sustained_fps << " ok=" << rec.gpu.ok << " " << rec.gpu.error << "\n" << std::flush;
                records.push_back(std::move(rec));
            };
            add_exact(hypercine::Phase3AInputMode::HostPageableEndToEnd);
            add_exact(hypercine::Phase3AInputMode::HostPinnedApi);
            add_exact(hypercine::Phase3AInputMode::DeviceInput);
        }
        if (opt.suite == "all" || opt.suite == "headline" || opt.suite == "fresh") {
            const int trials = (opt.suite == "fresh") ? 1 : opt.trials;
            const hypercine::Phase3AInputMode orders[3][3] = {
                {hypercine::Phase3AInputMode::HostPageableEndToEnd, hypercine::Phase3AInputMode::HostPinnedApi, hypercine::Phase3AInputMode::DeviceInput},
                {hypercine::Phase3AInputMode::DeviceInput, hypercine::Phase3AInputMode::HostPageableEndToEnd, hypercine::Phase3AInputMode::HostPinnedApi},
                {hypercine::Phase3AInputMode::HostPinnedApi, hypercine::Phase3AInputMode::DeviceInput, hypercine::Phase3AInputMode::HostPageableEndToEnd}};
            for (int t = 0; t < trials; ++t) {
                if (opt.suite != "fresh") {
                    TrialRecord cpu;
                    cpu.suite = "headline";
                    cpu.resolution = spec.name;
                    cpu.width = spec.width;
                    cpu.height = spec.height;
                    cpu.is_cpu = true;
                    cpu.trial = t;
                    cpu.pool_note = pool.note;
                    std::cout << "  headline " << spec.name << " upstream trial=" << t << "\n" << std::flush;
                    cpu.cpu = RunUpstream(pool, spec.headline_frames);
                    std::cout << "    cpu fps=" << cpu.cpu.fps << "\n" << std::flush;
                    records.push_back(std::move(cpu));
                }
                const int ord = t % 3;
                for (auto mode : orders[ord]) {
                    add_gpu(opt.suite == "fresh" ? "fresh" : "headline", mode, true, 0, t, ord, spec.headline_frames, false);
                }
            }
        }
        if (opt.suite == "all" || opt.suite == "slots") {
            const int trials = std::min(opt.trials, 3);
            for (int slots = 1; slots <= 4; ++slots) {
                for (int t = 0; t < trials; ++t) {
                    add_gpu("slots", hypercine::Phase3AInputMode::HostPageableEndToEnd, true, slots, t, 0,
                            spec.study_frames, false);
                }
            }
        }
        if (opt.suite == "all" || opt.suite == "graph") {
            for (int t = 0; t < opt.trials; ++t) {
                add_gpu("graph", hypercine::Phase3AInputMode::HostPageableEndToEnd, false, 0, t, 0, spec.study_frames, false);
                add_gpu("graph", hypercine::Phase3AInputMode::HostPageableEndToEnd, true, 0, t, 1, spec.study_frames, false);
            }
            FramePool exact;
            const int slots = SlotCountFor(spec, 0);
            const int warmup = std::max(slots, 2);
            const int frames = spec.key == std::string("8k") ? 8 : 16;
            if (BuildPool(spec, warmup + frames, warmup, &exact, &error)) {
                auto add_exact_graph = [&](bool graph_on) {
                    TrialRecord rec;
                    rec.suite = "graph_identity";
                    rec.resolution = spec.name;
                    rec.width = spec.width;
                    rec.height = spec.height;
                    rec.mode = "HostPageableEndToEnd";
                    rec.graph = graph_on;
                    rec.pool_note = exact.note;
                    std::cout << "  graph identity " << spec.name << " graph=" << graph_on << "\n" << std::flush;
                    rec.gpu = RunGpu(spec, exact, hypercine::Phase3AInputMode::HostPageableEndToEnd,
                                     graph_on, 0, frames, true);
                    std::cout << "    fps=" << rec.gpu.sustained_fps << " ok=" << rec.gpu.ok
                              << " " << rec.gpu.error << "\n" << std::flush;
                    records.push_back(std::move(rec));
                };
                add_exact_graph(false);
                add_exact_graph(true);
            }
        }
    };

    std::cout << "Phase 3A.1 canonical benchmark suite=" << opt.suite
              << " dirty=" << git.dirty << " commit=" << git.commit << "\n" << std::flush;
    std::vector<const ResolutionSpec*> ordered;
    std::stringstream spec_list(opt.resolutions);
    std::string spec_tok;
    while (std::getline(spec_list, spec_tok, ',')) {
        for (const auto& spec : kSpecs) {
            const bool match = spec_tok == spec.key || spec_tok == spec.name ||
                               (spec_tok == "1080" && std::string(spec.key) == "1080p");
            if (match) ordered.push_back(&spec);
        }
    }
    if (ordered.empty()) {
        for (const auto& spec : kSpecs) ordered.push_back(&spec);
    }
    for (const ResolutionSpec* spec : ordered) consider(*spec);

    std::ostringstream json;
    json << std::fixed << std::setprecision(4);
    json << "{\n";
    json << "  \"publication\": \"" << (git.dirty ? "DIRTY" : "CLEAN") << "\",\n";
    json << "  \"canonical\": " << (git.dirty ? "false" : "true") << ",\n";
    json << "  \"git_commit\": \"" << JsonEscape(git.commit) << "\",\n";
    json << "  \"git_branch\": \"" << JsonEscape(git.branch) << "\",\n";
    json << "  \"git_status_clean\": " << (git.dirty ? "false" : "true") << ",\n";
    json << "  \"benchmark_contract\": \"ProductionEndToEnd\",\n";
    json << "  \"primary_public_benchmark\": \"HostPageable ProductionEndToEnd\",\n";
    json << "  \"git\": {\n";
    json << "    \"commit\": \"" << JsonEscape(git.commit) << "\",\n";
    json << "    \"branch\": \"" << JsonEscape(git.branch) << "\",\n";
    json << "    \"dirty\": " << (git.dirty ? "true" : "false") << ",\n";
    json << "    \"clean\": " << (git.dirty ? "false" : "true") << ",\n";
    json << "    \"status\": \"" << JsonEscape(git.status) << "\",\n";
    json << "    \"upstream_status\": \"" << JsonEscape(git.upstream_status) << "\"\n";
    json << "  },\n";
    json << "  \"build\": {\n";
    json << "    \"compiler\": \"" << HYPERCINE_COMPILER_ID << " " << HYPERCINE_COMPILER_VERSION << "\",\n";
    json << "    \"compiler_id\": \"" << HYPERCINE_COMPILER_ID << "\",\n";
    json << "    \"compiler_version\": \"" << HYPERCINE_COMPILER_VERSION << "\",\n";
    json << "    \"config\": \"" << HYPERCINE_BUILD_TYPE << "\",\n";
    json << "    \"cmake_options\": \"" << JsonEscape(HYPERCINE_CMAKE_OPTS) << "\",\n";
    json << "    \"cuda_toolkit\": \"" << HYPERCINE_CUDA_COMPILER_VERSION << "\",\n";
    json << "    \"cuda_runtime\": \"" << JsonEscape(hw.cuda_runtime) << "\",\n";
    json << "    \"cuda_driver_api\": \"" << JsonEscape(hw.cuda_driver_api) << "\",\n";
    json << "    \"nvidia_driver\": \"" << JsonEscape(hw.nvidia_driver) << "\",\n";
    json << "    \"cuda_architecture\": \"" << JsonEscape(hw.cuda_arch) << "\",\n";
    json << "    \"cmake_cuda_architectures\": \"" << JsonEscape(HYPERCINE_CMAKE_CUDA_ARCH) << "\",\n";
    json << "    \"optimization\": \"MSVC Release default /O2. HyperCine CMake does not force /arch:AVX2.\",\n";
    json << "    \"threaded_encoder\": \"_THREADED_ENCODER=0. CFHDCodecThreaded is EXCLUDE_FROM_ALL and does not build.\"\n";
    json << "  },\n";
    json << "  \"hardware\": {\n";
    json << "    \"cpu\": \"" << JsonEscape(hw.cpu) << "\",\n";
    json << "    \"logical_processors\": " << hw.logical_processors << ",\n";
    json << "    \"ram_bytes\": " << hw.ram_bytes << ",\n";
    json << "    \"gpu_model\": \"" << JsonEscape(hw.gpu_model) << "\",\n";
    json << "    \"vram_bytes\": " << hw.vram_bytes << ",\n";
    json << "    \"vram_mib\": " << hw.vram_mib << ",\n";
    json << "    \"pcie_configuration\": \"" << JsonEscape(hw.pcie_config) << "\",\n";
    json << "    \"os\": \"" << JsonEscape(hw.os) << "\",\n";
    json << "    \"ac_online\": " << hw.ac_online << ",\n";
    json << "    \"gpu_list\": \"" << JsonEscape(hw.gpu_list) << "\",\n";
    json << "    \"gpu_query\": \"" << JsonEscape(hw.gpu_query) << "\"\n";
    json << "  },\n";
    json << "  \"software\": {\n";
    json << "    \"encoder\": \"Phase3A RunPhase3A\",\n";
    json << "    \"quality\": \"FilmScan3\",\n";
    json << "    \"quality_setting\": \"CFHD_ENCODING_QUALITY_FILMSCAN3\",\n";
    json << "    \"format\": \"YUV 4:2:2\",\n";
    json << "    \"encoded_format\": \"CFHD_ENCODED_FORMAT_YUV_422\",\n";
    json << "    \"source\": \"BGRA8\",\n";
    json << "    \"input_format\": \"CFHD_PIXEL_FORMAT_BGRA (8-bit BGRA, progressive, GOP=1)\",\n";
    json << "    \"rate_control_mode\": \"DynamicUpstreamState (sequence-stateful FSratelimiter)\",\n";
    json << "    \"graph_mode\": \"enabled for headline GPU runs; compared against disabled in graph suite\",\n";
    json << "    \"slot_count\": {\"1080p\": 2, \"4K_UHD\": 3, \"8K_UHD\": 2, \"policy\": \"DefaultAuto\"},\n";
    json << "    \"trial_count\": " << opt.trials << ",\n";
    json << "    \"benchmark_order\": \"resolutions=" << JsonEscape(opt.resolutions) << "; per-trial GPU mode rotation across 3 permutations\",\n";
    json << "    \"sequence_definition\": \"1080p: 1000 measured frames (96-frame distinct cycle); 4K_UHD: 300 measured frames (36-frame distinct cycle); 8K_UHD: 120 measured frames (12-frame distinct cycle); warmup >= slot_count excluded from wall clock; pipeline fully drained and cudaDeviceSynchronize completed before timer stop\"\n";
    json << "  },\n";
    json << "  \"cpu_baseline_note\": \"Baseline 1 is pristine CFHD_EncodeSample with _THREADED_ENCODER=0 and one encode thread. Baseline 2, the HyperCine CPU reference inside VerifyPhase3ASequence120, is an equivalence oracle using the upstream spatial wavelet and CPU entropy. It is not a second throughput product, so speedups use Baseline 1 only.\",\n";
    json << "  \"device_input_note\": \"DeviceInput measures GPU-resident integration throughput when BGRA frames are already in device memory; it must never be presented as conventional file-transcode performance.\",\n";
    json << "  \"headline_summary\": [\n";
    {
        const char* mode_names[] = {"UpstreamCpuEncodeSample", "HostPageableEndToEnd", "HostPinnedApi", "DeviceInput"};
        bool first_summary = true;
        for (const auto& spec : kSpecs) {
            double cpu_median_fps = 0.0;
            for (const char* mname : mode_names) {
                std::vector<const TrialRecord*> matching;
                for (const auto& rec : records) {
                    if (rec.suite != "headline" || rec.resolution != spec.name) continue;
                    const std::string rmode = rec.is_cpu ? "UpstreamCpuEncodeSample" : rec.mode;
                    if (rmode == mname) matching.push_back(&rec);
                }
                if (matching.empty()) continue;
                std::vector<double> fps_vals, mean_lat_vals, med_lat_vals, p95_lat_vals, p99_lat_vals;
                std::vector<double> clk_vals, mem_clk_vals, pwr_vals;
                int max_temp = 0;
                int slots_used = 1;
                bool graph_used = false;
                size_t mean_bytes = 0;
                std::string throttles;
                for (const TrialRecord* tr : matching) {
                    if (tr->is_cpu) {
                        fps_vals.push_back(tr->cpu.fps);
                        mean_lat_vals.push_back(tr->cpu.mean_ms);
                        med_lat_vals.push_back(tr->cpu.median_ms);
                        p95_lat_vals.push_back(tr->cpu.p95_ms);
                        p99_lat_vals.push_back(tr->cpu.p99_ms);
                        mean_bytes = tr->cpu.mean_bytes;
                    } else {
                        fps_vals.push_back(tr->gpu.sustained_fps);
                        mean_lat_vals.push_back(tr->gpu.mean_latency_ms);
                        med_lat_vals.push_back(tr->gpu.median_latency_ms);
                        p95_lat_vals.push_back(tr->gpu.p95_latency_ms);
                        p99_lat_vals.push_back(tr->gpu.p99_latency_ms);
                        clk_vals.push_back(tr->gpu.nvml.graphics_clock_mhz_mean);
                        mem_clk_vals.push_back(tr->gpu.nvml.mem_clock_mhz_mean);
                        pwr_vals.push_back(tr->gpu.nvml.power_w_mean);
                        max_temp = std::max(max_temp, static_cast<int>(tr->gpu.nvml.temperature_c_max));
                        slots_used = tr->gpu.slot_selection.selected_slots;
                        graph_used = tr->graph;
                        mean_bytes = tr->gpu.mean_bitstream_bytes;
                        if (!tr->gpu.nvml.throttle_reasons_str.empty()) {
                            if (!throttles.empty()) throttles += "; ";
                            throttles += tr->gpu.nvml.throttle_reasons_str;
                        }
                    }
                }
                double mean_fps = 0.0, median_fps = 0.0, stddev_fps = 0.0, min_fps = 0.0, max_fps = 0.0;
                Stats(fps_vals, &mean_fps, &median_fps, &stddev_fps, &min_fps, &max_fps);
                if (std::string(mname) == "UpstreamCpuEncodeSample") cpu_median_fps = median_fps;
                double mean_lat = 0.0, med_lat = 0.0, p95_lat = 0.0, p99_lat = 0.0;
                Stats(mean_lat_vals, &mean_lat, nullptr, nullptr, nullptr, nullptr);
                Stats(med_lat_vals, &med_lat, nullptr, nullptr, nullptr, nullptr);
                Stats(p95_lat_vals, &p95_lat, nullptr, nullptr, nullptr, nullptr);
                Stats(p99_lat_vals, &p99_lat, nullptr, nullptr, nullptr, nullptr);
                double mean_clk = 0.0, mean_mem_clk = 0.0, mean_pwr = 0.0;
                Stats(clk_vals, &mean_clk, nullptr, nullptr, nullptr, nullptr);
                Stats(mem_clk_vals, &mean_mem_clk, nullptr, nullptr, nullptr, nullptr);
                Stats(pwr_vals, &mean_pwr, nullptr, nullptr, nullptr, nullptr);
                const double raw_bytes = static_cast<double>(spec.width) * static_cast<double>(spec.height) * 4.0;
                const double speedup = (cpu_median_fps > 0.0) ? (median_fps / cpu_median_fps) : 1.0;
                if (!first_summary) json << ",\n";
                first_summary = false;
                json << "    {\n";
                json << "      \"resolution\": \"" << spec.name << "\",\n";
                json << "      \"width\": " << spec.width << ",\n";
                json << "      \"height\": " << spec.height << ",\n";
                json << "      \"input_mode\": \"" << mname << "\",\n";
                json << "      \"trials\": " << matching.size() << ",\n";
                json << "      \"graph_mode\": " << (graph_used ? "true" : "false") << ",\n";
                json << "      \"slot_count\": " << slots_used << ",\n";
                json << "      \"median_fps\": " << median_fps << ",\n";
                json << "      \"mean_fps\": " << mean_fps << ",\n";
                json << "      \"stddev_fps\": " << stddev_fps << ",\n";
                json << "      \"stddev_pct\": " << (mean_fps > 0.0 ? (100.0 * stddev_fps / mean_fps) : 0.0) << ",\n";
                json << "      \"min_fps\": " << min_fps << ",\n";
                json << "      \"max_fps\": " << max_fps << ",\n";
                json << "      \"mean_latency_ms\": " << mean_lat << ",\n";
                json << "      \"median_latency_ms\": " << med_lat << ",\n";
                json << "      \"p95_latency_ms\": " << p95_lat << ",\n";
                json << "      \"p99_latency_ms\": " << p99_lat << ",\n";
                json << "      \"raw_input_gbps_at_median\": " << (raw_bytes * median_fps / 1.0e9) << ",\n";
                json << "      \"output_mbps_at_median\": " << (static_cast<double>(mean_bytes) * median_fps / 1.0e6) << ",\n";
                json << "      \"mean_bitstream_bytes\": " << mean_bytes << ",\n";
                json << "      \"speedup_vs_upstream_cpu_median\": " << speedup << ",\n";
                json << "      \"graphics_clock_mhz_mean\": " << mean_clk << ",\n";
                json << "      \"mem_clock_mhz_mean\": " << mean_mem_clk << ",\n";
                json << "      \"power_w_mean\": " << mean_pwr << ",\n";
                json << "      \"temperature_c_max\": " << max_temp << ",\n";
                json << "      \"throttle_reasons\": \"" << JsonEscape(throttles) << "\"\n";
                json << "    }";
            }
        }
    }
    json << "\n  ],\n";
    json << "  \"runs\": [\n";
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].is_cpu) WriteCpu(json, records[i]);
        else WriteGpu(json, records[i]);
        if (i + 1 < records.size()) json << ",\n";
        else json << "\n";
    }
    json << "  ]\n}\n";

    std::filesystem::path out(opt.out_path);
    if (out.is_relative()) out = std::filesystem::path(git.repo_root) / out;
    std::error_code ec;
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream file(out, std::ios::binary);
    if (!file) {
        std::cerr << "failed to write " << out.string() << "\n";
        return 1;
    }
    file << json.str();
    std::cout << "Wrote " << out.string() << " (" << json.str().size()
              << " bytes, runs=" << records.size() << ")\n";
    return 0;
}
