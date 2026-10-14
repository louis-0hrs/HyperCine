// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2e.h"

namespace HyperCine {

struct CudaDecoderPipelinePhase2E::Impl {};

CudaDecoderPipelinePhase2E::CudaDecoderPipelinePhase2E() : impl_(nullptr) {}
CudaDecoderPipelinePhase2E::~CudaDecoderPipelinePhase2E() = default;
CudaDecoderPipelinePhase2E::CudaDecoderPipelinePhase2E(CudaDecoderPipelinePhase2E&&) noexcept = default;
CudaDecoderPipelinePhase2E& CudaDecoderPipelinePhase2E::operator=(CudaDecoderPipelinePhase2E&&) noexcept = default;

bool CudaDecoderPipelinePhase2E::IsInitialized() const { return false; }
bool CudaDecoderPipelinePhase2E::Initialize(int, int, int, int, int, int, int) { return false; }
void CudaDecoderPipelinePhase2E::Release() {}

void CudaDecoderPipelinePhase2E::SetPipelineMode(Phase2EPipelineMode) {}
Phase2EPipelineMode CudaDecoderPipelinePhase2E::GetPipelineMode() const { return Phase2EPipelineMode::Phase2EFullOverlap; }

void CudaDecoderPipelinePhase2E::SetSchedulingPolicy(Phase2ESchedulingPolicy) {}
Phase2ESchedulingPolicy CudaDecoderPipelinePhase2E::GetSchedulingPolicy() const { return Phase2ESchedulingPolicy::PolicyA_LargestFirst; }

bool CudaDecoderPipelinePhase2E::SetWorkerCount(int) { return false; }
int CudaDecoderPipelinePhase2E::GetWorkerCount() const { return 0; }

bool CudaDecoderPipelinePhase2E::SetReplayStreamCount(int) { return false; }
int CudaDecoderPipelinePhase2E::GetReplayStreamCount() const { return 0; }

void CudaDecoderPipelinePhase2E::SetEntropyMode(GpuEntropyMode, int) {}
GpuEntropyMode CudaDecoderPipelinePhase2E::GetEntropyMode() const { return GpuEntropyMode::ParallelAdaptive; }
int CudaDecoderPipelinePhase2E::GetFixedSegmentsPerBand() const { return 32; }

void CudaDecoderPipelinePhase2E::SetTestDelayHook(DelayHookFn) {}

uint64_t CudaDecoderPipelinePhase2E::QueueDecode(const uint8_t*, size_t, DeviceSurfaceType, ColorMatrix, ColorRange, RowOrientation, cudaStream_t) { return 0; }
uint64_t CudaDecoderPipelinePhase2E::QueueDecodeParsed(const uint8_t*, size_t,
    const std::vector<GpuEntropyBandDescriptor>&, DeviceSurfaceType, ColorMatrix,
    ColorRange, RowOrientation, bool, float, bool) { return 0; }
uint64_t CudaDecoderPipelinePhase2E::QueueDecodeFieldPlusParsed(const uint8_t*, size_t,
    const FieldPlusSampleDescriptor&, int, DeviceSurfaceType, ColorMatrix, ColorRange,
    RowOrientation, bool) { return 0; }
DecoderMemoryStats CudaDecoderPipelinePhase2E::GetMemoryStats() const { return {}; }
void CudaDecoderPipelinePhase2E::PrepareProductionScratch() {}
DecodeTicketStatus CudaDecoderPipelinePhase2E::PollDecode(uint64_t) { return DecodeTicketStatus::Invalid; }
bool CudaDecoderPipelinePhase2E::WaitForDecode(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2E::StreamWait(uint64_t, cudaStream_t) { return false; }
bool CudaDecoderPipelinePhase2E::GetDeviceBgraOutput(uint64_t, DeviceBgraSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2E::GetDevicePlanarOutput(uint64_t, DevicePlanarSurfaceDescriptor*) { return false; }
bool CudaDecoderPipelinePhase2E::InspectReconstruction(uint64_t, DecoderReconstruction*) { return false; }
bool CudaDecoderPipelinePhase2E::ReleaseOutput(uint64_t) { return false; }
bool CudaDecoderPipelinePhase2E::DownloadBgraToHost(uint64_t, uint8_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2E::DownloadPlanarToHost(uint64_t, int16_t*, size_t, int16_t*, size_t, int16_t*, size_t) { return false; }
bool CudaDecoderPipelinePhase2E::GetStageTimings(uint64_t, Phase2eSlotStageTimings*) { return false; }
bool CudaDecoderPipelinePhase2E::GetBandDecodeResults(uint64_t, std::vector<GpuBandDecodeResult>&) { return false; }
bool CudaDecoderPipelinePhase2E::GetSegmentDecodeResults(uint64_t, std::vector<GpuSegmentDecodeResult>&) { return false; }
bool CudaDecoderPipelinePhase2E::GetHostCheckpoints(uint64_t, std::vector<GpuEntropySegmentDescriptor>&) { return false; }
bool CudaDecoderPipelinePhase2E::DownloadBandCoefficientsToHost(uint64_t, int, std::vector<int16_t>&, int*, int*, int*) { return false; }
std::string CudaDecoderPipelinePhase2E::ExportTimelineJson(uint64_t) { return "{}"; }

bool CudaDecoderPipelinePhase2E::GetGpuCheckpoints(uint64_t, std::vector<GpuEntropySegmentDescriptor>&) { return false; }
bool CudaDecoderPipelinePhase2E::GetGpuIndexResults(uint64_t, std::vector<GpuCheckpointIndexResult>&) { return false; }
bool CudaDecoderPipelinePhase2E::DownloadIntermediateLl(uint64_t, int, int, std::vector<int16_t>&) { return false; }
bool CudaDecoderPipelinePhase2E::DownloadInterlacedFieldStage(uint64_t, int, std::vector<int16_t>&,
    std::vector<int16_t>&, std::vector<int16_t>&, int*, int*, int*, int*) { return false; }
bool CudaDecoderPipelinePhase2E::DownloadFieldPlusTemporalStages(uint64_t, int,
    std::vector<int16_t>&, std::vector<int16_t>&, std::vector<int16_t>&,
    std::vector<int16_t>&, int*, int*) { return false; }
bool CudaDecoderPipelinePhase2E::DownloadFieldPlusSpatialStages(uint64_t, int,
    std::vector<int16_t>&, std::vector<int16_t>&, int*, int*) { return false; }

void CudaDecoderPipelinePhase2E::SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy) {}
Phase2FCSelectorPolicy CudaDecoderPipelinePhase2E::GetPhase2FCSelectorPolicy() const { return Phase2FCSelectorPolicy::StaticHeuristic; }
void CudaDecoderPipelinePhase2E::SetPhase2FCChunkSize(uint32_t) {}
uint32_t CudaDecoderPipelinePhase2E::GetPhase2FCChunkSize() const { return 256; }
void CudaDecoderPipelinePhase2E::SetPhase2FCOracleDecisions(const std::vector<BandEntropyMethod>&) {}
bool CudaDecoderPipelinePhase2E::GetPhase2FCBandRecords(uint64_t, std::vector<Phase2FCBandRecord>&) const { return false; }

} // namespace HyperCine
