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
// Conservative allocation budget, independent of the 16-bit wire dimensions.
inline constexpr int kMaxDimension = 4096;
inline constexpr std::size_t kMaxPixels = 2097152; // legacy frame/sample budget, unchanged
// Separate bounded storage budget for CPU FilmScan3 4K, not wire geometry.
inline constexpr std::size_t kMaxPlaneElements = 3840u * 2160u;
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
// Per-session wavelet ISA policy (issue #70).
//
// `Auto` is the only value that consults the process-global NEON switch, and a
// session resolves it exactly once at construction. A session that asked for a
// concrete ISA therefore keeps that kernel for its whole lifetime, even while
// another thread changes the global default. The two kernels are bit-exact, so
// the choice never changes a packet byte -- only which code ran.
//
// `Neon` on a target where NEON is not built in throws instead of silently
// degrading, matching how an explicitly requested Metal backend behaves.
enum class WaveletIsa { Auto, Scalar, Neon };
// Prescale 0 or 2. The two-argument form keeps the historical behaviour: `Auto`
// resolved per call against the process-global switch.
//
// Both forms agree: `Auto` follows the process-global default once per call and may
// degrade to scalar on a host without the vector path. Only an explicit `Scalar` or
// `Neon` is a demand, and a demand that cannot be met fails instead of degrading.
WaveletLevel ForwardLevel(const Plane& input, int prescale);
WaveletLevel ForwardLevel(const Plane& input, int prescale, WaveletIsa isa);
namespace detail {
// Resolve a session policy to a concrete kernel choice exactly once. `Auto`
// consults the process-global default at that moment; `Neon` throws when NEON is
// not compiled into this build rather than silently degrading to scalar.
WaveletIsa ResolveWaveletIsa(WaveletIsa requested);
// Which kernel a resolved policy actually runs (issue #70).
//
// This exists because "the vector entry point returned NOT_SUPPORTED" and "the
// caller asked for the vector path" are different statements. A graceful `Auto`
// call may fall back to scalar -- that is its documented meaning -- but a caller
// that explicitly asked for `Neon` must be told it did not get it rather than
// silently receiving scalar output under a NeON label.
//
// `vector_succeeded` is true only when the vector entry point reported success. It is
// false both when the kernel refused the call and when it was never called at all.
// This header stays free of `wavelet.h` on purpose: the public standalone interface
// must not pull in the C status enum for one dispatch predicate.
enum class WaveletDispatchDecision { Vector, Scalar, FailClosed };
WaveletDispatchDecision DecideWaveletDispatch(WaveletIsa resolved, bool allow_fallback,
                                             bool vector_succeeded);
// Test seam: make the vector entry point report "not supported" without calling it.
//
// Without this the fail-closed arm is unreachable on every architecture: on a host
// with the kernel, a legal geometry always succeeds, and on a host without it,
// `ResolveWaveletIsa` throws before any wrapper runs. A branch that no input can
// enter is a comment, not a guard.
//
// Thread-local and false by default, so it cannot affect another thread or a
// production encode.
void SetWaveletVectorForcedUnsupported(bool forced) noexcept;
bool WaveletVectorForcedUnsupported() noexcept;
// Deterministic dispatch witness for tests (issue #70).
//
// A packet cannot reveal which kernel ran, because the two are bit-exact -- that is
// the whole reason the bit-exactness test exists. Timing cannot reveal it either
// once a second thread flips the global default. So count the dispatch decisions
// themselves, per thread, and let a test assert which kernel its own session
// actually entered. Counts are thread-local so a concurrent test cannot blur them.
struct WaveletDispatchCounts {
    std::uint64_t scalar = 0;
    std::uint64_t neon = 0;
};
WaveletDispatchCounts WaveletDispatchSnapshot() noexcept;
}
int16_t Quantize(int16_t value, unsigned divisor);
// Table17, cubic companding, MSB first, end code and zero pad to 32 bits.
// Width must be a multiple of 8 (the fixed profile has no virtual row gaps).
std::vector<uint8_t> PackHighpass(const Plane& band, std::size_t capacity);
std::vector<uint8_t> EncodeBgra(const uint8_t* data, std::size_t bytes, int width, int height, std::size_t stride);
// Compatibility wrappers retain exactly the original 128x128 packet bytes.
std::vector<uint8_t> EncodeBgra128(const uint8_t* data, std::size_t bytes, std::size_t stride);
}}
