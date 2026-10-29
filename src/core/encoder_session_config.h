// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Internal session contract for issue #259 parts A and B.
// Not a public header and not a product-qualification claim.
#pragma once

#include "hypercine/quality_state.h"

#include <cstddef>
#include <cstdint>

namespace hypercine { namespace standalone {

// Codec family is not a quality switch. Legacy fixed-Q2 and RGB444 stay
// outside the FilmScan3 writer; this prototype does not retarget either one.
enum class CodecFamily : int {
    FilmScan3 = 1,
    LegacyFixedQ2 = 2,
    Rgb444 = 3
};

enum class SourcePixelFormat : int {
    Bgra8 = 1,
    Yuy2 = 2,
    Rgb48 = 3
};

enum class GopMode : int {
    IntraGop1 = 1,
    Gop2 = 2
};

// Preference only. Presence is a property of EncoderDevice, not of this enum.
// CpuSimd has no per-session policy in this revision (see issue #255).
enum class SessionBackend : int {
    CpuScalar = 1,
    CpuSimd = 2,
    Metal = 3,
    Cuda = 4,
    Vulkan = 5
};

enum class DiagnosticMode : int {
    None = 0,
    // Side channel only. The current writer already returns coefficient FNV
    // on FilmScan3Frame and does not embed those hashes in the sample.
    CoefficientFnv64 = 1,
    // Would change the bitstream. Not implemented.
    EmbedHashesInSample = 2
};

struct Rational {
    std::int64_t numerator = 30;
    std::int64_t denominator = 1;
};

// Fixed for one session. No process-global toggle. Legacy FilmScan3Encoder
// constructors are intentionally not replaced by this type.
struct EncoderSessionConfig {
    CodecFamily codec = CodecFamily::FilmScan3;
    EncodingQuality quality = EncodingQuality::FilmScan3;
    SourcePixelFormat pixel_format = SourcePixelFormat::Bgra8;
    std::int64_t width = 1920;
    std::int64_t height = 1080;
    // Bytes. Zero together with input_bytes means storage is not bound yet.
    std::uint64_t input_stride_bytes = 0;
    std::uint64_t input_bytes = 0;
    Rational frame_rate{};
    GopMode gop = GopMode::IntraGop1;
    bool progressive = true;
    SessionBackend backend = SessionBackend::CpuScalar;
    // Explicit only. Never implied by a missing GPU.
    bool allow_cpu_scalar_fallback = false;
    // Caller ceilings. uint64 max means "no tighter ceiling than the implementation".
    std::uint64_t cpu_memory_budget_bytes = ~std::uint64_t{0};
    std::uint64_t gpu_memory_budget_bytes = ~std::uint64_t{0};
    // Zero selects the writer's geometric sample bound. A nonzero value may
    // only reduce that bound; this query also rejects a value below the
    // uncompressed lowpass payload before any plane allocation.
    std::uint64_t max_sample_bytes = 0;
    DiagnosticMode diagnostic = DiagnosticMode::CoefficientFnv64;
    // When set, a missing original-SDK oracle blocks even an experimental
    // CPU attempt. This revision never has that oracle.
    bool require_reference_oracle = false;
};

// Build and device facts supplied by the caller. This function does not probe
// hardware. Observing a CUDA card does not make a CUDA encoder available.
struct EncoderDevice {
    bool cpu_scalar_available = true;
    bool cpu_simd_selectable = false;
    bool metal_available = false;
    bool cuda_available = false;
    bool vulkan_available = false;
    bool cuda_hardware_observed = false;
    // Required for a present Metal session; zero limits fail closed.
    std::uint64_t metal_max_buffer_length_bytes = 0;
    std::uint64_t metal_recommended_working_set_bytes = 0;
    // Required for a present CUDA session; zero fails closed. Only a real
    // runtime probe may set this, never a build flag alone.
    std::uint64_t cuda_total_global_memory_bytes = 0;
};

enum class CapabilityReason : int {
    FormatMathematicallyInvalid = 1,
    ValidButNotImplemented = 2,
    BackendNotPresent = 3,
    BackendNotQualifiedForGeometry = 4,
    MemoryBudgetExceeded = 5,
    ReferenceMissing = 6,
    ExperimentallySupported = 7,
    // The writer can emit the profile, but this backend has no native
    // packet measurement for it. Fail closed; do not relabel it as FilmScan3.
    ProfileNotQualifiedForBackend = 8
};

// Independent axes. A single "unsupported" is intentionally not the result.
struct CapabilityFlags {
    bool format_mathematically_invalid = false;
    bool valid_but_not_implemented = false;
    bool backend_not_present = false;
    bool backend_not_qualified_for_geometry = false;
    bool memory_budget_exceeded = false;
    bool reference_missing = false;
    bool experimentally_supported = false;
    bool profile_not_qualified_for_backend = false;
};

struct EncoderCapabilityResult {
    CapabilityFlags reasons{};
    CapabilityReason primary = CapabilityReason::FormatMathematicallyInvalid;
    const char* detail = "unclassified rejection";
    // True only when an experimental scalar CPU or bounded Metal attempt may allocate.
    // Never a CODEC_PASS and never an original-SDK reference.
    bool allocation_permitted = false;
    bool geometry_validator_passed = false;
    bool geometry_validator_invoked = false;
    bool pixel_budget_ok = false;
    bool outside_qualified_dimension_envelope = false;
    bool evidence_record = false;
    bool reference_qualified = false;
    bool requested_backend_present = false;
    bool fell_back_to_cpu_scalar = false;
    SessionBackend effective_backend = SessionBackend::CpuScalar;
    const char* evidence_note = "";
};

// Fail-closed. Does not allocate frames, planes, wavelet scratch, or a sample.
// ValidateFilmScan3Geometry is the implementation geometry authority and may
// throw; that exception is caught here and is not a frame allocation.
EncoderCapabilityResult CheckEncoderCapability(const EncoderSessionConfig& config,
                                               const EncoderDevice& device);

// Regular profiles the SDK-free writer can emit are not automatically
// admissible on every backend. CPU scalar admits all six. Metal and CUDA
// admit a profile only when a native packet measurement exists for that
// backend. This is not a product qualification and not an original-SDK or
// SMPTE conformance claim.
bool ProfileAdmitted(SessionBackend backend, EncodingQuality quality) noexcept;

const char* CapabilityReasonName(CapabilityReason reason) noexcept;

}}
