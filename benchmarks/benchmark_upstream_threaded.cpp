// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "CFHDEncoder.h"
#include "hypercine/canonical.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto origin = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - origin).count();
}

int CountThreads() {
#ifdef _WIN32
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) ++count;
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    return count;
#else
    return -1;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1)
        ? argv[1]
        : "benchmarks/results/upstream_threaded.json";
    const int idle_threads = CountThreads();
    std::cout << "UPSTREAM_THREADED linked library idle threads " << idle_threads << "\n";
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "cannot write " << path << "\n";
        return 1;
    }
    out << "{\n  \"configuration\": \"UPSTREAM_THREADED\",\n";
    out << "  \"threaded_encoder_flag\": 1,\n";
    out << "  \"idle_process_threads\": " << idle_threads << ",\n";
    out << "  \"note\": \"Built from pristine upstream sources with _THREADED_ENCODER=1. upstream/ was not edited.\",\n";
    out << "  \"runs\": [\n";
    bool first = true;
    int rc = 0;
    for (const auto& res : hypercine::kResolutions) {
        const int frames = res.height >= 4000 ? 2 : hypercine::kCanonicalTimedFrames;
        auto frame = hypercine::MakeFrame(res.width, res.height, hypercine::CanonicalPattern::Canonical);
        const int pitch = res.width * 4;
        CFHD_EncoderRef encoder = nullptr;
        if (CFHD_OpenEncoder(&encoder, nullptr) != CFHD_ERROR_OKAY ||
            CFHD_PrepareToEncode(encoder, res.width, res.height, CFHD_PIXEL_FORMAT_BGRA,
                                 CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE,
                                 CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
            std::cerr << "prepare failed " << res.name << "\n";
            rc = 1;
            break;
        }
        std::atomic<int> peak{idle_threads};
        std::atomic<bool> sampling{true};
        std::thread sampler([&] {
            while (sampling.load()) {
                const int now = CountThreads();
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        bool encode_ok = true;
        for (int i = 0; i < hypercine::kCanonicalWarmupFrames; ++i) {
            if (CFHD_EncodeSample(encoder, frame.data(), pitch) != CFHD_ERROR_OKAY) encode_ok = false;
        }
        const double t0 = NowMs();
        for (int i = 0; i < frames && encode_ok; ++i) {
            if (CFHD_EncodeSample(encoder, frame.data(), pitch) != CFHD_ERROR_OKAY) encode_ok = false;
        }
        const double ms = encode_ok ? (NowMs() - t0) / frames : 0;
        sampling.store(false);
        sampler.join();
        void* data = nullptr;
        size_t size = 0;
        const bool got = encode_ok && CFHD_GetSampleData(encoder, &data, &size) == CFHD_ERROR_OKAY && size > 0;
        const int peak_threads = peak.load();
        const int extra = peak_threads - idle_threads;
        std::cout << res.name << " " << ms << " ms  fps " << (ms > 0 ? 1000.0 / ms : 0)
                  << " peak_threads " << peak_threads << " extra " << extra
                  << " sample " << size << "\n";
        if (!first) out << ",\n";
        first = false;
        out << "    {\"resolution\": \"" << res.name << "\", \"width\": " << res.width
            << ", \"height\": " << res.height
            << ", \"frames\": " << frames
            << ", \"encode_ok\": " << (encode_ok ? "true" : "false")
            << ", \"sample_bytes\": " << size
            << ", \"sample_ok\": " << (got ? "true" : "false")
            << ", \"ms_per_frame\": " << ms
            << ", \"sustained_fps\": " << (ms > 0 ? 1000.0 / ms : 0)
            << ", \"latency_ms\": " << ms
            << ", \"idle_threads\": " << idle_threads
            << ", \"peak_threads\": " << peak_threads
            << ", \"extra_threads\": " << extra
            << ", \"channel_threading_observed\": " << (extra >= 2 ? "true" : "false") << "}";
        CFHD_CloseEncoder(encoder);
        if (!encode_ok) rc = 1;
    }
    out << "\n  ]\n}\n";
    std::cout << "Wrote " << path << "\n";
    return rc;
}
