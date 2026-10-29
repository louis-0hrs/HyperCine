// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include "hypercine/rgb444.h"
#include "hypercine/standalone.h"

#include <array>

namespace hypercine { namespace rgb444 {

// Signed spatial pyramid for the RGB 4:4:4 p12 family, one three-level wavelet
// per full-resolution channel.
//
// This is a transform stage only. It produces no CFHD packet, applies no band
// quantization and writes no header. `PENDING_SDK_INTERNAL_STAGE` from
// docs/RGB444_ENCODER.md is unchanged: no qualified dump of the SDK's signed
// pre-quant stage exists, so nothing here may be described as bit-exact against
// the original encoder.
//
// Why a separate stage at all: the RGB444 p12 family is not the merged YUV 4:2:2
// p10 writer with a different table bolted on. Two things differ before any band
// is quantized, and both are decided by the *precision*, not by the profile:
//
//   * every channel is full resolution, so there is no luma/chroma split;
//   * the SDK prescale table for 12-bit spatial transforms carries one more
//     non-zero entry than the 10-bit table does.
//
// The second point is measured, not inferred. tools/rgb444_prescale_probe.py
// reads CODEC_TAG_PRESCALE_TABLE (tag 83) out of the 24 compressed packets of the
// frozen upstream/Example/videos/CFHDsample444.AVI and every one of them declares
// [0, 2, 2, 0, 0, 0, 0, 0] at precision 12, against [0, 2, 0, 0, 0, 0, 0, 0] for
// the 10-bit spatial table in upstream/Codec/wavelet.c.
//
// Level binding: the merged 10-bit writer is byte-exact against the original SDK
// (240/240 packets, #318), and it prescales the second pyramid level only
// (src/core/filmscan3_backend.cpp). That convention is the validated one, so the
// extra non-zero entry at p12 is bound to the next level in the same convention:
//
//   precision | level 1 | level 2 | level 3
//   ----------+---------+---------+--------
//   10-bit    |    0    |    2    |    0
//   12-bit    |    0    |    2    |    2
//
// The last row is the stage this header exists for. It is derived from a measured
// declaration plus a validated convention, not from a decoded original frame.

// Prescale shift applied when decomposing one pyramid level. Only the two values
// the frozen tables use are representable; the stage rejects anything else
// instead of silently treating it as "no prescale".
enum class Prescale : int { None = 0, Quarter = 2 };

// One three-level pyramid for one channel: level 0 is the finest decomposition.
//
// The members carry tiny valid placeholder planes because standalone::Plane has
// no default constructor. This mirrors the existing detail::FrontendFrame pattern
// in src/core/filmscan3_backend.h; a caller must still test the prescale record
// before reading a band, so a default-constructed pyramid can never masquerade as
// a completed transform.
using ChannelPyramid = std::array<standalone::WaveletLevel, 3>;

namespace detail {
inline standalone::Plane PlaceholderPlane() { return standalone::Plane(1, 1); }
inline standalone::WaveletLevel EmptyLevel() {
    return {{PlaceholderPlane(), PlaceholderPlane(), PlaceholderPlane(), PlaceholderPlane()}};
}
inline ChannelPyramid EmptyPyramid() { return {EmptyLevel(), EmptyLevel(), EmptyLevel()}; }
} // namespace detail

struct Rgb444Pyramid {
    // Stream order, matching rgb444_quality.h Plane: Green, Red, Blue.
    std::array<ChannelPyramid, 3> channels{
        {detail::EmptyPyramid(), detail::EmptyPyramid(), detail::EmptyPyramid()}};
    // Prescale actually applied per level, recorded so a caller can assert what
    // ran rather than re-deriving it from the precision. A default-constructed
    // pyramid therefore reads as three None entries and its placeholder planes
    // are 1x1, which no real frame geometry can be (the transform requires at
    // least 6x6 and even dimensions).
    std::array<Prescale, 3> prescale{{Prescale::None, Prescale::None, Prescale::None}};
    // True only after a real decomposition completed for all three channels.
    bool transformed = false;
};

// Prescale sequence for a source precision, in level order 1..3.
//
// Returns false for a precision outside {10, 12}; the caller must not fall back
// on its own, because a silently reused p10 sequence is exactly the defect this
// stage exists to avoid.
bool PrescaleSequenceForPrecision(int precision_bits, std::array<Prescale, 3>* out) noexcept;

// Decomposes one p12 frame into three full-resolution three-level pyramids.
//
// The input frame is taken as-is: canonical unsigned 12-bit source planes, no
// centring, no colour conversion, no range scaling. That matches the original
// encoder's RG48 path, which stores `value >> (16 - precision)` directly without
// removing an offset (upstream/Codec/frame.c, ConvertRGB48ToFrame16s).
//
// Throws std::invalid_argument for a frame whose planes are not the size the
// caller declared, and std::runtime_error if a wavelet dispatch fails. No
// pyramid is returned on any failure.
Rgb444Pyramid TransformRgb444P12(const Rgb444Frame& frame,
                                 standalone::WaveletIsa isa = standalone::WaveletIsa::Auto);

}} // namespace hypercine::rgb444
