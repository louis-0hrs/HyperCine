// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_TEST_VECTORS_H
#define HYPERCINE_TEST_VECTORS_H

/**
 * @file test_vectors.h
 * @brief Deterministic synthetic test vector generator and hash utilities for HyperCine
 */

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <new>

namespace HyperCineTest {

template <typename T, size_t Alignment = 16>
struct AlignedAllocator {
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    template <typename U> struct rebind { using other = AlignedAllocator<U, Alignment>; };
    AlignedAllocator() noexcept {}
    template <typename U> AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}
    bool operator==(const AlignedAllocator&) const noexcept { return true; }
    bool operator!=(const AlignedAllocator&) const noexcept { return false; }
    T* allocate(size_t n) {
        if (n == 0) return nullptr;
#if defined(_WIN32)
        void* p = _aligned_malloc(n * sizeof(T), Alignment);
#else
        void* p = nullptr;
        if (posix_memalign(&p, Alignment, n * sizeof(T)) != 0) p = nullptr;
#endif
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) noexcept {
        if (!p) return;
#if defined(_WIN32)
        _aligned_free(p);
#else
        free(p);
#endif
    }
};

template <typename T>
using AlignedVector = std::vector<T, AlignedAllocator<T, 16>>;

enum TestPatternType {
    PATTERN_CONSTANT_BLACK = 0,
    PATTERN_CONSTANT_WHITE,
    PATTERN_HORIZONTAL_GRADIENT,
    PATTERN_VERTICAL_GRADIENT,
    PATTERN_CHECKERBOARD,
    PATTERN_SINGLE_PIXEL_IMPULSE,
    PATTERN_HARD_VERTICAL_EDGE,
    PATTERN_HARD_HORIZONTAL_EDGE,
    PATTERN_PSEUDO_RANDOM_NOISE,
    PATTERN_COUNT
};

inline const char* GetPatternName(TestPatternType pattern) {
    switch (pattern) {
        case PATTERN_CONSTANT_BLACK: return "constant_black";
        case PATTERN_CONSTANT_WHITE: return "constant_white";
        case PATTERN_HORIZONTAL_GRADIENT: return "horizontal_gradient";
        case PATTERN_VERTICAL_GRADIENT: return "vertical_gradient";
        case PATTERN_CHECKERBOARD: return "checkerboard";
        case PATTERN_SINGLE_PIXEL_IMPULSE: return "single_pixel_impulse";
        case PATTERN_HARD_VERTICAL_EDGE: return "hard_vertical_edge";
        case PATTERN_HARD_HORIZONTAL_EDGE: return "hard_horizontal_edge";
        case PATTERN_PSEUDO_RANDOM_NOISE: return "pseudo_random_noise";
        default: return "unknown";
    }
}

// 64-bit FNV-1a deterministic hash
inline uint64_t ComputeFNV1a64(const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(ptr[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Deterministic Pseudo-Random Number Generator (fixed seed)
class DeterministicPRNG {
private:
    uint32_t state;
public:
    explicit DeterministicPRNG(uint32_t seed = 0x1337BEEF) : state(seed) {}
    uint32_t Next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    uint8_t NextByte() {
        return static_cast<uint8_t>(Next() >> 16);
    }
    uint16_t Next16(uint16_t maxVal = 4095) {
        return static_cast<uint16_t>((Next() >> 8) % (maxVal + 1));
    }
};

/**
 * @brief Generate a deterministic integer test image buffer (row-major).
 */
inline std::vector<int> GenerateTestVectorInt(TestPatternType pattern, int width, int height, int bitDepth = 12) {
    std::vector<int> buffer(static_cast<size_t>(width) * height, 0);
    const int maxVal = (1 << bitDepth) - 1;
    DeterministicPRNG prng(0xACE1337u + static_cast<uint32_t>(pattern));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            switch (pattern) {
                case PATTERN_CONSTANT_BLACK:
                    buffer[idx] = 0;
                    break;
                case PATTERN_CONSTANT_WHITE:
                    buffer[idx] = maxVal;
                    break;
                case PATTERN_HORIZONTAL_GRADIENT:
                    buffer[idx] = (width > 1) ? (x * maxVal / (width - 1)) : 0;
                    break;
                case PATTERN_VERTICAL_GRADIENT:
                    buffer[idx] = (height > 1) ? (y * maxVal / (height - 1)) : 0;
                    break;
                case PATTERN_CHECKERBOARD: {
                    int blockSize = 8;
                    int cx = (x / blockSize) % 2;
                    int cy = (y / blockSize) % 2;
                    buffer[idx] = (cx ^ cy) ? maxVal : 0;
                    break;
                }
                case PATTERN_SINGLE_PIXEL_IMPULSE: {
                    int midX = width / 2;
                    int midY = height / 2;
                    buffer[idx] = (x == midX && y == midY) ? maxVal : 0;
                    break;
                }
                case PATTERN_HARD_VERTICAL_EDGE:
                    buffer[idx] = (x >= width / 2) ? maxVal : 0;
                    break;
                case PATTERN_HARD_HORIZONTAL_EDGE:
                    buffer[idx] = (y >= height / 2) ? maxVal : 0;
                    break;
                case PATTERN_PSEUDO_RANDOM_NOISE:
                    buffer[idx] = static_cast<int>(prng.Next16(static_cast<uint16_t>(maxVal)));
                    break;
                default:
                    buffer[idx] = 0;
                    break;
            }
        }
    }
    return buffer;
}

/**
 * @brief Generate a deterministic RGBA/BGRA byte image buffer (8-bit per channel, 4 bytes/pixel).
 */
inline AlignedVector<uint8_t> GenerateTestVectorRGBA(TestPatternType pattern, int width, int height) {
    AlignedVector<uint8_t> buffer(static_cast<size_t>(width) * height * 4, 0);
    DeterministicPRNG prng(0x5EEDCAFEu + static_cast<uint32_t>(pattern));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            uint8_t v = 0;
            switch (pattern) {
                case PATTERN_CONSTANT_BLACK:
                    v = 0;
                    break;
                case PATTERN_CONSTANT_WHITE:
                    v = 255;
                    break;
                case PATTERN_HORIZONTAL_GRADIENT:
                    v = static_cast<uint8_t>((width > 1) ? (x * 255 / (width - 1)) : 0);
                    break;
                case PATTERN_VERTICAL_GRADIENT:
                    v = static_cast<uint8_t>((height > 1) ? (y * 255 / (height - 1)) : 0);
                    break;
                case PATTERN_CHECKERBOARD: {
                    int cx = (x / 8) % 2;
                    int cy = (y / 8) % 2;
                    v = (cx ^ cy) ? 255 : 0;
                    break;
                }
                case PATTERN_SINGLE_PIXEL_IMPULSE: {
                    v = (x == width / 2 && y == height / 2) ? 255 : 0;
                    break;
                }
                case PATTERN_HARD_VERTICAL_EDGE:
                    v = (x >= width / 2) ? 255 : 0;
                    break;
                case PATTERN_HARD_HORIZONTAL_EDGE:
                    v = (y >= height / 2) ? 255 : 0;
                    break;
                case PATTERN_PSEUDO_RANDOM_NOISE:
                    v = prng.NextByte();
                    break;
                default:
                    v = 0;
                    break;
            }
            // BGRA layout: B, G, R, A
            buffer[idx + 0] = v; // Blue
            buffer[idx + 1] = v; // Green
            buffer[idx + 2] = v; // Red
            buffer[idx + 3] = 255; // Alpha
        }
    }
    return buffer;
}

/**
 * @brief Generate a deterministic YUY2 (4:2:2) byte image buffer (2 bytes/pixel).
 */
inline AlignedVector<uint8_t> GenerateTestVectorYUY2(TestPatternType pattern, int width, int height) {
    AlignedVector<uint8_t> buffer(static_cast<size_t>(width) * height * 2, 0);
    DeterministicPRNG prng(0x5EEDCAFEu + static_cast<uint32_t>(pattern));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; x += 2) {
            size_t idx = (static_cast<size_t>(y) * width + x) * 2;
            uint8_t v0 = 0, v1 = 0;
            switch (pattern) {
                case PATTERN_CONSTANT_BLACK:
                    v0 = v1 = 16;
                    break;
                case PATTERN_CONSTANT_WHITE:
                    v0 = v1 = 235;
                    break;
                case PATTERN_HORIZONTAL_GRADIENT:
                    v0 = static_cast<uint8_t>(16 + ((width > 1) ? (x * (235 - 16) / (width - 1)) : 0));
                    v1 = static_cast<uint8_t>(16 + ((width > 1) ? ((x + 1) * (235 - 16) / (width - 1)) : 0));
                    break;
                case PATTERN_VERTICAL_GRADIENT:
                    v0 = v1 = static_cast<uint8_t>(16 + ((height > 1) ? (y * (235 - 16) / (height - 1)) : 0));
                    break;
                case PATTERN_CHECKERBOARD: {
                    int cx0 = (x / 8) % 2;
                    int cx1 = ((x + 1) / 8) % 2;
                    int cy = (y / 8) % 2;
                    v0 = (cx0 ^ cy) ? 235 : 16;
                    v1 = (cx1 ^ cy) ? 235 : 16;
                    break;
                }
                case PATTERN_SINGLE_PIXEL_IMPULSE: {
                    v0 = (x == width / 2 && y == height / 2) ? 235 : 16;
                    v1 = ((x + 1) == width / 2 && y == height / 2) ? 235 : 16;
                    break;
                }
                case PATTERN_HARD_VERTICAL_EDGE:
                    v0 = (x >= width / 2) ? 235 : 16;
                    v1 = ((x + 1) >= width / 2) ? 235 : 16;
                    break;
                case PATTERN_HARD_HORIZONTAL_EDGE:
                    v0 = v1 = (y >= height / 2) ? 235 : 16;
                    break;
                case PATTERN_PSEUDO_RANDOM_NOISE:
                    v0 = static_cast<uint8_t>(16 + (prng.NextByte() % (235 - 16)));
                    v1 = static_cast<uint8_t>(16 + (prng.NextByte() % (235 - 16)));
                    break;
                default:
                    v0 = v1 = 16;
                    break;
            }
            // YUY2 layout: Y0, U, Y1, V
            buffer[idx + 0] = v0;   // Y0
            buffer[idx + 1] = 128;  // U
            buffer[idx + 2] = v1;   // Y1
            buffer[idx + 3] = 128;  // V
        }
    }
    return buffer;
}

/**
 * @file Calculate Peak Signal-to-Noise Ratio (PSNR) between original and reconstructed BGRA buffers,
 *        comparing the active color channels (B, G, R).
 */
inline double ComputePSNR_BGRA(const uint8_t* orig, const uint8_t* recon, int width, int height, bool inverted = false, double maxVal = 255.0) {
    double mse = 0.0;
    size_t count = 0;
    for (int y = 0; y < height; ++y) {
        int ry = inverted ? (height - 1 - y) : y;
        for (int x = 0; x < width; ++x) {
            size_t oIdx = (static_cast<size_t>(y) * width + x) * 4;
            size_t rIdx = (static_cast<size_t>(ry) * width + x) * 4;
            for (int c = 0; c < 3; ++c) { // B, G, R
                double diff = static_cast<double>(orig[oIdx + c]) - static_cast<double>(recon[rIdx + c]);
                mse += diff * diff;
                count++;
            }
        }
    }
    mse /= static_cast<double>(count);
    if (mse <= 1e-10) return 999.0; // effectively lossless / infinity
    return 10.0 * std::log10((maxVal * maxVal) / mse);
}

inline double ComputePSNR_YUY2(const uint8_t* orig, const uint8_t* recon, int width, int height, bool lumaOnly = true, double maxVal = 255.0) {
    double mse = 0.0;
    size_t count = 0;
    size_t totalBytes = static_cast<size_t>(width) * height * 2;
    for (size_t i = 0; i < totalBytes; i += 2) {
        // YUY2 layout: Y0 at offset 0, Y1 at offset 2 (all even bytes are Luma)
        double diff = static_cast<double>(orig[i]) - static_cast<double>(recon[i]);
        mse += diff * diff;
        count++;
        if (!lumaOnly) {
            double cdiff = static_cast<double>(orig[i + 1]) - static_cast<double>(recon[i + 1]);
            mse += cdiff * cdiff;
            count++;
        }
    }
    mse /= static_cast<double>(count);
    if (mse <= 1e-10) return 999.0;
    return 10.0 * std::log10((maxVal * maxVal) / mse);
}

inline double ComputePSNR(const uint8_t* orig, const uint8_t* recon, size_t numPixels, double maxVal = 255.0) {
    double mse = 0.0;
    for (size_t i = 0; i < numPixels; ++i) {
        double diff = static_cast<double>(orig[i]) - static_cast<double>(recon[i]);
        mse += diff * diff;
    }
    mse /= static_cast<double>(numPixels);
    if (mse <= 1e-10) return 999.0;
    return 10.0 * std::log10((maxVal * maxVal) / mse);
}

} // namespace HyperCineTest

#endif // HYPERCINE_TEST_VECTORS_H
