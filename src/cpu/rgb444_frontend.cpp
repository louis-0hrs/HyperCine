// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/rgb444.h"
#include <limits>
#include <stdexcept>

namespace hypercine { namespace rgb444 {
namespace {
std::size_t Multiply(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
        throw std::length_error("RGB444 storage multiplication overflow");
    return a * b;
}
std::size_t Add(std::size_t a, std::size_t b) {
    if (a > std::numeric_limits<std::size_t>::max() - b)
        throw std::length_error("RGB444 storage addition overflow");
    return a + b;
}
void ValidatePlane(const PlaneView& plane, std::size_t row_bytes, int height) {
    if (!plane.data || reinterpret_cast<std::uintptr_t>(plane.data) % 16 ||
        plane.stride_bytes < row_bytes || plane.stride_bytes % 16)
        throw std::invalid_argument("RGB444 requires aligned planes and row strides");
    const auto required = Add(Multiply(static_cast<std::size_t>(height - 1),
                                       plane.stride_bytes), row_bytes);
    if (plane.bytes < required)
        throw std::invalid_argument("RGB444 source plane is truncated");
    // Bound pointer offsets as well as the size_t arithmetic above.
    if (required > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        throw std::length_error("RGB444 source offset is not representable");
}
std::size_t DestinationStride(int width, int height) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("RGB444 source dimensions must be positive");
    if (width > kMaxFrameDimension || height > kMaxFrameDimension ||
        Multiply(static_cast<std::size_t>(width), height) > kMaxFramePixels)
        throw std::length_error("RGB444 source exceeds the P0 storage budget");
    const auto row_bytes = Multiply(static_cast<std::size_t>(width), sizeof(std::uint16_t));
    const auto stride = Add(row_bytes, 15) & ~std::size_t{15};
    if (Multiply(Multiply(stride, height), 3) > kMaxFrameStorageBytes)
        throw std::length_error("RGB444 destination exceeds the P0 storage budget");
    return stride;
}
} // namespace

Rgb444Frame::Rgb444Frame(int width, int height, std::size_t stride_bytes)
    : width_(width), height_(height), stride_bytes_(stride_bytes) {
    const auto elements = Multiply(stride_bytes / sizeof(std::uint16_t),
                                   static_cast<std::size_t>(height));
    for (auto& plane : planes_) plane.resize(elements, 0);
}

Rgb444Frame Rgb444Frame::Copy(const FrameView& source) {
    const auto stride = DestinationStride(source.width, source.height);
    const auto row_bytes = Multiply(static_cast<std::size_t>(source.width), sizeof(std::uint16_t));
    const std::array<PlaneView, 3> inputs{source.red, source.green, source.blue};
    // Validate every span before allocating or reading any source sample.
    for (const auto& input : inputs) ValidatePlane(input, row_bytes, source.height);
    Rgb444Frame result(source.width, source.height, stride);
    for (std::size_t channel = 0; channel < inputs.size(); ++channel) {
        const auto& input = inputs[channel];
        for (int y = 0; y < source.height; ++y) {
            const auto* row = input.data + static_cast<std::size_t>(y) *
                                             (input.stride_bytes / sizeof(std::uint16_t));
            auto* output = result.planes_[channel].data() + static_cast<std::size_t>(y) *
                                                               (stride / sizeof(std::uint16_t));
            for (int x = 0; x < source.width; ++x) {
                const auto value = row[x];
                if (value > 4095)
                    throw std::invalid_argument("RGB444 source sample exceeds unsigned p12");
                output[x] = value;
            }
        }
    }
    return result;
}

Rgb444Frame Rgb444Frame::CopyRg48(const Rg48View& source) {
    const auto stride = DestinationStride(source.width, source.height);
    const auto row_bytes = Multiply(static_cast<std::size_t>(source.width), 6);
    ValidatePlane({source.data, source.bytes, source.stride_bytes}, row_bytes, source.height);
    Rgb444Frame result(source.width, source.height, stride);
    for (int y = 0; y < source.height; ++y) {
        const auto* input = source.data + static_cast<std::size_t>(y) * (source.stride_bytes / 2);
        const auto offset = static_cast<std::size_t>(y) * (stride / 2);
        for (int x = 0; x < source.width; ++x)
            for (std::size_t c = 0; c < 3; ++c)
                result.planes_[c][offset + x] = input[static_cast<std::size_t>(x) * 3 + c] >> 4;
    }
    return result;
}

const standalone::AlignedVector<std::uint16_t>& Rgb444Frame::plane(Channel channel) const {
    switch (channel) {
    case Channel::Red: return planes_[0];
    case Channel::Green: return planes_[1];
    case Channel::Blue: return planes_[2];
    }
    throw std::invalid_argument("invalid RGB444 source channel");
}
}} // namespace hypercine::rgb444
