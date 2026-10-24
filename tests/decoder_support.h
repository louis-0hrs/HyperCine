// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "hypercine/decoder_cuda.h"
#include "hypercine/research/decoder_diagnostics.h"
#include "decode_phase0_support.h"
#include <chrono>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
namespace ProductionDecodeTest {
using namespace HyperCine;
using namespace DecodePhase0;
using Clock = std::chrono::steady_clock;
inline void Require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
inline void GpuCheck(cudaError_t e) { if (e != cudaSuccess) throw std::runtime_error(cudaGetErrorString(e)); }
inline DecodeTicket Begin(Decoder& d, const Sample& s, const DecodeRequest& r = {}) {
    DecodeError error;
    auto t = d.QueueDecode(s.compressed.data(), s.compressed.size(), r, &error);
    Require(bool(t) && error == DecodeError::None, "queue valid sample");
    Require(d.WaitForDecode(t).state == DecodeState::Ready, "valid decode ready");
    return t;
}
inline void Download(Decoder& d, DecodeTicket t, uint8_t* p, size_t pitch) {
    DeviceOutput out;
    Require(d.GetDeviceOutput(t, &out) && out.plane_count == 1, "published BGRA output");
    GpuCheck(cudaMemcpy2D(p, pitch, out.planes[0].device_pointer, out.planes[0].pitch_bytes,
                          size_t(out.width) * 4, out.height, cudaMemcpyDeviceToHost));
}
inline Bytes Image(Decoder& d, DecodeTicket t, const Sample& s) {
    Bytes result(size_t(s.width) * s.height * 4);
    Download(d, t, result.data(), size_t(s.width) * 4);
    return result;
}
inline uint64_t WorkingSet(bool peak = false) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pm{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm))) return peak ? pm.PeakWorkingSetSize : pm.WorkingSetSize;
#endif
    return 0;
}
inline double CpuMs() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        return double(k.QuadPart + u.QuadPart) * 1e-4;
    }
#endif
    return 0;
}
inline bool Same(const std::array<DecoderBandDecision, 27>& a, const std::array<DecoderBandDecision, 27>& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].method != b[i].method || a[i].fell_back != b[i].fell_back ||
            a[i].corrected_nibbles != b[i].corrected_nibbles || a[i].full_chunk_corrections != b[i].full_chunk_corrections) return false;
    return true;
}
inline const char* StrategyName(DecodeStrategy s) {
    return s == DecodeStrategy::Lightweight ? "lightweight" : s == DecodeStrategy::Hybrid ? "hybrid" : "host_streaming";
}
struct Stream {
    cudaStream_t handle = nullptr;
    Stream() { GpuCheck(cudaStreamCreateWithFlags(&handle, cudaStreamNonBlocking)); }
    ~Stream() { if (handle) cudaStreamDestroy(handle); }
    Stream(const Stream&) = delete;
};
template<class T> struct DeviceBuffer {
    T* p = nullptr;
    explicit DeviceBuffer(size_t n) { GpuCheck(cudaMalloc(reinterpret_cast<void**>(&p), sizeof(T) * n)); }
    ~DeviceBuffer() { if (p) cudaFree(p); }
    DeviceBuffer(const DeviceBuffer&) = delete;
};
} // namespace ProductionDecodeTest
