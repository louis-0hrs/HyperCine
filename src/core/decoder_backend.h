// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/decoder.h"
#include "hypercine/decode_entropy_cuda.h"
namespace HyperCine {
// GOP-1 spatial frames. InterlacedGop1 is YUV 4:2:2 coded precision 10 only.
// FIELDPLUS / GOP-2 never uses this value.
enum class FrameReconstruction : uint8_t {
    Progressive = 0,
    InterlacedGop1 = 1
};
// Private production validation; legacy research entry points retain their ABI.
bool ParseCineFormSampleValidated(const uint8_t*, size_t, int,
                                  std::vector<GpuEntropyBandDescriptor>&, std::string*, DecodeError* = nullptr,
                                  FrameReconstruction* mode = nullptr);
bool GenerateSingleBandCheckpointsValidated(const uint8_t*, size_t,
    const GpuEntropyBandDescriptor&, int, const GpuFsmEntry*, size_t,
    GpuEntropyMode, int, std::vector<GpuEntropySegmentDescriptor>&, std::string*);
DecodeStrategy ClassifyDecodeFrame(const std::vector<GpuEntropyBandDescriptor>&, DecodeFrameInfo&);
#if defined(HYPERCINE_CUDA_AVAILABLE) || defined(__CUDACC__)
cudaError_t LaunchRestoreDecodePeaks(const uint8_t*, size_t, const GpuEntropyBandDescriptor*,
                                   int, cudaStream_t, uint32_t* frame_error = nullptr);
#endif
} // namespace HyperCine
