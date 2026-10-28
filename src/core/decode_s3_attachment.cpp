// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_s3_attachment.h"
#include "decode_s3_consumer.h"
#include "decode_spatial_scalar.h"
#include <cstring>
#include <stdexcept>

namespace hypercine::detail {
TaskSpec MakeSpatialLowpassTask(Generation generation, std::uint32_t channel,
                               std::uint32_t width, std::uint32_t height,
                               std::size_t input_pitch) {
    // The primitive reads native int16 objects; this attachment promises LE.
    const std::uint16_t one = 1;
    unsigned char first;
    std::memcpy(&first, &one, 1);
    if (first != 1 || channel > 2 || !generation.session || !generation.value ||
        width < 16 || width > 480 || width % 8 || height < 4 || height > 270 ||
        input_pitch % 32 || input_pitch < std::size_t(width) * 2 ||
        input_pitch > kS3MaxPlaneBytes / height)
        throw std::invalid_argument("S3 attachment identity or extent");
    TaskSpec task;
    task.key = {channel, 0, 4, 0, 0};
    for (std::uint32_t band = 0; band < 4; ++band)
        task.inputs.push_back({channel, 0, 5, 0, band});
    task.execute = [=](const TaskContext& context) {
        std::array<PlaneEdge, 4> bands{};
        // The graph retains byte copies. Materialize signed-16 objects before
        // the C++17 primitive reads them; do not alias its unsigned-char storage
        // as an array of int16_t. Scratch is bounded to four input extents.
        std::array<OwnedSignedPlane, 4> samples{
            OwnedSignedPlane(width,height), OwnedSignedPlane(width,height),
            OwnedSignedPlane(width,height), OwnedSignedPlane(width,height)};
        for (std::uint32_t band = 0; band < 4; ++band) {
            PlaneIdentity expected{generation, channel, 5, band, 0, 0,
                                   width, height, input_pitch};
            const PlaneView view = context.input_plane(band, expected);
            if (!view.valid()) throw std::invalid_argument("S3 typed input not admitted");
            auto native = samples[band].view();
            for (std::uint32_t row = 0; row < height; ++row)
                std::memcpy(native.data + row * native.pitch / 2,
                    static_cast<const unsigned char*>(view.data) + row * view.pitch,
                    std::size_t(width) * 2);
            bands[band] = {generation, channel, 5, band,
                {native.data, native.width, native.height, native.pitch, native.bytes}};
        }
        OwnedSignedPlane storage(width * 2, height * 2);
        const auto v = storage.view();
        OutputEdge output{generation, channel, 4, 0,
                          {v.data, v.width, v.height, v.pitch, v.bytes}};
        const auto result = consume_wavelet5_lowpass(bands, output,
            [&] { return context.cancellation_requested(); });
        // A stopped generation discards the partial plane. complete() retains
        // its first error/cancel decision and never releases a successor.
        if (result == ConsumeStatus::Canceled) return;
        if (result != ConsumeStatus::Complete ||
            !context.publish_plane(v.data, v.bytes, v.width, v.height, v.pitch))
            throw std::invalid_argument("S3 output not published");
    };
    task.published = [] { return true; }; // completion still requires owned version
    return task;
}
} // namespace hypercine::detail
