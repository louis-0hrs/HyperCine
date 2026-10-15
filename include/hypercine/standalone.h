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

// Experimental fixed 128x128 progressive YUV422 p10, GOP1. Throws on invalid
// bounds/profile/capacity. No SDK or GPU types are part of this interface.
struct Plane {
    int width, height, stride; // stride in int16 elements, padded to 16 bytes
    AlignedVector<int16_t> pixels;
    Plane(int width, int height);
};
struct WaveletLevel { std::array<Plane, 4> bands; }; // LL, LH, HL, HH
using Planes = std::array<Plane, 3>; // Y, Cr, Cb (CFHD channel order)
Planes ConvertBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride);
WaveletLevel ForwardLevel(const Plane& input, int prescale); // 0 or 2
int16_t Quantize(int16_t value, unsigned divisor);
// Table17, cubic companding, MSB first, end code and zero pad to 32 bits.
// Width must be a multiple of 8 (the fixed profile has no virtual row gaps).
std::vector<uint8_t> PackHighpass(const Plane& band, std::size_t capacity);
std::vector<uint8_t> EncodeBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride);
}}
