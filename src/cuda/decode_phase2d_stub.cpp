// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2d.h"

namespace HyperCine {

struct CudaDecoderPipelinePhase2D::Impl {};

CudaDecoderPipelinePhase2D::CudaDecoderPipelinePhase2D() : impl_(nullptr) {}
CudaDecoderPipelinePhase2D::~CudaDecoderPipelinePhase2D() = default;
CudaDecoderPipelinePhase2D::CudaDecoderPipelinePhase2D(CudaDecoderPipelinePhase2D&&) noexcept = default;
CudaDecoderPipelinePhase2D& CudaDecoderPipelinePhase2D::operator=(CudaDecoderPipelinePhase2D&&) noexcept = default;

bool CudaDecoderPipelinePhase2D::IsInitialized() const { return false; }
bool CudaDecoderPipelinePhase2D::Initialize(int, int, int, int, int, int) { return false; }
void CudaDecoderPipelinePhase2D::Release() {}

void CudaDecoderPipelinePhase2D::SetHostIndexMode(HostIndexExecutionMode) {}
HostIndexExecutionMode CudaDecoderPipelinePhase2D::GetHostIndexMode() const { return HostIndexExecutionMode::ParallelWorkerPool; }

void CudaDecoderPipelinePhase2D::SetSchedulingPolicy(HostIndexSchedulingPolicy) {}
HostIndexSchedulingPolicy CudaDecoderPipelinePhase2D::GetSchedulingPolicy() const { return HostIndexSchedulingPolicy::LargestFirst; }

bool CudaDecoderPipelinePhase2D::SetWorkerCount(int) { return false; }
int CudaDecoderPipelinePhase2D::GetWorkerCount() const { return 0; }

void CudaDecoderPipelinePhase2D::SetEntropyMode(GpuEntropyMode, int) {}
GpuEntropyMode CudaDecoderPipelinePhase2D::GetEntropyMode() const { return GpuEntropyMode::ParallelAdaptive; }
int CudaDecoderPipelinePhase2D::GetFixedSegmentsPerBand() const { return 32; }

uint64_t CudaDecoderPipelinePhase2D::QueueDecode(const uint8_t*, size_t, DeviceSurfaceType, ColorMatrix, ColorRange, RowOrientation, cudaStream_t) { return 0; }
DecodeTicketStatus CudaDecoderPipelinePhase2D::PollDecode(uint64_t) { return DecodeTicketStatus::Invalid; }
bool CudaDecoderPipelinePhase2D::WaitForDecode(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2D::StreamWait(uint64_t, cudaStream_t) { return false; }
bool CudaDecoderPipelinePhase2D::GetDeviceBgraOutput(uint64_t, DeviceBgraSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2D::GetDevicePlanarOutput(uint64_t, DevicePlanarSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2D::ReleaseOutput(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2D::DownloadBgraToHost(uint64_t, uint8_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2D::DownloadPlanarToHost(uint64_t, int16_t*, size_t, int16_t*, size_t, int16_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2D::GetStageTimings(uint64_t, Phase2dSlotStageTimings*) { return false; }
bool CudaDecoderPipelinePhase2D::GetBandDecodeResults(uint64_t, std::vector<GpuBandDecodeResult>&) { return false; }
bool CudaDecoderPipelinePhase2D::GetSegmentDecodeResults(uint64_t, std::vector<GpuSegmentDecodeResult>&) { return false; }
bool CudaDecoderPipelinePhase2D::GetHostCheckpoints(uint64_t, std::vector<GpuEntropySegmentDescriptor>&) { return false; }
bool CudaDecoderPipelinePhase2D::DownloadBandCoefficientsToHost(uint64_t, int, std::vector<int16_t>&, int*, int*, int*) { return false; }

} // namespace HyperCine
