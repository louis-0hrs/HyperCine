// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/rgb444_transform.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace hypercine { namespace rgb444 {
namespace {

// The frozen SDK stores the spatial prescale table with two bits per entry and
// reads it back as `value >> (14 - i*2) & 0x3` (upstream/Codec/decoder.c). The
// tables only ever use 0 and 2; anything else is not a shift this stage models.
Prescale ToPrescale(int shift, int level) {
    if (shift == 0) return Prescale::None;
    if (shift == 2) return Prescale::Quarter;
    throw std::invalid_argument("unsupported prescale shift at level " + std::to_string(level));
}

// Copies one unsigned 12-bit source plane into a signed working plane.
//
// The original RG48 path stores `value >> (16 - precision)` and applies no
// offset removal (upstream/Codec/frame.c, ConvertRGB48ToFrame16s), so a value in
// 0..4095 stays a positive signed sample. This stage therefore does not centre
// the data; centring would be a different transform and is not what the frozen
// encoder does on this path.
standalone::Plane WorkingPlane(const standalone::AlignedVector<std::uint16_t>& source,
                               int width, int height, std::size_t source_stride_bytes) {
    if (width < 6 || height < 6 || (width % 2) || (height % 2))
        throw std::invalid_argument("RGB444 p12 transform requires even dimensions of at least 6");
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint16_t);
    if (source_stride_bytes < row_bytes)
        throw std::invalid_argument("RGB444 p12 source row is shorter than its declared width");
    const std::size_t required =
        source_stride_bytes * static_cast<std::size_t>(height - 1) + row_bytes;
    if (source.size() * sizeof(std::uint16_t) < required)
        throw std::invalid_argument("RGB444 p12 source plane is truncated");

    standalone::Plane out(width, height);
    const std::size_t source_stride_elems = source_stride_bytes / sizeof(std::uint16_t);
    for (int y = 0; y < height; ++y) {
        const auto* row = source.data() + static_cast<std::size_t>(y) * source_stride_elems;
        auto* destination = out.pixels.data() + static_cast<std::size_t>(y) * out.stride;
        for (int x = 0; x < width; ++x) {
            const std::uint32_t value = row[x];
            if (value > 0x0FFFu)
                throw std::invalid_argument("RGB444 p12 source sample exceeds 12 bits");
            destination[x] = static_cast<std::int16_t>(value);
        }
        // Padding stays zero so the transform never reads an indeterminate tap.
        for (int x = width; x < out.stride; ++x) destination[x] = 0;
    }
    return out;
}

ChannelPyramid Decompose(const standalone::Plane& source, const std::array<Prescale, 3>& sequence,
                         standalone::WaveletIsa isa) {
    ChannelPyramid pyramid = detail::EmptyPyramid();
    pyramid[0] = standalone::ForwardLevel(source, static_cast<int>(sequence[0]), isa);
    pyramid[1] = standalone::ForwardLevel(pyramid[0].bands[0],
                                          static_cast<int>(sequence[1]), isa);
    pyramid[2] = standalone::ForwardLevel(pyramid[1].bands[0],
                                          static_cast<int>(sequence[2]), isa);
    return pyramid;
}

} // namespace

bool PrescaleSequenceForPrecision(int precision_bits, std::array<Prescale, 3>* out) noexcept {
    if (!out) return false;
    // Level order is finest first, matching the order the merged writer calls
    // ForwardLevel in (src/core/filmscan3_backend.cpp). The 10-bit row is the
    // table that produced 240/240 byte-identical packets against the original SDK
    // (#318); the 12-bit row is the same table with the extra non-zero entry the
    // frozen p12 sample actually declares.
    switch (precision_bits) {
    case 10:
        *out = {{Prescale::None, Prescale::Quarter, Prescale::None}};
        return true;
    case 12:
        *out = {{Prescale::None, Prescale::Quarter, Prescale::Quarter}};
        return true;
    default:
        return false;
    }
}

Rgb444Pyramid TransformRgb444P12(const Rgb444Frame& frame, standalone::WaveletIsa isa) {
    if (frame.width() <= 0 || frame.height() <= 0)
        throw std::invalid_argument("RGB444 p12 transform requires a non-empty frame");

    std::array<Prescale, 3> sequence{};
    if (!PrescaleSequenceForPrecision(12, &sequence))
        throw std::invalid_argument("RGB444 p12 transform requires a 12-bit prescale sequence");

    Rgb444Pyramid result;
    result.prescale = sequence;

    // Rgb444Frame exposes the canonical source planes R/G/B; the encoded stream
    // order is G/R/B (rgb444_quality.h Plane). Map explicitly instead of relying
    // on the array order so the two conventions cannot silently drift apart.
    const Channel source_channels[3] = {Channel::Green, Channel::Red, Channel::Blue};
    for (int channel = 0; channel < 3; ++channel) {
        const auto working = WorkingPlane(frame.plane(source_channels[channel]),
                                          frame.width(), frame.height(), frame.stride_bytes());
        result.channels[static_cast<std::size_t>(channel)] =
            Decompose(working, sequence, isa);
    }
    result.transformed = true;
    return result;
}

}} // namespace hypercine::rgb444
