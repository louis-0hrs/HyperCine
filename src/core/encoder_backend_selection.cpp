// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "encoder_backend_selection.h"

namespace hypercine { namespace standalone { namespace detail {
namespace {
bool BackendOnlyRefusal(const EncoderCapabilityResult& capability) {
    const auto& flags = capability.reasons;
    // Backend fallback is not permission to repair a format, profile, resource,
    // or reference failure. A different CPU budget is checked again below.
    return !flags.format_mathematically_invalid && !flags.valid_but_not_implemented &&
           !flags.memory_budget_exceeded && capability.primary != CapabilityReason::ReferenceMissing &&
           (capability.primary == CapabilityReason::BackendNotPresent ||
            capability.primary == CapabilityReason::BackendNotQualifiedForGeometry);
}
BackendSelection RequireSelection(BackendSelection selection) {
    if (!selection) throw BackendSelectionError(selection);
    return selection;
}
}

BackendSelection SelectBackend(const EncoderSessionConfig& config, const EncoderDevice& device) {
    return SelectBackend(config, device, BackendFactories{});
}

BackendSelection SelectBackend(const EncoderSessionConfig& config, const EncoderDevice& device,
                               ScalarFrontendFactory factory) {
    auto factories = BackendFactories{};
    factories.scalar = factory;
    return SelectBackend(config, device, factories);
}

BackendSelection SelectBackend(const EncoderSessionConfig& config, const EncoderDevice& device,
                               const BackendFactories& factories) {
    BackendSelection result;
    result.requested = CheckEncoderCapability(config, device);
    result.effective = result.requested;
    if (!result.effective.allocation_permitted && config.allow_cpu_scalar_fallback &&
        config.backend != SessionBackend::CpuScalar && BackendOnlyRefusal(result.requested)) {
        auto scalar = config;
        scalar.backend = SessionBackend::CpuScalar;
        scalar.allow_cpu_scalar_fallback = false;
        result.effective = CheckEncoderCapability(scalar, device);
        result.fallback = result.requested.primary == CapabilityReason::BackendNotPresent
            ? ScalarFallbackReason::BackendNotPresent : ScalarFallbackReason::BackendNotQualified;
    }
    if (!result.effective.allocation_permitted) return result;
    // The capability query may already have selected the explicitly permitted
    // scalar fallback for an absent backend. Keep that origin observable.
    if (result.requested.fell_back_to_cpu_scalar)
        result.fallback = ScalarFallbackReason::BackendNotPresent;
    // Do not acquire a new product backend implicitly when a future capability
    // revision changes. It needs its own reviewed factory/qualification wiring.
    if (result.effective.effective_backend != SessionBackend::CpuScalar &&
        result.effective.effective_backend != SessionBackend::Metal &&
        result.effective.effective_backend != SessionBackend::Cuda) {
        result.effective.allocation_permitted = false;
        result.effective.primary = CapabilityReason::ValidButNotImplemented;
        result.effective.reasons.valid_but_not_implemented = true;
        result.effective.detail = "backend has no reviewed session factory";
        return result;
    }
    const FilmScan3Geometry geometry{static_cast<int>(config.width), static_cast<int>(config.height),
                                    static_cast<std::size_t>(config.max_sample_bytes)};
    try {
        // Explicit scalar policy is session-bound; never Auto/process-global.
        const auto backend = result.effective.effective_backend;
        const bool metal = backend == SessionBackend::Metal;
        const bool cuda = backend == SessionBackend::Cuda;
        if (metal || cuda) {
            const auto factory = metal ? factories.metal : factories.cuda;
            if (factory) result.frontend = factory(geometry);
        } else if (factories.scalar) {
            result.frontend = factories.scalar(geometry, WaveletIsa::Scalar);
        }
        // GPU backends are not a CPU ISA: only the scalar session pins Scalar and
        // only Metal claims Auto. Anything else would let one GPU frontend pass
        // for another or for the scalar reference.
        const auto expected_backend = metal ? FrontendBackend::Metal
            : (cuda ? FrontendBackend::Cuda : FrontendBackend::Cpu);
        const auto expected_isa = metal || cuda ? WaveletIsa::Auto : WaveletIsa::Scalar;
        if (!result.frontend || result.frontend->Geometry().width != geometry.width ||
            result.frontend->Geometry().height != geometry.height ||
            result.frontend->Geometry().sample_capacity_bytes != geometry.sample_capacity_bytes ||
            result.frontend->Backend() != expected_backend ||
            result.frontend->Isa() != expected_isa) {
            result.frontend.reset();
            result.status = BackendSelectionStatus::InitializationFailed;
            return result;
        }
    } catch (const std::runtime_error&) {
        // Availability/initialization failures are typed; bad_alloc and logic
        // errors propagate rather than being hidden as an unavailable backend.
        result.status = BackendSelectionStatus::InitializationFailed;
        return result;
    }
    result.status = BackendSelectionStatus::Selected;
    return result;
}

BackendSelectionError::BackendSelectionError(const BackendSelection& result)
    : std::runtime_error(result.status == BackendSelectionStatus::InitializationFailed
          ? "encoder backend initialization failed" : result.effective.detail),
      status_(result.status), reason_(result.effective.primary), capability_(result.effective) {}

EncoderSession::EncoderSession(const EncoderSessionConfig& config, const EncoderDevice& device)
    : config_(config), selection_(RequireSelection(SelectBackend(config_, device))) {
    FrontendAccess::Use(encoder_, selection_.frontend);
    encoder_.SetCoefficientHashing(config_.diagnostic == DiagnosticMode::CoefficientFnv64);
}

FilmScan3Frame EncoderSession::Encode(const uint8_t* data, std::size_t bytes, std::size_t stride) {
    if (config_.input_stride_bytes != 0 &&
        (stride != config_.input_stride_bytes || bytes < config_.input_bytes))
        throw std::invalid_argument("input does not match the session storage contract");
    // Still validate the actual pointer, extent and alignment on every frame;
    // a successful preallocation query is not proof of caller-owned storage.
    ValidateFilmScan3Input(data, bytes, geometry().width, geometry().height, stride);
    return encoder_.Encode(data, bytes, stride);
}

}}}
