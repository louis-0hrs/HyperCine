// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace hypercine { namespace standalone {
// Explicit alignment for CPU buffers and the future Metal staging boundary.
template<class T> struct AlignedAllocator {
    using value_type = T;
    AlignedAllocator() noexcept = default;
    template<class U> AlignedAllocator(const AlignedAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{32}));
    }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p, std::align_val_t{32}); }
};
template<class T, class U> bool operator==(const AlignedAllocator<T>&, const AlignedAllocator<U>&) { return true; }
template<class T, class U> bool operator!=(const AlignedAllocator<T>&, const AlignedAllocator<U>&) { return false; }
template<class T> using AlignedVector = std::vector<T, AlignedAllocator<T>>;

// Experimental fixed-quant progressive YUV422 p10, GOP1. Throws on invalid
// bounds/profile/capacity. No SDK or GPU types are part of this interface.
// Three separate concepts. Keeping them apart matters: a format that is legal but
// too large for the current session is a resource problem, not an invalid format,
// and must not be reported as one.
//
// 1. Format legality -- only the wavelet shape and the 16-bit wire field bound.
//    Width: multiple of 128 (deepest chroma band multiple of 8).
//    Height: multiple of 8, >= 24 (six-tap input, final LL height >= 3).
//    See ValidateGeometry() / ValidateFilmScan3Geometry().
inline constexpr int kMaxWireDimension = 65535; // 16-bit wire dimension field
//
// 2. Resource budget -- what one session may allocate on this build. Bounded by
//    RAM, not by the format. A geometry beyond it is rejected as std::length_error
//    with its own message, so callers can tell "cannot afford" from "illegal".
inline constexpr std::size_t kMaxPixels = 2097152; // legacy frame/sample budget, unchanged
// Separate bounded storage budget for CPU FilmScan3 4K, not wire geometry.
inline constexpr std::size_t kMaxPlaneElements = 3840u * 2160u;
//
// 3. Backend qualification -- which backend can actually encode which geometry.
//    Tracked per backend, deliberately not expressed as a global constant.
//
// kMaxDimension is retired from the format path. It survives as the allocation
// guard for the legacy non-FilmScan3 plane path, where it bounds storage rather
// than declaring formats invalid.
inline constexpr int kMaxDimension = 4096;
inline bool PlaneStorageFits(int stride, int height) noexcept {
    return stride > 0 && height > 0 &&
        static_cast<std::size_t>(stride) <= kMaxPlaneElements / static_cast<std::size_t>(height);
}
// Width: multiple of 128 (deepest chroma band multiple of 8).
// Height: multiple of 8, >=24 (six-tap input, final LL height >=3).
void ValidateGeometry(int width, int height);
struct Plane {
    int width, height, stride; // stride in int16 elements, padded to 16 bytes
    AlignedVector<int16_t> pixels;
    Plane(int width, int height);
};
struct WaveletLevel { std::array<Plane, 4> bands; }; // LL, LH, HL, HH
using Planes = std::array<Plane, 3>; // Y, Cr, Cb (CFHD channel order)
Planes ConvertBgra(const uint8_t* data, std::size_t bytes, int width, int height, std::size_t stride);
Planes ConvertBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride);
WaveletLevel ForwardLevel(const Plane& input, int prescale); // 0 or 2
int16_t Quantize(int16_t value, unsigned divisor);
// Table17, cubic companding, MSB first, end code and zero pad to 32 bits.
// Width must be a multiple of 8 (the fixed profile has no virtual row gaps).
std::vector<uint8_t> PackHighpass(const Plane& band, std::size_t capacity);
std::vector<uint8_t> EncodeBgra(const uint8_t* data, std::size_t bytes, int width, int height, std::size_t stride);
// Compatibility wrappers retain exactly the original 128x128 packet bytes.
std::vector<uint8_t> EncodeBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride);
}}
