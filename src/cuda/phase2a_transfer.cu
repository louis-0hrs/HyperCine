// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/phase2a_gpu.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr size_t kSizes[] = {
    4ull << 20, 8ull << 20, 16ull << 20, 32ull << 20, 64ull << 20, 128ull << 20
};

float GbPerSec(size_t bytes, float ms) {
    if (ms <= 0.f) return 0.f;
    return static_cast<float>(static_cast<double>(bytes) / (static_cast<double>(ms) * 1.0e6));
}

void SetName(char* dst, int cap, const char* text) {
    std::memset(dst, 0, static_cast<size_t>(cap));
    if (!text) return;
    std::strncpy(dst, text, static_cast<size_t>(cap - 1));
}

float TimeAsync(void* dst, void* src, size_t bytes, cudaMemcpyKind kind, cudaStream_t stream, int iters) {
    cudaMemcpyAsync(dst, src, bytes, kind, stream);
    cudaStreamSynchronize(stream);
    cudaEvent_t a = nullptr;
    cudaEvent_t b = nullptr;
    cudaEventCreate(&a);
    cudaEventCreate(&b);
    cudaEventRecord(a, stream);
    for (int i = 0; i < iters; ++i) cudaMemcpyAsync(dst, src, bytes, kind, stream);
    cudaEventRecord(b, stream);
    cudaEventSynchronize(b);
    float ms = 0.f;
    cudaEventElapsedTime(&ms, a, b);
    cudaEventDestroy(a);
    cudaEventDestroy(b);
    return ms / static_cast<float>(iters);
}

float TimeSync(void* dst, void* src, size_t bytes, cudaMemcpyKind kind, int iters) {
    cudaMemcpy(dst, src, bytes, kind);
    cudaEvent_t a = nullptr;
    cudaEvent_t b = nullptr;
    cudaEventCreate(&a);
    cudaEventCreate(&b);
    cudaEventRecord(a);
    for (int i = 0; i < iters; ++i) cudaMemcpy(dst, src, bytes, kind);
    cudaEventRecord(b);
    cudaEventSynchronize(b);
    float ms = 0.f;
    cudaEventElapsedTime(&ms, a, b);
    cudaEventDestroy(a);
    cudaEventDestroy(b);
    return ms / static_cast<float>(iters);
}

float TimeBidirectional(void* d_a, void* d_b, void* h_a, void* h_b, size_t bytes, int iters) {
    cudaStream_t up = nullptr;
    cudaStream_t down = nullptr;
    cudaStreamCreate(&up);
    cudaStreamCreate(&down);
    cudaMemcpyAsync(d_a, h_a, bytes, cudaMemcpyHostToDevice, up);
    cudaMemcpyAsync(h_b, d_b, bytes, cudaMemcpyDeviceToHost, down);
    cudaStreamSynchronize(up);
    cudaStreamSynchronize(down);
    cudaEvent_t start_up = nullptr;
    cudaEvent_t start_down = nullptr;
    cudaEvent_t end_up = nullptr;
    cudaEvent_t end_down = nullptr;
    cudaEventCreate(&start_up);
    cudaEventCreate(&start_down);
    cudaEventCreate(&end_up);
    cudaEventCreate(&end_down);
    cudaEventRecord(start_up, up);
    cudaEventRecord(start_down, down);
    for (int i = 0; i < iters; ++i) {
        cudaMemcpyAsync(d_a, h_a, bytes, cudaMemcpyHostToDevice, up);
        cudaMemcpyAsync(h_b, d_b, bytes, cudaMemcpyDeviceToHost, down);
    }
    cudaEventRecord(end_up, up);
    cudaEventRecord(end_down, down);
    cudaEventSynchronize(end_up);
    cudaEventSynchronize(end_down);
    float up_ms = 0.f;
    float down_ms = 0.f;
    cudaEventElapsedTime(&up_ms, start_up, end_up);
    cudaEventElapsedTime(&down_ms, start_down, end_down);
    cudaEventDestroy(start_up);
    cudaEventDestroy(start_down);
    cudaEventDestroy(end_up);
    cudaEventDestroy(end_down);
    cudaStreamDestroy(up);
    cudaStreamDestroy(down);
    return std::max(up_ms, down_ms) / static_cast<float>(iters);
}

void Push(std::vector<HyperCineTransferSample>* out, const char* kind, const char* direction,
          size_t bytes, float ms, int traffic_multiplier) {
    HyperCineTransferSample sample{};
    SetName(sample.host_kind, 32, kind);
    SetName(sample.direction, 24, direction);
    sample.bytes = bytes;
    sample.ms = ms;
    sample.gbps = GbPerSec(bytes * static_cast<size_t>(traffic_multiplier), ms);
    out->push_back(sample);
}

}  // namespace

extern "C" int HyperCine_QueryDeviceLink(HyperCineDeviceLink* info) {
    if (!info) return -1;
    std::memset(info, 0, sizeof(*info));
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) return -2;
    SetName(info->name, 256, prop.name);
    cudaDeviceGetAttribute(&info->async_engines, cudaDevAttrAsyncEngineCount, 0);
    cudaDeviceGetAttribute(&info->can_map_host, cudaDevAttrCanMapHostMemory, 0);
    cudaDeviceGetAttribute(&info->concurrent_kernels, cudaDevAttrConcurrentKernels, 0);
    cudaDeviceGetAttribute(&info->gpu_overlap, cudaDevAttrGpuOverlap, 0);
    cudaDeviceGetAttribute(&info->l2_bytes, cudaDevAttrL2CacheSize, 0);
    cudaDeviceGetAttribute(&info->mem_bus_bits, cudaDevAttrGlobalMemoryBusWidth, 0);
    cudaDeviceGetAttribute(&info->mem_clock_khz, cudaDevAttrMemoryClockRate, 0);
    cudaDeviceGetAttribute(&info->pci_bus, cudaDevAttrPciBusId, 0);
    cudaDeviceGetAttribute(&info->pci_device, cudaDevAttrPciDeviceId, 0);
    cudaDeviceGetAttribute(&info->unified_addressing, cudaDevAttrUnifiedAddressing, 0);
    cudaDriverGetVersion(&info->driver_version);
    cudaRuntimeGetVersion(&info->runtime_version);
    info->sm_count = prop.multiProcessorCount;
    info->compute_major = prop.major;
    info->compute_minor = prop.minor;
    info->total_vram_bytes = prop.totalGlobalMem;
    return 0;
}

extern "C" size_t HyperCine_QueryUsedVramBytes() {
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    if (cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess) return 0;
    return total_bytes >= free_bytes ? (total_bytes - free_bytes) : 0;
}

extern "C" int HyperCine_BenchmarkTransfers(HyperCineTransferSample* out, int capacity) {
    if (!out || capacity <= 0) return -1;
    const size_t biggest = kSizes[5];
    void* device_a = nullptr;
    void* device_b = nullptr;
    if (cudaMalloc(&device_a, biggest) != cudaSuccess) return -2;
    if (cudaMalloc(&device_b, biggest) != cudaSuccess) {
        cudaFree(device_a);
        return -2;
    }
    cudaMemset(device_a, 1, biggest);
    cudaMemset(device_b, 2, biggest);

    std::vector<uint8_t> pageable(biggest, 3);
    void* malloc_host = nullptr;
    void* alloc_host = nullptr;
    void* wc_host = nullptr;
    void* malloc_host_b = nullptr;
    cudaMallocHost(&malloc_host, biggest);
    cudaMallocHost(&malloc_host_b, biggest);
    cudaHostAlloc(&alloc_host, biggest, cudaHostAllocDefault);
    cudaHostAlloc(&wc_host, biggest, cudaHostAllocWriteCombined);
    if (!malloc_host || !malloc_host_b || !alloc_host || !wc_host) {
        cudaFree(device_a);
        cudaFree(device_b);
        if (malloc_host) cudaFreeHost(malloc_host);
        if (malloc_host_b) cudaFreeHost(malloc_host_b);
        if (alloc_host) cudaFreeHost(alloc_host);
        if (wc_host) cudaFreeHost(wc_host);
        return -3;
    }
    std::memset(malloc_host, 4, biggest);
    std::memset(malloc_host_b, 5, biggest);
    std::memset(alloc_host, 6, biggest);

    cudaStream_t stream = nullptr;
    cudaStreamCreate(&stream);
    std::vector<HyperCineTransferSample> samples;
    for (size_t bytes : kSizes) {
        const int iters = bytes >= (64ull << 20) ? 4 : 8;
        Push(&samples, "pageable", "h2d_sync", bytes, TimeSync(device_a, pageable.data(), bytes, cudaMemcpyHostToDevice, iters), 1);
        Push(&samples, "pageable", "d2h_sync", bytes, TimeSync(pageable.data(), device_a, bytes, cudaMemcpyDeviceToHost, iters), 1);
        Push(&samples, "cudaMallocHost", "h2d_sync", bytes, TimeSync(device_a, malloc_host, bytes, cudaMemcpyHostToDevice, iters), 1);
        Push(&samples, "cudaMallocHost", "d2h_sync", bytes, TimeSync(malloc_host, device_a, bytes, cudaMemcpyDeviceToHost, iters), 1);
        Push(&samples, "cudaHostAlloc", "h2d_sync", bytes, TimeSync(device_a, alloc_host, bytes, cudaMemcpyHostToDevice, iters), 1);
        Push(&samples, "cudaHostAlloc", "d2h_sync", bytes, TimeSync(alloc_host, device_a, bytes, cudaMemcpyDeviceToHost, iters), 1);
        Push(&samples, "write_combined", "h2d_sync", bytes, TimeSync(device_a, wc_host, bytes, cudaMemcpyHostToDevice, iters), 1);
        Push(&samples, "write_combined", "d2h_sync", bytes, TimeSync(wc_host, device_a, bytes, cudaMemcpyDeviceToHost, iters), 1);
        Push(&samples, "cudaMallocHost", "h2d_async", bytes, TimeAsync(device_a, malloc_host, bytes, cudaMemcpyHostToDevice, stream, iters), 1);
        Push(&samples, "cudaMallocHost", "d2h_async", bytes, TimeAsync(malloc_host, device_a, bytes, cudaMemcpyDeviceToHost, stream, iters), 1);
        Push(&samples, "cudaHostAlloc", "h2d_async", bytes, TimeAsync(device_a, alloc_host, bytes, cudaMemcpyHostToDevice, stream, iters), 1);
        Push(&samples, "cudaHostAlloc", "d2h_async", bytes, TimeAsync(alloc_host, device_a, bytes, cudaMemcpyDeviceToHost, stream, iters), 1);
        Push(&samples, "write_combined", "h2d_async", bytes, TimeAsync(device_a, wc_host, bytes, cudaMemcpyHostToDevice, stream, iters), 1);
        Push(&samples, "write_combined", "d2h_async", bytes, TimeAsync(wc_host, device_a, bytes, cudaMemcpyDeviceToHost, stream, iters), 1);
        const float both = TimeBidirectional(device_a, device_b, malloc_host, malloc_host_b, bytes, iters);
        Push(&samples, "cudaMallocHost", "bidirectional_async", bytes, both, 2);
    }
    cudaStreamDestroy(stream);
    cudaFreeHost(malloc_host);
    cudaFreeHost(malloc_host_b);
    cudaFreeHost(alloc_host);
    cudaFreeHost(wc_host);
    cudaFree(device_a);
    cudaFree(device_b);
    const int n = std::min(capacity, static_cast<int>(samples.size()));
    std::memcpy(out, samples.data(), static_cast<size_t>(n) * sizeof(HyperCineTransferSample));
    return n;
}

namespace {

HyperCineTransferStats SummarizeRuns(const char* kind, const char* direction, size_t bytes,
                                     std::vector<float>& ms_runs, int traffic_multiplier) {
    HyperCineTransferStats st{};
    SetName(st.host_kind, 32, kind);
    SetName(st.direction, 24, direction);
    st.bytes = bytes;
    st.runs = static_cast<int>(ms_runs.size());
    if (ms_runs.empty()) return st;
    std::sort(ms_runs.begin(), ms_runs.end());
    double sum_ms = 0;
    for (float v : ms_runs) sum_ms += v;
    st.mean_ms = static_cast<float>(sum_ms / ms_runs.size());
    st.median_ms = ms_runs[ms_runs.size() / 2];
    const size_t p95_idx = std::min(ms_runs.size() - 1, (ms_runs.size() * 95) / 100);
    st.p95_ms = ms_runs[p95_idx];

    std::vector<float> gbps_runs;
    gbps_runs.reserve(ms_runs.size());
    double sum_gbps = 0;
    for (float v : ms_runs) {
        const float g = GbPerSec(bytes * static_cast<size_t>(traffic_multiplier), v);
        gbps_runs.push_back(g);
        sum_gbps += g;
    }
    std::sort(gbps_runs.begin(), gbps_runs.end());
    st.mean_gbps = static_cast<float>(sum_gbps / gbps_runs.size());
    st.median_gbps = gbps_runs[gbps_runs.size() / 2];
    st.p95_gbps = gbps_runs[p95_idx];
    return st;
}

}  // namespace

extern "C" int HyperCine_BenchmarkTransfersCanonical(HyperCineTransferStats* out, int capacity, int runs) {
    if (!out || capacity <= 0) return -1;
    if (runs < 3) runs = 9;
    const size_t biggest = kSizes[5];
    void* device_a = nullptr;
    void* device_b = nullptr;
    if (cudaMalloc(&device_a, biggest) != cudaSuccess) return -2;
    if (cudaMalloc(&device_b, biggest) != cudaSuccess) {
        cudaFree(device_a);
        return -2;
    }
    cudaMemset(device_a, 1, biggest);
    cudaMemset(device_b, 2, biggest);

    std::vector<uint8_t> pageable(biggest, 3);
    void* malloc_host = nullptr;
    void* alloc_host = nullptr;
    void* wc_host = nullptr;
    void* malloc_host_b = nullptr;
    cudaMallocHost(&malloc_host, biggest);
    cudaMallocHost(&malloc_host_b, biggest);
    cudaHostAlloc(&alloc_host, biggest, cudaHostAllocDefault);
    cudaHostAlloc(&wc_host, biggest, cudaHostAllocWriteCombined);
    if (!malloc_host || !malloc_host_b || !alloc_host || !wc_host) {
        cudaFree(device_a);
        cudaFree(device_b);
        if (malloc_host) cudaFreeHost(malloc_host);
        if (malloc_host_b) cudaFreeHost(malloc_host_b);
        if (alloc_host) cudaFreeHost(alloc_host);
        if (wc_host) cudaFreeHost(wc_host);
        return -3;
    }
    std::memset(malloc_host, 4, biggest);
    std::memset(malloc_host_b, 5, biggest);
    std::memset(alloc_host, 6, biggest);

    cudaStream_t stream = nullptr;
    cudaStreamCreate(&stream);
    std::vector<HyperCineTransferStats> stats;

    auto collect_sync = [&](const char* kind, const char* dir, void* dst, void* src, size_t bytes, cudaMemcpyKind mkind) {
        TimeSync(dst, src, bytes, mkind, 1);
        std::vector<float> ms_runs(static_cast<size_t>(runs));
        for (int r = 0; r < runs; ++r) ms_runs[static_cast<size_t>(r)] = TimeSync(dst, src, bytes, mkind, 1);
        stats.push_back(SummarizeRuns(kind, dir, bytes, ms_runs, 1));
    };
    auto collect_async = [&](const char* kind, const char* dir, void* dst, void* src, size_t bytes, cudaMemcpyKind mkind) {
        TimeAsync(dst, src, bytes, mkind, stream, 1);
        std::vector<float> ms_runs(static_cast<size_t>(runs));
        for (int r = 0; r < runs; ++r) ms_runs[static_cast<size_t>(r)] = TimeAsync(dst, src, bytes, mkind, stream, 1);
        stats.push_back(SummarizeRuns(kind, dir, bytes, ms_runs, 1));
    };
    auto collect_bidir = [&](size_t bytes) {
        TimeBidirectional(device_a, device_b, malloc_host, malloc_host_b, bytes, 1);
        std::vector<float> ms_runs(static_cast<size_t>(runs));
        for (int r = 0; r < runs; ++r) ms_runs[static_cast<size_t>(r)] = TimeBidirectional(device_a, device_b, malloc_host, malloc_host_b, bytes, 1);
        stats.push_back(SummarizeRuns("cudaMallocHost", "bidirectional_async", bytes, ms_runs, 2));
    };

    for (size_t bytes : kSizes) {
        collect_sync("pageable", "h2d_sync", device_a, pageable.data(), bytes, cudaMemcpyHostToDevice);
        collect_sync("pageable", "d2h_sync", pageable.data(), device_a, bytes, cudaMemcpyDeviceToHost);
        collect_sync("cudaMallocHost", "h2d_sync", device_a, malloc_host, bytes, cudaMemcpyHostToDevice);
        collect_sync("cudaMallocHost", "d2h_sync", malloc_host, device_a, bytes, cudaMemcpyDeviceToHost);
        collect_sync("cudaHostAlloc", "h2d_sync", device_a, alloc_host, bytes, cudaMemcpyHostToDevice);
        collect_sync("cudaHostAlloc", "d2h_sync", alloc_host, device_a, bytes, cudaMemcpyDeviceToHost);
        collect_sync("write_combined", "h2d_sync", device_a, wc_host, bytes, cudaMemcpyHostToDevice);
        collect_sync("write_combined", "d2h_sync", wc_host, device_a, bytes, cudaMemcpyDeviceToHost);
        collect_async("cudaMallocHost", "h2d_async", device_a, malloc_host, bytes, cudaMemcpyHostToDevice);
        collect_async("cudaMallocHost", "d2h_async", malloc_host, device_a, bytes, cudaMemcpyDeviceToHost);
        collect_async("cudaHostAlloc", "h2d_async", device_a, alloc_host, bytes, cudaMemcpyHostToDevice);
        collect_async("cudaHostAlloc", "d2h_async", alloc_host, device_a, bytes, cudaMemcpyDeviceToHost);
        collect_async("write_combined", "h2d_async", device_a, wc_host, bytes, cudaMemcpyHostToDevice);
        collect_async("write_combined", "d2h_async", wc_host, device_a, bytes, cudaMemcpyDeviceToHost);
        collect_bidir(bytes);
    }

    cudaStreamDestroy(stream);
    cudaFreeHost(malloc_host);
    cudaFreeHost(malloc_host_b);
    cudaFreeHost(alloc_host);
    cudaFreeHost(wc_host);
    cudaFree(device_a);
    cudaFree(device_b);
    const int n = std::min(capacity, static_cast<int>(stats.size()));
    std::memcpy(out, stats.data(), static_cast<size_t>(n) * sizeof(HyperCineTransferStats));
    return n;
}

