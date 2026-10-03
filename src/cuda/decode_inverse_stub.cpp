// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_inverse_cuda.h"

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

} // namespace HyperCine
