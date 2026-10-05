// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_entropy_cuda.h"
#include "hypercine/decode_phase2c.h"

namespace HyperCine {

cudaError_t LaunchZeroVlcBandsGpu(const GpuEntropyBandDescriptor*, int, cudaStream_t) {
    return cudaErrorNotSupported;
}

cudaError_t LaunchDecodeEntropyParallelGpu(
    const uint8_t*, size_t, const GpuEntropyBandDescriptor*, int,
    const GpuEntropySegmentDescriptor*, int, const GpuFsmEntry*,
    GpuSegmentDecodeResult*, cudaStream_t
) {
    return cudaErrorNotSupported;
}

cudaError_t LaunchCooperativeDifferenceCodingGpu(const GpuEntropyBandDescriptor*, int, cudaStream_t) {
    return cudaErrorNotSupported;
}

cudaError_t LaunchDecodeEntropyParallelDiagnosticGpu(
    const uint8_t*, size_t, const GpuEntropyBandDescriptor*,
    const GpuEntropySegmentDescriptor*, int, const GpuFsmEntry*,
    GpuSegmentDiagnostic*, cudaStream_t
) {
    return cudaErrorNotSupported;
}

struct CudaDecoderPipelinePhase2C::Impl {};

CudaDecoderPipelinePhase2C::CudaDecoderPipelinePhase2C() : impl_(nullptr) {}
CudaDecoderPipelinePhase2C::~CudaDecoderPipelinePhase2C() = default;
CudaDecoderPipelinePhase2C::CudaDecoderPipelinePhase2C(CudaDecoderPipelinePhase2C&&) noexcept = default;
CudaDecoderPipelinePhase2C& CudaDecoderPipelinePhase2C::operator=(CudaDecoderPipelinePhase2C&&) noexcept = default;

bool CudaDecoderPipelinePhase2C::IsInitialized() const { return false; }
bool CudaDecoderPipelinePhase2C::Initialize(int, int, int, int, int) { return false; }
void CudaDecoderPipelinePhase2C::Release() {}
void CudaDecoderPipelinePhase2C::SetEntropyMode(GpuEntropyMode, int) {}
GpuEntropyMode CudaDecoderPipelinePhase2C::GetEntropyMode() const { return GpuEntropyMode::ParallelAdaptive; }
int CudaDecoderPipelinePhase2C::GetFixedSegmentsPerBand() const { return 32; }

uint64_t CudaDecoderPipelinePhase2C::QueueDecode(const uint8_t*, size_t, DeviceSurfaceType, ColorMatrix, ColorRange, RowOrientation, cudaStream_t) { return 0; }
DecodeTicketStatus CudaDecoderPipelinePhase2C::PollDecode(uint64_t) { return DecodeTicketStatus::Invalid; }
bool CudaDecoderPipelinePhase2C::WaitForDecode(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2C::StreamWait(uint64_t, cudaStream_t) { return false; }
bool CudaDecoderPipelinePhase2C::GetDeviceBgraOutput(uint64_t, DeviceBgraSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2C::GetDevicePlanarOutput(uint64_t, DevicePlanarSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2C::ReleaseOutput(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2C::DownloadBgraToHost(uint64_t, uint8_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2C::DownloadPlanarToHost(uint64_t, int16_t*, size_t, int16_t*, size_t, int16_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2C::GetStageTimings(uint64_t, Phase2cSlotStageTimings*) { return false; }
bool CudaDecoderPipelinePhase2C::GetBandDecodeResults(uint64_t, std::vector<GpuBandDecodeResult>&) { return false; }
bool CudaDecoderPipelinePhase2C::GetSegmentDecodeResults(uint64_t, std::vector<GpuSegmentDecodeResult>&) { return false; }
bool CudaDecoderPipelinePhase2C::DownloadBandCoefficientsToHost(uint64_t, int, std::vector<int16_t>&, int*, int*, int*) { return false; }

} // namespace HyperCine
