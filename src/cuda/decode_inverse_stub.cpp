// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_inverse_cuda.h"
#include "hypercine/decode_color_cuda.h"
#include "hypercine/decode_phase2a.h"

namespace HyperCine {

struct CudaInverseWaveletPipeline::Impl {};

CudaInverseWaveletPipeline::CudaInverseWaveletPipeline() : impl_(nullptr) {}
CudaInverseWaveletPipeline::~CudaInverseWaveletPipeline() = default;
CudaInverseWaveletPipeline::CudaInverseWaveletPipeline(CudaInverseWaveletPipeline&&) noexcept = default;
CudaInverseWaveletPipeline& CudaInverseWaveletPipeline::operator=(CudaInverseWaveletPipeline&&) noexcept = default;

bool CudaInverseWaveletPipeline::IsInitialized() const { return false; }
bool CudaInverseWaveletPipeline::Initialize(int, int, int) { return false; }
void CudaInverseWaveletPipeline::Release() {}
bool CudaInverseWaveletPipeline::UploadCoefficients(const std::vector<DecodeBandDescriptor>&) { return false; }
bool CudaInverseWaveletPipeline::Reconstruct() { return false; }
bool CudaInverseWaveletPipeline::DownloadPlanes(int16_t*, int, int16_t*, int, int16_t*, int) { return false; }
bool CudaInverseWaveletPipeline::DownloadIntermediateLL(int, int, int16_t*, int) { return false; }
float CudaInverseWaveletPipeline::GetH2DTimeMs() const { return 0.0f; }
float CudaInverseWaveletPipeline::GetInverseTimeMs() const { return 0.0f; }
float CudaInverseWaveletPipeline::GetD2HTimeMs() const { return 0.0f; }
const int16_t* CudaInverseWaveletPipeline::GetDevicePlane(int) const { return nullptr; }
int CudaInverseWaveletPipeline::GetDevicePlanePitch(int) const { return 0; }

bool LaunchYuv422ToBgra8(
    const int16_t*, size_t,
    const int16_t*, size_t,
    const int16_t*, size_t,
    uint8_t*, size_t,
    int, int,
    ColorMatrix,
    ColorRange,
    RowOrientation,
    cudaStream_t)
{
    return false;
}

struct CudaDecoderPipelinePhase2A::Impl {};

CudaDecoderPipelinePhase2A::CudaDecoderPipelinePhase2A() : impl_(nullptr) {}
CudaDecoderPipelinePhase2A::~CudaDecoderPipelinePhase2A() = default;
CudaDecoderPipelinePhase2A::CudaDecoderPipelinePhase2A(CudaDecoderPipelinePhase2A&&) noexcept = default;
CudaDecoderPipelinePhase2A& CudaDecoderPipelinePhase2A::operator=(CudaDecoderPipelinePhase2A&&) noexcept = default;

bool CudaDecoderPipelinePhase2A::Initialize(int, int, int, int, int) { return false; }
void CudaDecoderPipelinePhase2A::Release() {}
bool CudaDecoderPipelinePhase2A::IsInitialized() const { return false; }
uint64_t CudaDecoderPipelinePhase2A::QueueDecode(
    const std::vector<DecodeBandDescriptor>&,
    DeviceSurfaceType,
    ColorMatrix,
    ColorRange,
    RowOrientation,
    cudaStream_t)
{
    return 0;
}
DecodeTicketStatus CudaDecoderPipelinePhase2A::PollDecode(uint64_t) { return DecodeTicketStatus::Invalid; }
bool CudaDecoderPipelinePhase2A::WaitForDecode(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2A::StreamWait(uint64_t, cudaStream_t) { return false; }
bool CudaDecoderPipelinePhase2A::GetDeviceBgraOutput(uint64_t, DeviceBgraSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2A::GetDevicePlanarOutput(uint64_t, DevicePlanarSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2A::ReleaseOutput(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2A::DownloadBgraToHost(uint64_t, uint8_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2A::DownloadPlanarToHost(uint64_t, int16_t*, size_t, int16_t*, size_t, int16_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2A::GetStageTimings(uint64_t, SlotStageTimings*) { return false; }

} // namespace HyperCine
