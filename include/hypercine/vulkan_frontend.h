// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Portable public interface for the experimental Vulkan compute frontend.
//
// This header deliberately does NOT include <vulkan/vulkan.h>: the Vulkan
// headers stay inside src/vulkan/*.cpp so that the portable core and the
// standalone CPU build remain free of the Vulkan SDK. Callers pass plain
// int16_t planes, exactly as the SDK-free CPU path does.
//
// Only the first P1 slice is exposed: headless device discovery, the frozen
// BGRA8 -> CG709 Y/Cr/Cb p10 color conversion, and the L1 1D horizontal 2-6
// forward wavelet. Everything beyond P1 (L2/L3, quantization, entropy,
// framing) is intentionally out of scope here.

#ifndef HYPERCINE_VULKAN_FRONTEND_H
#define HYPERCINE_VULKAN_FRONTEND_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hypercine {
namespace vulkan {

// Human-readable report of the selected physical device and the runtime ICD.
struct DeviceInfo {
    bool available = false;       // a usable compute-capable device was found
    std::string device_name;
    std::string driver_name;
    std::string driver_info;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint32_t compute_queue_family = 0;
    uint64_t max_storage_buffer_range = 0;
    uint64_t max_compute_workgroup_invocations = 0;
    std::string notes;            // e.g. why discovery failed
};

// Query without creating a device. Never throws.
DeviceInfo DiscoverDevice();

// Returns true when a Vulkan compute device can actually be created.
bool IsAvailable();

// Throws std::runtime_error with a precise message when Vulkan is unavailable.
//
// Bit-exactness contract: every method reproduces the CPU reference in
// src/cpu/filmscan3_frontend.cpp / src/cpu/wavelet_cpu.cpp exactly - no
// floating-point tolerance, no nearest rounding. Planes are int16_t with an
// explicit stride in elements; padding elements are written deterministically
// (see the implementation) so that a full-buffer comparison is meaningful.
class VulkanFrontend {
public:
    VulkanFrontend();
    ~VulkanFrontend();
    VulkanFrontend(const VulkanFrontend&) = delete;
    VulkanFrontend& operator=(const VulkanFrontend&) = delete;

    const DeviceInfo& info() const noexcept { return info_; }

    // Frozen BGRA8 -> CG709 Y/Cr/Cb p10 conversion.
    //   data     : top-down BGRA8 bytes, row stride `stride` bytes (>= width*4)
    //   bytes    : total size of `data`
    //   y/cr/cb  : int16_t planes; Y is width x height, Cr/Cb are (width/2) x height
    // A read of source row `height-1-y` for output row `y` is part of the contract.
    void ConvertBgra(const uint8_t* data, std::size_t bytes,
                     int width, int height, std::size_t stride,
                     int16_t* y, int y_stride,
                     int16_t* cr, int16_t* cb, int c_stride);

    // Production 1D horizontal 2-6 forward wavelet (prescale=0 branch of
    // ForwardLevel), matching HyperCine_ForwardHorizontal26_CPU bit for bit.
    void ForwardHorizontal26(const int16_t* src, int width, int height, int src_stride,
                             int16_t* low, int low_stride,
                             int16_t* high, int high_stride);

    // Production 1D vertical 2-6 forward wavelet, matching
    // HyperCine_ForwardVertical26_CPU bit for bit. Together with the horizontal
    // pass this reproduces the four L1 subbands LL/LH/HL/HH of ForwardLevel
    // (prescale=0) exactly.
    void ForwardVertical26(const int16_t* src, int width, int height, int src_stride,
                           int16_t* low, int low_stride,
                           int16_t* high, int high_stride);

    // Build fingerprint of the compiled shaders ("shader <id> fnv1a64 <hex>"),
    // for the validation report.
    std::string ShaderFingerprint() const;

    // GPU time (nanoseconds) of the most recent dispatch from VkQueryPool
    // timestamps where supported; -1 when unavailable. Kernel window only:
    // excludes the host->device upload and device->host download.
    double last_dispatch_gpu_ns() const;

private:
    struct Impl;
    Impl* impl_;
    DeviceInfo info_;
};

}} // namespace hypercine::vulkan

#endif // HYPERCINE_VULKAN_FRONTEND_H
