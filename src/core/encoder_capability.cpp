// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "encoder_session_config.h"

#include "hypercine/filmscan3.h"

#include <limits>
#include <stdexcept>

namespace hypercine { namespace standalone {
namespace {

struct Evidence {
    int width;
    int height;
    const char* note;
};

// Explicit CPU-scalar attempt list. Absence is refusal, not an implicit crop.
// 4096x2024 is deliberately missing: it passes the pixel budget and is not qualified.
// Prior gates for 1920x1080 and 3840x2160 are existing repository tests, not new claims.
constexpr Evidence kCpuExperimentalEvidence[] = {
    {128, 24, "minimum 3-level YUV422 geometry; experimental CPU attempt only"},
    {256, 256, "noncanonical square; experimental CPU attempt only"},
    {1280, 720, "structurally legal 720p; experimental CPU attempt only, no native reference"},
    {1920, 1080, "default FilmScan3 session; existing CPU golden and FFmpeg gates on main"},
    {2048, 1080, "noncanonical; experimental CPU attempt only, no native reference"},
    {3840, 2160, "experimental 4K CPU; original-SDK 4K parity remains pending"},
};

// Separate bounded Metal records: CPU evidence alone never qualifies a GPU.
constexpr Evidence kMetalExperimentalEvidence[] = {
    {128, 24, "bounded experimental Metal; original-SDK reference missing"},
    {256, 256, "bounded experimental Metal; original-SDK reference missing"},
    {1280, 720, "bounded experimental Metal; original-SDK reference missing"},
    {1920, 1080, "bounded experimental Metal; original-SDK reference missing"},
    {2048, 1080, "bounded experimental Metal; original-SDK reference missing"},
    {3840, 2160, "bounded experimental Metal; original-SDK reference missing"},
};

// Separate bounded CUDA records. Metal and CPU evidence never qualify CUDA,
// which additionally carries device-memory limits. Vulkan has no records at
// all in this revision, so it stays unqualified even when present.
constexpr Evidence kCudaExperimentalEvidence[] = {
    {128, 24, "bounded experimental CUDA; original-SDK reference missing"},
    {256, 256, "bounded experimental CUDA; original-SDK reference missing"},
    {1280, 720, "bounded experimental CUDA; original-SDK reference missing"},
    {1920, 1080, "bounded experimental CUDA; 1080p A2 packet parity exists on main"},
    {2048, 1080, "bounded experimental CUDA; original-SDK reference missing"},
    {3840, 2160, "bounded experimental CUDA; original-SDK reference missing"},
};

bool FitsInt(std::int64_t value) noexcept {
    return value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
}

bool Product(std::int64_t width, std::int64_t height, std::uint64_t& pixels) noexcept {
    if (width <= 0 || height <= 0) return false;
    const auto w = static_cast<std::uint64_t>(width);
    const auto h = static_cast<std::uint64_t>(height);
    if (w > std::numeric_limits<std::uint64_t>::max() / h) return false;
    pixels = w * h;
    return true;
}

// Shared 3-level YUV422 wavelet constraint (standalone.h). The 4096 envelope
// and the 8294400-pixel budget stay inside ValidateFilmScan3Geometry.
bool WaveletShape(std::int64_t width, std::int64_t height) noexcept {
    return width >= 128 && height >= 24 && width % 128 == 0 && height % 8 == 0;
}

bool BackendPresent(const EncoderDevice& device, SessionBackend backend) noexcept {
    switch (backend) {
    case SessionBackend::CpuScalar: return device.cpu_scalar_available;
    case SessionBackend::CpuSimd: return device.cpu_simd_selectable;
    case SessionBackend::Metal: return device.metal_available;
    case SessionBackend::Cuda: return device.cuda_available;
    case SessionBackend::Vulkan: return device.vulkan_available;
    }
    return false;
}

const Evidence* FindEvidence(int width, int height, SessionBackend backend) noexcept {
    const Evidence* records = kCpuExperimentalEvidence;
    std::size_t count = sizeof(kCpuExperimentalEvidence) / sizeof(kCpuExperimentalEvidence[0]);
    if (backend == SessionBackend::Metal) {
        records = kMetalExperimentalEvidence;
        count = sizeof(kMetalExperimentalEvidence) / sizeof(kMetalExperimentalEvidence[0]);
    } else if (backend == SessionBackend::Cuda) {
        records = kCudaExperimentalEvidence;
        count = sizeof(kCudaExperimentalEvidence) / sizeof(kCudaExperimentalEvidence[0]);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& item = records[i];
        if (item.width == width && item.height == height) return &item;
    }
    return nullptr;
}

std::uint64_t LowpassPayloadBytes(int width, int height) noexcept {
    // Uncompressed LL samples the writer emits as U16. Chroma width is half.
    const auto luma = static_cast<std::uint64_t>(width / 8) * static_cast<std::uint64_t>(height / 8) * 2u;
    const auto chroma = static_cast<std::uint64_t>(width / 16) * static_cast<std::uint64_t>(height / 8) * 2u;
    return luma + chroma + chroma;
}

bool PlanningCeiling(std::uint64_t pixels, std::uint64_t extra, std::uint64_t& out) noexcept {
    // Static ceiling from docs/MACOS_GEOMETRY_P4.md ("<36P+1MiB"), not a measured RSS.
    constexpr std::uint64_t kMiB = 1024u * 1024u;
    if (pixels > (std::numeric_limits<std::uint64_t>::max() - kMiB) / 36u) return false;
    const auto base = pixels * 36u + kMiB;
    if (extra > std::numeric_limits<std::uint64_t>::max() - base) return false;
    out = base + extra;
    return true;
}

}  // namespace

const char* CapabilityReasonName(CapabilityReason reason) noexcept {
    switch (reason) {
    case CapabilityReason::FormatMathematicallyInvalid: return "FormatMathematicallyInvalid";
    case CapabilityReason::ValidButNotImplemented: return "ValidButNotImplemented";
    case CapabilityReason::BackendNotPresent: return "BackendNotPresent";
    case CapabilityReason::BackendNotQualifiedForGeometry: return "BackendNotQualifiedForGeometry";
    case CapabilityReason::MemoryBudgetExceeded: return "MemoryBudgetExceeded";
    case CapabilityReason::ReferenceMissing: return "ReferenceMissing";
    case CapabilityReason::ExperimentallySupported: return "ExperimentallySupported";
    case CapabilityReason::ProfileNotQualifiedForBackend: return "ProfileNotQualifiedForBackend";
    }
    return "Unclassified";
}

// Profile x backend admission matrix for this contract revision (issue #259,
// implementation-order step 3). This is a session-contract admission, not a
// product qualification and never an original-SDK reference.
//
// CPU scalar: the six regular profiles share one writer, one rate-state
// calculator and one quality stamp. #318 pinned 16-frame 1080p packets for all
// six profiles, one 4K frame zero for all six, and 120-frame FilmScan2 /
// FilmScan3 sequences against the original SDK, so all six are admitted here.
//
// Metal and CUDA: this tree holds native session packet evidence for FilmScan3
// only. Admitting another profile there would emit unqualified bytes behind a
// GPU claim, so it is refused until that pair has its own packet oracle.
bool ProfileAdmitted(SessionBackend backend, hypercine::EncodingQuality quality) noexcept {
    if (!hypercine::IsValidEncodingQuality(quality)) return false;
    switch (backend) {
    case SessionBackend::CpuScalar:
        return true;
    case SessionBackend::Metal:
    case SessionBackend::Cuda:
        return quality == hypercine::EncodingQuality::FilmScan3;
    default:
        return false;
    }
}

const char* ProfileAdmissionNote(SessionBackend backend, hypercine::EncodingQuality quality) noexcept {
    if (!hypercine::IsValidEncodingQuality(quality))
        return "quality profile is not one of the six regular profiles";
    if (ProfileAdmitted(backend, quality)) {
        if (backend == SessionBackend::CpuScalar)
            return "six-profile shared writer admitted; experimental CPU attempt only";
        return "FilmScan3-only session packet evidence on this backend; experimental attempt only";
    }
    return "this backend has session packet evidence for the FilmScan3 profile only";
}

namespace {

// One implementation for both source domains. `native_domain` selects which
// entry is answering; it never weakens a geometry, budget, profile or evidence
// axis, so the two queries cannot drift into different resource logic.
EncoderCapabilityResult Evaluate(const EncoderSessionConfig& config, const EncoderDevice& device,
                                 bool native_domain) {
    EncoderCapabilityResult result;
    result.reference_qualified = false;
    result.reasons.reference_missing = true;
    const char* format_detail = "invalid format";
    const char* unimplemented_detail = "not implemented";
    const char* memory_detail = "memory budget exceeded";

    // The codec family, not the profile, decides whether the FilmScan3 wire and
    // geometry contract applies: all six regular profiles share this writer and
    // therefore this validator. Profile admissibility is a separate axis that
    // depends on the effective backend, so it is checked further below.
    const bool filmscan3 = config.codec == CodecFamily::FilmScan3;
    if (config.codec != CodecFamily::FilmScan3 && config.codec != CodecFamily::LegacyFixedQ2 &&
        config.codec != CodecFamily::Rgb444) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "unknown codec family";
    } else if (config.codec != CodecFamily::FilmScan3) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "codec family is outside the FilmScan3 session writer";
    }
    if (config.codec == CodecFamily::FilmScan3 && !hypercine::IsValidEncodingQuality(config.quality)) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "quality profile is not one of the six regular profiles";
    }
    // Source domain. Native YUV422p10 planes are a distinct storage domain: the
    // samples are already the Y, Cr, Cb codes the wavelet consumes, so that entry
    // skips Convert instead of reinterpreting storage. Which backends actually
    // consume the domain is decided against the effective backend below, after an
    // explicitly permitted scalar fallback has been resolved.
    const bool native_planes = config.pixel_format == SourcePixelFormat::Yuv422p10Native;
    if (native_planes && !native_domain) {
        // A native planes configuration must never reach a session whose only
        // storage entry is interleaved BGRA bytes: that would be the silent BGRA
        // substitution this domain exists to prevent. Typed refusal before any
        // geometry evaluation, allocation or factory call; the native query
        // answers the domain question.
        result.reasons.valid_but_not_implemented = true;
        result.primary = CapabilityReason::ValidButNotImplemented;
        result.detail = "native YUV422p10 plane input is not an interleaved BGRA session source; "
                        "query the native planes capability instead";
        return result;
    }
    if (!native_planes && native_domain) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "the native planes capability query requires the native YUV422p10 source format";
    }
    if (config.pixel_format != SourcePixelFormat::Bgra8 && !native_planes) {
        if (config.pixel_format == SourcePixelFormat::Yuy2 || config.pixel_format == SourcePixelFormat::Rgb48)
            result.reasons.valid_but_not_implemented = true;
        else result.reasons.format_mathematically_invalid = true;
        unimplemented_detail = "only BGRA8 and native YUV422p10 plane input are implemented";
    }
    if (config.gop != GopMode::IntraGop1) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "only progressive GOP1 is implemented";
    }
    if (!config.progressive) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "interlaced video is not implemented";
    }
    if (config.diagnostic == DiagnosticMode::EmbedHashesInSample) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "embedding diagnostic hashes would change the sample";
    } else if (config.diagnostic != DiagnosticMode::None &&
               config.diagnostic != DiagnosticMode::CoefficientFnv64) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "unknown diagnostic mode";
    }
    if (config.frame_rate.numerator <= 0 || config.frame_rate.denominator <= 0) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "frame rate rational must be positive";
    } else if (config.codec == CodecFamily::FilmScan3 &&
               (config.frame_rate.numerator != 30 || config.frame_rate.denominator != 1)) {
        // The current metadata-less FilmScan3 writer generates TIMC with a
        // fixed 30fps split (pinned by #262). Accepting an arbitrary positive
        // rate here would falsely promise a timebase the writer cannot emit.
        // Per-session rational timecode is future #259 work, not a validator fix.
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "FilmScan3 automatic TIMC supports only 30/1; requested timebase is not implemented";
    }

    std::uint64_t pixels = 0;
    const bool product_ok = Product(config.width, config.height, pixels);
    if (config.width <= 0 || config.height <= 0) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "width and height must be positive";
    } else if (!product_ok) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "width*height overflows uint64";
    } else if (!WaveletShape(config.width, config.height)) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "FilmScan3 YUV422 3-level shape requires width%128, height%8, width>=128, height>=24";
    }
    result.pixel_budget_ok = product_ok && pixels <= kMaxPlaneElements;
    if (product_ok && !result.pixel_budget_ok) {
        result.reasons.memory_budget_exceeded = true;
        memory_detail = "pixel count exceeds kMaxPlaneElements";
    }

    // Storage binding. Each source domain has exactly one addressing contract, and
    // the two are not interchangeable: bytes+interleaved stride for BGRA, sample
    // strides for the three native planes. A request that carries both (or the
    // plane strides without the plane format) is refused as a format error rather
    // than guessed, because a capability query that budgeted the wrong domain
    // would later admit a buffer the encoder cannot address.
    const bool storage_bound = config.input_stride_bytes != 0 || config.input_bytes != 0;
    const auto luma_stride = config.native_planes.luma_stride_samples;
    const auto chroma_stride = config.native_planes.chroma_stride_samples;
    const bool native_storage_bound = native_planes && (luma_stride != 0 || chroma_stride != 0);
    if (native_planes && storage_bound) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "a native planes source must not carry a BGRA stride/byte binding";
    } else if (!native_planes && (luma_stride != 0 || chroma_stride != 0)) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "native plane strides require the native YUV422p10 source format";
    }
    if (native_planes) {
        const auto chroma_width = config.width / 2;
        if (luma_stride < 0 || chroma_stride < 0) {
            result.reasons.format_mathematically_invalid = true;
            format_detail = "native plane strides are sample counts and cannot be negative";
        } else if ((luma_stride == 0) != (chroma_stride == 0)) {
            result.reasons.format_mathematically_invalid = true;
            format_detail = "native luma and chroma strides must be bound together";
        } else if (native_storage_bound && product_ok &&
                   WaveletShape(config.width, config.height) && FitsInt(config.height)) {
            if (!FitsInt(luma_stride) || !FitsInt(chroma_stride) || !FitsInt(chroma_width)) {
                result.reasons.format_mathematically_invalid = true;
                format_detail = "native plane stride does not fit the int16 Plane element type";
            } else if (luma_stride < config.width || luma_stride % 8 != 0) {
                result.reasons.format_mathematically_invalid = true;
                format_detail = "native luma stride must cover the row and be 16-byte aligned (8 int16 samples)";
            } else if (chroma_stride < chroma_width || chroma_stride % 8 != 0) {
                result.reasons.format_mathematically_invalid = true;
                format_detail = "native chroma stride must cover width/2 and be 16-byte aligned (8 int16 samples)";
            } else if (!PlaneStorageFits(static_cast<int>(luma_stride), static_cast<int>(config.height)) ||
                       !PlaneStorageFits(static_cast<int>(chroma_stride), static_cast<int>(config.height))) {
                result.reasons.memory_budget_exceeded = true;
                memory_detail = "native plane stride*height exceeds the per-plane allocation budget";
            }
        }
    } else if (storage_bound && product_ok && WaveletShape(config.width, config.height)) {
        const auto row = static_cast<std::uint64_t>(config.width) * 4u;
        if (config.input_stride_bytes == 0 || config.input_bytes == 0) {
            result.reasons.format_mathematically_invalid = true;
            format_detail = "stride and input size must be bound together";
        } else if (config.input_stride_bytes < row || config.input_stride_bytes % 16u != 0u) {
            result.reasons.format_mathematically_invalid = true;
            format_detail = "BGRA stride must be 16-byte aligned and cover the row";
        } else if (config.height > 1 &&
                   config.input_stride_bytes >
                       (std::numeric_limits<std::uint64_t>::max() - row) /
                           static_cast<std::uint64_t>(config.height - 1)) {
            result.reasons.format_mathematically_invalid = true;
            format_detail = "stride*height overflows uint64";
        } else {
            const auto extent = config.input_stride_bytes * static_cast<std::uint64_t>(config.height - 1) + row;
            if (config.input_bytes < extent) {
                result.reasons.format_mathematically_invalid = true;
                format_detail = "input buffer is smaller than stride*(height-1)+row";
            }
        }
    }

    switch (config.backend) {
    case SessionBackend::CpuScalar: case SessionBackend::CpuSimd:
    case SessionBackend::Metal: case SessionBackend::Cuda: case SessionBackend::Vulkan: break;
    default:
        result.reasons.format_mathematically_invalid = true;
        format_detail = "unknown session backend";
    }
    result.requested_backend_present = BackendPresent(device, config.backend);
    if (!result.requested_backend_present) result.reasons.backend_not_present = true;
    SessionBackend effective = config.backend;
    const bool simd_request = config.backend == SessionBackend::CpuSimd;
    if (simd_request && (!config.allow_cpu_scalar_fallback || result.requested_backend_present)) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "per-session CPU SIMD selection is not implemented";
    }
    if (!result.requested_backend_present && config.allow_cpu_scalar_fallback &&
        config.backend != SessionBackend::CpuScalar && device.cpu_scalar_available) {
        result.fell_back_to_cpu_scalar = true;
        effective = SessionBackend::CpuScalar;
    }
    result.effective_backend = effective;

    // Native planes on a GPU backend this revision cannot serve are refused here,
    // before the geometry validator, the profile matrix, any factory and any
    // allocation. There is no Metal/CUDA/Vulkan planes frontend, and the base
    // Frontend::TransformPlanes refuses: answering such a request with BGRA
    // (reinterpreting plane storage) or with the CPU scalar frontend would change
    // the source domain the caller asked for, so it is a typed refusal instead.
    //
    // The explicit scalar-fallback flag above is a *presence* policy and is
    // resolved first: when the caller permitted it and the requested device is
    // genuinely absent, the admitted CPU session is observable through
    // fell_back_to_cpu_scalar. Nothing here substitutes silently.
    if (native_planes && effective != SessionBackend::CpuScalar) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail =
            "native YUV422p10 plane input is implemented for the CPU scalar session only; "
            "this backend has no planes frontend and BGRA substitution is refused";
        if (!result.reasons.format_mathematically_invalid && !result.reasons.memory_budget_exceeded) {
            result.primary = CapabilityReason::ValidButNotImplemented;
            result.detail = unimplemented_detail;
            result.allocation_permitted = false;
            return result;
        }
    }

    // Profile admission is evaluated against the backend that will actually
    // run, so an explicitly permitted scalar fallback does not inherit a GPU's
    // narrower profile matrix. This axis never repairs a format, memory or
    // reference failure; those keep their own primary reason. An enum outside the
    // six regular profiles is a format error, not an evidence gap, so it never
    // sets the profile axis.
    //
    // Only backends with an actual profile matrix take part. CPU SIMD and Vulkan
    // have no admitted session at all in this revision, so they must not borrow
    // the "profile has no evidence here" wording: their own axis already says
    // why they are closed.
    const bool profile_matrix_backend = effective == SessionBackend::CpuScalar ||
                                        effective == SessionBackend::Metal ||
                                        effective == SessionBackend::Cuda;
    if (filmscan3 && hypercine::IsValidEncodingQuality(config.quality) && profile_matrix_backend) {
        result.profile_note = ProfileAdmissionNote(effective, config.quality);
        result.profile_admitted = ProfileAdmitted(effective, config.quality);
        if (!result.profile_admitted) {
            result.reasons.profile_not_qualified_for_backend = true;
        }
    }
    const bool geometry_is_filmscan3 = filmscan3 && FitsInt(config.width) && FitsInt(config.height);
    if (geometry_is_filmscan3) {
        // Qualification is a distinct axis from the geometry validator's
        // result. The geometry validator now accepts wavelet-aligned >4096
        // SHAPES then rejects their pixel budget with length_error. Preserve
        // "outside qualified envelope" independently of exception type, so
        // 8K does not silently lose its backend-not-qualified reason.
        if (config.width > kMaxDimension || config.height > kMaxDimension)
            result.outside_qualified_dimension_envelope = true;
        std::size_t sample_cap = 0;
        const bool cap_fits = config.max_sample_bytes <= std::numeric_limits<std::size_t>::max();
        if (!cap_fits) {
            result.reasons.memory_budget_exceeded = true;
            memory_detail = "max sample size does not fit size_t";
        } else {
            sample_cap = static_cast<std::size_t>(config.max_sample_bytes);
        }
        FilmScan3Geometry geometry;
        geometry.width = static_cast<int>(config.width);
        geometry.height = static_cast<int>(config.height);
        geometry.sample_capacity_bytes = cap_fits ? sample_cap : 0;
        result.geometry_validator_invoked = true;
        try {
            ValidateFilmScan3Geometry(geometry);
            result.geometry_validator_passed = true;
        } catch (const std::invalid_argument&) {
            // Shape or wire-field rejection, never qualification evidence.
        } catch (const std::length_error&) {
            result.reasons.memory_budget_exceeded = true;
            memory_detail = result.pixel_budget_ok
                ? "sample capacity rejected by ValidateFilmScan3Geometry"
                : "pixel budget rejected by ValidateFilmScan3Geometry";
        }
        if (result.geometry_validator_passed && config.max_sample_bytes != 0 &&
            config.max_sample_bytes < LowpassPayloadBytes(geometry.width, geometry.height)) {
            result.reasons.memory_budget_exceeded = true;
            memory_detail = "sample budget is below the uncompressed lowpass payload";
        }
    } else if (FitsInt(config.width) && FitsInt(config.height) &&
               (config.width > kMaxDimension || config.height > kMaxDimension)) {
        result.outside_qualified_dimension_envelope = true;
    }

    if (const auto* evidence = (FitsInt(config.width) && FitsInt(config.height))
            ? FindEvidence(static_cast<int>(config.width), static_cast<int>(config.height), effective)
            : nullptr) {
        result.evidence_record = true;
        result.evidence_note = evidence->note;
    }

    const bool cpu_attempt = effective == SessionBackend::CpuScalar && device.cpu_scalar_available;
    const bool metal_attempt = effective == SessionBackend::Metal && device.metal_available;
    const bool cuda_attempt = effective == SessionBackend::Cuda && device.cuda_available;
    const bool gpu_attempt = metal_attempt || cuda_attempt;
    const bool gpu_effective = effective == SessionBackend::Metal || effective == SessionBackend::Cuda ||
                               effective == SessionBackend::Vulkan;
    const bool backend_blocks_early = result.reasons.backend_not_present && !result.fell_back_to_cpu_scalar;
    if (!backend_blocks_early && (result.outside_qualified_dimension_envelope || (gpu_effective && !gpu_attempt) ||
        ((cpu_attempt || gpu_attempt) && result.geometry_validator_passed && !result.evidence_record))) {
        result.reasons.backend_not_qualified_for_geometry = true;
    }

    // Missing limits/storage are ValidButNotImplemented, not a backend-only
    // refusal that fallback may erase. Absent Metal inherits no GPU requirements.
    if (metal_attempt && (!storage_bound || device.metal_max_buffer_length_bytes == 0 ||
                          device.metal_recommended_working_set_bytes == 0)) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "Metal requires bound input storage and nonzero device limits";
    }
    // Same rule for CUDA: a build flag or an observed card is not a bound
    // session. The device size has to come from a real runtime probe.
    if (cuda_attempt && (!storage_bound || device.cuda_total_global_memory_bytes == 0)) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "CUDA requires bound input storage and a probed device memory size";
    }
    if (product_ok && (cpu_attempt || gpu_effective)) {
        std::uint64_t extra = 0;
        bool extra_ok = true;
        if (storage_bound && config.input_stride_bytes != 0 && config.height > 0) {
            const auto h = static_cast<std::uint64_t>(config.height);
            if (config.input_stride_bytes > std::numeric_limits<std::uint64_t>::max() / h) extra_ok = false;
            else extra = config.input_stride_bytes * h;
        }
        // Last-row storage may fit while full padded storage overflows. That
        // must reject explicitly, never skip all budget/device checks.
        if (!extra_ok) {
            result.reasons.memory_budget_exceeded = true;
            memory_detail = "padded stride*height overflows uint64";
        }
        std::uint64_t ceiling = 0;
        // Conservative static planning, not RSS: 36P + 1MiB + stride*height.
        const bool planning_ok = extra_ok && PlanningCeiling(pixels, extra, ceiling);
        const auto budget = gpu_effective ? config.gpu_memory_budget_bytes : config.cpu_memory_budget_bytes;
        // Both GPU frontends still allocate host color/wavelet/writer storage.
        // A sufficient device ceiling cannot waive the caller's CPU ceiling.
        if (extra_ok && (!planning_ok || budget < ceiling ||
            (gpu_attempt && config.cpu_memory_budget_bytes < ceiling) ||
            (metal_attempt && device.metal_recommended_working_set_bytes != 0 &&
             device.metal_recommended_working_set_bytes < ceiling))) {
            result.reasons.memory_budget_exceeded = true;
            if (result.pixel_budget_ok)
                memory_detail = "host/device budget is below the static planning ceiling";
        }
        // Transform uses uint32 pitch/extent; largest U16 plane is 2P bytes.
        if (metal_attempt && extra_ok &&
            (extra > std::numeric_limits<std::uint32_t>::max() ||
             (device.metal_max_buffer_length_bytes != 0 &&
              (extra > device.metal_max_buffer_length_bytes ||
               pixels > device.metal_max_buffer_length_bytes / 2u)))) {
            result.reasons.memory_budget_exceeded = true;
            memory_detail = "Metal input extent or plane exceeds GPU buffer/index limits";
        }
        // CUDA stages one source plane plus four output bands per level, all
        // through cudaMalloc; the probed total device memory must cover the
        // static planning ceiling before any allocation is attempted.
        if (cuda_attempt && extra_ok && result.pixel_budget_ok &&
            device.cuda_total_global_memory_bytes != 0 &&
            device.cuda_total_global_memory_bytes < ceiling) {
            result.reasons.memory_budget_exceeded = true;
            memory_detail = "probed CUDA device memory is below the static planning ceiling";
        }
    }

    const bool reference_blocks = config.require_reference_oracle && !result.reference_qualified;
    const bool backend_blocks = result.reasons.backend_not_present && !result.fell_back_to_cpu_scalar;
    const bool blocked = result.reasons.format_mathematically_invalid ||
                         result.reasons.valid_but_not_implemented || backend_blocks ||
                         result.reasons.memory_budget_exceeded ||
                         result.reasons.backend_not_qualified_for_geometry ||
                         result.reasons.profile_not_qualified_for_backend || reference_blocks;
    if (!blocked && (cpu_attempt || gpu_attempt) && result.evidence_record && result.geometry_validator_passed) {
        result.reasons.experimentally_supported = true;
        result.allocation_permitted = true;
    }

    if (result.reasons.format_mathematically_invalid) {
        result.primary = CapabilityReason::FormatMathematicallyInvalid;
        result.detail = format_detail;
    } else if (result.reasons.valid_but_not_implemented) {
        result.primary = CapabilityReason::ValidButNotImplemented;
        result.detail = unimplemented_detail;
    } else if (backend_blocks) {
        result.primary = CapabilityReason::BackendNotPresent;
        result.detail = device.cuda_hardware_observed && config.backend == SessionBackend::Cuda
            ? "CUDA hardware was observed but no usable CUDA FilmScan3 session is bound"
            : "requested backend is not present";
    } else if (result.outside_qualified_dimension_envelope &&
               result.reasons.backend_not_qualified_for_geometry) {
        result.primary = CapabilityReason::BackendNotQualifiedForGeometry;
        result.detail = "dimensions are outside the qualified implementation envelope";
    } else if (result.reasons.memory_budget_exceeded) {
        result.primary = CapabilityReason::MemoryBudgetExceeded;
        result.detail = memory_detail;
    } else if (result.reasons.backend_not_qualified_for_geometry) {
        result.primary = CapabilityReason::BackendNotQualifiedForGeometry;
        result.detail = gpu_effective
            ? "backend has no geometry qualification record in this contract revision"
            : "geometry is inside the pixel budget but has no CPU qualification record";
    } else if (reference_blocks) {
        // A missing required oracle outranks the profile-evidence gap: an
        // operator who demanded a reference must be told the reference is what
        // is missing, not that the profile is unproven on this backend.
        result.primary = CapabilityReason::ReferenceMissing;
        result.detail = "original-SDK reference oracle is not available";
    } else if (result.reasons.profile_not_qualified_for_backend) {
        result.primary = CapabilityReason::ProfileNotQualifiedForBackend;
        result.detail = result.profile_note;
    } else if (result.reasons.experimentally_supported) {
        result.primary = CapabilityReason::ExperimentallySupported;
        result.detail = result.fell_back_to_cpu_scalar
            ? "experimental CPU scalar fallback; original-SDK reference is still missing"
            : (metal_attempt ? "bounded experimental Metal attempt; original-SDK reference is still missing"
                             : (cuda_attempt ? "bounded experimental CUDA attempt; original-SDK reference is still missing"
                                             : "experimental CPU scalar attempt; original-SDK reference is still missing"));
    } else {
        result.primary = CapabilityReason::FormatMathematicallyInvalid;
        result.detail = "fail-closed: request did not match an experimental CPU session";
        result.reasons.format_mathematically_invalid = true;
        result.allocation_permitted = false;
    }
    if (result.primary != CapabilityReason::ExperimentallySupported) result.allocation_permitted = false;
    return result;
}

}  // namespace

EncoderCapabilityResult CheckEncoderCapability(const EncoderSessionConfig& config,
                                               const EncoderDevice& device) {
    // Interleaved BGRA storage session entry. A native planes configuration is
    // refused inside Evaluate, never reinterpreted as byte storage.
    return Evaluate(config, device, false);
}

EncoderCapabilityResult CheckNativePlaneCapability(const EncoderSessionConfig& config,
                                                   const EncoderDevice& device) {
    // Native YUV422p10 plane entry. The same geometry, pixel-budget, profile and
    // evidence axes as the BGRA session query; the source domain is the only
    // difference, and there is no byte-storage fallback in either direction.
    return Evaluate(config, device, true);
}

}}