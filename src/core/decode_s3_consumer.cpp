// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "decode_s3_consumer.h"
#include "decode_spatial_scalar.h"

#include <stdexcept>

namespace hypercine::detail {
namespace {

bool same_band_geometry(const PlaneEdge& band, const PlaneEdge& first) {
    return band.plane.width == first.plane.width && band.plane.height == first.plane.height &&
           band.channel == first.channel && band.wavelet == first.wavelet &&
           (band.generation == first.generation);
}

SignedPlaneView as_view(const SignedPlaneDesc& plane) {
    return SignedPlaneView{plane.data, plane.bytes, plane.pitch, plane.width, plane.height};
}

MutableSignedPlaneView as_mutable(const MutablePlaneDesc& plane) {
    return MutableSignedPlaneView{plane.data, plane.bytes, plane.pitch, plane.width, plane.height};
}

}  // namespace

ConsumeStatus consume_wavelet5_lowpass(const std::array<PlaneEdge, 4>& bands, OutputEdge& output,
                                       const std::function<bool()>& canceled) {
    const PlaneEdge& first = bands[0];
    if (check_signed_plane(first.plane) != PlaneStatus::Ok) return ConsumeStatus::Rejected;
    if (first.channel > 2 || first.wavelet != 5 || first.generation.session == 0 ||
        first.generation.value == 0)
        return ConsumeStatus::Rejected;
    for (std::uint32_t band = 0; band < 4; ++band) {
        if (bands[band].band != band || !same_band_geometry(bands[band], first))
            return ConsumeStatus::Rejected;
        if (check_signed_plane(bands[band].plane) != PlaneStatus::Ok) return ConsumeStatus::Rejected;
    }
    for (std::size_t a = 0; a < bands.size(); ++a)
        for (std::size_t b = 0; b < a; ++b)
            if (bands[a].plane.data == bands[b].plane.data) return ConsumeStatus::Rejected;

    if (output.wavelet != 4 || output.band != 0 || output.channel != first.channel ||
        !(output.generation == first.generation) ||
        output.plane.width != first.plane.width * 2 ||
        output.plane.height != first.plane.height * 2)
        return ConsumeStatus::Rejected;
    if (check_signed_plane(output.plane, true, true) != PlaneStatus::Ok)
        return ConsumeStatus::Rejected;
    for (const PlaneEdge& band : bands)
        if (band.plane.data == output.plane.data) return ConsumeStatus::Rejected;

    // Fail closed before any output store. A partial plane is not published.
    if (canceled && canceled()) return ConsumeStatus::Canceled;

    std::array<SpatialBandInput, 4> input{};
    for (std::uint32_t band = 0; band < 4; ++band) {
        input[band] = SpatialBandInput{as_view(bands[band].plane), bands[band].generation,
                                       bands[band].channel, /*wavelet=*/5, band};
    }
    SpatialLowpassOutput lowpass{as_mutable(output.plane), output.generation, output.channel,
                                 /*wavelet=*/4, /*band=*/0};
    try {
        const SpatialStageResult result = InverseWavelet5To4(input, lowpass, canceled);
        if (result == SpatialStageResult::Canceled) return ConsumeStatus::Canceled;
        if (result != SpatialStageResult::Complete) return ConsumeStatus::Rejected;
    } catch (const std::invalid_argument&) {
        return ConsumeStatus::Rejected;
    }
    return ConsumeStatus::Complete;
}

}  // namespace hypercine::detail
