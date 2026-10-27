// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include <cstdint>
#include <stdexcept>

namespace hypercine { namespace standalone {
struct Frontend;
namespace detail {
// Test seams on the real CUDA spatial-launch path. Not production controls.
//
// Every seam below takes the frontend it applies to. Fault injection used to be
// process-global, which meant one session arming a failure could trip a concurrent
// unrelated session's kernel launch -- a test-only hazard that is indistinguishable
// from a real isolation defect when it fires. The state now lives in the session.
//
// Launches counted are only those that actually call the CUDA wavelet. The
// prescale-2 middle level stays on the scalar CPU path and must not increment it.
std::uint64_t CudaSpatialDispatchCount(const Frontend& frontend) noexcept;

// Monotonic process-wide total of successful spatial launches. Accounting only:
// nothing reads it to decide a failure, so it cannot leak state between sessions.
std::uint64_t ProcessCudaSpatialDispatchTotal() noexcept;

// Arms this session only. 0 disarms. The next spatial launch whose success count is
// already >= successful_dispatches throws before that kernel, after a real failing
// CUDA allocation. The runtime's sticky last-error is cleared before the exception
// leaves, so a later frame is not diagnosed with this failure.
// Throws std::invalid_argument when the frontend is not a CUDA frontend.
void ArmCudaMidframeFailureAfter(Frontend& frontend, std::uint64_t successful_dispatches);

// True when this session's own injection is armed and due. Never consults another
// session's counters.
bool CudaSpatialLaunchShouldFail(const Frontend& frontend) noexcept;
}}}
