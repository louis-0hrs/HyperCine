// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include "hypercine/standalone.h" // Reuse the SDK-free aligned allocator only.

namespace hypercine { namespace rgb444 {

// Canonical SOURCE samples, not signed/transformed GoPro internal planes.
// Top-down, native-endian uint16, right-justified unsigned values 0..4095.
// Plane addresses and positive row strides must be multiples of 16 bytes.
struct PlaneView {
    const std::uint16_t* data = nullptr;
    std::size_t bytes = 0;
    std::size_t stride_bytes = 0;
};
struct FrameView {
    int width = 0;
    int height = 0;
    PlaneView red, green, blue;
};

// Packed R,G,B native-endian uint16 components, top-down, full 16-bit range.
// Address/pitch alignment and readable-extent rules match PlaneView.
struct Rg48View {
    int width = 0;
    int height = 0;
    const std::uint16_t* data = nullptr;
    std::size_t bytes = 0;
    std::size_t stride_bytes = 0;
};

enum class Channel { Red, Green, Blue };

// Storage limits only; they confer no CFHD encoding/geometry support.
inline constexpr int kMaxFrameDimension = 4096;
inline constexpr std::size_t kMaxFramePixels = 2097152;
inline constexpr std::size_t kMaxFrameStorageBytes = 16 * 1024 * 1024;

class Rgb444Frame {
public:
    // Copies all three full-resolution planes, independently of source padding.
    // Throws invalid_argument for malformed views/p12 values, length_error for
    // storage limits, or bad_alloc. No frame is returned on any failure.
    static Rgb444Frame Copy(const FrameView& source);
    // Deinterleaves RG48 into canonical R/G/B source planes, truncating each
    // component's low four bits (value / 16). No internal G/R/B codec mapping,
    // signed transform, color conversion, range scaling or rounding is applied.
    static Rgb444Frame CopyRg48(const Rg48View& source);
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    std::size_t stride_bytes() const noexcept { return stride_bytes_; }
    const standalone::AlignedVector<std::uint16_t>& plane(Channel channel) const;

private:
    Rgb444Frame(int width, int height, std::size_t stride_bytes);
    int width_, height_;
    std::size_t stride_bytes_;
    std::array<standalone::AlignedVector<std::uint16_t>, 3> planes_;
};

// No Encode method or RGB quality/metadata adapter is exposed before the F1
// original-SDK RGB oracle qualifies transforms, band framing and p12 tables.
}} // namespace hypercine::rgb444
