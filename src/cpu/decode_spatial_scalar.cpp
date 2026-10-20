// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_spatial_scalar.h"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace hypercine::detail {
namespace {
constexpr std::size_t alignment = 32;
constexpr std::uint32_t max_dimension = 960;
std::size_t product(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
        throw std::invalid_argument("Scalar stage storage overflow");
    return a * b;
}
// Explicit floor division: C++17 negative signed shifts are not portable.
std::int32_t floor_shift(std::int32_t v, int shift) {
    const std::int64_t d = std::int64_t{1} << shift;
    return static_cast<std::int32_t>(v >= 0 ? v / d : -((-std::int64_t(v) + d - 1) / d));
}
std::int16_t saturate(std::int32_t v) {
    return static_cast<std::int16_t>(std::max(-32768, std::min(32767, v)));
}
std::int16_t wrap(std::int32_t v) {
    auto r = v % 65536;
    if (r < 0) r += 65536;
    return static_cast<std::int16_t>(r >= 32768 ? r - 65536 : r);
}
struct Range { std::uintptr_t begin, end; };
Range validate(SignedPlaneView p) {
    if (!p.data || !p.width || !p.height || p.width > max_dimension || p.height > max_dimension ||
        p.pitch % alignment || p.pitch < product(p.width, 2) ||
        reinterpret_cast<std::uintptr_t>(p.data) % alignment)
        throw std::invalid_argument("Scalar stage alignment or dimensions invalid");
    const auto required = product(p.pitch, p.height);
    const auto begin = reinterpret_cast<std::uintptr_t>(p.data);
    if (required > 2*1024*1024 || p.bytes < required ||
        required > std::numeric_limits<std::uintptr_t>::max() - begin)
        throw std::invalid_argument("Scalar stage extent invalid");
    return {begin, begin + required};
}
bool overlap(Range a, Range b) { return a.begin < b.end && b.begin < a.end; }
std::int32_t at(SignedPlaneView p, std::uint32_t x, std::uint32_t y) {
    return p.data[std::size_t(y) * (p.pitch / 2) + x];
}
// Overflow-protected vertical: all intermediates are int32 until final S16.
void vertical(SignedPlaneView low, SignedPlaneView high, std::uint32_t y,
              std::int16_t* even, std::int16_t* odd) {
    for (std::uint32_t x = 0; x < low.width; ++x) {
        std::int32_t e, o;
        const auto l = at(low, x, y), h = at(high, x, y);
        if (y == 0) {
            const auto b = at(low, x, 1), c = at(low, x, 2);
            e = floor_shift(11*l - 4*b + c + 4, 3);
            o = floor_shift(5*l + 4*b - c + 4, 3);
        } else if (y + 1 == low.height) {
            const auto b = at(low, x, y-1), c = at(low, x, y-2);
            e = floor_shift(5*l + 4*b - c + 4, 3);
            o = floor_shift(11*l - 4*b + c + 4, 3);
        } else {
            const auto a = at(low, x, y-1), c = at(low, x, y+1);
            e = l + floor_shift(a-c+4, 3);
            o = l + floor_shift(c-a+4, 3);
        }
        even[x] = saturate(floor_shift(e+h, 1));
        odd[x] = saturate(floor_shift(o-h, 1));
    }
}
void horizontal(const std::int16_t* low, const std::int16_t* high,
                std::int16_t* out, std::uint32_t width) {
    const auto last = width-1;
    const auto post = (last / 8) * 8;
    for (std::uint32_t x = 0; x < width; ++x) {
        const std::int32_t l = low[x], h = high[x];
        std::int32_t e, o;
        if (x == 0) {
            e = floor_shift(floor_shift(11*l - 4*low[1] + low[2] + 4, 3)+h, 1);
            o = floor_shift(floor_shift(5*l + 4*low[1] - low[2] + 4, 3)-h, 1);
            // Original aligned SSE insertion retains the low16 bits.
            out[0] = wrap(e); out[1] = wrap(o);
        } else if (x == last) {
            e = floor_shift(floor_shift(5*l + 4*low[x-1] - low[x-2] + 4, 3)+h, 1);
            o = floor_shift(floor_shift(11*l - 4*low[x-1] + low[x-2] + 4, 3)-h, 1);
            out[2*x] = saturate(e); out[2*x+1] = saturate(o);
        } else if (x <= post) {
            // Preserve EACH saturated SSE operation, not just output clipping.
            e = floor_shift(saturate(std::int32_t(saturate(low[x-1]-low[x+1]))+4), 3);
            o = floor_shift(saturate(std::int32_t(saturate(low[x+1]-low[x-1]))+4), 3);
            out[2*x] = static_cast<std::int16_t>(floor_shift(saturate(std::int32_t(saturate(e+l))+h), 1));
            out[2*x+1] = static_cast<std::int16_t>(floor_shift(saturate(std::int32_t(saturate(o+l))-h), 1));
        } else {
            // Original x86 short compound assignments narrow after EACH step.
            e = floor_shift(wrap(std::int32_t(wrap(low[x-1]-low[x+1]))+4), 3);
            o = floor_shift(wrap(std::int32_t(wrap(std::int32_t(wrap(-std::int32_t(low[x-1])))+low[x+1]))+4), 3);
            out[2*x] = static_cast<std::int16_t>(floor_shift(wrap(std::int32_t(wrap(e+l))+h), 1));
            out[2*x+1] = static_cast<std::int16_t>(floor_shift(wrap(std::int32_t(wrap(o+l))-h), 1));
        }
    }
}
} // namespace

OwnedSignedPlane::OwnedSignedPlane(std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > max_dimension || height > max_dimension)
        throw std::invalid_argument("Owned scalar plane dimensions invalid");
    width_ = width; height_ = height;
    pitch_ = (product(width, 2) + alignment-1) / alignment * alignment;
    bytes_ = product(pitch_, height);
    data_.reset(static_cast<std::int16_t*>(::operator new(bytes_, std::align_val_t{alignment})));
    // Start the scalar element lifetimes in raw aligned storage (C++17).
    std::uninitialized_value_construct_n(data_.get(), bytes_/2);
}
void OwnedSignedPlane::Delete::operator()(std::int16_t* p) const noexcept {
    ::operator delete(p, std::align_val_t{alignment});
}
OwnedSignedPlane::OwnedSignedPlane(OwnedSignedPlane&& other) noexcept { *this = std::move(other); }
OwnedSignedPlane& OwnedSignedPlane::operator=(OwnedSignedPlane&& other) noexcept {
    if (this != &other) {
        data_ = std::move(other.data_);
        width_ = std::exchange(other.width_, 0); height_ = std::exchange(other.height_, 0);
        pitch_ = std::exchange(other.pitch_, 0); bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}
MutableSignedPlaneView OwnedSignedPlane::view() noexcept { return {data_.get(), bytes_, pitch_, width_, height_}; }
SignedPlaneView OwnedSignedPlane::view() const noexcept { return {data_.get(), bytes_, pitch_, width_, height_}; }

SpatialStageResult InverseWavelet5To4(const std::array<SpatialBandInput, 4>& input,
                                    SpatialLowpassOutput output, const std::function<bool()>& canceled) {
    const auto& first = input[0];
    const auto w = first.plane.width, h = first.plane.height;
    if (w < 16 || w > 480 || w % 8 || h < 3 || h > 270 || first.channel > 2 ||
        !first.generation.session || !first.generation.value || output.wavelet != 4 || output.band != 0 ||
        output.channel != first.channel || !(output.generation == first.generation) ||
        output.plane.width != w*2 || output.plane.height != h*2)
        throw std::invalid_argument("Unsupported scalar stage identity or geometry");
    std::array<Range, 5> ranges{};
    for (std::size_t b = 0; b < input.size(); ++b) {
        const auto& item = input[b];
        if (item.wavelet != 5 || item.band != b || item.channel != first.channel ||
            !(item.generation == first.generation) || item.plane.width != w || item.plane.height != h)
            throw std::invalid_argument("Scalar stage input identity differs");
        ranges[b] = validate(item.plane);
    }
    ranges[4] = validate(output.plane);
    for (std::size_t a = 0; a < ranges.size(); ++a)
        for (std::size_t b = 0; b < a; ++b)
            if (overlap(ranges[a], ranges[b])) throw std::invalid_argument("Scalar stage planes overlap");
    if (canceled && canceled()) return SpatialStageResult::Canceled;
    OwnedSignedPlane scratch(w, 4);
    const auto s = scratch.view();
    auto* le = s.data; auto* he = le+s.pitch/2; auto* lo = he+s.pitch/2; auto* ho = lo+s.pitch/2;
    for (std::uint32_t y = 0; y < h; ++y) {
        if (canceled && canceled()) return SpatialStageResult::Canceled;
        vertical(input[0].plane, input[2].plane, y, le, lo);
        vertical(input[1].plane, input[3].plane, y, he, ho);
        auto* even = output.plane.data+std::size_t(2*y)*(output.plane.pitch/2);
        auto* odd = even+output.plane.pitch/2;
        horizontal(le, he, even, w); horizontal(lo, ho, odd, w);
        std::fill(even+2*w, even+output.plane.pitch/2, std::int16_t{0});
        std::fill(odd+2*w, odd+output.plane.pitch/2, std::int16_t{0});
    }
    return canceled && canceled() ? SpatialStageResult::Canceled : SpatialStageResult::Complete;
}
} // namespace hypercine::detail
