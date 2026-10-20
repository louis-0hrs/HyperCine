// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include "decode_task_graph.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace hypercine::detail {
// Private research unit; no CFHD parsing, temporal inverse or decoder profile.
struct SignedPlaneView {
    const std::int16_t* data;
    std::size_t bytes, pitch; // bytes, including allocated row padding
    std::uint32_t width, height;
};
struct MutableSignedPlaneView {
    std::int16_t* data;
    std::size_t bytes, pitch;
    std::uint32_t width, height;
    operator SignedPlaneView() const { return {data, bytes, pitch, width, height}; }
};
class OwnedSignedPlane {
public:
    OwnedSignedPlane(std::uint32_t width, std::uint32_t height);
    OwnedSignedPlane(OwnedSignedPlane&& other) noexcept;
    OwnedSignedPlane& operator=(OwnedSignedPlane&& other) noexcept;
    OwnedSignedPlane(const OwnedSignedPlane&) = delete;
    OwnedSignedPlane& operator=(const OwnedSignedPlane&) = delete;
    MutableSignedPlaneView view() noexcept;
    SignedPlaneView view() const noexcept;
private:
    struct Delete { void operator()(std::int16_t*) const noexcept; };
    std::unique_ptr<std::int16_t[], Delete> data_;
    std::uint32_t width_ = 0, height_ = 0;
    std::size_t pitch_ = 0, bytes_ = 0;
};
struct SpatialBandInput {
    SignedPlaneView plane;
    Generation generation;
    std::uint32_t channel, wavelet, band;
};
struct SpatialLowpassOutput {
    MutableSignedPlaneView plane;
    Generation generation;
    std::uint32_t channel, wavelet, band;
};
enum class SpatialStageResult { Complete, Canceled };

// Inputs must be entropy-completed/dequantized wavelet5 bands0..3, scale0.
// Four distinct input allocations and a distinct output allocation are required.
// Width 16..480, multiple of8; height3..270. Output has doubled dimensions.
// Pointers/pitches are 32-byte aligned; storage covers all rows, <=2MiB/plane.
// Invalid identity/storage is rejected before writes. Allocation failure throws.
// Cancellation/exception may leave partial output: callers MUST NOT publish it.
// No lifetime, producer readiness or exclusive-write guarantee is inferred from
// a view: the owner/graph must retain storage and publish only after Complete.
SpatialStageResult InverseWavelet5To4(
    const std::array<SpatialBandInput, 4>& input, SpatialLowpassOutput output,
    const std::function<bool()>& canceled = {});
} // namespace hypercine::detail
