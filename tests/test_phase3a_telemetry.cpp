// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "phase3a_telemetry.h"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace hypercine;

// Fake Windows/NVML boundary; the generated sampler is the production class.
// No driver or GPU is loaded by this executable, including on Windows.
using HMODULE = void*;
static bool provider_present = true;
static bool readings_work = true;
static int power_status = 0;
static const char* missing_symbol = "";
static int init_status = 0, device_status = 0;
static int init_calls = 0, shutdown_calls = 0, load_calls = 0, unload_calls = 0;
static int init() { ++init_calls; return init_status; }
static int shutdown_nvml() { ++shutdown_calls; return 0; }
static int get_device(unsigned, void** dev) {
    if (device_status) return device_status;
    *dev = reinterpret_cast<void*>(1); return 0;
}
struct NvmlUtilization;
static int utilization(void*, NvmlUtilization* ptr);

static int clock_info(void*, int type, unsigned* v) {
    if (!readings_work) return 3;
    *v = type == 0 ? 1200 : 5000; return 0;
}
static int temperature(void*, int, unsigned* v) {
    if (!readings_work) return 3;
    *v = 60; return 0;
}
static int power(void*, unsigned* v) {
    if (!readings_work || power_status) return 3;
    *v = 125000; return 0;
}
static int throttle(void*, unsigned long long* v) {
    if (!readings_work) return 3;
    *v = 0; return 0;
}
static HMODULE LoadLibraryA(const char*) {
    if (!provider_present) return nullptr;
    ++load_calls; return reinterpret_cast<void*>(1);
}
static void FreeLibrary(HMODULE) { ++unload_calls; }
static void* GetProcAddress(HMODULE, const char* name) {
    if (!std::strcmp(name, missing_symbol)) return nullptr;
    if (!std::strcmp(name, "nvmlInit_v2")) return reinterpret_cast<void*>(&init);
    if (!std::strcmp(name, "nvmlShutdown")) return reinterpret_cast<void*>(&shutdown_nvml);
    if (!std::strcmp(name, "nvmlDeviceGetHandleByIndex_v2")) return reinterpret_cast<void*>(&get_device);
    if (!std::strcmp(name, "nvmlDeviceGetUtilizationRates")) return reinterpret_cast<void*>(&utilization);
    if (!std::strcmp(name, "nvmlDeviceGetClockInfo")) return reinterpret_cast<void*>(&clock_info);
    if (!std::strcmp(name, "nvmlDeviceGetTemperature")) return reinterpret_cast<void*>(&temperature);
    if (!std::strcmp(name, "nvmlDeviceGetPowerUsage")) return reinterpret_cast<void*>(&power);
    if (!std::strcmp(name, "nvmlDeviceGetCurrentClocksThrottleReasons")) return reinterpret_cast<void*>(&throttle);
    return nullptr;
}
#ifndef _WIN32
#define _WIN32
#define HYPERCINE_TEST_UNDEFINE_WIN32
#endif
#include "telemetry_sampler.inc"
#ifdef HYPERCINE_TEST_UNDEFINE_WIN32
#undef _WIN32
#endif
static int utilization(void*, NvmlUtilization* ptr) {
    if (!readings_work) return 3;
    ptr->gpu = 0;
    ptr->memory = 50;
    return 0;
}

namespace hypercine {
const char* Phase3AInputModeName(Phase3AInputMode) { return "HostPageableEndToEnd"; }
const char* Phase3AQuantModeName(Phase3AQuantMode) { return "DynamicUpstreamState"; }
}
static std::string JsonEscape(const std::string& value) { return value; }
#include "telemetry_writer.inc"
struct TrialRecord {
    std::string suite, resolution, mode, pool_note, prep_error;
    int width = 0, height = 0, requested_slots = 0, trial = 0, order_index = 0;
    bool graph = false;
    Phase3AResult gpu;
};
static const char* SlotPolicyName(Phase2DSlotPolicy) { return "DefaultAuto"; }
#include "telemetry_canonical_writer.inc"

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        detail::NvmlAccumulator acc;
        Check(!acc.Finish(false).nvml_available, "absent provider");
        detail::NvmlSample first;
        first.gpu_util = 0; first.mem_util = 0; first.gclk = 1000;
        first.mclk = 4000; first.temp = 50; first.power_w = 0; first.throttle = 0;
        acc.Add(first);
        acc.Add({}); // failed poll must not bias mean, extrema or endpoints
        detail::NvmlSample last;
        last.gpu_util = 100; last.mem_util = 80; last.gclk = 1500;
        last.temp = 70; last.power_w = 200; last.throttle = 4;
        acc.Add(last);
        auto mixed = acc.Finish(true);
        Check(mixed.sample_count == 3 && mixed.utilization_sample_count == 2, "success denominator");
        Check(mixed.hardware_gpu_utilization_pct_mean == 50 && mixed.hardware_mem_utilization_pct_mean == 40, "utilization means");
        Check(mixed.graphics_clock_mhz_min == 1000 && mixed.graphics_clock_mhz_mean == 1250, "clock extrema/mean");
        Check(mixed.mem_clock_mhz_mean == 4000 && mixed.mem_clock_sample_count == 1, "independent clock failures");
        Check(mixed.temperature_c_start == 50 && mixed.temperature_c_end == 70 && mixed.temperature_c_max == 70, "temperature endpoints");
        Check(mixed.power_w_mean == 100 && mixed.power_w_max == 200 && mixed.throttle_reasons_bitmask == 4, "zero readings and throttle union");
        detail::NvmlAccumulator long_run;
        for (int i = 0; i < 1000000; ++i) long_run.Add(first);
        auto million = long_run.Finish(true);
        Check(million.sample_count == 1000000 && million.graphics_clock_mhz_mean == 1000, "million-poll aggregation");
        Check(sizeof(detail::NvmlAccumulator) < 512, "fixed accumulator storage");

        Phase3AResult memory;
        memory.slot_selection.total_estimated_vram_bytes = 9999;
        detail::SetMeasuredVramUsage(memory, 100, 150);
        Check(memory.measured_global_vram_delta_bytes == 50 && memory.vram_used_bytes == 50, "positive measured delta");
        detail::SetMeasuredVramUsage(memory, 100, 100);
        Check(memory.measured_global_vram_delta_bytes == 0 && memory.vram_used_bytes == 0, "measured zero is not estimate");
        detail::SetMeasuredVramUsage(memory, 150, 100);
        Check(memory.measured_global_vram_delta_bytes == -50 && memory.vram_used_bytes == 0, "negative global delta preserved");
        for (auto before : {std::optional<size_t>{}, std::optional<size_t>{100}}) {
            detail::SetMeasuredVramUsage(memory, before, std::nullopt);
            Check(!memory.measured_global_vram_delta_bytes && memory.vram_used_bytes == 0, "missing after snapshot");
        }
        detail::SetMeasuredVramUsage(memory, std::nullopt, 100);
        Check(!memory.measured_global_vram_delta_bytes, "missing before snapshot");
        detail::SetMeasuredVramUsage(memory, 0, 0);
        Check(memory.measured_global_vram_delta_bytes == 0, "two successful zero snapshots");
        if (std::numeric_limits<size_t>::max() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            detail::SetMeasuredVramUsage(memory, 0, std::numeric_limits<size_t>::max());
            Check(!memory.measured_global_vram_delta_bytes, "unrepresentable delta unavailable");
        }

        for (const char* symbol : {"nvmlInit_v2", "nvmlShutdown", "nvmlDeviceGetHandleByIndex_v2"}) {
            // The alternate device symbol is absent in this fake provider.
            missing_symbol = symbol;
            const int starts = init_calls, stops = shutdown_calls, loads = load_calls;
            {
                NvmlRuntimeSampler broken;
                broken.Start();
                Check(!broken.Stop().nvml_available, "missing required lifecycle symbol");
            }
            Check(init_calls == starts && shutdown_calls == stops, "must not initialize without balanced lifecycle");
            Check(load_calls == loads + 1 && unload_calls == load_calls, "missing-symbol library cleanup");
        }
        missing_symbol = "";
        for (int status : {1, 2}) {
            init_status = status == 1 ? 3 : 0;
            device_status = status == 2 ? 3 : 0;
            const int starts = init_calls, stops = shutdown_calls;
            {
                NvmlRuntimeSampler broken;
                broken.Start();
                Check(!broken.Stop().nvml_available, "initialization/device failure unavailable");
            }
            Check(init_calls == starts + 1, "one initialization attempt");
            Check(shutdown_calls == stops + (status == 2 ? 1 : 0), "shutdown only successful initialization");
            Check(unload_calls == load_calls, "failed-provider library cleanup");
        }
        init_status = device_status = 0;

        NvmlRuntimeSampler sampler;
        sampler.Start(); sampler.Start(); // duplicate start must not replace a joinable thread
        auto live = sampler.Stop();
        Check(live.sample_count >= 2 && live.power_sample_count == live.sample_count, "sampler callbacks");
        Check(live.hardware_gpu_utilization_pct_mean == 0 && live.temperature_c_max == 60, "real zero vs value");
        Check(sampler.Stop().sample_count == live.sample_count, "idempotent stop");
        power_status = 3;
        sampler.Start();
        auto partial = sampler.Stop();
        Check(partial.power_sample_count == 0 && partial.graphics_clock_sample_count > 0, "unsupported power counter");
        readings_work = false;
        sampler.Start();
        auto failed = sampler.Stop();
        Check(failed.nvml_available && failed.utilization_sample_count == 0, "initialized provider with failed counters");
        provider_present = false;
        NvmlRuntimeSampler absent;
        absent.Start();
        auto missing = absent.Stop();
        Check(!missing.nvml_available && missing.sample_count == 0, "missing DLL");

        std::cout << "[";
        int index = 0;
        for (auto telemetry : {live, partial, failed, missing}) {
            Phase3AResult result;
            result.nvml = telemetry;
            result.slot_selection.total_estimated_vram_bytes = 9999;
            if (index == 0) detail::SetMeasuredVramUsage(result, 100, 150);
            if (index == 1) detail::SetMeasuredVramUsage(result, 100, 100);
            if (index == 2) detail::SetMeasuredVramUsage(result, 150, 100);
            if (index == 3) detail::SetMeasuredVramUsage(result, std::nullopt, 100);
            result.mean_latency_ms = 3; result.median_latency_ms = 2;
            result.p95_latency_ms = 7; result.p99_latency_ms = 9.125;
            std::ostringstream json;
            WritePhase3AResultJson(json, result, "");
            if (index++) std::cout << ",";
            std::cout << "{\"phase3a\":" << json.str();
            TrialRecord trial;
            trial.gpu = result;
            std::ostringstream canonical;
            WriteGpu(canonical, trial);
            std::cout << ",\"canonical\":" << canonical.str() << "}";
        }
        std::cout << "]\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
