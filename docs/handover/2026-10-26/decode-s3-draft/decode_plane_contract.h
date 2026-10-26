// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace hypercine::detail {

// Forward declarations for downstream consumers (full defs in wavelet_consumer.h)
class TypedPlane;

// ---------------------------------------------------------------------------
// PlaneContractError — thrown by validate_all / validate_identity
// ---------------------------------------------------------------------------

class PlaneContractError : public std::invalid_argument {
public:
    explicit PlaneContractError(std::string msg) : std::invalid_argument(std::move(msg)) {}
};

// ---------------------------------------------------------------------------
// Signed 16-bit plane describing one wavelet band (LL, LH, HL or HH).
// Must appear before any function that uses it.
// ---------------------------------------------------------------------------

struct SignedPlane {
    std::int16_t* data{nullptr};       // non-null, 32-byte aligned
    std::uint32_t width{0}, height{0}; // coefficient dimensions
    std::size_t   pitch{0};             // bytes per row (includes padding)
    std::size_t   buffer_length{0};     // total allocated bytes for this plane

    [[nodiscard]] std::string validate() const noexcept {
        if (!data) return "plane data is null";
        if (reinterpret_cast<std::uintptr_t>(data) % 32 != 0)
            return "plane data not 32-byte aligned";
        if (!width || width > 960) return "plane width out of range";
        if (width % 8)   return "plane width not a multiple of 8";
        if (!height || height > 270) return "plane height out of range";
        const auto row_bytes = std::size_t(width) * sizeof(std::int16_t);
        if (pitch < row_bytes) return "pitch smaller than packed row size";
        if (pitch % 32 != 0)   return "pitch not aligned to 32";
        if (!buffer_length)    return "plane buffer length is zero";
        // Compute required size — product() defined below.
        std::size_t computed = pitch;
        if (height > 0 && computed > std::numeric_limits<std::size_t>::max() / height)
            return "plane buffer length too small";
        computed *= height;
        if (buffer_length < computed)
            return "plane buffer length too small";
        return "";
    }

    static constexpr std::size_t max_buffer_bytes = 2 * 1024 * 1024; // 2 MiB
};

// ---------------------------------------------------------------------------
// Overflow-checked size computation — after SignedPlane (which uses inline math)
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::size_t product(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
        throw std::overflow_error("plane size overflow");
    return a * b;
}

static_assert(std::is_same_v<decltype(product(1, 1)), std::size_t>);

[[nodiscard]] inline std::size_t compute_pitch(std::uint32_t width,
                                               std::size_t align = 32) {
    const auto packed = std::size_t(width) * sizeof(std::int16_t);
    return (packed + align - 1) / align * align;
}

[[nodiscard]] inline std::size_t compute_buffer_length(std::size_t pitch,
                                                       std::uint32_t height) {
    return product(pitch, height);
}

// ---------------------------------------------------------------------------
// BufferBudget — per-task and per-generation tracking
// ---------------------------------------------------------------------------

class BufferBudgetError : public PlaneContractError {
public:
    explicit BufferBudgetError(std::string msg) : PlaneContractError(std::move(msg)) {}
};

class BufferBudget {
public:
    explicit BufferBudget(std::uint64_t session, std::uint64_t generation_limit,
                          std::size_t total_limit_bytes = 0)
        : session_(session), limit_per_task_(generation_limit > 0 ? generation_limit : 0),
          limit_total_(total_limit_bytes > 0 ? total_limit_bytes : 0) {}

    void allocate(std::string label, std::size_t needed) {
        if (label.empty()) throw BufferBudgetError("budget label empty");
        overflow_check(needed);
        if (limit_per_task_ > 0 && needed > limit_per_task_)
            throw BufferBudgetError("per-task budget exceeded: " + label);
        if (limit_total_ > 0) {
            auto new_total = used_total_ + needed;
            overflow_check(new_total);
            if (new_total > limit_total_)
                throw BufferBudgetError("total budget exceeded: " + label);
        }
        used_total_ += needed;
    }

    void deallocate(std::size_t used) {
        if (used > used_total_)
            throw std::logic_error("deallocate more than used");
        used_total_ -= used;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return limit_total_ == 0 ? std::size_t(-1) : limit_total_ - used_total_;
    }
    [[nodiscard]] std::uint64_t session() const noexcept { return session_; }
    [[nodiscard]] std::size_t total_allocated() const noexcept { return used_total_; }

private:
    void overflow_check(std::size_t val) const {
        if (limit_total_ > 0 && val > limit_total_)
            throw BufferBudgetError("budget overflow in check");
    }
    std::uint64_t session_;
    const std::size_t limit_per_task_;
    const std::size_t limit_total_;
    std::size_t used_total_{0};
};

// ---------------------------------------------------------------------------
// Four-band input wrapper — identity checking before forwarding
// ---------------------------------------------------------------------------

struct WaveletBandInput {
    SignedPlane plane;
    std::uint64_t generation{0};   // session/slot/value combined
    std::uint32_t channel{0};      // 0..2
    std::uint32_t wavelet{5};      // filter length (fixed at 5)
    std::uint32_t band{0};         // 0=LL, 1=LH, 2=HL, 3=HH

    void validate_identity(const WaveletBandInput& first, std::uint32_t expected_band) const {
        if (band != expected_band)
            throw PlaneContractError("band mismatch: expected " + std::to_string(expected_band));
        if (wavelet != first.wavelet)
            throw PlaneContractError("wavelet differs across bands");
        if (channel != first.channel)
            throw PlaneContractError("channel differs across bands");
        if (generation != first.generation)
            throw PlaneContractError("generation differs across bands");
        if (plane.width != first.plane.width || plane.height != first.plane.height)
            throw PlaneContractError("dimensions differ across bands");
    }

    [[nodiscard]] std::string validate() const noexcept {
        if (!generation) return "band generation is zero";
        if (wavelet != 5) return "wavelet must be 5";
        if (channel > 2)  return "channel out of range";
        auto err = plane.validate();
        if (!err.empty()) return "plane: " + err;
        return "";
    }
};

// Forward-declare downstream types for BandMapper::to_scalar_view() signature.
struct SignedPlaneView;
std::array<SignedPlaneView, 4> plane_to_view(const std::array<SignedPlane, 4>&);

class BandMapper {
public:
    static void validate_all(const std::array<WaveletBandInput, 4>& bands) {
        if (bands[0].plane.data == nullptr ||
            bands[1].plane.data == nullptr ||
            bands[2].plane.data == nullptr ||
            bands[3].plane.data == nullptr) {
            throw PlaneContractError("one or more band data pointers are null");
        }
        for (std::uint32_t b = 0; b < 4; ++b) {
            auto err = bands[b].validate();
            if (!err.empty()) throw PlaneContractError(err);
        }
        for (std::uint32_t b = 1; b < 4; ++b)
            bands[b].validate_identity(bands[0], b);
    }

    // Declaration only — def in wavelet_consumer.h after all types resolved.
    static std::array<SignedPlaneView, 4> to_scalar_view(
        const std::array<WaveletBandInput, 4>& bands);
};

} // namespace hypercine::detail
