// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Issue #259, implementation-order step 3: quality profiles inside the internal
// capability/session contract. This suite pins the fail-closed profile x backend
// admission matrix, the typed refusal reason for a legal profile that has no
// packet evidence on the effective backend, and the fact that the authorized
// profile actually reaches the single writer instead of being silently dropped.
//
// It is not a per-profile original-SDK qualification claim. The packet numbers
// asserted here are the shared-writer consequences of the profile's own tables.
#include "../src/core/encoder_backend_selection.h"
#include "hypercine/quality_state.h"
#include "reference_frame_generator.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hypercine;
using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;

namespace {

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

constexpr int kWidth = 128;
constexpr int kHeight = 24;

const std::array<EncodingQuality, 6> kProfiles = {
    EncodingQuality::Low, EncodingQuality::Medium, EncodingQuality::High,
    EncodingQuality::FilmScan1, EncodingQuality::FilmScan2, EncodingQuality::FilmScan3};

const char* ProfileLabel(EncodingQuality quality) { return EncodingQualityName(quality); }

// Frozen per-profile quant tables for 128x24 BGRA8, frame 0, starting limiter 4.
// These are literal constants on purpose: comparing the writer against the same
// shared calculator it uses would let one calculator bug pass on both sides.
// They are pinned from this revision and must only change with a reviewed,
// source-backed reason (see docs/ENCODER_PROFILE_SESSIONS.md).
struct FrozenProfile {
    EncodingQuality quality;
    int limiter;
    std::array<unsigned, 36> quant;
};
const std::array<FrozenProfile, 6> kFrozenProfiles = {{
    {EncodingQuality::Low, 0,
     {1, 128, 128, 192, 1, 16, 16, 12, 1, 64, 64, 48, 1, 128, 128, 192, 1, 16, 16, 12,
      1, 64, 64, 48, 1, 128, 128, 192, 1, 16, 16, 12, 1, 64, 64, 48}},
    {EncodingQuality::Medium, 0,
     {1, 64, 64, 96, 1, 12, 12, 8, 1, 48, 48, 32, 1, 64, 64, 128, 1, 12, 12, 8,
      1, 48, 48, 32, 1, 64, 64, 128, 1, 12, 12, 8, 1, 48, 48, 32}},
    {EncodingQuality::High, 0,
     {1, 32, 32, 48, 1, 8, 8, 6, 1, 32, 32, 24, 1, 32, 32, 64, 1, 12, 12, 8,
      1, 48, 48, 32, 1, 32, 32, 64, 1, 12, 12, 8, 1, 48, 48, 32}},
    {EncodingQuality::FilmScan1, 0,
     {1, 24, 24, 36, 1, 6, 6, 3, 1, 24, 24, 12, 1, 24, 24, 48, 1, 6, 6, 3,
      1, 24, 24, 12, 1, 24, 24, 48, 1, 6, 6, 3, 1, 24, 24, 12}},
    {EncodingQuality::FilmScan2, 8,
     {1, 8, 8, 12, 1, 4, 4, 2, 1, 16, 16, 8, 1, 8, 8, 16, 1, 4, 4, 2,
      1, 16, 16, 8, 1, 8, 8, 16, 1, 4, 4, 2, 1, 16, 16, 8}},
    {EncodingQuality::FilmScan3, 4,
     {1, 6, 6, 9, 1, 4, 4, 2, 1, 16, 16, 8, 1, 6, 6, 12, 1, 4, 4, 2,
      1, 16, 16, 8, 1, 6, 6, 12, 1, 4, 4, 2, 1, 16, 16, 8}},
}};

const FrozenProfile& Frozen(EncodingQuality quality) {
    for (const auto& item : kFrozenProfiles)
        if (item.quality == quality) return item;
    throw std::runtime_error("no frozen profile record");
}

EncoderSessionConfig Film(EncodingQuality quality) {
    EncoderSessionConfig config;
    config.codec = CodecFamily::FilmScan3;
    config.quality = quality;
    config.pixel_format = SourcePixelFormat::Bgra8;
    config.width = kWidth;
    config.height = kHeight;
    config.frame_rate = {30, 1};
    config.gop = GopMode::IntraGop1;
    config.progressive = true;
    config.backend = SessionBackend::CpuScalar;
    config.diagnostic = DiagnosticMode::CoefficientFnv64;
    config.input_stride_bytes = static_cast<std::uint64_t>(kWidth) * 4u;
    config.input_bytes = config.input_stride_bytes * static_cast<std::uint64_t>(kHeight);
    return config;
}

EncoderDevice Host() {
    EncoderDevice device;  // CPU scalar present, every GPU absent
    return device;
}

EncoderDevice MetalDevice() {
    EncoderDevice device;
    device.metal_available = true;
    device.metal_max_buffer_length_bytes = 1ull << 30;
    device.metal_recommended_working_set_bytes = 1ull << 30;
    return device;
}

EncoderDevice CudaDevice() {
    EncoderDevice device;
    device.cuda_available = true;
    device.cuda_hardware_observed = true;
    device.cuda_total_global_memory_bytes = 1ull << 34;
    return device;
}

// Factory seams: a refusal must never reach one of these.
unsigned scalar_calls = 0, metal_calls = 0, cuda_calls = 0;

std::shared_ptr<Frontend> CountingScalar(FilmScan3Geometry g, WaveletIsa isa) {
    ++scalar_calls;
    return CpuFrontend(g, isa);
}
std::shared_ptr<Frontend> CountingMetal(FilmScan3Geometry g) {
    ++metal_calls;
    return MetalFrontend(g);
}
std::shared_ptr<Frontend> CountingCuda(FilmScan3Geometry g) {
    ++cuda_calls;
    return CudaSessionFrontend(g);
}

void ResetCounts() { scalar_calls = metal_calls = cuda_calls = 0; }

BackendFactories Factories() {
    BackendFactories factories;
    factories.scalar = CountingScalar;
    factories.metal = CountingMetal;
    factories.cuda = CountingCuda;
    return factories;
}

// Walks the emitted packet the way the writer lays it out and returns tag -80,
// the SDK quality enum. Payload bytes cannot be mistaken for tags because
// negative chunk tags are skipped by their declared word count.
unsigned HeaderQualityStamp(const std::vector<std::uint8_t>& sample) {
    std::size_t at = 0;
    while (at + 4 <= sample.size()) {
        const int tag = static_cast<int16_t>(sample[at] << 8 | sample[at + 1]);
        const unsigned value = unsigned(sample[at + 2]) << 8 | sample[at + 3];
        if (tag == -80) return value;
        if (tag < 0) {
            const int code = -tag;
            const int group = code & 0xff00;
            if (group == 0x2000) { at += 4 + std::size_t(((code & 255) << 16) | value) * 4; continue; }
            if (group == 0x4000) { at += 4 + std::size_t(value) * 4; continue; }
            if (group == 0x2100 || group == 0x2200) { at += 4; continue; }
        }
        at += 4;
    }
    throw std::runtime_error("emitted packet carries no quality stamp");
}

// Every admitted profile must be reachable on CPU scalar with the exact tables
// the shared calculator produces for that profile, and must stamp its own enum.
void CpuAdmitsAllSix() {
    for (const auto quality : kProfiles) {
        const auto config = Film(quality);
        const auto capability = CheckEncoderCapability(config, Host());
        Require(capability.reasons.experimentally_supported,
                std::string("CPU scalar must admit profile ") + ProfileLabel(quality));
        Require(capability.allocation_permitted, std::string("CPU profile not permitted: ") + ProfileLabel(quality));
        Require(capability.profile_admitted, std::string("profile_admitted false on CPU: ") + ProfileLabel(quality));
        Require(capability.effective_backend == SessionBackend::CpuScalar, "CPU admission changed backend");
        Require(!capability.reasons.profile_not_qualified_for_backend, "CPU profile wrongly marked unqualified");
        Require(!capability.reference_qualified, "CPU profile claimed a reference");
        Require(capability.primary == CapabilityReason::ExperimentallySupported,
                std::string("CPU profile primary reason: ") + ProfileLabel(quality));

        ResetCounts();
        const auto selection = SelectBackend(config, Host(), Factories());
        Require(static_cast<bool>(selection), std::string("CPU selection failed: ") + ProfileLabel(quality));
        Require(scalar_calls == 1 && metal_calls == 0 && cuda_calls == 0, "CPU selection used another factory");

        EncoderSession session(config, Host());
        Require(session.config().quality == quality, "session dropped the requested profile");
        Require(session.selection().effective.profile_admitted, "session admission lost");

        std::vector<std::uint8_t> input(static_cast<std::size_t>(config.input_bytes), 0);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x) {
                auto* pixel = input.data() + static_cast<std::size_t>(y) * config.input_stride_bytes + 4u * x;
                pixel[0] = static_cast<std::uint8_t>(x + 7);
                pixel[1] = static_cast<std::uint8_t>(y + 11);
                pixel[2] = static_cast<std::uint8_t>((x ^ y) + 3);
                pixel[3] = 255;
            }
        const auto frame = session.Encode(input.data(), input.size(), config.input_stride_bytes);

        // The profile is not cosmetic: it must be visible in the packet header.
        Require(HeaderQualityStamp(frame.sample) == static_cast<unsigned>(static_cast<int>(quality)),
                std::string("packet does not carry the session profile: ") + ProfileLabel(quality));

        // The tables come from the shared calculator's frozen per-profile result,
        // not from the FilmScan3 table relabelled. The comparison target is a
        // literal constant, so a bug inside the shared calculator cannot satisfy
        // both sides of the check.
        const auto& frozen = Frozen(quality);
        for (unsigned i = 0; i < 36; ++i) {
            Require(frame.tables.quant[i] == frozen.quant[i],
                    std::string("writer quant table diverges from the frozen profile record at slot ") +
                        std::to_string(i) + " for " + ProfileLabel(quality));
        }
        Require(frame.state.fs_rate_limiter == frozen.limiter,
                std::string("unexpected starting limiter for ") + ProfileLabel(quality));
        // Scale is a shared 3x3x4 layout independent of the profile, but the
        // writer must still emit exactly it rather than a profile-specific one.
        Require(frame.tables.scale == ComputeFilmScan3Tables(frozen.limiter).scale,
                std::string("writer scale table diverges for ") + ProfileLabel(quality));
        std::cout << "PASS cpu-profile " << ProfileLabel(quality)
                  << " stamp=" << HeaderQualityStamp(frame.sample)
                  << " limiter=" << frame.state.fs_rate_limiter
                  << " bytes=" << frame.sample.size() << '\n';
    }
    // The six profiles must not collapse into one table set.
    std::array<std::array<unsigned, 36>, 6> first_quant{};
    for (std::size_t p = 0; p < kProfiles.size(); ++p)
        first_quant[p] = Frozen(kProfiles[p]).quant;
    Require(first_quant[0] != first_quant[5], "Low and FilmScan3 produced identical tables");
    std::cout << "PASS six distinct profile table sets\n";
}

// A legal profile without packet evidence on that backend is refused with the
// dedicated reason, before any factory is called, and neither presence nor an
// explicit scalar fallback repairs it into a GPU claim.
//
// Admission of FilmScan3 on a GPU backend is asserted at capability level: this
// CPU-only build has no Metal/CUDA frontend compiled in, so a factory-level
// success here would only prove a stub. The native GPU session gates live in
// test_encoder_metal_session / test_encoder_cuda_session.
void GpuAdmitsFilmScan3Only() {
    for (const auto quality : kProfiles) {
        const bool filmscan3 = quality == EncodingQuality::FilmScan3;
        for (const auto backend : {SessionBackend::Metal, SessionBackend::Cuda}) {
            auto config = Film(quality);
            config.backend = backend;
            const auto device = backend == SessionBackend::Metal ? MetalDevice() : CudaDevice();
            const auto capability = CheckEncoderCapability(config, device);
            if (filmscan3) {
                Require(capability.allocation_permitted,
                        std::string("FilmScan3 must be admitted on ") +
                            (backend == SessionBackend::Metal ? "Metal" : "CUDA"));
                Require(capability.profile_admitted && !capability.reasons.profile_not_qualified_for_backend,
                        "FilmScan3 was refused on the profile axis by a GPU backend");
                Require(capability.effective_backend == backend, "GPU admission changed effective backend");
                continue;
            }
            Require(!capability.allocation_permitted,
                    std::string("unqualified profile admitted on a GPU backend: ") + ProfileLabel(quality));
            Require(capability.primary == CapabilityReason::ProfileNotQualifiedForBackend,
                    std::string("wrong refusal reason for ") + ProfileLabel(quality));
            Require(capability.reasons.profile_not_qualified_for_backend && !capability.profile_admitted,
                    "profile axis not set on refusal");

            ResetCounts();
            const auto selection = SelectBackend(config, device, Factories());
            Require(!selection && !selection.frontend,
                    std::string("unqualified profile produced a selection: ") + ProfileLabel(quality));
            Require(selection.status == BackendSelectionStatus::CapabilityRefused,
                    std::string("profile refusal lost typed status: ") + ProfileLabel(quality));
            Require(selection.effective.primary == CapabilityReason::ProfileNotQualifiedForBackend,
                    "selection lost the profile refusal reason");
            Require(scalar_calls == 0 && metal_calls == 0 && cuda_calls == 0,
                    "profile refusal reached a factory");
            // Fallback must not convert a profile refusal into a backend success.
            config.allow_cpu_scalar_fallback = true;
            ResetCounts();
            const auto fell_back = SelectBackend(config, device, Factories());
            Require(!fell_back && fell_back.status == BackendSelectionStatus::CapabilityRefused,
                    "profile refusal was repaired by an explicit scalar fallback");
            Require(fell_back.effective.primary == CapabilityReason::ProfileNotQualifiedForBackend,
                    "fallback repair changed the profile refusal reason");
            Require(scalar_calls == 0, "fallback repaired a profile mismatch by allocating a CPU frontend");
            bool threw = false;
            try { EncoderSession refused(config, device); }
            catch (const BackendSelectionError& error) {
                threw = error.status() == BackendSelectionStatus::CapabilityRefused &&
                        error.reason() == CapabilityReason::ProfileNotQualifiedForBackend;
            }
            Require(threw, std::string("session did not enforce the profile refusal: ") + ProfileLabel(quality));
        }
    }
    std::cout << "PASS gpu-backends admit FilmScan3 only; other profiles typed-refused with no factory call\n";
}

// The refusal must be a profile reason, not a geometry or memory reason, and it
// must survive the presence of a complete, otherwise qualified request.
void RefusalIsProfileSpecific() {
    auto config = Film(EncodingQuality::Medium);
    config.backend = SessionBackend::Metal;
    const auto device = MetalDevice();
    const auto result = CheckEncoderCapability(config, device);
    Require(result.reasons.profile_not_qualified_for_backend, "profile axis missing");
    Require(!result.reasons.backend_not_present && !result.reasons.backend_not_qualified_for_geometry &&
            !result.reasons.memory_budget_exceeded && !result.reasons.valid_but_not_implemented &&
            !result.reasons.format_mathematically_invalid,
            "profile refusal was contaminated by another axis");
    Require(result.geometry_validator_passed && result.evidence_record,
            "profile refusal did not keep its independent geometry evidence");
    Require(result.requested_backend_present, "profile refusal lost backend presence");
    Require(!result.allocation_permitted && !result.profile_admitted, "profile refusal permitted allocation");
    // An invalid enum is a format error, not a profile-evidence gap.
    auto invalid = config;
    invalid.quality = static_cast<EncodingQuality>(9);
    const auto format = CheckEncoderCapability(invalid, device);
    Require(format.primary == CapabilityReason::FormatMathematicallyInvalid &&
            format.reasons.format_mathematically_invalid, "invalid enum was not a format refusal");
    Require(!format.reasons.profile_not_qualified_for_backend, "invalid enum reused the profile axis");
    // And it stays a format refusal even with fallback enabled.
    invalid.allow_cpu_scalar_fallback = true;
    Require(CheckEncoderCapability(invalid, device).primary == CapabilityReason::FormatMathematicallyInvalid,
            "fallback repaired an invalid profile enum");
    std::cout << "PASS profile refusal is an independent axis and invalid enums stay format errors\n";
}

// Two concurrent sessions on different profiles must not share rate state or
// tables: this would be the classic "one process-global switch" defect.
void IndependentProfilesConcurrently() {
    auto low = Film(EncodingQuality::Low);
    auto high = Film(EncodingQuality::FilmScan3);
    std::vector<std::uint8_t> input(static_cast<std::size_t>(low.input_bytes), 0);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            auto* pixel = input.data() + static_cast<std::size_t>(y) * low.input_stride_bytes + 4u * x;
            pixel[0] = static_cast<std::uint8_t>(x * 3);
            pixel[1] = static_cast<std::uint8_t>(y * 5);
            pixel[2] = static_cast<std::uint8_t>(x + y);
            pixel[3] = 255;
        }
    // Sequential reference first, then the same work interleaved.
    EncoderSession low_seq(low, Host());
    EncoderSession high_seq(high, Host());
    const auto low_reference = low_seq.Encode(input.data(), input.size(), low.input_stride_bytes);
    const auto high_reference = high_seq.Encode(input.data(), input.size(), high.input_stride_bytes);
    Require(HeaderQualityStamp(low_reference.sample) == 1 && HeaderQualityStamp(high_reference.sample) == 6,
            "sequential profile stamps are wrong");
    Require(low_reference.sample != high_reference.sample, "Low and FilmScan3 emitted identical bytes");

    EncoderSession a(low, Host());
    EncoderSession b(high, Host());
    for (int i = 0; i < 4; ++i) {
        const auto frame_a = a.Encode(input.data(), input.size(), low.input_stride_bytes);
        const auto frame_b = b.Encode(input.data(), input.size(), high.input_stride_bytes);
        Require(HeaderQualityStamp(frame_a.sample) == 1, "interleaved Low session changed profile");
        Require(HeaderQualityStamp(frame_b.sample) == 6, "interleaved FilmScan3 session changed profile");
        Require(a.config().quality == EncodingQuality::Low &&
                b.config().quality == EncodingQuality::FilmScan3, "interleaved session config drifted");
    }
    // Reset keeps the profile, as the shared writer locks it at the first frame.
    a.Reset();
    const auto after_reset = a.Encode(input.data(), input.size(), low.input_stride_bytes);
    Require(HeaderQualityStamp(after_reset.sample) == 1, "Reset changed the session profile");
    Require(a.config().quality == EncodingQuality::Low, "Reset changed the frozen session config");
    std::cout << "PASS two concurrent profiles keep independent state, tables and stamps across Reset\n";
}

// The profile axis must not outrank the axes the contract already ordered before
// it, and a profile refusal must not become fallback-eligible when a different
// backend-only refusal happens to be primary.
void PriorityAndFallbackBoundaries() {
    // 1. A memory refusal still wins over the profile gap, even on a GPU backend
    //    and even though the profile is also unproven there.
    auto metal_medium = Film(EncodingQuality::Medium);
    metal_medium.backend = SessionBackend::Metal;
    metal_medium.gpu_memory_budget_bytes = 1;
    const auto memory = CheckEncoderCapability(metal_medium, MetalDevice());
    Require(memory.primary == CapabilityReason::MemoryBudgetExceeded,
            "profile gap outranked a memory refusal");
    Require(memory.reasons.profile_not_qualified_for_backend,
            "memory refusal lost the independent profile axis");
    Require(!memory.allocation_permitted, "memory+profile case allocated");

    // 2. A demanded original oracle outranks the profile gap: the operator asked
    //    for a reference, so that is what is missing.
    auto oracle = metal_medium;
    oracle.gpu_memory_budget_bytes = ~std::uint64_t{0};
    oracle.require_reference_oracle = true;
    // Bind the input contract too, otherwise the Metal storage axis refuses first
    // and this case would test that axis instead of the priority under review.
    oracle.input_stride_bytes = static_cast<std::uint64_t>(kWidth) * 4u;
    oracle.input_bytes = oracle.input_stride_bytes * static_cast<std::uint64_t>(kHeight);
    const auto reference = CheckEncoderCapability(oracle, MetalDevice());
    Require(reference.primary == CapabilityReason::ReferenceMissing,
            "profile gap outranked a required reference");
    Require(reference.reasons.profile_not_qualified_for_backend,
            "reference refusal lost the independent profile axis");
    Require(!reference.allocation_permitted, "reference+profile case allocated");

    // 3. Fallback eligibility follows the *reason for the refusal*, not the
    //    profile. An unqualified geometry is fallback-eligible, so the request
    //    becomes an ordinary CPU session and the CPU matrix decides the profile.
    //    That is not a repair of a profile refusal: nothing was claimed on the
    //    requested backend, and the CPU path has its own packet evidence.
    auto small_medium = Film(EncodingQuality::Medium);
    small_medium.width = 384;
    small_medium.height = 40;
    small_medium.backend = SessionBackend::Metal;
    small_medium.input_stride_bytes = 384u * 4u;
    small_medium.input_bytes = small_medium.input_stride_bytes * 40u;
    const auto geometry_first = CheckEncoderCapability(small_medium, MetalDevice());
    Require(geometry_first.primary == CapabilityReason::BackendNotQualifiedForGeometry,
            "unqualified geometry lost its own primary reason");
    Require(geometry_first.reasons.profile_not_qualified_for_backend,
            "unqualified geometry hid the profile gap");
    Require(!geometry_first.allocation_permitted, "geometry+profile case allocated on Metal");
    // Without fallback the request stays closed at the requested backend.
    ResetCounts();
    const auto no_fallback = SelectBackend(small_medium, MetalDevice(), Factories());
    Require(!no_fallback && no_fallback.effective.primary == CapabilityReason::BackendNotQualifiedForGeometry,
            "geometry+profile request was admitted without fallback");
    Require(scalar_calls == 0 && metal_calls == 0, "geometry+profile refusal reached a factory");
    // With explicit fallback the geometry is still not admissible anywhere: the
    // CPU evidence list has the same six records, so this stays a typed refusal
    // and never becomes a different-profile session.
    auto opted_in = small_medium;
    opted_in.allow_cpu_scalar_fallback = true;
    ResetCounts();
    const auto repaired = SelectBackend(opted_in, MetalDevice(), Factories());
    Require(!repaired && scalar_calls == 0 && metal_calls == 0,
            "fallback admitted a geometry that no backend has evidence for");
    Require(repaired.requested.primary == CapabilityReason::BackendNotQualifiedForGeometry,
            "fallback lost the original geometry refusal");

    // 3c. The contrast that makes 3b meaningful: an *absent* backend is
    //     fallback-eligible, so the request becomes an ordinary CPU session and
    //     the CPU matrix admits its profile. Same caller opt-in, different reason.
    auto metal_absent_medium = Film(EncodingQuality::Medium);
    metal_absent_medium.backend = SessionBackend::Metal;
    metal_absent_medium.allow_cpu_scalar_fallback = true;
    ResetCounts();
    const auto cpu_session = SelectBackend(metal_absent_medium, Host(), Factories());
    Require(static_cast<bool>(cpu_session) && scalar_calls == 1,
            "absent Metal with explicit fallback did not build the CPU session");
    Require(cpu_session.effective.effective_backend == SessionBackend::CpuScalar &&
            cpu_session.effective.profile_admitted,
            "the fallback CPU session did not admit its own profile");
    // The capability query folds an allowed absent-backend fallback into its own
    // result, so the origin is observable through fallback and the flag, not by
    // expecting a bare BackendNotPresent primary on the requested result.
    Require(cpu_session.fallback == ScalarFallbackReason::BackendNotPresent &&
            cpu_session.requested.fell_back_to_cpu_scalar &&
            cpu_session.requested.reasons.backend_not_present,
            "absent-backend fallback lost its origin");

    // 3b. A profile refusal is NOT fallback-eligible, and that asymmetry is the
    //     tested rule: on a backend that could otherwise run the request, the
    //     profile gap is the only blocker, so fallback must not step around it.
    auto metal_medium_ok = Film(EncodingQuality::Medium);
    metal_medium_ok.backend = SessionBackend::Metal;
    metal_medium_ok.allow_cpu_scalar_fallback = true;
    metal_medium_ok.input_stride_bytes = static_cast<std::uint64_t>(kWidth) * 4u;
    metal_medium_ok.input_bytes = metal_medium_ok.input_stride_bytes * static_cast<std::uint64_t>(kHeight);
    ResetCounts();
    const auto profile_blocked = SelectBackend(metal_medium_ok, MetalDevice(), Factories());
    Require(!profile_blocked && scalar_calls == 0 && metal_calls == 0,
            "fallback stepped around a profile-only refusal");
    Require(profile_blocked.effective.primary == CapabilityReason::ProfileNotQualifiedForBackend,
            "profile-only refusal lost its reason under fallback");

    // 4. Backends with no session at all carry no profile matrix, so FilmScan3
    //    there must not be labelled a profile-evidence gap.
    for (const auto backend : {SessionBackend::CpuSimd, SessionBackend::Vulkan}) {
        auto config = Film(EncodingQuality::FilmScan3);
        config.backend = backend;
        auto device = Host();
        device.cpu_simd_selectable = true;
        device.vulkan_available = true;
        const auto result = CheckEncoderCapability(config, device);
        Require(!result.reasons.profile_not_qualified_for_backend,
                "a backend without a profile matrix blamed the profile for FilmScan3");
        Require(result.primary != CapabilityReason::ProfileNotQualifiedForBackend,
                "a backend without a profile matrix reported the profile reason");
        Require(!result.allocation_permitted, "an unimplemented backend allocated");
    }

    // 5. Documented asymmetry, asserted so a silent change is caught: a request to
    //    a backend that cannot run at all has no profile matrix, so an explicit
    //    scalar fallback is the ordinary CPU session and does admit the profile.
    auto vulkan_medium = Film(EncodingQuality::Medium);
    vulkan_medium.backend = SessionBackend::Vulkan;
    vulkan_medium.allow_cpu_scalar_fallback = true;
    auto device = Host();
    device.vulkan_available = true;
    ResetCounts();
    const auto fell_back = SelectBackend(vulkan_medium, device, Factories());
    Require(static_cast<bool>(fell_back), "explicit fallback from an unimplemented backend was refused");
    Require(fell_back.effective.effective_backend == SessionBackend::CpuScalar,
            "fallback from an unimplemented backend lost its CPU origin");
    Require(fell_back.requested.effective_backend == SessionBackend::Vulkan,
            "the original backend was not preserved in the requested diagnostics");
    Require(fell_back.fallback == ScalarFallbackReason::BackendNotQualified,
            "fallback from an unimplemented backend lost its reason");
    Require(scalar_calls == 1, "fallback from an unimplemented backend did not build the CPU session");
    std::cout << "PASS priority order, no fallback repair of a profile gap, and the documented "
                 "unimplemented-backend fallback\n";
}

}  // namespace

int main() try {
    CpuAdmitsAllSix();
    GpuAdmitsFilmScan3Only();
    RefusalIsProfileSpecific();
    PriorityAndFallbackBoundaries();
    IndependentProfilesConcurrently();
    std::cout << "PASS internal session profile matrix\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
