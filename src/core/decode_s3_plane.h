// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

// Minimal S3 signed-plane contract (issues #144 / #195).
// Synthetic attachment in front of the S2 scheduler. Not a CFHD/GoPro parser,
// not a CUDA or Metal consumer, and not a change to InverseWavelet5To4.
//
// A plane is admitted only when address, geometry, stride and byte extent
// agree. One broken field rejects the whole view. An empty
// TaskSpec::published predicate is not a buffer and is not permission to
// complete without one (GraphLimits::require_published_output).

#include "decode_task_graph.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace hypercine::detail {

inline constexpr std::size_t kS3PlaneAlign = 32;
inline constexpr std::size_t kS3MaxPlaneBytes = 2u * 1024u * 1024u;

enum class PlaneStatus {
    Ok = 0,
    Null,
    Misaligned,
    BadGeometry,
    BadStride,
    BadExtent,
};

// One signed 16-bit plane. `pitch` and `bytes` are byte counts.
// `pitch` includes row padding. Samples are little-endian int16.
struct SignedPlaneDesc {
    const std::int16_t* data = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch = 0;
    std::size_t bytes = 0;
};

// Research-unit extents already pinned for the first spatial consumer:
// width 16..480 and a multiple of 8, height 4..270. Not a frame size.
// `require_data_align` defaults to true and admission no longer relaxes it. A
// graph-owned copy always satisfies it because OwnedBufferAllocator allocates at
// kOwnedBufferAlign; a caller-supplied descriptor that does not is rejected here
// rather than at the consuming kernel. Pitch alignment and the byte extent apply
// in every case. lowpass_output admits the doubled upper bounds (960x540);
// the consumer separately requires its exact doubled input dimensions.
// Legacy small synthetic lowpass records keep their existing lower bounds.
// InverseWavelet5To4 keeps its own 32-byte pointer check.
inline PlaneStatus check_signed_plane(const SignedPlaneDesc& plane,
                                      bool require_data_align = true,
                                      bool lowpass_output = false) noexcept {
    if (plane.data == nullptr) return PlaneStatus::Null;
    if (require_data_align &&
        reinterpret_cast<std::uintptr_t>(plane.data) % kS3PlaneAlign != 0)
        return PlaneStatus::Misaligned;
    const std::uint32_t factor = lowpass_output ? 2 : 1;
    if (plane.width < 16 || plane.width > 480 * factor ||
        (plane.width % 8) != 0 ||
        plane.height < 4 || plane.height > 270 * factor)
        return PlaneStatus::BadGeometry;
    const std::size_t packed = std::size_t(plane.width) * sizeof(std::int16_t);
    if (plane.pitch < packed || (plane.pitch % kS3PlaneAlign) != 0)
        return PlaneStatus::BadStride;
    if (plane.height != 0 &&
        plane.pitch > std::numeric_limits<std::size_t>::max() / plane.height)
        return PlaneStatus::BadExtent;
    const std::size_t need = plane.pitch * plane.height;
    if (need > kS3MaxPlaneBytes) return PlaneStatus::BadExtent;
    // S3-EXTENT: the buffer must cover every pitched row. A shorter span is
    // not a plane, even when width, height and stride are otherwise legal.
    if (plane.bytes < need || plane.bytes > kS3MaxPlaneBytes) return PlaneStatus::BadExtent;
    return PlaneStatus::Ok;
}

// TaskKey layout for this attachment (synthetic, one frame):
//   channel   = 0 Y, 1 V, 2 U. V and U are not interchangeable.
//   transform = 0 (spatial scale; not a second filter)
//   level     = wavelet length (5 for the four inputs, 4 for the lowpass)
//   frame     = 0
//   band      = 0..3 on wavelet5 inputs, 0 on the lowpass output
enum class EdgeStatus {
    Match = 0,
    Plane,
    Generation,
    Channel,
    Wavelet,
    Band,
    Scale,
};

struct PlaneEdge {
    Generation generation{};
    std::uint32_t channel = 0;
    std::uint32_t wavelet = 5;
    std::uint32_t band = 0;
    SignedPlaneDesc plane{};
};

inline EdgeStatus match_plane_edge(const PlaneEdge& edge, const TaskKey& key, Generation graph,
                                   bool require_data_align = true) noexcept {
    if (check_signed_plane(edge.plane, require_data_align, edge.wavelet == 4) != PlaneStatus::Ok)
        return EdgeStatus::Plane;
    if (!(edge.generation == graph)) return EdgeStatus::Generation;
    if (edge.channel > 2 || edge.channel != key.channel) return EdgeStatus::Channel;
    if (edge.wavelet != key.level || (edge.wavelet != 5 && edge.wavelet != 4))
        return EdgeStatus::Wavelet;
    if (edge.band > 3 || edge.band != key.band) return EdgeStatus::Band;
    if (key.transform != 0 || key.frame != 0) return EdgeStatus::Scale;
    return EdgeStatus::Match;
}

// Joint admission of one scheduler BufferVersion. Generation, version, the
// graph-owned address and the typed extent are one decision. A foreign
// generation or a short span yields an empty view and does not read samples.
struct AdmittedPlane {
    SignedPlaneDesc plane{};
    bool ok = false;
};

inline AdmittedPlane admit_owned_plane(const BufferVersion& view, const PlaneEdge& claimed,
                                       const TaskKey& key, Generation graph) noexcept {
    AdmittedPlane out;
    if (!view.valid() || view.data == nullptr || view.bytes == 0) return out;
    if (!(view.generation == graph) || !(claimed.generation == graph)) return out;
    PlaneEdge edge = claimed;
    edge.generation = view.generation;
    edge.plane.data = static_cast<const std::int16_t*>(view.data);
    edge.plane.bytes = view.bytes;
    // Guard (S3-ALIGN-ADMIT): require the alignment, do not assume it.
    //
    // Every graph-owned copy is allocated by OwnedBufferAllocator at
    // kOwnedBufferAlign, so a genuine publication is always 32-byte aligned and this
    // check costs a real publication nothing. Trusting the caller instead meant a
    // hand-built BufferVersion could name an unaligned address and be admitted into
    // a view the SIMD consumer would later reject -- or worse, read misaligned. The
    // check now rejects it at admission, where the failure is still local.
    if (match_plane_edge(edge, key, graph, /*require_data_align=*/true) != EdgeStatus::Match)
        return out;
    out.plane = edge.plane;
    out.ok = true;
    return out;
}

}  // namespace hypercine::detail
