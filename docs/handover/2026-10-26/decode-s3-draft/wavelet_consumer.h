// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include "decode_plane_contract.h"
#include "decode_spatial_scalar.h"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hypercine::detail {

// Result of a bounded inverse wavelet consumer pass.
enum class ConsumerResult : int {
    Complete = 0,       // all bands processed successfully
    Canceled = 1,       // cancellation requested mid-process
    FirstError = 2,     // an input band failed validation before processing
    InsufficientBudget = 3,   // remaining budget < needed
};

// One pass of the typed wavelet consumer: validates all four bands against
// identity constraints, checks buffer budgets, forwards to InverseWavelet5To4
// and enforces the rule that *no output data is consumed* after cancellation or
// first error.
class BoundedWaveletConsumer {
public:
    // `budget` must outlive this object; a null budget means no limit checks.
    explicit BoundedWaveletConsumer(BufferBudget* budget = nullptr)
        : budget_(budget) {}

    // Run the consumer for exactly four bands [0..3].  Returns Complete on
    // success, or one of the error codes above.
    ConsumerResult consume(const std::array<WaveletBandInput, 4>& bands,
                           MutableSignedPlaneView output,
                           Generation gen, uint32_t channel) {
        uint64_t gv = (static_cast<uint64_t>(gen.session) << 48) |
                       (static_cast<uint64_t>(gen.slot) << 24) |
                        static_cast<uint64_t>(gen.value);

        // --- Phase 1: validation (no side-effects) --------------------------
        auto err = validate_bands(bands);
        if (!err.empty()) {
            return ConsumerResult::FirstError;
        }

        // Joint identity check.
        try { BandMapper::validate_all(bands); } catch (...) {
            return ConsumerResult::FirstError;
        }

        // --- Phase 2: budget --------------------------------------------------
        if (budget_) {
            auto needed = compute_buffer_length(output.pitch, output.height);
            try {
                std::string lbl = "wavelet output band";
                budget_->allocate(lbl, needed);
                owned_bytes_ += needed;   // track for deallocation on success
            } catch (const BufferBudgetError&) {
                return ConsumerResult::InsufficientBudget;
            } catch (const std::overflow_error&) {
                return ConsumerResult::InsufficientBudget;
            }
        }

        // --- Phase 3: forward to scalar primitive -----------------------------
        const auto& first = bands[0];
        if (!first.generation || !gv)
            return ConsumerResult::FirstError;

        WaveletBandInput verified{bands[0]};
        verified.generation = gv;
        std::array<WaveletBandInput, 4> v{bands};
        for (auto& b : v) {
            if (b.band != static_cast<uint32_t>(&b - v.data()))
                return ConsumerResult::FirstError;
            b.generation = gv;
            b.channel = channel;
            b.wavelet = 5;
        }

        auto scalar_bands = BandMapper::to_scalar_view(v);

        MutableSignedPlaneView out = output;
        out.generation  = gv;
        out.channel     = channel;
        out.wavelet     = 4;   // wavelet5→4 produces scale-0 (wavelet level 4)
        out.band        = 0;

        auto result = InverseWavelet5To4(scalar_bands, out, [this]() -> bool {
            if (!canceled_.load(std::memory_order_relaxed)) return false;
            // Clean up budget on cancel before returning.
            if (budget_ && owned_bytes_) {
                auto released = std::exchange(owned_bytes_, 0);
                budget_->deallocate(released);
            }
            return true;
        });

        if (result == SpatialStageResult::Canceled) {
            // Budget already cleaned up inside the cancel callback above.
            return ConsumerResult::Canceled;
        }
        // Complete: keep owned_bytes_ for later deallocation if needed.
        return ConsumerResult::Complete;
    }

    // Expose a cancellation seam (test-only).
    void request_cancellation() { canceled_.store(true, std::memory_order_release); }
    void reset_cancellation()  { canceled_.store(false, std::memory_order_release); }

private:
    [[nodiscard]] std::string validate_bands(const std::array<WaveletBandInput, 4>& bands) const {
        // Null band pointers — early guard.
        for (std::uint32_t b = 0; b < 4; ++b)
            if (!bands[b].plane.data) return "band[" + std::to_string(b) + "] data null";

        // Overflow check: pitch * height must fit in size_t without overflow.
        for (std::uint32_t b = 0; b < 4; ++b) {
            try { product(bands[b].plane.pitch, bands[b].plane.height); }
            catch (...) { return "band[" + std::to_string(b) + "] size overflow"; }
        }
        return "";
    }

    BufferBudget*     budget_{nullptr};
    std::atomic<bool> canceled_{false};
    std::size_t       owned_bytes_{0}; // bytes allocated against budget during this consume()
};

// ---------------------------------------------------------------------------
// Typed plane storage — aligned allocation with automatic deallocation.
// ---------------------------------------------------------------------------

class TypedPlane {
public:
    TypedPlane(std::uint32_t width, std::uint32_t height) {
        if (!width || !height) throw PlaneContractError("plane dimensions must be non-zero");
        width_  = width;
        height_ = height;
        pitch_  = compute_pitch(width);
        buf_len_ = compute_buffer_length(pitch_, height);

        std::int16_t* p{nullptr};
        if (posix_memalign(reinterpret_cast<void**>(&p), 32, buf_len_) != 0)
            throw std::bad_alloc{};
        data_ = p;
    }

    TypedPlane(TypedPlane&& o) noexcept
        : data_(std::exchange(o.data_, nullptr)), width_(o.width_), height_(o.height_),
          pitch_(o.pitch_), buf_len_(o.buf_len_) {}

    TypedPlane& operator=(TypedPlane&& o) noexcept {
        if (this != &o) { ::operator delete(data_, 32); data_ = std::exchange(o.data_, nullptr);
            width_ = o.width_; height_ = o.height_; pitch_ = o.pitch_; buf_len_ = o.buf_len_; }
        return *this;
    }

    ~TypedPlane() { if (data_) ::operator delete(data_, 32); }
    TypedPlane(const TypedPlane&) = delete; TypedPlane& operator=(const TypedPlane&) = delete;

    [[nodiscard]] MutableSignedPlaneView view()      noexcept { return {data_, buf_len_, pitch_, width_, height_}; }
    [[nodiscard]] SignedPlane     view() const       noexcept { return {data_, buf_len_, pitch_, width_, height_}; }
    [[nodiscard]] std::size_t buffer_length() const  noexcept { return buf_len_; }
    [[nodiscard]] std::size_t pitch_value()   const  noexcept { return pitch_; }

private:
    std::int16_t* data_{nullptr};
    std::uint32_t width_{0}, height_{0};
    std::size_t   pitch_{0}, buf_len_{0};
};

} // namespace hypercine::detail
