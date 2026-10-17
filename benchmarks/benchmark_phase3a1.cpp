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
#include "../src/core/phase3a_telemetry.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

#ifndef HYPERCINE_TARGET_CUDA_ARCH
#define HYPERCINE_TARGET_CUDA_ARCH HYPERCINE_CMAKE_CUDA_ARCH
#endif

namespace {

constexpr int kCommonWarmupFrames = 4;

struct Options {
    std::string suite = "all";
    std::string out_path;
    std::string intended_release_tag = "phase3a2c-release-candidate";
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
    int validation_frames;
};

const ResolutionSpec kSpecs[] = {
    {"1080p", "1080p", 1920, 1080, 1000, 400, 96, 24},
    {"4k", "4K_UHD", 3840, 2160, 300, 120, 36, 24},
    {"8k", "8K_UHD", 7680, 4320, 120, 48, 12, 12},
};

struct GitInfo {
    std::string repo_root = ".";
    std::string commit;
    std::string branch;
    std::string status;
    std::string upstream_status;
    bool dirty = false;
    bool upstream_clean = true;
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
    std::string cuda_arch_digits;
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
    git.repo_root = std::filesystem::absolute(root).string();
    git.commit = Capture("git -C \"" + git.repo_root + "\" rev-parse HEAD");
    git.branch = Capture("git -C \"" + git.repo_root + "\" rev-parse --abbrev-ref HEAD");
    git.status = Capture("git -C \"" + git.repo_root + "\" status --porcelain");
    git.upstream_status = Capture("git -C \"" + git.repo_root + "\" status --porcelain upstream/");
    git.dirty = !git.status.empty();
    git.upstream_clean = git.upstream_status.empty();
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
        hw.cuda_arch_digits = std::to_string(prop.major) + std::to_string(prop.minor);
        hw.cuda_arch = "sm_" + hw.cuda_arch_digits;
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

bool ArchListMatchesRuntime(const std::string& arch_list, const std::string& runtime_digits) {
    if (runtime_digits.empty()) return false;
    std::stringstream ss(arch_list);
    std::string tok;
    while (std::getline(ss, tok, ';')) {
        if (tok == runtime_digits || tok == ("sm_" + runtime_digits) || tok == (runtime_digits + "-real") ||
            tok == (runtime_digits + "-virtual")) {
            return true;
        }
    }
    return arch_list == runtime_digits;
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

uint64_t FnvMixU64(uint64_t h, uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        h ^= static_cast<uint8_t>((v >> (b * 8)) & 0xffu);
        h *= 1099511628211ull;
    }
    return h;
}

std::string HexU64(uint64_t v) {
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << v;
    return os.str();
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
              "; logical sequence frame s uses pool[s % unique_cycle]; warmup=0.." +
              std::to_string(kCommonWarmupFrames - 1) + ", timed=" +
              std::to_string(kCommonWarmupFrames) + "..W+N-1";
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

int SlotCountFor(const ResolutionSpec& spec, int override_slots) {
    const auto sel = hypercine::SelectPhase2DFrameSlots(
        spec.width, spec.height,
        override_slots > 0 ? hypercine::Phase2DSlotPolicy::UserOverride : hypercine::Phase2DSlotPolicy::DefaultAuto,
        override_slots, hypercine::Phase2DD2HPolicy::Throughput);
    return std::max(1, sel.selected_slots);
}

int CommonWarmupFor(const ResolutionSpec& spec, int override_slots) {
    return std::max(SlotCountFor(spec, override_slots), kCommonWarmupFrames);
}

std::vector<hypercine::Phase3AFrameInput> MakeInputs(const FramePool& pool, int warmup, int frames,
                                                     hypercine::Phase3AInputMode mode) {
    std::vector<hypercine::Phase3AFrameInput> inputs(static_cast<size_t>(warmup + frames));
    for (int s = 0; s < warmup + frames; ++s) {
        const int src = pool.exact ? s : (s % pool.unique);
        const int stamp_id = pool.exact ? std::max(0, s - warmup) : src;
        auto& in = inputs[static_cast<size_t>(s)];
        in.frame_index = stamp_id;
        in.pattern = hypercine::SelectSequencePattern(stamp_id);
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

hypercine::Phase3AResult RunGpu(const ResolutionSpec& spec, const FramePool& pool,
                                hypercine::Phase3AInputMode mode, bool graph, int override_slots,
                                int frames, bool verify, bool retain_records = false) {
    const int warmup = pool.exact ? std::max(0, pool.unique - frames) : CommonWarmupFor(spec, override_slots);
    hypercine::Phase3AConfig cfg{};
    cfg.width = spec.width;
    cfg.height = spec.height;
    cfg.frames = frames;
    cfg.warmup_frames = warmup;
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
    cfg.retain_frame_records = verify || retain_records;
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
    int warmup_frames = kCommonWarmupFrames;
    int first_measured_logical_frame = kCommonWarmupFrames;
    int frames = 0;
    int distinct_fs_rate_limiters = 0;
    uint64_t sample_bytes_fnv64 = 14695981039346656037ull;
    uint64_t fs_rate_limiter_fnv64 = 14695981039346656037ull;
    std::vector<hypercine::Phase3AFrameRecord> frame_records;
    std::string error;
};

CpuTrial RunUpstream(const FramePool& pool, int warmup, int frames, bool capture_records = false) {
    CpuTrial out;
    out.warmup_frames = warmup;
    out.first_measured_logical_frame = warmup;
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
    for (int s = 0; s < warmup; ++s) {
        const int src = pool.exact ? s : (s % pool.unique);
        const auto& frame = pool.pageable[static_cast<size_t>(src)];
        if (CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), pool.pitch) != CFHD_ERROR_OKAY) {
            CFHD_CloseEncoder(ref);
            out.error = "upstream warmup failed";
            return out;
        }
    }

    auto* sample_enc = reinterpret_cast<CSampleEncoder*>(ref);
    auto* enc = sample_enc ? reinterpret_cast<ENCODER*>(sample_enc->m_encoder) : nullptr;
    auto* tfs = sample_enc ? reinterpret_cast<TRANSFORM**>(sample_enc->m_transformArray) : nullptr;

    std::vector<double> times;
    times.reserve(static_cast<size_t>(frames));
    if (capture_records) out.frame_records.reserve(static_cast<size_t>(frames));
    std::set<int> distinct_fs;
    size_t sum_bytes = 0;
    size_t min_bytes = 0;
    size_t max_bytes = 0;
    uint64_t bytes_fnv = 14695981039346656037ull;
    uint64_t fs_fnv = 14695981039346656037ull;

    const double t0 = NowMs();
    for (int i = 0; i < frames; ++i) {
        const int seq_idx = warmup + i;
        const int src = pool.exact ? seq_idx : (seq_idx % pool.unique);
        const int stamp_id = pool.exact ? i : src;
        const auto& frame = pool.pageable[static_cast<size_t>(src)];

        const int64_t prev_bits = enc ? enc->lastgopbitcount : 0;
        const size_t prev_bytes = static_cast<size_t>(prev_bits >> 3);

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

        enc = sample_enc ? reinterpret_cast<ENCODER*>(sample_enc->m_encoder) : nullptr;
        tfs = sample_enc ? reinterpret_cast<TRANSFORM**>(sample_enc->m_transformArray) : nullptr;
        const int cur_fs = enc ? enc->q.FSratelimiter : 0;
        distinct_fs.insert(cur_fs);
        bytes_fnv = FnvMixU64(bytes_fnv, static_cast<uint64_t>(nbytes));
        fs_fnv = FnvMixU64(fs_fnv, static_cast<uint64_t>(static_cast<uint32_t>(cur_fs)));

        if (capture_records) {
            hypercine::Phase3AFrameRecord rec{};
            rec.frame_index = i;
            rec.pattern = hypercine::SelectSequencePattern(stamp_id);
            rec.prev_encoded_bytes = prev_bytes;
            rec.prev_lastgopbitcount = prev_bits;
            rec.current_fs_rate_limiter = cur_fs;
            if (enc && tfs) {
                for (int c = 0; c < 3; ++c) {
                    for (int lv = 0; lv < 3; ++lv) {
                        IMAGE* wav = tfs[c] ? tfs[c]->wavelet[lv] : nullptr;
                        if (!wav) continue;
                        for (int band = 0; band < 4; ++band) {
                            rec.quant_table[(c * 3 + lv) * 4 + band] = wav->quant[band];
                            rec.scale_table[(c * 3 + lv) * 4 + band] = wav->scale[band];
                        }
                    }
                }
            }
            rec.sample_bytes = nbytes;
            const auto* s_bytes = static_cast<const uint8_t*>(sample);
            const std::vector<uint8_t> raw_sample(s_bytes, s_bytes + nbytes);
            rec.sample_sha256 = hypercine::Sha256Hex(raw_sample.data(), raw_sample.size());
            const std::vector<uint8_t> masked = hypercine::MaskCfhdVolatileMetadata(raw_sample);
            rec.masked_sample_sha256 = hypercine::Sha256Hex(masked.data(), masked.size());
            rec.latency_ms = b - a;
            out.frame_records.push_back(std::move(rec));
        }
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
    out.distinct_fs_rate_limiters = static_cast<int>(distinct_fs.size());
    out.sample_bytes_fnv64 = bytes_fnv;
    out.fs_rate_limiter_fnv64 = fs_fnv;
    out.ok = true;
    return out;
}

struct EquivalenceReport {
    std::string resolution;
    int width = 0;
    int height = 0;
    int warmup_frames = kCommonWarmupFrames;
    int first_measured_logical_frame = kCommonWarmupFrames;
    int validation_frames = 0;
    int headline_frames = 0;
    bool logical_indices_match = false;
    bool patterns_match = false;
    bool prev_encoded_bytes_match = false;
    bool lastgopbitcount_match = false;
    bool fs_rate_limiter_trajectory_match = false;
    bool quant_table_trajectory_match = false;
    bool sample_bytes_trajectory_match = false;
    bool masked_sample_sha256_match_upstream = false;
    bool gpu_modes_unmasked_sha256_match = false;
    bool decode_and_stamp_verified = false;
    bool headline_output_size_stats_match = true;
    int matched_validation_frames = 0;
    int first_divergent_frame = -1;
    std::string divergence_reason;
    size_t validation_mean_bytes = 0;
    size_t validation_min_bytes = 0;
    size_t validation_max_bytes = 0;
    int validation_distinct_fs_limiters = 0;
    std::string validation_sample_bytes_fnv64;
    std::string validation_fs_rate_limiter_fnv64;
    std::string validation_masked_sequence_fnv64;
    size_t headline_mean_bytes = 0;
    size_t headline_min_bytes = 0;
    size_t headline_max_bytes = 0;
    int headline_distinct_fs_limiters = 0;
    bool ok = false;
};

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

EquivalenceReport ValidateEquivalenceGate(const ResolutionSpec& spec, FramePool& pool,
                                          std::vector<TrialRecord>* records) {
    EquivalenceReport rep;
    rep.resolution = spec.name;
    rep.width = spec.width;
    rep.height = spec.height;
    rep.warmup_frames = CommonWarmupFor(spec, 0);
    rep.first_measured_logical_frame = rep.warmup_frames;
    rep.validation_frames = spec.validation_frames;
    rep.headline_frames = spec.headline_frames;

    std::cout << "  equivalence_gate " << spec.name << " warmup=" << rep.warmup_frames
              << " first_logical=" << rep.first_measured_logical_frame
              << " frames=" << rep.validation_frames << "\n" << std::flush;

    CpuTrial cpu_val = RunUpstream(pool, rep.warmup_frames, rep.validation_frames, true);
    {
        TrialRecord rec;
        rec.suite = "equivalence";
        rec.resolution = spec.name;
        rec.width = spec.width;
        rec.height = spec.height;
        rec.mode = "UpstreamCpuEncodeSample";
        rec.is_cpu = true;
        rec.pool_note = pool.note;
        rec.cpu = cpu_val;
        records->push_back(rec);
    }

    auto run_gpu_val = [&](hypercine::Phase3AInputMode mode) -> hypercine::Phase3AResult {
        TrialRecord rec;
        rec.suite = "equivalence";
        rec.resolution = spec.name;
        rec.width = spec.width;
        rec.height = spec.height;
        rec.mode = hypercine::Phase3AInputModeName(mode);
        rec.graph = true;
        rec.pool_note = pool.note;
        if (mode == hypercine::Phase3AInputMode::HostPinnedApi && !EnsurePinned(&pool, &rec.prep_error)) {
            records->push_back(rec);
            return rec.gpu;
        }
        if (mode == hypercine::Phase3AInputMode::DeviceInput && !EnsureDevice(&pool, &rec.prep_error)) {
            records->push_back(rec);
            return rec.gpu;
        }
        rec.gpu = RunGpu(spec, pool, mode, true, 0, rep.validation_frames, true, true);
        std::cout << "    equivalence " << rec.mode << " fps=" << rec.gpu.sustained_fps
                  << " mean_bytes=" << rec.gpu.mean_bitstream_bytes
                  << " ok=" << rec.gpu.ok << " " << rec.gpu.error << "\n" << std::flush;
        hypercine::Phase3AResult out = rec.gpu;
        records->push_back(std::move(rec));
        return out;
    };

    const hypercine::Phase3AResult gpu_page = run_gpu_val(hypercine::Phase3AInputMode::HostPageableEndToEnd);
    const hypercine::Phase3AResult gpu_pin  = run_gpu_val(hypercine::Phase3AInputMode::HostPinnedApi);
    const hypercine::Phase3AResult gpu_dev  = run_gpu_val(hypercine::Phase3AInputMode::DeviceInput);

    if (!cpu_val.ok || !gpu_page.ok || !gpu_pin.ok || !gpu_dev.ok) {
        rep.divergence_reason = "encoder execution failed during equivalence gate";
        return rep;
    }
    const size_t n = static_cast<size_t>(rep.validation_frames);
    if (cpu_val.frame_records.size() != n || gpu_page.frame_records.size() != n ||
        gpu_pin.frame_records.size() != n || gpu_dev.frame_records.size() != n) {
        rep.divergence_reason = "frame_records count mismatch";
        return rep;
    }

    rep.logical_indices_match = true;
    rep.patterns_match = true;
    rep.prev_encoded_bytes_match = true;
    rep.lastgopbitcount_match = true;
    rep.fs_rate_limiter_trajectory_match = true;
    rep.quant_table_trajectory_match = true;
    rep.sample_bytes_trajectory_match = true;
    rep.masked_sample_sha256_match_upstream = true;
    rep.gpu_modes_unmasked_sha256_match = true;
    rep.decode_and_stamp_verified = gpu_page.decode_ok && gpu_page.stamp_verify_ok &&
                                    gpu_pin.decode_ok && gpu_pin.stamp_verify_ok &&
                                    gpu_dev.decode_ok && gpu_dev.stamp_verify_ok &&
                                    (gpu_page.min_psnr_db > 35.0) &&
                                    (gpu_pin.min_psnr_db > 35.0) &&
                                    (gpu_dev.min_psnr_db > 35.0);

    uint64_t masked_seq_fnv = 14695981039346656037ull;
    const hypercine::Phase3AResult* gpu_all[3] = {&gpu_page, &gpu_pin, &gpu_dev};
    const char* gpu_names[3] = {"HostPageableEndToEnd", "HostPinnedApi", "DeviceInput"};

    for (size_t i = 0; i < n; ++i) {
        const auto& cr = cpu_val.frame_records[i];
        for (unsigned char ch : cr.masked_sample_sha256) {
            masked_seq_fnv ^= ch;
            masked_seq_fnv *= 1099511628211ull;
        }
        bool frame_ok = true;
        for (int m = 0; m < 3; ++m) {
            const auto& gr = gpu_all[m]->frame_records[i];
            if (cr.frame_index != gr.frame_index) {
                rep.logical_indices_match = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " frame_index mismatch";
                }
            }
            if (cr.pattern != gr.pattern) {
                rep.patterns_match = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " pattern mismatch";
                }
            }
            if (cr.prev_encoded_bytes != gr.prev_encoded_bytes) {
                rep.prev_encoded_bytes_match = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " prev_encoded_bytes mismatch";
                }
            }
            if (cr.prev_lastgopbitcount != gr.prev_lastgopbitcount) {
                rep.lastgopbitcount_match = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " lastgopbitcount mismatch";
                }
            }
            if (cr.current_fs_rate_limiter != gr.current_fs_rate_limiter) {
                rep.fs_rate_limiter_trajectory_match = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " FSratelimiter mismatch";
                }
            }
            if (std::memcmp(cr.quant_table, gr.quant_table, sizeof(cr.quant_table)) != 0 ||
                std::memcmp(cr.scale_table, gr.scale_table, sizeof(cr.scale_table)) != 0) {
                rep.quant_table_trajectory_match = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " quant_table mismatch";
                }
            }
            if (cr.sample_bytes != gr.sample_bytes) {
                rep.sample_bytes_trajectory_match = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " sample_bytes mismatch";
                }
            }
            if (cr.masked_sample_sha256 != gr.masked_sample_sha256) {
                rep.masked_sample_sha256_match_upstream = false;
                frame_ok = false;
                if (rep.first_divergent_frame < 0) {
                    rep.first_divergent_frame = static_cast<int>(i);
                    rep.divergence_reason = std::string(gpu_names[m]) + " masked_sample_sha256 mismatch";
                }
            }
        }
        if (gpu_page.frame_records[i].sample_sha256 != gpu_pin.frame_records[i].sample_sha256 ||
            gpu_page.frame_records[i].sample_sha256 != gpu_dev.frame_records[i].sample_sha256) {
            rep.gpu_modes_unmasked_sha256_match = false;
            frame_ok = false;
            if (rep.first_divergent_frame < 0) {
                rep.first_divergent_frame = static_cast<int>(i);
                rep.divergence_reason = "GPU input modes unmasked sample_sha256 mismatch";
            }
        }
        if (frame_ok) ++rep.matched_validation_frames;
    }

    rep.validation_mean_bytes = cpu_val.mean_bytes;
    rep.validation_min_bytes = cpu_val.min_bytes;
    rep.validation_max_bytes = cpu_val.max_bytes;
    rep.validation_distinct_fs_limiters = cpu_val.distinct_fs_rate_limiters;
    rep.validation_sample_bytes_fnv64 = HexU64(cpu_val.sample_bytes_fnv64);
    rep.validation_fs_rate_limiter_fnv64 = HexU64(cpu_val.fs_rate_limiter_fnv64);
    rep.validation_masked_sequence_fnv64 = HexU64(masked_seq_fnv);

    rep.ok = rep.logical_indices_match &&
             rep.patterns_match &&
             rep.prev_encoded_bytes_match &&
             rep.lastgopbitcount_match &&
             rep.fs_rate_limiter_trajectory_match &&
             rep.quant_table_trajectory_match &&
             rep.sample_bytes_trajectory_match &&
             rep.masked_sample_sha256_match_upstream &&
             rep.gpu_modes_unmasked_sha256_match &&
             rep.decode_and_stamp_verified &&
             (rep.matched_validation_frames == rep.validation_frames);

    std::cout << "    equivalence_gate " << spec.name << " ok=" << rep.ok
              << " matched=" << rep.matched_validation_frames << "/" << rep.validation_frames
              << " mean_bytes=" << rep.validation_mean_bytes
              << " min_bytes=" << rep.validation_min_bytes
              << " max_bytes=" << rep.validation_max_bytes << "\n" << std::flush;
    return rep;
}

const char* SlotPolicyName(hypercine::Phase2DSlotPolicy p) {
    switch (p) {
    case hypercine::Phase2DSlotPolicy::DefaultAuto: return "DefaultAuto";
    case hypercine::Phase2DSlotPolicy::LowLatency: return "LowLatency";
    case hypercine::Phase2DSlotPolicy::MaxThroughput: return "MaxThroughput";
    case hypercine::Phase2DSlotPolicy::UserOverride: return "UserOverride";
    default: return "DefaultAuto";
    }
}

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
    json << "      \"benchmark_contract\": \"ProductionEndToEnd_v2\",\n";
    json << "      \"throughput_kind\": \"completed_frame_sequence\",\n";
    json << "      \"graph_mode\": " << (rec.graph ? "true" : "false") << ",\n";
    json << "      \"slot_policy\": \"" << SlotPolicyName(r.slot_selection.policy) << "\",\n";
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
    json << "        \"first_measured_logical_frame_index\": " << r.config.warmup_frames << ",\n";
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
    json << "      \"memory_bytes\": {\n";
    json << "        \"per_slot_estimated_vram_bytes\": " << r.slot_selection.per_slot_vram_bytes << ",\n";
    json << "        \"session_workspace_estimated_vram_bytes\": " << r.slot_selection.session_workspace_vram_bytes << ",\n";
    json << "        ";
    hypercine::detail::WriteVramUsageJson(json, r);
    json << ",\n";
    json << "        \"slot_vram_bytes\": " << r.slot_vram_bytes << ",\n";
    json << "        \"slot_pinned_host_bytes\": " << r.slot_pinned_host_bytes << "\n";
    json << "      },\n";
    json << "      \"telemetry\": {\n";
    json << "        \"graphics_clock_mhz_mean\": " << hypercine::detail::TelemetryJson(r.nvml.graphics_clock_mhz_mean, r.nvml.graphics_clock_sample_count) << ",\n";
    json << "        \"graphics_clock_mhz_min\": " << hypercine::detail::TelemetryJson(r.nvml.graphics_clock_mhz_min, r.nvml.graphics_clock_sample_count) << ",\n";
    json << "        \"graphics_clock_mhz_max\": " << hypercine::detail::TelemetryJson(r.nvml.graphics_clock_mhz_max, r.nvml.graphics_clock_sample_count) << ",\n";
    json << "        \"mem_clock_mhz_mean\": " << hypercine::detail::TelemetryJson(r.nvml.mem_clock_mhz_mean, r.nvml.mem_clock_sample_count) << ",\n";
    json << "        \"temperature_c_max\": " << hypercine::detail::TelemetryJson(r.nvml.temperature_c_max, r.nvml.temperature_sample_count) << ",\n";
    json << "        \"power_w_mean\": " << hypercine::detail::TelemetryJson(r.nvml.power_w_mean, r.nvml.power_sample_count) << ",\n";
    json << "        \"power_w_max\": " << hypercine::detail::TelemetryJson(r.nvml.power_w_max, r.nvml.power_sample_count) << ",\n";
    json << "        \"gpu_util_mean\": " << hypercine::detail::TelemetryJson(r.nvml.hardware_gpu_utilization_pct_mean, r.nvml.utilization_sample_count) << ",\n";
    json << "        \"throttle\": " << hypercine::detail::TelemetryTextJson(r.nvml.throttle_reasons_str, r.nvml.throttle_sample_count) << ",\n";
    json << "        \"counter_status\": " << hypercine::detail::NvmlCounterStatusJson(r.nvml) << ",\n";
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
    json << "      \"benchmark_contract\": \"ProductionEndToEnd_v2\",\n";
    json << "      \"throughput_kind\": \"completed_frame_sequence\",\n";
    json << "      \"baseline\": \"pristine CFHD_EncodeSample, _THREADED_ENCODER=0, one encode thread\",\n";
    json << "      \"trial\": " << rec.trial << ",\n";
    json << "      \"ok\": " << (c.ok ? "true" : "false") << ",\n";
    json << "      \"error\": \"" << JsonEscape(c.error) << "\",\n";
    json << "      \"pool\": \"" << JsonEscape(rec.pool_note) << "\",\n";
    json << "      \"sequence\": {\"frames\": " << c.frames
         << ", \"warmup_frames_excluded\": " << c.warmup_frames
         << ", \"first_measured_logical_frame_index\": " << c.first_measured_logical_frame << "},\n";
    json << "      \"throughput\": {\"sequence_fps\": " << c.fps << ", \"wall_ms\": " << c.wall_ms << "},\n";
    json << "      \"latency\": {\"mean_ms\": " << c.mean_ms << ", \"median_ms\": " << c.median_ms
         << ", \"p95_ms\": " << c.p95_ms << ", \"p99_ms\": " << c.p99_ms
         << ", \"first_frame_ms\": " << c.first_ms << "},\n";
    json << "      \"bandwidth\": {\"raw_input_gbps\": " << (raw_bytes * c.fps / 1.0e9)
         << ", \"output_mbps\": " << (static_cast<double>(c.mean_bytes) * c.fps / 1.0e6) << "},\n";
    json << "      \"compression\": {\"mean_bytes\": " << c.mean_bytes << ", \"min_bytes\": " << c.min_bytes
         << ", \"max_bytes\": " << c.max_bytes
         << ", \"distinct_fs_rate_limiters\": " << c.distinct_fs_rate_limiters
         << ", \"sample_bytes_fnv64\": \"" << HexU64(c.sample_bytes_fnv64) << "\""
         << ", \"fs_rate_limiter_fnv64\": \"" << HexU64(c.fs_rate_limiter_fnv64) << "\""
         << ", \"quality\": \"CFHD_ENCODING_QUALITY_FILMSCAN3\","
         << " \"encoded_format\": \"CFHD_ENCODED_FORMAT_YUV_422\", \"source_format\": \"BGRA8\"}\n";
    json << "    }";
}

bool WantRes(const Options& opt, const char* key) {
    return opt.resolutions.find(key) != std::string::npos || opt.resolutions == "all";
}

void WarmGpu(const ResolutionSpec& spec, FramePool& pool) {
    std::cout << "  warmup " << spec.name << "\n" << std::flush;
    auto r = RunGpu(spec, pool, hypercine::Phase3AInputMode::HostPageableEndToEnd, true, 0,
                    std::min(48, spec.study_frames), false, false);
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
        else if (arg.rfind("--intended-tag=", 0) == 0) opt.intended_release_tag = arg.substr(15);
        else if (arg.rfind("--intended-release-tag=", 0) == 0) opt.intended_release_tag = arg.substr(23);
        else if (arg.rfind("--release-commit=", 0) == 0) opt.intended_release_tag = arg.substr(17);
        else if (arg.rfind("--trials=", 0) == 0) opt.trials = std::max(1, std::atoi(arg.substr(9).c_str()));
        else if (arg.rfind("--resolutions=", 0) == 0) opt.resolutions = arg.substr(14);
        else if (const std::string v = need("--suite"); !v.empty()) opt.suite = v;
        else if (const std::string v = need("--out"); !v.empty()) opt.out_path = v;
        else if (const std::string v = need("--intended-tag"); !v.empty()) opt.intended_release_tag = v;
        else if (const std::string v = need("--intended-release-tag"); !v.empty()) opt.intended_release_tag = v;
        else if (const std::string v = need("--release-commit"); !v.empty()) opt.intended_release_tag = v;
        else if (const std::string v = need("--trials"); !v.empty()) opt.trials = std::max(1, std::atoi(v.c_str()));
        else if (const std::string v = need("--resolutions"); !v.empty()) opt.resolutions = v;
    }

    GitInfo git = ReadGit();
    if ((git.dirty || !git.upstream_clean) && !opt.allow_dirty) {
        std::cerr << "REFUSING publication: working tree is dirty. Pass --allow-dirty to record a DIRTY result.\n";
        std::cerr << git.status << "\n";
        return 2;
    }

    std::filesystem::path out_file(opt.out_path);
    if (out_file.empty()) {
        std::error_code ec;
        out_file = std::filesystem::temp_directory_path(ec) / "phase3a2c_release_candidate.json";
    } else if (out_file.is_relative()) {
        out_file = std::filesystem::absolute(std::filesystem::path(git.repo_root) / out_file);
    } else {
        out_file = std::filesystem::absolute(out_file);
    }

    bool out_is_inside_repo = false;
    {
        std::error_code ec;
        std::filesystem::path can_out = std::filesystem::weakly_canonical(out_file, ec);
        std::filesystem::path can_repo = std::filesystem::weakly_canonical(git.repo_root, ec);
        auto [root_end, _] = std::mismatch(can_repo.begin(), can_repo.end(), can_out.begin(), can_out.end());
        if (root_end == can_repo.end()) {
            out_is_inside_repo = true;
        }
    }
    if (out_is_inside_repo && !opt.allow_dirty) {
        std::cerr << "REFUSING publication: canonical benchmark output path must be outside the repository.\n";
        std::cerr << "Specified out: " << out_file.string() << "\n";
        return 2;
    }
    const HardwareInfo hw = ReadHardware();
    std::vector<TrialRecord> records;
    std::vector<EquivalenceReport> eq_reports;

    auto consider = [&](const ResolutionSpec& spec) {
        if (!WantRes(opt, spec.key == std::string("1080p") ? "1080" : spec.key)) return;
        FramePool pool;
        std::string error;
        if (!BuildPool(spec, spec.unique_frames, -1, &pool, &error)) {
            std::cerr << error << "\n";
            return;
        }
        if (opt.suite != "fresh") WarmGpu(spec, pool);

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
            rec.gpu = RunGpu(spec, pool, mode, graph, slots, frames, verify, false);
            std::cout << "    fps=" << rec.gpu.sustained_fps
                      << " steady=" << rec.gpu.steady_state_fps
                      << " lat=" << rec.gpu.mean_latency_ms
                      << " clock=" << rec.gpu.nvml.graphics_clock_mhz_mean
                      << " slots=" << rec.gpu.slot_selection.selected_slots
                      << " mean_bytes=" << rec.gpu.mean_bitstream_bytes
                      << " ok=" << rec.gpu.ok << " " << rec.gpu.error << "\n" << std::flush;
            records.push_back(std::move(rec));
        };

        if (opt.suite == "all" || opt.suite == "headline" || opt.suite == "equivalence") {
            EquivalenceReport eq = ValidateEquivalenceGate(spec, pool, &records);
            eq_reports.push_back(std::move(eq));
        }

        if (opt.suite == "all" || opt.suite == "headline" || opt.suite == "fresh") {
            const int trials = (opt.suite == "fresh") ? 1 : opt.trials;
            const int warmup = CommonWarmupFor(spec, 0);
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
                    cpu.cpu = RunUpstream(pool, warmup, spec.headline_frames, false);
                    std::cout << "    cpu fps=" << cpu.cpu.fps
                              << " mean_bytes=" << cpu.cpu.mean_bytes << "\n" << std::flush;
                    records.push_back(std::move(cpu));
                }
                const int ord = t % 3;
                for (auto mode : orders[ord]) {
                    add_gpu(opt.suite == "fresh" ? "fresh" : "headline", mode, true, 0, t, ord, spec.headline_frames, false);
                }
            }

            // Verify headline output-size fairness across all modes for this resolution:
            for (auto& eq : eq_reports) {
                if (eq.resolution != spec.name) continue;
                bool first_rec = true;
                for (const auto& rec : records) {
                    if (rec.suite != "headline" || rec.resolution != spec.name) continue;
                    const size_t mb = rec.is_cpu ? rec.cpu.mean_bytes : rec.gpu.mean_bitstream_bytes;
                    const size_t minb = rec.is_cpu ? rec.cpu.min_bytes : rec.gpu.min_bitstream_bytes;
                    const size_t maxb = rec.is_cpu ? rec.cpu.max_bytes : rec.gpu.max_bitstream_bytes;
                    const int dfs = rec.is_cpu ? rec.cpu.distinct_fs_rate_limiters : rec.gpu.distinct_fs_rate_limiters;
                    if (first_rec) {
                        eq.headline_mean_bytes = mb;
                        eq.headline_min_bytes = minb;
                        eq.headline_max_bytes = maxb;
                        eq.headline_distinct_fs_limiters = dfs;
                        first_rec = false;
                    } else if (mb != eq.headline_mean_bytes || minb != eq.headline_min_bytes ||
                               maxb != eq.headline_max_bytes || dfs != eq.headline_distinct_fs_limiters) {
                        eq.headline_output_size_stats_match = false;
                        eq.ok = false;
                        if (eq.divergence_reason.empty()) {
                            eq.divergence_reason = "headline output sample byte statistics differed between CPU and GPU";
                        }
                    }
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
            const int frames = spec.key == std::string("8k") ? 8 : 16;
            auto add_exact_graph = [&](bool graph_on) {
                TrialRecord rec;
                rec.suite = "graph_identity";
                rec.resolution = spec.name;
                rec.width = spec.width;
                rec.height = spec.height;
                rec.mode = "HostPageableEndToEnd";
                rec.graph = graph_on;
                rec.pool_note = pool.note;
                std::cout << "  graph identity " << spec.name << " graph=" << graph_on << "\n" << std::flush;
                rec.gpu = RunGpu(spec, pool, hypercine::Phase3AInputMode::HostPageableEndToEnd,
                                 graph_on, 0, frames, true, true);
                std::cout << "    fps=" << rec.gpu.sustained_fps << " ok=" << rec.gpu.ok
                          << " " << rec.gpu.error << "\n" << std::flush;
                records.push_back(std::move(rec));
            };
            add_exact_graph(false);
            add_exact_graph(true);
        }
    };

    std::cout << "Phase 3A.2c canonical benchmark suite=" << opt.suite
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

    bool all_eq_verified = !eq_reports.empty();
    for (const auto& eq : eq_reports) {
        if (!eq.ok || !eq.headline_output_size_stats_match) all_eq_verified = false;
    }
    const bool native_arch_match =
        ArchListMatchesRuntime(HYPERCINE_CMAKE_CUDA_ARCH, hw.cuda_arch_digits) &&
        ArchListMatchesRuntime(HYPERCINE_TARGET_CUDA_ARCH, hw.cuda_arch_digits);

    // Re-verify repository cleanliness before assembling publication JSON
    const GitInfo git_check = ReadGit();
    if ((git_check.dirty || !git_check.upstream_clean) && !opt.allow_dirty) {
        std::cerr << "REFUSING publication: working tree became dirty during benchmark run. Pass --allow-dirty to record a DIRTY result.\n";
        std::cerr << git_check.status << "\n";
        return 2;
    }
    git = git_check;

    const bool is_canonical = !git.dirty && git.upstream_clean && all_eq_verified && native_arch_match && !out_is_inside_repo;

    std::ostringstream json;
    json << std::fixed << std::setprecision(4);
    json << "{\n";
    json << "  \"publication\": \"" << (git.dirty ? "DIRTY" : "CLEAN") << "\",\n";
    json << "  \"canonical\": " << (is_canonical ? "true" : "false") << ",\n";
    json << "  \"benchmark_artifact_schema\": \"3.0\",\n";
    json << "  \"benchmarked_source_commit\": \"" << JsonEscape(git.commit) << "\",\n";
    json << "  \"intended_release_tag\": \"" << JsonEscape(opt.intended_release_tag) << "\",\n";
    json << "  \"benchmark_tree_clean\": " << (git.dirty ? "false" : "true") << ",\n";
    json << "  \"upstream_tree_clean\": " << (git.upstream_clean ? "true" : "false") << ",\n";
    json << "  \"git_branch\": \"" << JsonEscape(git.branch) << "\",\n";
    json << "  \"benchmark_contract\": \"ProductionEndToEnd_v2\",\n";
    json << "  \"primary_public_benchmark\": \"HostPageable ProductionEndToEnd_v2\",\n";
    json << "  \"benchmark_equivalence_verified\": " << (all_eq_verified ? "true" : "false") << ",\n";
    json << "  \"git\": {\n";
    json << "    \"benchmarked_source_commit\": \"" << JsonEscape(git.commit) << "\",\n";
    json << "    \"branch\": \"" << JsonEscape(git.branch) << "\",\n";
    json << "    \"benchmark_tree_clean\": " << (git.dirty ? "false" : "true") << ",\n";
    json << "    \"upstream_tree_clean\": " << (git.upstream_clean ? "true" : "false") << ",\n";
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
    json << "    \"target_cuda_architectures\": \"" << JsonEscape(HYPERCINE_TARGET_CUDA_ARCH) << "\",\n";
    json << "    \"native_architecture_match\": " << (native_arch_match ? "true" : "false") << ",\n";
    json << "    \"generated_cuda_code\": \"--generate-code=arch=compute_" << JsonEscape(HYPERCINE_TARGET_CUDA_ARCH)
         << ",code=[compute_" << JsonEscape(HYPERCINE_TARGET_CUDA_ARCH) << ",sm_" << JsonEscape(HYPERCINE_TARGET_CUDA_ARCH)
         << "] (native cubin sm_" << JsonEscape(HYPERCINE_TARGET_CUDA_ARCH) << " + PTX compute_"
         << JsonEscape(HYPERCINE_TARGET_CUDA_ARCH) << ")\",\n";
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
    json << "    \"slot_policy_name\": \"DefaultAuto\",\n";
    json << "    \"slot_count\": {\"1080p\": 2, \"4K_UHD\": 3, \"8K_UHD\": 2, \"policy\": \"DefaultAuto\"},\n";
    json << "    \"common_warmup_frames\": " << kCommonWarmupFrames << ",\n";
    json << "    \"first_measured_logical_frame_index\": " << kCommonWarmupFrames << ",\n";
    json << "    \"trial_count\": " << opt.trials << ",\n";
    json << "    \"benchmark_order\": \"resolutions=" << JsonEscape(opt.resolutions) << "; per-trial GPU mode rotation across 3 permutations\",\n";
    json << "    \"sequence_cycle_definition\": \"Common warmup W=4 encodes pool frames 0..3; timed section encodes N frames starting at logical sequence index 4 using pool[(4 + i) % unique_cycle] for both UpstreamCpuEncodeSample and all HyperCine GPU modes without restarting at frame 0 after warmup (1080p: N=1000, cycle=96; 4K_UHD: N=300, cycle=36; 8K_UHD: N=120, cycle=12).\"\n";
    json << "  },\n";
    json << "  \"default_auto_policy\": {\n";
    json << "    \"canonical_policy_name\": \"DefaultAuto\",\n";
    json << "    \"tuning_override_policy_name\": \"UserOverride\",\n";
    json << "    \"source_location\": \"src/core/phase2d_encoder.cpp:SelectPhase2DFrameSlots\",\n";
    json << "    \"policy_distinction\": \"DefaultAuto is the canonical balanced/default runtime policy used for all headline benchmarks. UserOverride is an explicit manual tuning/experimentation policy used only in the separate Manual Slot Scaling / Throughput Tuning Study.\",\n";
    json << "    \"source_code_rationale\": \"SelectPhase2DFrameSlots computes per_slot_vram_bytes = 20*px + 4,718,592 and session_workspace_vram_bytes = 12*px + 33,554,432, caps vram_max_slots to 75% of free VRAM after a 256 MiB + workspace reserve, and under DefaultAuto clamps a resolution-bucket target_slots hardcoded during Phase 2D empirical tuning (target_slots=2 for <=1080p, target_slots=3 for <=4K UHD, target_slots=2 for >4K UHD / 8K UHD). It does not dynamically optimize slot counts per GPU SKU at runtime.\",\n";
    json << "    \"resolutions\": [\n";
    for (size_t si = 0; si < sizeof(kSpecs) / sizeof(kSpecs[0]); ++si) {
        const auto& sp = kSpecs[si];
        const auto sel = hypercine::SelectPhase2DFrameSlots(
            sp.width, sp.height, hypercine::Phase2DSlotPolicy::DefaultAuto, 0,
            hypercine::Phase2DD2HPolicy::Throughput);
        json << "      {\n";
        json << "        \"resolution\": \"" << sp.name << "\",\n";
        json << "        \"width\": " << sp.width << ",\n";
        json << "        \"height\": " << sp.height << ",\n";
        const char* policy_reason = (sp.width <= 1920)
            ? "Hardcoded Phase 2D DefaultAuto target_slots=2 for px<=1920x1080 (clamped by vram_max_slots); FrameConsolidated D2H selected under Throughput policy."
            : ((sp.width <= 3840)
                ? "Hardcoded Phase 2D DefaultAuto target_slots=3 for 1080p<px<=3840x2160 (clamped by vram_max_slots); FrameConsolidated D2H selected under Throughput policy."
                : "Hardcoded Phase 2D DefaultAuto target_slots=2 for px>3840x2160 (8K UHD, clamped by vram_max_slots); FrameConsolidated D2H selected under Throughput policy.");
        json << "        \"slot_policy\": \"" << SlotPolicyName(sel.policy) << "\",\n";
        json << "        \"policy_target_slots\": " << sel.policy_target_slots << ",\n";
        json << "        \"selected_slots\": " << sel.selected_slots << ",\n";
        json << "        \"vram_max_slots\": " << sel.vram_max_slots << ",\n";
        json << "        \"per_slot_estimated_vram_bytes\": " << sel.per_slot_vram_bytes << ",\n";
        json << "        \"session_workspace_estimated_vram_bytes\": " << sel.session_workspace_vram_bytes << ",\n";
        json << "        \"total_estimated_vram_bytes\": " << sel.total_estimated_vram_bytes << ",\n";
        json << "        \"d2h_policy\": \"Throughput\",\n";
        json << "        \"d2h_mode\": \"" << (sel.selected_d2h_mode == hypercine::CompressedD2HMode::FrameConsolidated
                                                ? "FrameConsolidated"
                                                : "BandStreamed") << "\",\n";
        json << "        \"policy_reason\": \"" << JsonEscape(policy_reason) << "\"\n";
        json << "      }" << (si + 1 < sizeof(kSpecs) / sizeof(kSpecs[0]) ? ",\n" : "\n");
    }
    json << "    ]\n";
    json << "  },\n";
    json << "  \"benchmark_equivalence\": {\n";
    json << "    \"verified\": " << (all_eq_verified ? "true" : "false") << ",\n";
    json << "    \"compared_fields\": [\n";
    json << "      \"measured_logical_frame_index (W .. W+N-1)\",\n";
    json << "      \"source_pattern\",\n";
    json << "      \"prev_encoded_bytes\",\n";
    json << "      \"lastgopbitcount\",\n";
    json << "      \"FSratelimiter\",\n";
    json << "      \"quant_table[36] and scale_table[36]\",\n";
    json << "      \"output_sample_bytes (per-frame and full-headline mean/min/max)\",\n";
    json << "      \"masked_sample_sha256 (UpstreamCpuEncodeSample == HostPageableEndToEnd == HostPinnedApi == DeviceInput)\",\n";
    json << "      \"unmasked_sample_sha256 across GPU modes in deterministic verification mode\",\n";
    json << "      \"upstream CFHD_DecodeSample frame stamp and PSNR > 35 dB\"\n";
    json << "    ],\n";
    json << "    \"resolutions\": [\n";
    for (size_t ei = 0; ei < eq_reports.size(); ++ei) {
        const auto& eq = eq_reports[ei];
        json << "      {\n";
        json << "        \"resolution\": \"" << eq.resolution << "\",\n";
        json << "        \"width\": " << eq.width << ",\n";
        json << "        \"height\": " << eq.height << ",\n";
        json << "        \"ok\": " << (eq.ok ? "true" : "false") << ",\n";
        json << "        \"warmup_frames\": " << eq.warmup_frames << ",\n";
        json << "        \"first_measured_logical_frame_index\": " << eq.first_measured_logical_frame << ",\n";
        json << "        \"validation_frames\": " << eq.validation_frames << ",\n";
        json << "        \"matched_validation_frames\": " << eq.matched_validation_frames << ",\n";
        json << "        \"logical_indices_match\": " << (eq.logical_indices_match ? "true" : "false") << ",\n";
        json << "        \"patterns_match\": " << (eq.patterns_match ? "true" : "false") << ",\n";
        json << "        \"prev_encoded_bytes_match\": " << (eq.prev_encoded_bytes_match ? "true" : "false") << ",\n";
        json << "        \"lastgopbitcount_match\": " << (eq.lastgopbitcount_match ? "true" : "false") << ",\n";
        json << "        \"fs_rate_limiter_trajectory_match\": " << (eq.fs_rate_limiter_trajectory_match ? "true" : "false") << ",\n";
        json << "        \"quant_table_trajectory_match\": " << (eq.quant_table_trajectory_match ? "true" : "false") << ",\n";
        json << "        \"sample_bytes_trajectory_match\": " << (eq.sample_bytes_trajectory_match ? "true" : "false") << ",\n";
        json << "        \"masked_sample_sha256_match_upstream\": " << (eq.masked_sample_sha256_match_upstream ? "true" : "false") << ",\n";
        json << "        \"gpu_modes_unmasked_sha256_match\": " << (eq.gpu_modes_unmasked_sha256_match ? "true" : "false") << ",\n";
        json << "        \"decode_and_stamp_verified\": " << (eq.decode_and_stamp_verified ? "true" : "false") << ",\n";
        json << "        \"headline_output_size_stats_match\": " << (eq.headline_output_size_stats_match ? "true" : "false") << ",\n";
        json << "        \"first_divergent_frame\": " << eq.first_divergent_frame << ",\n";
        json << "        \"divergence_reason\": \"" << JsonEscape(eq.divergence_reason) << "\",\n";
        json << "        \"validation_mean_sample_bytes\": " << eq.validation_mean_bytes << ",\n";
        json << "        \"validation_min_sample_bytes\": " << eq.validation_min_bytes << ",\n";
        json << "        \"validation_max_sample_bytes\": " << eq.validation_max_bytes << ",\n";
        json << "        \"validation_distinct_fs_rate_limiters\": " << eq.validation_distinct_fs_limiters << ",\n";
        json << "        \"validation_sample_bytes_fnv64\": \"" << eq.validation_sample_bytes_fnv64 << "\",\n";
        json << "        \"validation_fs_rate_limiter_fnv64\": \"" << eq.validation_fs_rate_limiter_fnv64 << "\",\n";
        json << "        \"validation_masked_sequence_fnv64\": \"" << eq.validation_masked_sequence_fnv64 << "\",\n";
        json << "        \"headline_frames\": " << eq.headline_frames << ",\n";
        json << "        \"headline_mean_sample_bytes\": " << eq.headline_mean_bytes << ",\n";
        json << "        \"headline_min_sample_bytes\": " << eq.headline_min_bytes << ",\n";
        json << "        \"headline_max_sample_bytes\": " << eq.headline_max_bytes << ",\n";
        json << "        \"headline_distinct_fs_rate_limiters\": " << eq.headline_distinct_fs_limiters << "\n";
        json << "      }" << (ei + 1 < eq_reports.size() ? ",\n" : "\n");
    }
    json << "    ]\n";
    json << "  },\n";
    json << "  \"cpu_baseline_note\": \"Baseline 1 is pristine CFHD_EncodeSample with _THREADED_ENCODER=0 and one encode thread. Baseline 2, the HyperCine CPU reference inside VerifyPhase3ASequence120, is an equivalence oracle using the upstream spatial wavelet and CPU entropy. It is not a second throughput product, so speedups use Baseline 1 only.\",\n";
    json << "  \"device_input_note\": \"DeviceInput measures GPU-resident integration throughput when BGRA frames are already in device memory; it must never be presented as conventional file-transcode performance.\",\n";
    json << "  \"headline_summary\": [\n";
    {
        const char* mode_names[] = {"UpstreamCpuEncodeSample", "HostPageableEndToEnd", "HostPinnedApi", "DeviceInput"};
        bool first_summary = true;
        for (const auto& spec : kSpecs) {
            double cpu_median_fps = 0.0;
            size_t cpu_mean_bytes = 0, cpu_min_bytes = 0, cpu_max_bytes = 0;
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
                int temperature_trials = 0, throttle_trials = 0;
                int slots_used = 1;
                bool graph_used = false;
                size_t mean_bytes = 0, min_bytes = 0, max_bytes = 0;
                int distinct_fs = 0;
                std::string throttles;
                for (const TrialRecord* tr : matching) {
                    if (tr->is_cpu) {
                        fps_vals.push_back(tr->cpu.fps);
                        mean_lat_vals.push_back(tr->cpu.mean_ms);
                        med_lat_vals.push_back(tr->cpu.median_ms);
                        p95_lat_vals.push_back(tr->cpu.p95_ms);
                        p99_lat_vals.push_back(tr->cpu.p99_ms);
                        mean_bytes = tr->cpu.mean_bytes;
                        min_bytes = tr->cpu.min_bytes;
                        max_bytes = tr->cpu.max_bytes;
                        distinct_fs = tr->cpu.distinct_fs_rate_limiters;
                    } else {
                        fps_vals.push_back(tr->gpu.sustained_fps);
                        mean_lat_vals.push_back(tr->gpu.mean_latency_ms);
                        med_lat_vals.push_back(tr->gpu.median_latency_ms);
                        p95_lat_vals.push_back(tr->gpu.p95_latency_ms);
                        p99_lat_vals.push_back(tr->gpu.p99_latency_ms);
                        if (tr->gpu.nvml.graphics_clock_sample_count) clk_vals.push_back(tr->gpu.nvml.graphics_clock_mhz_mean);
                        if (tr->gpu.nvml.mem_clock_sample_count) mem_clk_vals.push_back(tr->gpu.nvml.mem_clock_mhz_mean);
                        if (tr->gpu.nvml.power_sample_count) pwr_vals.push_back(tr->gpu.nvml.power_w_mean);
                        if (tr->gpu.nvml.temperature_sample_count) {
                            ++temperature_trials;
                            max_temp = std::max(max_temp, tr->gpu.nvml.temperature_c_max);
                        }
                        slots_used = tr->gpu.slot_selection.selected_slots;
                        graph_used = tr->graph;
                        mean_bytes = tr->gpu.mean_bitstream_bytes;
                        min_bytes = tr->gpu.min_bitstream_bytes;
                        max_bytes = tr->gpu.max_bitstream_bytes;
                        distinct_fs = tr->gpu.distinct_fs_rate_limiters;
                        if (tr->gpu.nvml.throttle_sample_count) {
                            ++throttle_trials;
                            if (!throttles.empty()) throttles += "; ";
                            throttles += tr->gpu.nvml.throttle_reasons_str;
                        }
                    }
                }
                double mean_fps = 0.0, median_fps = 0.0, stddev_fps = 0.0, min_fps = 0.0, max_fps = 0.0;
                Stats(fps_vals, &mean_fps, &median_fps, &stddev_fps, &min_fps, &max_fps);
                if (std::string(mname) == "UpstreamCpuEncodeSample") {
                    cpu_median_fps = median_fps;
                    cpu_mean_bytes = mean_bytes;
                    cpu_min_bytes = min_bytes;
                    cpu_max_bytes = max_bytes;
                }
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
                const bool bytes_match_cpu = (mean_bytes == cpu_mean_bytes) &&
                                             (min_bytes == cpu_min_bytes) &&
                                             (max_bytes == cpu_max_bytes);
                if (!first_summary) json << ",\n";
                first_summary = false;
                json << "    {\n";
                json << "      \"resolution\": \"" << spec.name << "\",\n";
                json << "      \"width\": " << spec.width << ",\n";
                json << "      \"height\": " << spec.height << ",\n";
                json << "      \"input_mode\": \"" << mname << "\",\n";
                json << "      \"trials\": " << matching.size() << ",\n";
                json << "      \"slot_policy\": \"" << (std::string(mname) == "UpstreamCpuEncodeSample" ? "N/A" : "DefaultAuto") << "\",\n";
                json << "      \"graph_mode\": " << (graph_used ? "true" : "false") << ",\n";
                json << "      \"slot_count\": " << slots_used << ",\n";
                json << "      \"warmup_frames\": " << kCommonWarmupFrames << ",\n";
                json << "      \"first_measured_logical_frame_index\": " << kCommonWarmupFrames << ",\n";
                json << "      \"measured_frames\": " << spec.headline_frames << ",\n";
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
                json << "      \"mean_sample_bytes\": " << mean_bytes << ",\n";
                json << "      \"min_sample_bytes\": " << min_bytes << ",\n";
                json << "      \"max_sample_bytes\": " << max_bytes << ",\n";
                json << "      \"mean_bitstream_bytes\": " << mean_bytes << ",\n";
                json << "      \"distinct_fs_rate_limiters\": " << distinct_fs << ",\n";
                json << "      \"sample_bytes_match_upstream_exact\": " << (bytes_match_cpu ? "true" : "false") << ",\n";
                json << "      \"speedup_vs_upstream_cpu_median\": " << speedup << ",\n";
                json << "      \"graphics_clock_mhz_mean\": " << hypercine::detail::TelemetryJson(mean_clk, clk_vals.size()) << ",\n";
                json << "      \"mem_clock_mhz_mean\": " << hypercine::detail::TelemetryJson(mean_mem_clk, mem_clk_vals.size()) << ",\n";
                json << "      \"power_w_mean\": " << hypercine::detail::TelemetryJson(mean_pwr, pwr_vals.size()) << ",\n";
                json << "      \"temperature_c_max\": " << hypercine::detail::TelemetryJson(max_temp, temperature_trials) << ",\n";
                json << "      \"throttle_reasons\": " << hypercine::detail::TelemetryTextJson(throttles, throttle_trials) << "\n";
                json << "    }";
            }
        }
    }
    json << "\n  ],\n";
    json << "  \"slot_scaling_study_summary\": {\n";
    json << "    \"study_name\": \"Manual Slot Scaling / Throughput Tuning Study\",\n";
    json << "    \"slot_policy\": \"UserOverride\",\n";
    json << "    \"note\": \"The canonical benchmark uses the encoder's DefaultAuto slot policy. The slot-scaling study intentionally overrides that policy to measure throughput/latency/VRAM trade-offs and is not the public headline configuration.\",\n";
    json << "    \"results\": [\n";
    {
        bool first_slot_entry = true;
        for (const auto& spec : kSpecs) {
            const int default_slots = SlotCountFor(spec, 0);
            for (int slots = 1; slots <= 4; ++slots) {
                std::vector<const TrialRecord*> matching;
                for (const auto& rec : records) {
                    if (rec.suite == "slots" && rec.resolution == spec.name && rec.requested_slots == slots) {
                        matching.push_back(&rec);
                    }
                }
                if (matching.empty()) continue;
                std::vector<double> fps_vals, mean_lat_vals, med_lat_vals, p95_lat_vals, vram_used_vals;
                size_t est_vram = 0, per_slot_vram = 0, ws_vram = 0;
                for (const TrialRecord* tr : matching) {
                    fps_vals.push_back(tr->gpu.sustained_fps);
                    mean_lat_vals.push_back(tr->gpu.mean_latency_ms);
                    med_lat_vals.push_back(tr->gpu.median_latency_ms);
                    p95_lat_vals.push_back(tr->gpu.p95_latency_ms);
                    if (tr->gpu.measured_global_vram_delta_bytes)
                        vram_used_vals.push_back(static_cast<double>(*tr->gpu.measured_global_vram_delta_bytes));
                    est_vram = tr->gpu.slot_selection.total_estimated_vram_bytes;
                    per_slot_vram = tr->gpu.slot_selection.per_slot_vram_bytes;
                    ws_vram = tr->gpu.slot_selection.session_workspace_vram_bytes;
                }
                double mean_fps = 0.0, median_fps = 0.0, mean_lat = 0.0, med_lat = 0.0, p95_lat = 0.0, med_vram = 0.0;
                Stats(fps_vals, &mean_fps, &median_fps, nullptr, nullptr, nullptr);
                Stats(mean_lat_vals, &mean_lat, nullptr, nullptr, nullptr, nullptr);
                Stats(med_lat_vals, &med_lat, nullptr, nullptr, nullptr, nullptr);
                Stats(p95_lat_vals, &p95_lat, nullptr, nullptr, nullptr, nullptr);
                Stats(vram_used_vals, nullptr, &med_vram, nullptr, nullptr, nullptr);
                if (!first_slot_entry) json << ",\n";
                first_slot_entry = false;
                json << "      {\n";
                json << "        \"resolution\": \"" << spec.name << "\",\n";
                json << "        \"slots\": " << slots << ",\n";
                json << "        \"slot_policy\": \"UserOverride\",\n";
                json << "        \"matches_default_auto_slot_count\": " << (slots == default_slots ? "true" : "false") << ",\n";
                json << "        \"trials\": " << matching.size() << ",\n";
                json << "        \"median_fps\": " << median_fps << ",\n";
                json << "        \"mean_fps\": " << mean_fps << ",\n";
                json << "        \"mean_latency_ms\": " << mean_lat << ",\n";
                json << "        \"median_latency_ms\": " << med_lat << ",\n";
                json << "        \"p95_latency_ms\": " << p95_lat << ",\n";
                json << "        \"per_slot_estimated_vram_bytes\": " << per_slot_vram << ",\n";
                json << "        \"session_workspace_estimated_vram_bytes\": " << ws_vram << ",\n";
                json << "        \"total_estimated_vram_bytes\": " << est_vram << ",\n";
                json << "        \"measured_vram_trial_count\": " << vram_used_vals.size() << ",\n";
                json << "        \"measured_global_vram_delta_bytes_median\": ";
                if (vram_used_vals.empty()) json << "null";
                else json << med_vram;
                json << "\n";
                json << "      }";
            }
        }
    }
    json << "\n    ]\n";
    json << "  },\n";
    json << "  \"runs\": [\n";
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].is_cpu) WriteCpu(json, records[i]);
        else WriteGpu(json, records[i]);
        if (i + 1 < records.size()) json << ",\n";
        else json << "\n";
    }
    json << "  ]\n}\n";

    std::error_code ec;
    if (out_file.has_parent_path()) std::filesystem::create_directories(out_file.parent_path(), ec);
    std::ofstream file(out_file, std::ios::binary);
    if (!file) {
        std::cerr << "failed to write " << out_file.string() << "\n";
        return 1;
    }
    file << json.str();
    std::cout << "Wrote " << out_file.string() << " (" << json.str().size()
              << " bytes, runs=" << records.size()
              << ", equivalence_verified=" << (all_eq_verified ? "true" : "false") << ")\n";
    return all_eq_verified ? 0 : 3;
}

