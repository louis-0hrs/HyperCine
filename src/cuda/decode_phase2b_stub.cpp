// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_entropy_cuda.h"
#include "hypercine/decode_phase2b.h"

namespace HyperCine {

struct GpuFsmTableManager::Impl {};

GpuFsmTableManager::GpuFsmTableManager() : impl_(nullptr) {}
GpuFsmTableManager::~GpuFsmTableManager() = default;
bool GpuFsmTableManager::Initialize(int) { return false; }
void GpuFsmTableManager::Release() {}
bool GpuFsmTableManager::IsInitialized() const { return false; }
const GpuFsmEntry* GpuFsmTableManager::GetDeviceTable() const { return nullptr; }
size_t GpuFsmTableManager::GetCodebookOffset(int) const { return 0; }

cudaError_t LaunchDecodeLowpassGpu(const uint8_t*, size_t, const GpuEntropyBandDescriptor*, int, cudaStream_t) {
    return cudaErrorNotSupported;
}

cudaError_t LaunchDecodeEntropyGpu(const uint8_t*, size_t, const GpuEntropyBandDescriptor*, int, const GpuFsmEntry*, GpuBandDecodeResult*, cudaStream_t) {
    return cudaErrorNotSupported;
}

cudaError_t LaunchDecodeEntropyDiagnosticGpu(const uint8_t*, size_t, const GpuEntropyBandDescriptor*, const GpuFsmEntry*, DecodedSymbolDiagnostic*, int, int*, GpuBandDecodeResult*, cudaStream_t) {
    return cudaErrorNotSupported;
}

struct CudaDecoderPipelinePhase2B::Impl {};

CudaDecoderPipelinePhase2B::CudaDecoderPipelinePhase2B() : impl_(nullptr) {}
CudaDecoderPipelinePhase2B::~CudaDecoderPipelinePhase2B() = default;
CudaDecoderPipelinePhase2B::CudaDecoderPipelinePhase2B(CudaDecoderPipelinePhase2B&&) noexcept = default;
CudaDecoderPipelinePhase2B& CudaDecoderPipelinePhase2B::operator=(CudaDecoderPipelinePhase2B&&) noexcept = default;

bool CudaDecoderPipelinePhase2B::IsInitialized() const { return false; }
bool CudaDecoderPipelinePhase2B::Initialize(int, int, int, int, int) { return false; }
void CudaDecoderPipelinePhase2B::Release() {}
uint64_t CudaDecoderPipelinePhase2B::QueueDecode(const uint8_t*, size_t, DeviceSurfaceType, ColorMatrix, ColorRange, RowOrientation, cudaStream_t) { return 0; }
DecodeTicketStatus CudaDecoderPipelinePhase2B::PollDecode(uint64_t) { return DecodeTicketStatus::Invalid; }
bool CudaDecoderPipelinePhase2B::WaitForDecode(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2B::StreamWait(uint64_t, cudaStream_t) { return false; }
bool CudaDecoderPipelinePhase2B::GetDeviceBgraOutput(uint64_t, DeviceBgraSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2B::GetDevicePlanarOutput(uint64_t, DevicePlanarSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2B::ReleaseOutput(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2B::DownloadBgraToHost(uint64_t, uint8_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2B::DownloadPlanarToHost(uint64_t, int16_t*, size_t, int16_t*, size_t, int16_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2B::GetStageTimings(uint64_t, Phase2bSlotStageTimings*) { return false; }
bool CudaDecoderPipelinePhase2B::GetBandDecodeResults(uint64_t, std::vector<GpuBandDecodeResult>&) { return false; }
bool CudaDecoderPipelinePhase2B::DownloadBandCoefficientsToHost(uint64_t, int, std::vector<int16_t>&, int*, int*, int*) { return false; }

} // namespace HyperCine
