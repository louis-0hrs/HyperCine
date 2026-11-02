// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_TESTS_DIAGNOSTIC_DECODE_GUARD_H
#define HYPERCINE_TESTS_DIAGNOSTIC_DECODE_GUARD_H

#include "CFHDDecoder.h"

#include <cstdint>

namespace hypercine::test {

constexpr int kDiagnosticWidth = 1920;
constexpr int kDiagnosticHeight = 1080;
// CFHD_GetImagePitch() rounds a BGRA row up to a 16-byte boundary. For 1920
// pixels the row is already 7680 bytes, so any other value means the SDK
// prepared a geometry this diagnostic did not ask for.
constexpr int32_t kDiagnosticBgraPitch = kDiagnosticWidth * 4;

// Decision used by test_phase2a --decode after CFHD_PrepareToDecode() and
// before any output allocation or CFHD_DecodeSample() call. Keeping this
// predicate separate makes the fail-closed cases directly testable without
// changing the frozen decoder or crafting an inconsistent bitstream.
inline bool IsAcceptedDiagnosticDecodeGeometry(int width, int height,
                                               CFHD_PixelFormat format,
                                               CFHD_Error pitch_error,
                                               int32_t pitch) {
    return width == kDiagnosticWidth &&
           height == kDiagnosticHeight &&
           format == CFHD_PIXEL_FORMAT_BGRA &&
           pitch_error == CFHD_ERROR_OKAY &&
           pitch == kDiagnosticBgraPitch;
}

}  // namespace hypercine::test

#endif  // HYPERCINE_TESTS_DIAGNOSTIC_DECODE_GUARD_H
