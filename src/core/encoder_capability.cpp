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

bool ProfileAdmitted(SessionBackend backend, EncodingQuality quality) noexcept {
    if (!IsValidEncodingQuality(quality)) return false;
    switch (backend) {
    case SessionBackend::CpuScalar:
        // #318 measured SDK-free writer packets for every regular profile.
        return true;
    case SessionBackend::Metal:
        // No per-profile Metal packet measurement in this revision.
        // FilmScan3 remains the only Metal row with a bounded session record.
        return quality == EncodingQuality::FilmScan3;
    case SessionBackend::Cuda:
        // encoder_cuda_profile_matrix, 1920x1080, frames 0..3. A row is listed
        // only after that gate matched packet bytes, all 36 quant/scale entries,
        // rate state, the exact frame index and all 30 coefficient hashes
        // against an independent CPU scalar session on the same input.
        switch (quality) {
        case EncodingQuality::Low:
        case EncodingQuality::Medium:
        case EncodingQuality::High:
        case EncodingQuality::FilmScan1:
        case EncodingQuality::FilmScan2:
        case EncodingQuality::FilmScan3:
            return true;
        }
        return false;
    case SessionBackend::CpuSimd:
    case SessionBackend::Vulkan:
        return false;
    }
    return false;
}

EncoderCapabilityResult CheckEncoderCapability(const EncoderSessionConfig& config,
                                               const EncoderDevice& device) {
    EncoderCapabilityResult result;
    result.reference_qualified = false;
    result.reasons.reference_missing = true;
    const char* format_detail = "invalid format";
    const char* unimplemented_detail = "not implemented";
    const char* memory_detail = "memory budget exceeded";

    // Geometry stays on the FilmScan3 YUV422 path for every regular profile.
    // An out-of-range quality is not a profile the writer can name.
    const bool filmscan3 = config.codec == CodecFamily::FilmScan3 &&
                           IsValidEncodingQuality(config.quality);
    if (config.codec != CodecFamily::FilmScan3 && config.codec != CodecFamily::LegacyFixedQ2 &&
        config.codec != CodecFamily::Rgb444) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "unknown codec family";
    } else if (config.codec != CodecFamily::FilmScan3) {
        result.reasons.valid_but_not_implemented = true;
        unimplemented_detail = "codec family is outside the FilmScan3 session writer";
    }
    if (config.codec == CodecFamily::FilmScan3 && !IsValidEncodingQuality(config.quality)) {
        result.reasons.format_mathematically_invalid = true;
        format_detail = "quality profile is not one of the six regular profiles";
    }
    if (config.pixel_format != SourcePixelFormat::Bgra8) {
        if (config.pixel_format == SourcePixelFormat::Yuy2 || config.pixel_format == SourcePixelFormat::Rgb48)
            result.reasons.valid_but_not_implemented = true;
        else result.reasons.format_mathematically_invalid = true;
        if (config.pixel_format != SourcePixelFormat::Bgra8)
            unimplemented_detail = "only BGRA8 input is implemented";
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

    const bool storage_bound = config.input_stride_bytes != 0 || config.input_bytes != 0;
    if (storage_bound && product_ok && WaveletShape(config.width, config.height)) {
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
        if (extra_ok && (!planning_ok || budget < ceiling ||
            (metal_attempt && (config.cpu_memory_budget_bytes < ceiling ||
             (device.metal_recommended_working_set_bytes != 0 &&
              device.metal_recommended_working_set_bytes < ceiling))))) {
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

    // Profile evidence is per backend. CPU packet parity does not admit Metal
    // or CUDA, and a missing GPU profile is not repaired by scalar fallback.
    const char* profile_detail = "quality profile has no native packet evidence for this backend";
    if ((cpu_attempt || gpu_attempt) && config.codec == CodecFamily::FilmScan3 &&
        IsValidEncodingQuality(config.quality) && !ProfileAdmitted(effective, config.quality)) {
        result.reasons.profile_not_qualified_for_backend = true;
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
    } else if (result.reasons.profile_not_qualified_for_backend) {
        result.primary = CapabilityReason::ProfileNotQualifiedForBackend;
        result.detail = profile_detail;
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
        result.primary = CapabilityReason::ReferenceMissing;
        result.detail = "original-SDK reference oracle is not available";
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

}}