// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include "encoder_session_config.h"
#include "filmscan3_backend.h"

namespace hypercine { namespace standalone { namespace detail {

// Internal only: public constructors retain their existing compatibility policy.
// Presence is not qualification. The current capability table admits scalar CPU
// only; GPU factories must never be reached by overriding that table here.
enum class BackendSelectionStatus { Selected, CapabilityRefused, InitializationFailed };
enum class ScalarFallbackReason { None, BackendNotPresent, BackendNotQualified };
struct BackendSelection {
    BackendSelectionStatus status = BackendSelectionStatus::CapabilityRefused;
    ScalarFallbackReason fallback = ScalarFallbackReason::None;
    EncoderCapabilityResult requested{};
    EncoderCapabilityResult effective{};
    std::shared_ptr<Frontend> frontend;
    explicit operator bool() const noexcept {
        return status == BackendSelectionStatus::Selected && frontend != nullptr;
    }
};

// Per-call injection only, never global mutable dispatch. Invoked only AFTER
// capability permission. Production uses the existing shared CPU frontend.
using ScalarFrontendFactory = std::shared_ptr<Frontend>(*)(FilmScan3Geometry, WaveletIsa);
BackendSelection SelectBackend(const EncoderSessionConfig&, const EncoderDevice&);
BackendSelection SelectBackend(const EncoderSessionConfig&, const EncoderDevice&, ScalarFrontendFactory);

class BackendSelectionError final : public std::runtime_error {
public:
    explicit BackendSelectionError(const BackendSelection& result);
    BackendSelectionStatus status() const noexcept { return status_; }
    CapabilityReason reason() const noexcept { return reason_; }
    const EncoderCapabilityResult& capability() const noexcept { return capability_; }
private:
    BackendSelectionStatus status_;
    CapabilityReason reason_;
    EncoderCapabilityResult capability_;
};

// Internal entry point that actually consumes permission before constructing a
// frontend. Owns a config snapshot and the existing FilmScan3Encoder, not another
// writer. Bound storage and diagnostics are applied for every Encode call.
class EncoderSession final {
public:
    EncoderSession(const EncoderSessionConfig&, const EncoderDevice&);
    FilmScan3Frame Encode(const uint8_t*, std::size_t bytes, std::size_t stride);
    void Reset() { encoder_.Reset(); }
    const BackendSelection& selection() const noexcept { return selection_; }
    const EncoderSessionConfig& config() const noexcept { return config_; }
    FilmScan3Geometry geometry() const noexcept { return encoder_.geometry(); }
    WaveletIsa wavelet_isa() const noexcept { return encoder_.wavelet_isa(); }
private:
    const EncoderSessionConfig config_;
    const BackendSelection selection_;
    FilmScan3Encoder encoder_;
};

}}}
