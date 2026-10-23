// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

/**
 * @file benchmark_codec.cpp
 * @brief Standalone HyperCine benchmark tool for measuring CPU encode/decode FPS,
 *        frame latency, memory footprint, and throughput in Megapixels/second.
 */

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <cstring>
#include <thread>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
// Toolchain CPUID intrinsics; see benchmarks/CMakeLists.txt for the include
// boundary that keeps upstream/Codec/cpuid.h off this target's search path.
#include <cpuid.h>
#endif

#include "CFHDEncoder.h"
#include "CFHDDecoder.h"

struct BenchmarkConfig {
    std::string name;
    int width;
    int height;
    int frameCount;
    CFHD_PixelFormat pixelFormat;
    CFHD_EncodedFormat encodedFormat;
    CFHD_EncodingQuality quality;
};

struct BenchmarkMetrics {
    std::string resolutionName;
    int width;
    int height;
    int frames;
    double encodeTotalTimeSec;
    double encodeFps;
    double encodeAvgLatencyMs;
    double encodeMinLatencyMs;
    double encodeMaxLatencyMs;
    double encodeMegapixelsPerSec;

    double decodeTotalTimeSec;
    double decodeFps;
    double decodeAvgLatencyMs;
    double decodeMinLatencyMs;
    double decodeMaxLatencyMs;
    double decodeMegapixelsPerSec;

    size_t avgCompressedBytes;
    double compressionRatio;
    size_t peakMemoryBytes;
    std::string peakMemoryMetric;
};

// CPU brand string from CPUID leaves 0x80000002..0x80000004. Returns an empty
// string when the platform cannot answer, so the caller can distinguish "no
// identifier available" from a real model name instead of inventing one.
std::string GetCpuBrandString() {
#ifdef _WIN32
    int cpuInfo[4] = { -1 };
    char brand[0x40] = { '\0' };
    __cpuid(cpuInfo, 0x80000000);
    unsigned int nExIds = static_cast<unsigned int>(cpuInfo[0]);
    if (nExIds >= 0x80000004) {
        __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        return std::string(brand);
    }
#elif defined(__i386__) || defined(__x86_64__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000004u) {
        char brand[0x40] = { '\0' };
        __cpuid_count(0x80000002u, 0, eax, ebx, ecx, edx);
        std::memcpy(brand, &eax, 4); std::memcpy(brand + 4, &ebx, 4);
        std::memcpy(brand + 8, &ecx, 4); std::memcpy(brand + 12, &edx, 4);
        __cpuid_count(0x80000003u, 0, eax, ebx, ecx, edx);
        std::memcpy(brand + 16, &eax, 4); std::memcpy(brand + 20, &ebx, 4);
        std::memcpy(brand + 24, &ecx, 4); std::memcpy(brand + 28, &edx, 4);
        __cpuid_count(0x80000004u, 0, eax, ebx, ecx, edx);
        std::memcpy(brand + 32, &eax, 4); std::memcpy(brand + 36, &ebx, 4);
        std::memcpy(brand + 32, &eax, 4); std::memcpy(brand + 36, &ebx, 4);
        std::memcpy(brand + 40, &ecx, 4); std::memcpy(brand + 44, &edx, 4);
        return std::string(brand);
    }
#endif
    return std::string();
}

// Reported logical hardware threads. Windows keeps the system-info query it has
// always used so recorded Windows baselines stay comparable; other platforms use
// the portable standard query and never report a zero thread count.
int GetLogicalThreadCount() {
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return static_cast<int>(sysInfo.dwNumberOfProcessors);
#else
    unsigned int count = std::thread::hardware_concurrency();
    return count > 0 ? static_cast<int>(count) : 1;
#endif
}

// Host OS this binary was built for. Build-time constant, not a claim about a
// specific machine or its physical core/NUMA topology.
const char* GetBuildOsName() {
#if defined(_WIN32)
    return "Windows x64";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

// Build type this binary was compiled with, provided by CMake. A hardcoded
// "Release" is a claim about a build the tool did not inspect.
const char* GetBuildTypeName() {
#if defined(HYPERCINE_BENCH_BUILD_TYPE)
    return HYPERCINE_BENCH_BUILD_TYPE;
#else
    return "unknown";
#endif
}

// Compiler that built this binary. Uses the CMake-provided identifier/version
// (see benchmarks/CMakeLists.txt) and falls back to a plain toolchain name.
const char* GetCompilerDescription() {
#if defined(HYPERCINE_BENCH_COMPILER)
    return HYPERCINE_BENCH_COMPILER;
#elif defined(_MSC_VER)
    return "MSVC";
#elif defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#else
    return "Unknown";
#endif
}

// Peak memory is a platform-specific measurement (Windows working set vs POSIX
// resident set); the metric name travels with the value so unrelated counters
// are never silently compared across operating systems.
struct PeakMemoryReport {
    size_t bytes;
    const char* metric;
};

PeakMemoryReport GetPeakMemoryReport() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return { static_cast<size_t>(pmc.PeakWorkingSetSize), "windows_peak_working_set" };
    }
    return { 0, "unavailable" };
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            size_t kb = 0;
            if (status >> kb) {
                return { kb * 1024, "linux_vmhwm" };
            }
            break;
        }
    }
    return { 0, "unavailable" };
#else
    // No equivalent peak counter is read here; report it explicitly as
    // unavailable rather than reusing another platform's metric.
    return { 0, "unavailable" };
#endif
}

// Generate realistic synthetic frame data (checkerboard + diagonal gradient)
std::vector<uint8_t> GenerateBenchmarkFrame(int width, int height) {
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            uint8_t c1 = static_cast<uint8_t>((x * 255) / width);
            uint8_t c2 = static_cast<uint8_t>((y * 255) / height);
            uint8_t chk = (((x / 32) ^ (y / 32)) & 1) ? 200 : 50;

            frame[idx + 0] = static_cast<uint8_t>((c1 + chk) / 2); // B
            frame[idx + 1] = static_cast<uint8_t>((c2 + chk) / 2); // G
            frame[idx + 2] = static_cast<uint8_t>((c1 ^ c2));      // R
            frame[idx + 3] = 255;                                   // A
        }
    }
    return frame;
}

BenchmarkMetrics RunResolutionBenchmark(const BenchmarkConfig& cfg) {
    std::cout << "\n>>> Benchmarking " << cfg.name << " (" << cfg.width << "x" << cfg.height
              << ", " << cfg.frameCount << " frames)..." << std::endl;

    BenchmarkMetrics metrics{};
    metrics.resolutionName = cfg.name;
    metrics.width = cfg.width;
    metrics.height = cfg.height;
    metrics.frames = cfg.frameCount;

    // 1. Prepare raw frame
    auto rawFrame = GenerateBenchmarkFrame(cfg.width, cfg.height);
    int pitch = cfg.width * 4;
    size_t uncompressedFrameBytes = rawFrame.size();

    // 2. Setup encoder
    CFHD_EncoderRef encoder = nullptr;
    CFHD_Error err = CFHD_OpenEncoder(&encoder, nullptr);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "Error opening encoder: " << err << std::endl;
        return metrics;
    }

    err = CFHD_PrepareToEncode(encoder, cfg.width, cfg.height, cfg.pixelFormat,
                               cfg.encodedFormat, CFHD_ENCODING_FLAGS_NONE, cfg.quality);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "Error preparing encoder: " << err << std::endl;
        CFHD_CloseEncoder(encoder);
        return metrics;
    }

    // Warmup encode
    CFHD_EncodeSample(encoder, rawFrame.data(), pitch);
    void* dummySample = nullptr;
    size_t dummySize = 0;
    CFHD_GetSampleData(encoder, &dummySample, &dummySize);

    std::vector<double> encodeLatenciesMs;
    encodeLatenciesMs.reserve(cfg.frameCount);
    std::vector<std::vector<uint8_t>> encodedPayloads(cfg.frameCount);

    auto encodeStart = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < cfg.frameCount; ++i) {
        auto fStart = std::chrono::high_resolution_clock::now();
        CFHD_EncodeSample(encoder, rawFrame.data(), pitch);
        void* sData = nullptr;
        size_t sSize = 0;
        CFHD_GetSampleData(encoder, &sData, &sSize);
        auto fEnd = std::chrono::high_resolution_clock::now();

        double latMs = std::chrono::duration<double, std::milli>(fEnd - fStart).count();
        encodeLatenciesMs.push_back(latMs);

        encodedPayloads[i].resize(sSize);
        std::memcpy(encodedPayloads[i].data(), sData, sSize);
    }

    auto encodeEnd = std::chrono::high_resolution_clock::now();
    metrics.encodeTotalTimeSec = std::chrono::duration<double>(encodeEnd - encodeStart).count();
    metrics.encodeFps = cfg.frameCount / metrics.encodeTotalTimeSec;
    metrics.encodeMegapixelsPerSec = (static_cast<double>(cfg.width) * cfg.height * metrics.encodeFps) / 1e6;

    metrics.encodeAvgLatencyMs = std::accumulate(encodeLatenciesMs.begin(), encodeLatenciesMs.end(), 0.0) / cfg.frameCount;
    metrics.encodeMinLatencyMs = *std::min_element(encodeLatenciesMs.begin(), encodeLatenciesMs.end());
    metrics.encodeMaxLatencyMs = *std::max_element(encodeLatenciesMs.begin(), encodeLatenciesMs.end());

    CFHD_CloseEncoder(encoder);

    // Calculate compression metrics
    size_t totalCompBytes = 0;
    for (const auto& p : encodedPayloads) totalCompBytes += p.size();
    metrics.avgCompressedBytes = totalCompBytes / cfg.frameCount;
    metrics.compressionRatio = static_cast<double>(uncompressedFrameBytes) / metrics.avgCompressedBytes;

    // 3. Setup decoder
    CFHD_DecoderRef decoder = nullptr;
    err = CFHD_OpenDecoder(&decoder, nullptr);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "Error opening decoder: " << err << std::endl;
        return metrics;
    }

    int actW = 0, actH = 0;
    CFHD_PixelFormat actFmt = CFHD_PIXEL_FORMAT_UNKNOWN;
    err = CFHD_PrepareToDecode(decoder, 0, 0, cfg.pixelFormat, CFHD_DECODED_RESOLUTION_FULL,
                               CFHD_DECODING_FLAGS_NONE, encodedPayloads[0].data(), encodedPayloads[0].size(),
                               &actW, &actH, &actFmt);
    if (err != CFHD_ERROR_OKAY) {
        std::cerr << "Error preparing decoder: " << err << std::endl;
        CFHD_CloseDecoder(decoder);
        return metrics;
    }

    int32_t decPitch = 0;
    CFHD_GetImagePitch(actW, actFmt, &decPitch);
    std::vector<uint8_t> decBuffer(static_cast<size_t>(decPitch) * actH);

    // Warmup decode
    CFHD_DecodeSample(decoder, encodedPayloads[0].data(), encodedPayloads[0].size(), decBuffer.data(), decPitch);

    std::vector<double> decodeLatenciesMs;
    decodeLatenciesMs.reserve(cfg.frameCount);

    auto decodeStart = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < cfg.frameCount; ++i) {
        auto fStart = std::chrono::high_resolution_clock::now();
        CFHD_DecodeSample(decoder, encodedPayloads[i].data(), encodedPayloads[i].size(), decBuffer.data(), decPitch);
        auto fEnd = std::chrono::high_resolution_clock::now();

        double latMs = std::chrono::duration<double, std::milli>(fEnd - fStart).count();
        decodeLatenciesMs.push_back(latMs);
    }

    auto decodeEnd = std::chrono::high_resolution_clock::now();
    metrics.decodeTotalTimeSec = std::chrono::duration<double>(decodeEnd - decodeStart).count();
    metrics.decodeFps = cfg.frameCount / metrics.decodeTotalTimeSec;
    metrics.decodeMegapixelsPerSec = (static_cast<double>(cfg.width) * cfg.height * metrics.decodeFps) / 1e6;

    metrics.decodeAvgLatencyMs = std::accumulate(decodeLatenciesMs.begin(), decodeLatenciesMs.end(), 0.0) / cfg.frameCount;
    metrics.decodeMinLatencyMs = *std::min_element(decodeLatenciesMs.begin(), decodeLatenciesMs.end());
    metrics.decodeMaxLatencyMs = *std::max_element(decodeLatenciesMs.begin(), decodeLatenciesMs.end());

    CFHD_CloseDecoder(decoder);

    const PeakMemoryReport peakMemory = GetPeakMemoryReport();
    metrics.peakMemoryBytes = peakMemory.bytes;
    metrics.peakMemoryMetric = peakMemory.metric;

    // Print summary table row
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Encode: " << std::setw(7) << metrics.encodeFps << " fps | "
              << std::setw(7) << metrics.encodeMegapixelsPerSec << " MP/s | Avg Lat: "
              << std::setw(6) << metrics.encodeAvgLatencyMs << " ms ["
              << metrics.encodeMinLatencyMs << " - " << metrics.encodeMaxLatencyMs << " ms]\n";
    std::cout << "  Decode: " << std::setw(7) << metrics.decodeFps << " fps | "
              << std::setw(7) << metrics.decodeMegapixelsPerSec << " MP/s | Avg Lat: "
              << std::setw(6) << metrics.decodeAvgLatencyMs << " ms ["
              << metrics.decodeMinLatencyMs << " - " << metrics.decodeMaxLatencyMs << " ms]\n";
    std::cout << "  Comp:   " << (metrics.avgCompressedBytes / 1024) << " KB/frame ("
              << metrics.compressionRatio << ":1) | Peak RAM: "
              << (metrics.peakMemoryBytes / (1024 * 1024)) << " MB (" << metrics.peakMemoryMetric << ")\n";

    return metrics;
}

// Returns true only when the artefact was actually written and closed. A caller
// that reports success without this returning true would be claiming a
// measurement record that does not exist.
bool WriteResultsJson(const std::string& filepath,
                      const std::vector<BenchmarkMetrics>& results,
                      const std::string& cpuModel,
                      int threadCount)
{
#if defined(_WIN32)
    // Windows tooling expects backslash separators in the recorded path.
    std::string nativePath = filepath;
    for (char& c : nativePath) {
        if (c == '/') c = '\\';
    }
#else
    // POSIX paths are used as given; turning '/' into '\\' would create a
    // literal file name instead of a directory path.
    std::string nativePath = filepath;
#endif
    std::ofstream out(nativePath);
    if (!out.is_open()) {
        std::cerr << "Failed to write JSON output: " << nativePath << std::endl;
        perror("Reason");
        return false;
    }

    out << "{\n";
    out << "  \"system\": {\n";
    out << "    \"os\": \"" << GetBuildOsName() << "\",\n";
    out << "    \"cpu_model\": \"" << cpuModel << "\",\n";
    out << "    \"thread_count\": " << threadCount << ",\n";
    out << "    \"compiler\": \"" << GetCompilerDescription() << "\",\n";
    out << "    \"build_type\": \"" << GetBuildTypeName() << "\",\n";
    out << "    \"upstream_commit\": \"11574d0295771edccadd17af14af74a539f924c7\"\n";
    out << "  },\n";
    out << "  \"configuration\": {\n";
    out << "    \"pixel_format\": \"CFHD_PIXEL_FORMAT_BGRA\",\n";
    out << "    \"encoded_format\": \"CFHD_ENCODED_FORMAT_YUV_422\",\n";
    out << "    \"quality\": \"CFHD_ENCODING_QUALITY_FILM3\"\n";
    out << "  },\n";
    out << "  \"benchmarks\": [\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& m = results[i];
        out << "    {\n";
        out << "      \"name\": \"" << m.resolutionName << "\",\n";
        out << "      \"width\": " << m.width << ",\n";
        out << "      \"height\": " << m.height << ",\n";
        out << "      \"frames\": " << m.frames << ",\n";
        out << "      \"encode\": {\n";
        out << "        \"fps\": " << m.encodeFps << ",\n";
        out << "        \"megapixels_per_sec\": " << m.encodeMegapixelsPerSec << ",\n";
        out << "        \"latency_avg_ms\": " << m.encodeAvgLatencyMs << ",\n";
        out << "        \"latency_min_ms\": " << m.encodeMinLatencyMs << ",\n";
        out << "        \"latency_max_ms\": " << m.encodeMaxLatencyMs << "\n";
        out << "      },\n";
        out << "      \"decode\": {\n";
        out << "        \"fps\": " << m.decodeFps << ",\n";
        out << "        \"megapixels_per_sec\": " << m.decodeMegapixelsPerSec << ",\n";
        out << "        \"latency_avg_ms\": " << m.decodeAvgLatencyMs << ",\n";
        out << "        \"latency_min_ms\": " << m.decodeMinLatencyMs << ",\n";
        out << "        \"latency_max_ms\": " << m.decodeMaxLatencyMs << "\n";
        out << "      },\n";
        out << "      \"compression\": {\n";
        out << "        \"avg_bytes\": " << m.avgCompressedBytes << ",\n";
        out << "        \"ratio\": " << m.compressionRatio << "\n";
        out << "      },\n";
        out << "      \"peak_memory_bytes\": " << m.peakMemoryBytes << ",\n";
        out << "      \"peak_memory_metric\": \"" << m.peakMemoryMetric << "\"\n";
        out << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }

    out << "  ]\n";
    out << "}\n";
    out.close();
    if (!out) {
        std::cerr << "Failed to finish writing JSON output: " << nativePath << std::endl;
        return false;
    }

    std::cout << "\n[+] Benchmark results successfully saved to: " << filepath << std::endl;
    return true;
}

int main(int argc, char* argv[]) {
    std::cout << "=========================================================\n";
    std::cout << " HyperCine Reference Baseline Benchmark Framework       \n";
    std::cout << "=========================================================\n";

    std::string cpuModel = GetCpuBrandString();
    int threadCount = GetLogicalThreadCount();

    std::cout << "CPU:          " << (cpuModel.empty() ? "unavailable" : cpuModel) << "\n";
    std::cout << "Cores/Threads:" << threadCount << "\n";
    std::cout << "Compiler:     " << GetCompilerDescription() << "\n";
    std::cout << "Configuration:BGRA -> 4:2:2 Film3 (Reference CPU)\n";

#if defined(_WIN32)
    // Preserve the recorded Windows baseline output path on Windows.
    std::string jsonPath = "benchmarks/results/reference_windows_msvc.json";
#else
    // Distinct default on non-Windows so a POSIX run never overwrites the
    // recorded Windows baseline JSON.
    std::string jsonPath = "benchmarks/results/reference_local.json";
#endif
    bool run1080p = true;
    bool run4k = true;
    bool run8k = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json" && i + 1 < argc) {
            jsonPath = argv[++i];
        } else if (arg == "--1080p-only") {
            run1080p = true; run4k = false; run8k = false;
        } else if (arg == "--4k-only") {
            run1080p = false; run4k = true; run8k = false;
        } else if (arg == "--8k-only") {
            run1080p = false; run4k = false; run8k = true;
        }
    }

    std::vector<BenchmarkConfig> configs;
    if (run1080p) {
        configs.push_back({"1080p", 1920, 1080, 50,
                           CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                           CFHD_ENCODING_QUALITY_FILMSCAN3});
    }
    if (run4k) {
        configs.push_back({"4K_UHD", 3840, 2160, 25,
                           CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                           CFHD_ENCODING_QUALITY_FILMSCAN3});
    }
    if (run8k) {
        configs.push_back({"8K_UHD", 7680, 4320, 10,
                           CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                           CFHD_ENCODING_QUALITY_FILMSCAN3});
    }

    // Create the artifact directory instead of relying on the process working
    // directory being the repository root. The default path is repo-relative.
    {
        const std::string::size_type slash = jsonPath.find_last_of("/\\");
        if (slash != std::string::npos) {
            std::error_code ec;
            std::filesystem::create_directories(jsonPath.substr(0, slash), ec);
            if (ec) {
                std::cerr << "Failed to create output directory for " << jsonPath
                          << ": " << ec.message() << std::endl;
            }
        }
    }

    std::vector<BenchmarkMetrics> results;
    for (const auto& cfg : configs) {
        results.push_back(RunResolutionBenchmark(cfg));
    }

    // The exit status must express whether the measurement record exists. A
    // missing artifact with exit 0 is the same false-OKAY class this repository
    // has already been burned by: a run nobody can audit looks like a pass.
    if (!WriteResultsJson(jsonPath, results, cpuModel, threadCount)) {
        std::cerr << "\n=========================================================\n";
        std::cerr << " Benchmark Run FAILED: no result artifact was written  \n";
        std::cerr << "=========================================================\n";
        return 1;
    }

    std::cout << "\n=========================================================\n";
    std::cout << " Benchmark Run Completed Successfully                    \n";
    std::cout << "=========================================================\n";

    return 0;
}
