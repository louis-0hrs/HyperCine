// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

// Bounded wavelet5 -> wavelet4 lowpass consumer. Validates the typed plane
// contract, then calls the existing research primitive. It does not publish
// and it does not parse a bitstream. A cancel observed before the first
// write leaves the output untouched. A later cancel or a rejected plane
// must not be published by the caller.

#include "decode_s3_plane.h"

#include <array>
#include <cstdint>
#include <functional>

namespace hypercine::detail {

enum class ConsumeStatus { Complete, Rejected, Canceled };

struct MutablePlaneDesc {
    std::int16_t* data = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch = 0;
    std::size_t bytes = 0;
    operator SignedPlaneDesc() const { return {data, width, height, pitch, bytes}; }
};

struct OutputEdge {
    Generation generation{};
    std::uint32_t channel = 0;
    std::uint32_t wavelet = 4;
    std::uint32_t band = 0;
    MutablePlaneDesc plane{};
};

ConsumeStatus consume_wavelet5_lowpass(const std::array<PlaneEdge, 4>& bands, OutputEdge& output,
                                       const std::function<bool()>& canceled = {});

}  // namespace hypercine::detail
