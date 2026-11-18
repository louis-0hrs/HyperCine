// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Issue #259: native YUV422p10 plane input as an explicit source domain in the
// internal capability preflight. Asserts the domain/admission contract and that
// the admitted geometry really encodes through the existing Planes entry. It is
// not a GPU, SDK, GoPro or VC-5 qualification.
#include "../src/core/encoder_session_config.h"
#include "../src/core/encoder_wire_request.h"
#include "hypercine/filmscan3.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

namespace { std::atomic<unsigned long long> allocations{0}; }
void* operator new(std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) { ++allocations; return p; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;

namespace {

unsigned checks = 0;
// Counts real factory invocations on the wire/session path.
unsigned factory_calls = 0;
unsigned frontend_calls = 0;

void Require(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

std::unique_ptr<EncoderSession> CountingFactory(const EncoderSessionConfig& config,
                                                const EncoderDevice& device) {
    ++factory_calls;
    return std::make_unique<EncoderSession>(config, device);
}

std::shared_ptr<Frontend> CountingFrontend(FilmScan3Geometry geometry, WaveletIsa isa) {
    ++frontend_calls;
    return CpuFrontend(geometry, isa);
}

EncoderDevice Host() {
    EncoderDevice device;
    device.cpu_scalar_available = true;
    device.cpu_simd_selectable = false;
    return device;
}

// A device that actually claims every GPU backend, with the limits an admitted
// session would require. A refusal on this device must come from the missing
// planes domain, not from an absent backend or a zero device limit.
EncoderDevice AllBackends() {
    EncoderDevice device = Host();
    device.metal_available = true;
    device.cuda_available = true;
    device.vulkan_available = true;
    device.cuda_hardware_observed = true;
    device.metal_max_buffer_length_bytes = 1ull << 34;
    device.metal_recommended_working_set_bytes = 1ull << 34;
    device.cuda_total_global_memory_bytes = 1ull << 34;
    return device;
}

// 128x24 is the minimum legal three-level YUV422 shape and an existing CPU
// evidence record, so nothing here depends on a new qualification.
EncoderSessionConfig NativeConfig(int width = 128, int height = 24) {
    EncoderSessionConfig config;
    config.codec = CodecFamily::FilmScan3;
    config.quality = hypercine::EncodingQuality::FilmScan3;
    config.pixel_format = SourcePixelFormat::Yuv422p10Native;
    config.width = width;
    config.height = height;
    config.native_planes.luma_stride_samples = width;
    config.native_planes.chroma_stride_samples = width / 2;
    return config;
}

// A refusal must be typed, must not allocate, and must not reach any factory.
void RejectNative(const EncoderSessionConfig& config, const EncoderDevice& device,
                  CapabilityReason reason, const std::string& detail_part,
                  bool expect_no_allocation = true) {
    const auto before = allocations.load();
    const auto result = CheckNativePlaneCapability(config, device);
    const auto after = allocations.load();
    Require(!result.allocation_permitted, "native refusal admitted allocation: " + detail_part);
    Require(result.primary == reason,
            "native refusal reported the wrong reason (" +
                std::string(CapabilityReasonName(result.primary)) + ", expected " +
                std::string(CapabilityReasonName(reason)) + "): " + detail_part);
    Require(result.reasons.reference_missing && !result.reference_qualified,
            "native refusal invented reference qualification: " + detail_part);
    Require(std::strstr(result.detail, detail_part.c_str()) != nullptr,
            "native refusal detail did not name the cause: " + detail_part + " (got \"" +
                result.detail + "\")");
    if (expect_no_allocation)
        Require(before == after, "native refusal allocated memory: " + detail_part);

    // The new native session entry must consume the same refusal before the
    // backend factory, independently of the deliberately BGRA-only wire gate.
    frontend_calls = 0;
    const auto selected = SelectBackend(config, device, CountingFrontend);
    Require(!selected && !selected.frontend && frontend_calls == 0,
            "native refusal reached a frontend factory: " + detail_part);
    Require(selected.status == BackendSelectionStatus::CapabilityRefused &&
                selected.effective.primary == reason,
            "native session selection lost the capability refusal: " + detail_part);

    // The same configuration through the real wire/factory path: nothing is
    // created and the factory is never called.
    EncoderWireRequest request(WireDialect::Cfhd, St2073Edition::None, 0, DecoderTarget::Unspecified,
                               config);
    const auto wire_before = allocations.load();
    factory_calls = 0;
    auto session = CreateWireSession(request, device, CountingFactory);
    const auto wire_after = allocations.load();
    Require(!session && factory_calls == 0, "native refusal reached a session factory: " + detail_part);
    if (expect_no_allocation)
        Require(wire_before == wire_after, "native refusal allocated on the wire path: " + detail_part);
}

// Real planes in the documented Y, Cr, Cb layout, produced by the same
// conversion the BGRA entry uses.
AlignedVector<uint8_t> BgraFrame(int width, int height) {
    AlignedVector<uint8_t> bytes(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto at = (static_cast<std::size_t>(y) * width + x) * 4;
        bytes[at + 0] = static_cast<uint8_t>((x * 7 + y * 13 + ((x * y) >> 3)) & 255);
        bytes[at + 1] = static_cast<uint8_t>((x * 3 + y * 5 + 17) & 255);
        bytes[at + 2] = static_cast<uint8_t>((x * 11 + y * 2 + 91) & 255);
        bytes[at + 3] = 255;
    }
    return bytes;
}

Planes Converted(const AlignedVector<uint8_t>& input, int width, int height) {
    return ConvertFilmScan3Bgra(input.data(), input.size(), width, height,
                                static_cast<std::size_t>(width) * 4);
}

void RequireSameState(const FilmScan3State& a, const FilmScan3State& b, const std::string& label) {
    Require(a.frame_index == b.frame_index, label + ": frame counter consumed");
    Require(a.fs_rate_limiter == b.fs_rate_limiter, label + ": rate limiter consumed");
    Require(a.previous_sample_bytes == b.previous_sample_bytes, label + ": sample size consumed");
    Require(a.fs_overbitrate == b.fs_overbitrate, label + ": overbitrate state consumed");
}

bool SamePlanes(const Planes& a, const Planes& b) {
    for (int c = 0; c < 3; ++c) {
        if (a[c].width != b[c].width || a[c].height != b[c].height || a[c].stride != b[c].stride)
            return false;
        if (a[c].pixels != b[c].pixels) return false;
    }
    return true;
}

void RequireSameFrame(const FilmScan3Frame& actual, const FilmScan3Frame& expected,
                      const std::string& label) {
    Require(actual.sample == expected.sample, label + ": packet bytes differ");
    Require(actual.tables.quant == expected.tables.quant && actual.tables.scale == expected.tables.scale,
            label + ": shared quantization tables differ");
    RequireSameState(actual.state, expected.state, label + ": rate state");
    Require(actual.coefficient_hash == expected.coefficient_hash,
            label + ": diagnostic policy differs");
    if (actual.coefficient_hash == FilmScan3CoefficientHash::Full) {
        Require(FilmScan3FullCoefficientHashesEqual(actual, expected), label + ": coefficients differ");
    } else {
        Require(actual.coefficient_hash == FilmScan3CoefficientHash::DiagnosticOff,
                label + ": encoded frame has no diagnostic disposition");
        // Disabled-hash tombstones bind distinct session identities. Compare
        // only their prescribed marker/slot/serial, never call them equal hashes.
        for (unsigned slot = 0; slot != 30; ++slot) {
            const auto a = actual.coefficient_fnv64[slot], b = expected.coefficient_fnv64[slot];
            Require(a >> 48 == (0xd100u | slot) && b >> 48 == (0xd100u | slot) &&
                        (a & 0xffffffu) == (b & 0xffffffu),
                    label + ": invalid diagnostic tombstone or serial");
        }
    }
}

Planes Padded(const Planes& input) {
    Planes result = input;
    for (unsigned c = 0; c != 3; ++c) {
        auto& plane = result[c];
        plane.stride += 8;
        // Deliberately out-of-range padding is not an active component.
        plane.pixels.assign(static_cast<std::size_t>(plane.stride) * plane.height, 1025);
        for (int y = 0; y != plane.height; ++y)
            for (int x = 0; x != plane.width; ++x)
                plane.pixels[static_cast<std::size_t>(y) * plane.stride + x] =
                    input[c].pixels[static_cast<std::size_t>(y) * input[c].stride + x];
    }
    return result;
}

void NativeSessionContract() {
    constexpr int width = 128, height = 24;
    const FilmScan3Geometry geometry{width, height};
    const auto host = Host();
    const auto bgra = BgraFrame(width, height);
    const auto planes = Converted(bgra, width, height);

    // Selection reaches exactly the admitted scalar factory and never widens
    // profile/geometry/reference admission. Six immutable profile snapshots all
    // feed the same encoder, including their state over ordered distinct frames.
    for (const auto quality : {hypercine::EncodingQuality::Low, hypercine::EncodingQuality::Medium,
                                hypercine::EncodingQuality::High, hypercine::EncodingQuality::FilmScan1,
                                hypercine::EncodingQuality::FilmScan2, hypercine::EncodingQuality::FilmScan3}) {
        auto config = NativeConfig();
        config.quality = quality;
        frontend_calls = 0;
        const auto selected = SelectBackend(config, host, CountingFrontend);
        Require(selected && frontend_calls == 1 && selected.frontend->Backend() == FrontendBackend::Cpu &&
                    selected.frontend->Isa() == WaveletIsa::Scalar,
                "native session did not select the scalar factory exactly once");
        EncoderSession session(config, host);
        FilmScan3Encoder direct(geometry, WaveletIsa::Scalar);
        direct.SetSessionQuality(quality);
        config.quality = hypercine::EncodingQuality::Low;
        config.width = 256;
        config.native_planes.luma_stride_samples = 256;
        Require(session.config().quality == quality && session.config().width == width &&
                    session.config().native_planes.luma_stride_samples == width,
                "caller mutation changed the session profile/layout snapshot");
        for (int frame = 0; frame != 3; ++frame) {
            auto current = planes;
            current[0].pixels[0] = static_cast<int16_t>(64 + frame * 31);
            RequireSameFrame(session.Encode(current), direct.Encode(current), "native session sequence");
        }
        session.Reset();
        direct.Reset();
        Require(session.config().quality == quality, "Reset changed the session profile snapshot");
        RequireSameFrame(session.Encode(planes), direct.Encode(planes), "native session reset");
    }

    // Refused calls in an established session cannot consume frame/rate/hash
    // state. Compare the next successful packet with an uninterrupted control.
    {
        auto config = NativeConfig();
        EncoderSession session(config, host), control(config, host);
        RequireSameFrame(session.Encode(planes), control.Encode(planes), "refusal setup");
        bool refused = false;
        try { (void)session.Encode(bgra.data(), bgra.size(), width * 4); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "native session accepted BGRA bytes");
        for (int invalid : {-1, 1025}) {
            auto bad = planes;
            bad[2].pixels[0] = static_cast<int16_t>(invalid);
            const auto before = bad;
            refused = false;
            try { (void)session.Encode(bad); }
            catch (const std::invalid_argument&) { refused = true; }
            Require(refused && SamePlanes(before, bad), "native sample refusal changed source or succeeded");
        }
        auto bad_shape = planes;
        --bad_shape[1].height;
        refused = false;
        try { (void)session.Encode(bad_shape); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "native session accepted a wrong chroma shape");
        auto short_storage = planes;
        short_storage[0].pixels.pop_back();
        refused = false;
        try { (void)session.Encode(short_storage); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "native session accepted truncated plane storage");
        const auto padded = Padded(planes);
        refused = false;
        try { (void)session.Encode(padded); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "native session ignored its bound sample strides");
        RequireSameFrame(session.Encode(planes), control.Encode(planes), "all native refusals rollback");
    }
    {
        EncoderSessionConfig config;
        config.width = width; config.height = height;
        EncoderSession session(config, host), control(config, host);
        bool refused = false;
        try { (void)session.Encode(planes); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "BGRA session accepted native planes");
        RequireSameFrame(session.Encode(bgra.data(), bgra.size(), width * 4),
                         control.Encode(bgra.data(), bgra.size(), width * 4), "BGRA domain refusal rollback");
    }

    // A shape-only query does not charge storage until actual planes arrive.
    // Exact packed/padded boundaries prove the per-frame budget recheck; neither
    // an accepted shape nor a prior successful frame waives a later frame's cost.
    {
        const auto padded = Padded(planes);
        const std::uint64_t base = 36u * width * height + 1024u * 1024u;
        const std::uint64_t packed_bytes = 4u * width * height;
        const std::uint64_t padded_bytes = 2u * height * (padded[0].stride + 2u * padded[1].stride);
        auto config = NativeConfig();
        config.native_planes = NativePlaneStorage{};
        config.cpu_memory_budget_bytes = base + packed_bytes;
        EncoderSession session(config, host), control(config, host);
        RequireSameFrame(session.Encode(planes), control.Encode(planes), "shape-only packed budget");
        bool refused = false;
        try { (void)session.Encode(padded); }
        catch (const BackendSelectionError& error) {
            refused = error.status() == BackendSelectionStatus::CapabilityRefused &&
                      error.reason() == CapabilityReason::MemoryBudgetExceeded;
        }
        Require(refused, "actual padded planes bypassed the session memory budget");
        auto extra_storage = planes;
        extra_storage[0].pixels.push_back(0);
        refused = false;
        try { (void)session.Encode(extra_storage); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "inactive vector tail bypassed the native storage budget");
        RequireSameFrame(session.Encode(planes), control.Encode(planes), "budget refusal rollback");
        config.cpu_memory_budget_bytes = base + padded_bytes;
        EncoderSession padded_session(config, host);
        FilmScan3Encoder direct(geometry, WaveletIsa::Scalar);
        RequireSameFrame(padded_session.Encode(padded), direct.Encode(planes), "exact padded budget");
        auto unequal_chroma = padded;
        unequal_chroma[2] = planes[2];
        refused = false;
        try { (void)padded_session.Encode(unequal_chroma); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "shape-only session guessed an unequal chroma stride");
        config.cpu_memory_budget_bytes = base + packed_bytes - 1;
        EncoderSession short_budget(config, host);
        refused = false;
        try { (void)short_budget.Encode(planes); }
        catch (const BackendSelectionError& error) {
            refused = error.reason() == CapabilityReason::MemoryBudgetExceeded;
        }
        Require(refused, "shape-only session admitted one byte below packed planning ceiling");
    }

    // Explicit absent-backend fallback still works, but a present GPU cannot
    // borrow the CPU planes implementation. The selected origin stays visible.
    {
        auto config = NativeConfig();
        config.backend = SessionBackend::Metal;
        config.allow_cpu_scalar_fallback = true;
        EncoderSession session(config, host);
        Require(session.selection().fallback == ScalarFallbackReason::BackendNotPresent &&
                    session.selection().effective.effective_backend == SessionBackend::CpuScalar,
                "native scalar fallback origin was lost");
        FilmScan3Encoder direct(geometry, WaveletIsa::Scalar);
        RequireSameFrame(session.Encode(planes), direct.Encode(planes), "explicit native fallback");
        frontend_calls = 0;
        const auto present = SelectBackend(config, AllBackends(), CountingFrontend);
        Require(!present && frontend_calls == 0 &&
                    present.effective.primary == CapabilityReason::ValidButNotImplemented,
                "present Metal planes request became a silent scalar fallback");
    }

    // Move transfers the source-domain snapshot, quality, diagnostics and frame
    // state. Neither native Encode nor Reset can reactivate the source object.
    {
        auto config = NativeConfig();
        config.quality = hypercine::EncodingQuality::Medium;
        config.diagnostic = DiagnosticMode::None;
        EncoderSession original(config, host), control(config, host);
        const auto first = original.Encode(planes);
        RequireSameFrame(first, control.Encode(planes), "move setup");
        EncoderSession moved(std::move(original));
        bool refused = false;
        try { (void)original.Encode(planes); }
        catch (const std::logic_error&) { refused = true; }
        Require(refused && !original.active(), "moved-from native session remained active");
        original.Reset();
        Require(!original.active(), "Reset reactivated a moved-from native session");
        const auto second = moved.Encode(planes);
        RequireSameFrame(second, control.Encode(planes), "native move constructor");
        Require((first.coefficient_fnv64[0] & ~std::uint64_t{0xffffffu}) ==
                    (second.coefficient_fnv64[0] & ~std::uint64_t{0xffffffu}),
                "native move constructor changed diagnostic session identity");
        EncoderSession target(NativeConfig(), host);
        target = std::move(moved);
        Require(!moved.active() && target.config().quality == hypercine::EncodingQuality::Medium &&
                    target.config().diagnostic == DiagnosticMode::None,
                "native move assignment lost the profile/diagnostic snapshot");
        const auto third = target.Encode(planes);
        RequireSameFrame(third, control.Encode(planes), "native move assignment");
        Require((first.coefficient_fnv64[0] & ~std::uint64_t{0xffffffu}) ==
                    (third.coefficient_fnv64[0] & ~std::uint64_t{0xffffffu}),
                "native move assignment changed diagnostic session identity");
    }
}

}  // namespace

int main() {
    try {
        // The native domain is its own enumerator, not an alias of an existing
        // format: an alias would let one of them silently answer for the other.
        Require(static_cast<int>(SourcePixelFormat::Yuv422p10Native) !=
                        static_cast<int>(SourcePixelFormat::Bgra8) &&
                    static_cast<int>(SourcePixelFormat::Yuv422p10Native) !=
                        static_cast<int>(SourcePixelFormat::Yuy2) &&
                    static_cast<int>(SourcePixelFormat::Yuv422p10Native) !=
                        static_cast<int>(SourcePixelFormat::Rgb48),
                "native planes format aliases an existing source format");

        const auto host = Host();
        const auto gpu = AllBackends();
        NativeSessionContract();

        // 1. CPU-native is supported inside the existing geometry/budget limits,
        // for all six profiles through the one shared admission/evidence table.
        for (hypercine::EncodingQuality quality : {hypercine::EncodingQuality::Low,
                                                   hypercine::EncodingQuality::Medium,
                                                   hypercine::EncodingQuality::High,
                                                   hypercine::EncodingQuality::FilmScan1,
                                                   hypercine::EncodingQuality::FilmScan2,
                                                   hypercine::EncodingQuality::FilmScan3}) {
            auto config = NativeConfig();
            config.quality = quality;
            const auto before = allocations.load();
            const auto result = CheckNativePlaneCapability(config, host);
            const auto after = allocations.load();
            Require(result.allocation_permitted &&
                        result.primary == CapabilityReason::ExperimentallySupported,
                    std::string("CPU-native admitted for profile ") +
                        hypercine::EncodingQualityName(quality));
            Require(result.effective_backend == SessionBackend::CpuScalar &&
                        !result.fell_back_to_cpu_scalar,
                    "CPU-native request changed backend");
            Require(result.profile_admitted && result.evidence_record &&
                        result.geometry_validator_passed && result.pixel_budget_ok,
                    "CPU-native admission lost an existing axis");
            Require(before == after, "admitted CPU-native query allocated memory");
        }

        // A shape-only query (no planes allocated yet) is legal at the same
        // geometry and stays inside the same limits.
        {
            auto shape_only = NativeConfig();
            shape_only.native_planes = NativePlaneStorage{};
            Require(CheckNativePlaneCapability(shape_only, host).allocation_permitted,
                    "shape-only native query refused");
        }

        // Geometry limits are the existing ones, not a new table.
        RejectNative(NativeConfig(129, 24), host, CapabilityReason::FormatMathematicallyInvalid,
                     "requires width%128", false);
        RejectNative(NativeConfig(96, 24), host, CapabilityReason::FormatMathematicallyInvalid,
                     "width>=128", false);
        RejectNative(NativeConfig(128, 20), host, CapabilityReason::FormatMathematicallyInvalid,
                     "height%8", false);
        {
            // 3840x2160 is an admitted CPU-native geometry; a legal shape above
            // the plane budget is refused as a budget error. No planes are
            // allocated for either query.
            Require(CheckNativePlaneCapability(NativeConfig(3840, 2160), host).allocation_permitted,
                    "CPU-native 4K evidence geometry refused");
            auto over = NativeConfig(3840, 2176);
            over.native_planes = NativePlaneStorage{};
            RejectNative(over, host, CapabilityReason::MemoryBudgetExceeded, "budget", false);
        }

        // 1b. The planning ceiling counts the bound source storage. A bound native
        // layout must be costed like the equivalent BGRA buffer, otherwise the
        // native domain would silently widen admission against the existing rule.
        {
            constexpr std::uint64_t kWidth = 3840, kHeight = 2160;
            const std::uint64_t pixels = kWidth * kHeight;
            const std::uint64_t native_bytes = kWidth * kHeight * 4;  // Y + 2x(U16 chroma)
            const std::uint64_t bgra_bytes = kWidth * 4 * kHeight;    // same 4 bytes/pixel
            Require(native_bytes == bgra_bytes, "native/BGRA storage cost is not 4 bytes/pixel");
            const std::uint64_t base = pixels * 36u + 1024u * 1024u;
            // The reviewer's probe: one byte above 36P + 1MiB. With the native
            // storage term missing this budget admitted the native request.
            const std::uint64_t probe = base + 1u;
            const std::uint64_t exact = base + native_bytes;

            auto native = NativeConfig(kWidth, kHeight);
            native.cpu_memory_budget_bytes = probe;
            RejectNative(native, host, CapabilityReason::MemoryBudgetExceeded,
                         "planning ceiling", false);

            auto bgra = NativeConfig(kWidth, kHeight);
            bgra.pixel_format = SourcePixelFormat::Bgra8;
            bgra.native_planes = NativePlaneStorage{};
            bgra.input_stride_bytes = kWidth * 4;
            bgra.input_bytes = bgra.input_stride_bytes * kHeight;
            bgra.cpu_memory_budget_bytes = probe;
            Require(CheckEncoderCapability(bgra, host).primary ==
                        CapabilityReason::MemoryBudgetExceeded,
                    "BGRA parity probe did not refuse the same over-budget geometry");

            // At the exactly computed ceiling both domains are admitted: the rule
            // is the existing one, not a new tighter or wider one.
            native.cpu_memory_budget_bytes = exact;
            Require(CheckNativePlaneCapability(native, host).allocation_permitted,
                    "native request refused at its exact planning ceiling");
            bgra.cpu_memory_budget_bytes = exact;
            Require(CheckEncoderCapability(bgra, host).allocation_permitted,
                    "BGRA request refused at the same exact planning ceiling");
        }

        // 2. Metal / CUDA / Vulkan native planes: typed unimplemented, on a device
        // that really claims the backend and on a device that does not. A required
        // GPU backend is never answered by BGRA or by the CPU frontend.
        for (auto backend : {SessionBackend::Metal, SessionBackend::Cuda, SessionBackend::Vulkan}) {
            auto required = NativeConfig();
            required.backend = backend;
            required.allow_cpu_scalar_fallback = false;
            RejectNative(required, gpu, CapabilityReason::ValidButNotImplemented,
                         "CPU scalar session only");
            RejectNative(required, host, CapabilityReason::ValidButNotImplemented,
                         "CPU scalar session only");
            Require(!CheckNativePlaneCapability(required, host).fell_back_to_cpu_scalar,
                    "required GPU-native request fell back to the CPU scalar backend");
            // The refusal is a domain refusal, so it is decided before the
            // geometry validator is even asked about this shape.
            Require(!CheckNativePlaneCapability(required, gpu).geometry_validator_invoked &&
                        !CheckNativePlaneCapability(required, host).geometry_validator_invoked,
                    "GPU-native refusal invoked the geometry validator");
            // The same configuration on the CPU scalar backend is admitted, so the
            // test distinguishes the domain decision from a blanket refusal.
            auto cpu = required;
            cpu.backend = SessionBackend::CpuScalar;
            const auto admitted = CheckNativePlaneCapability(cpu, host);
            Require(admitted.allocation_permitted && admitted.geometry_validator_invoked,
                    "CPU-native control was not admitted");
            // Even an explicitly permitted fallback must not disguise a *present*
            // backend's missing planes domain.
            auto permitted = required;
            permitted.allow_cpu_scalar_fallback = true;
            RejectNative(permitted, gpu, CapabilityReason::ValidButNotImplemented,
                         "CPU scalar session only");
            // An invalid geometry does not replace the domain refusal: the format
            // flag is carried, the primary stays the missing planes frontend, and
            // the validator is still never invoked.
            auto odd = permitted;
            odd.width = 129;
            odd.native_planes.luma_stride_samples = 136;
            odd.native_planes.chroma_stride_samples = 72;
            RejectNative(odd, gpu, CapabilityReason::ValidButNotImplemented,
                         "CPU scalar session only");
            const auto odd_result = CheckNativePlaneCapability(odd, gpu);
            Require(odd_result.reasons.format_mathematically_invalid &&
                        !odd_result.geometry_validator_invoked,
                    "GPU-native refusal lost the format axis or ran the validator");
        }
        {
            // The only admitted GPU-native shape is the caller's own explicit
            // absent-backend fallback, which is observable rather than silent.
            auto config = NativeConfig();
            config.backend = SessionBackend::Cuda;
            config.allow_cpu_scalar_fallback = true;
            const auto result = CheckNativePlaneCapability(config, host);
            Require(result.allocation_permitted && result.fell_back_to_cpu_scalar &&
                        result.effective_backend == SessionBackend::CpuScalar,
                    "explicit native fallback lost its observable reason");
        }

        // 3. Wrong native stride is a format error, never a guess, and it is
        // decided before the geometry validator allocates exception storage.
        {
            auto bad = NativeConfig();
            bad.native_planes.luma_stride_samples = 120;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid, "native luma stride");
            bad = NativeConfig();
            bad.native_planes.luma_stride_samples = 132;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid, "native luma stride");
            bad = NativeConfig();
            bad.native_planes.chroma_stride_samples = 56;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid, "native chroma stride");
            bad = NativeConfig();
            bad.native_planes.chroma_stride_samples = 68;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid, "native chroma stride");
            bad = NativeConfig();
            bad.native_planes.chroma_stride_samples = 0;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid, "bound together");
            bad = NativeConfig();
            bad.native_planes.luma_stride_samples = -128;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid, "cannot be negative");
            // A padded stride that is legal but exceeds the per-plane budget.
            bad = NativeConfig();
            bad.native_planes.luma_stride_samples = 345608;  // 16-byte aligned, > 3840*2160/24
            RejectNative(bad, host, CapabilityReason::MemoryBudgetExceeded,
                         "per-plane allocation budget");
            // Domain mixing: plane strides on a BGRA source, a byte binding on a
            // native source, and the native query asked about a BGRA source.
            bad = NativeConfig();
            bad.pixel_format = SourcePixelFormat::Bgra8;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid,
                         "require the native YUV422p10 source format");
            bad = NativeConfig();
            bad.input_stride_bytes = 512;
            bad.input_bytes = 512 * 24;
            RejectNative(bad, host, CapabilityReason::FormatMathematicallyInvalid,
                         "must not carry a BGRA stride/byte binding");
        }
        {
            // The interleaved session query refuses a native configuration instead
            // of budgeting bytes for it.
            const auto before = allocations.load();
            const auto result = CheckEncoderCapability(NativeConfig(), host);
            const auto after = allocations.load();
            Require(!result.allocation_permitted &&
                        result.primary == CapabilityReason::ValidButNotImplemented,
                    "BGRA session query admitted a native planes source");
            Require(std::strstr(result.detail, "interleaved BGRA session source") != nullptr,
                    "BGRA session query did not name the domain mismatch");
            Require(before == after, "BGRA session query allocated for a native refusal");
            EncoderWireRequest request(WireDialect::Cfhd, St2073Edition::None, 0,
                                       DecoderTarget::Unspecified, NativeConfig());
            factory_calls = 0;
            auto session = CreateWireSession(request, host, CountingFactory);
            Require(!session && factory_calls == 0, "native planes reached a BGRA session factory");
        }

        // 4. Source-bound: the admitted geometry really encodes through the
        // existing Planes entry, the 1024 code is accepted, 1025/-1 are refused,
        // and a refused plane set never consumes session state.
        {
            constexpr int kWidth = 128, kHeight = 24;
            const FilmScan3Geometry geometry{kWidth, kHeight};
            const auto input = BgraFrame(kWidth, kHeight);
            auto planes = Converted(input, kWidth, kHeight);
            ValidateFilmScan3Planes(planes, geometry);

            // Every profile shares one writer and one rate state: the planes entry
            // must reproduce the BGRA packet exactly.
            for (hypercine::EncodingQuality quality : {hypercine::EncodingQuality::Low,
                                                       hypercine::EncodingQuality::FilmScan3}) {
                FilmScan3Encoder bgra(geometry), native(geometry);
                bgra.SetSessionQuality(quality);
                native.SetSessionQuality(quality);
                const auto from_bgra = bgra.Encode(input.data(), input.size(), kWidth * 4);
                const auto from_planes = native.Encode(planes);
                Require(from_bgra.sample == from_planes.sample,
                        "native profile packet differs from the shared writer");
                Require(from_bgra.tables.quant == from_planes.tables.quant &&
                            from_bgra.tables.scale == from_planes.tables.scale,
                        "native profile tables differ from the shared writer");
                RequireSameState(from_bgra.state, from_planes.state, "native profile rate state");
            }

            // 1024 is reachable through the documented conversion and is accepted.
            Planes boundary = planes;
            boundary[0].pixels[0] = 1024;
            boundary[1].pixels[0] = 1024;
            boundary[2].pixels[0] = 1024;
            {
                FilmScan3Encoder encoder(geometry);
                const auto frame = encoder.Encode(boundary);
                Require(!frame.sample.empty(), "1024 sample code produced no packet");
                Require(encoder.state().frame_index == 1,
                        "1024 encode did not advance the session");
            }

            // 1025 and -1 are refused instead of clipped, before the packet and
            // before any session state changes.
            for (int value : {1025, -1}) {
                Planes bad = planes;
                bad[2].pixels[0] = static_cast<int16_t>(value);
                FilmScan3Encoder encoder(geometry);
                const auto state_before = encoder.state();
                const auto bad_before = bad;
                bool threw = false;
                try { (void)encoder.Encode(bad); }
                catch (const std::invalid_argument&) { threw = true; }
                Require(threw, "out-of-range sample code was accepted");
                RequireSameState(state_before, encoder.state(), "refused sample code");
                Require(SamePlanes(bad_before, bad), "refused sample code mutated the caller planes");
            }

            // Wrong geometry in the source itself: a chroma plane of the wrong
            // height is refused before the pyramid is built or state is consumed.
            {
                Plane short_chroma(kWidth / 2, kHeight / 2);
                Planes bad{planes[0], short_chroma, planes[2]};
                FilmScan3Encoder encoder(geometry);
                const auto state_before = encoder.state();
                bool threw = false;
                try { (void)encoder.Encode(bad); }
                catch (const std::invalid_argument&) { threw = true; }
                Require(threw, "wrong chroma geometry was accepted");
                RequireSameState(state_before, encoder.state(), "refused chroma geometry");
            }
        }

        // 5. The shared six-profile rate state and the sole writer are unchanged:
        // a native session still locks its profile on the first successful frame.
        {
            constexpr int kWidth = 128, kHeight = 24;
            const FilmScan3Geometry geometry{kWidth, kHeight};
            const auto planes = Converted(BgraFrame(kWidth, kHeight), kWidth, kHeight);
            FilmScan3Encoder encoder(geometry);
            encoder.SetSessionQuality(hypercine::EncodingQuality::Medium);
            (void)encoder.Encode(planes);
            bool threw = false;
            try { encoder.SetSessionQuality(hypercine::EncodingQuality::FilmScan3); }
            catch (const std::logic_error&) { threw = true; }
            Require(threw, "native encode lost the session profile lock");
        }

        std::cout << checks << " checks PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
