// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include <cstdint>

namespace hypercine { namespace standalone { namespace detail {
// Test seam on the real CUDA spatial-launch path. Not a production control.
// Counts only launches that actually call the CUDA wavelet. The prescale-2
// middle level stays on the scalar CPU path and must not increment this.
std::uint64_t CudaSpatialDispatchCount() noexcept;
void ResetCudaSpatialDispatchCount() noexcept;

// 0 disarms. The next spatial launch whose successful-dispatch count is
// already >= successful_dispatches throws before that kernel, after a real
// failing CUDA allocation. The runtime's sticky last-error is cleared before
// the exception leaves, so a later frame is not diagnosed with this failure.
void ArmCudaMidframeFailureAfter(std::uint64_t successful_dispatches) noexcept;
bool CudaSpatialLaunchShouldFail() noexcept;
void NoteCudaSpatialDispatch() noexcept;
}}}
