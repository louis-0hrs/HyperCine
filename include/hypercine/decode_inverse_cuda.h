// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

namespace HyperCine {

struct DecodeBandDescriptor {
    int channel;      // 0 = Y, 1 = V, 2 = U
    int level;        // 2 (deepest, e.g. 240x135), 1 (480x270), 0 (960x540)
    int band;         // 0 = LL, 1 = LH (low-high), 2 = HL (high-low), 3 = HH (high-high)
    int width;        // width in samples
    int height;       // height in samples
    int pitch;        // pitch in bytes
    int prescale;     // 0 or 2
    int descale;      // 0 or 2
    int precision;    // e.g. 10
    const int16_t* host_data;
};

struct DecodeFrameGeometry {
    int width;
    int height;
    int precision;
    int channel_width[3];
    int channel_height[3];
};

class CudaInverseWaveletPipeline {
public:
    CudaInverseWaveletPipeline();
    ~CudaInverseWaveletPipeline();

    CudaInverseWaveletPipeline(const CudaInverseWaveletPipeline&) = delete;
    CudaInverseWaveletPipeline& operator=(const CudaInverseWaveletPipeline&) = delete;

    CudaInverseWaveletPipeline(CudaInverseWaveletPipeline&&) noexcept;
    CudaInverseWaveletPipeline& operator=(CudaInverseWaveletPipeline&&) noexcept;

    // Initialize allocations for a specific frame geometry
    bool Initialize(int frame_width, int frame_height, int precision = 10);
    void Release();
    bool IsInitialized() const;

    // Upload coefficient bands from CPU to GPU
    bool UploadCoefficients(const std::vector<DecodeBandDescriptor>& bands);

    // Run inverse wavelet reconstruction DAG
    bool Reconstruct();

    // Download reconstructed planar signed-16 channels to host
    bool DownloadPlanes(int16_t* host_y, int y_pitch,
                        int16_t* host_v, int v_pitch,
                        int16_t* host_u, int u_pitch);

    // Download intermediate LL bands for CPU vs GPU exactness verification
    bool DownloadIntermediateLL(int level, int channel, int16_t* host_dst, int dst_pitch);

    // Measured CUDA event timings in milliseconds
    float GetH2DTimeMs() const;
    float GetInverseTimeMs() const;
    float GetD2HTimeMs() const;

    // Direct access to device buffers
    const int16_t* GetDevicePlane(int channel) const;
    int GetDevicePlanePitch(int channel) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace HyperCine
