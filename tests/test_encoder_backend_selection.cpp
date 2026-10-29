// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../src/core/encoder_backend_selection.h"
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <string>

using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
unsigned calls = 0;
std::shared_ptr<Frontend> Count(FilmScan3Geometry g, WaveletIsa isa) {
    ++calls;
    return CpuFrontend(g, isa);
}
std::shared_ptr<Frontend> Missing(FilmScan3Geometry, WaveletIsa) { throw std::runtime_error("unavailable"); }
std::shared_ptr<Frontend> NoMemory(FilmScan3Geometry, WaveletIsa) { throw std::bad_alloc(); }
std::shared_ptr<Frontend> Logic(FilmScan3Geometry, WaveletIsa) { throw std::logic_error("broken contract"); }
std::shared_ptr<Frontend> WrongGeometry(FilmScan3Geometry, WaveletIsa) { return CpuFrontend({256,256}, WaveletIsa::Scalar); }
std::shared_ptr<Frontend> WrongCapacity(FilmScan3Geometry g, WaveletIsa isa) {
    g.sample_capacity_bytes = g.sample_capacity_bytes == 0 ? 1024 : 0;
    return CpuFrontend(g, isa);
}
struct AutoFrontend final : Frontend {
    FilmScan3Geometry geometry;
    explicit AutoFrontend(FilmScan3Geometry g) : geometry(g) {}
    FrontendBackend Backend() const noexcept override { return FrontendBackend::Cpu; }
    FilmScan3Geometry Geometry() const noexcept override { return geometry; }
    Planes Convert(const uint8_t*, std::size_t, int, int, std::size_t) override {
        throw std::logic_error("test frontend must never execute");
    }
    WaveletLevel Forward(const Plane&, int) override { throw std::logic_error("test frontend must never execute"); }
    std::string DeviceName() const override { return "test Auto ISA"; }
};
std::shared_ptr<Frontend> WrongIsa(FilmScan3Geometry g, WaveletIsa) { return std::make_shared<AutoFrontend>(g); }
EncoderSessionConfig Small() {
    EncoderSessionConfig config;
    config.width = 128; config.height = 24;
    return config;
}
void Refused(const EncoderSessionConfig& config, EncoderDevice device, CapabilityReason reason) {
    calls = 0;
    const auto result = SelectBackend(config, device, Count);
    Require(!result && !result.frontend && calls == 0, "refusal reached frontend allocation");
    Require(result.status == BackendSelectionStatus::CapabilityRefused, "refusal lost typed status");
    Require(result.effective.primary == reason, "wrong typed capability reason");
    bool threw = false;
    try { EncoderSession session(config, device); }
    catch (const BackendSelectionError& error) {
        threw = error.status() == BackendSelectionStatus::CapabilityRefused && error.reason() == reason;
    }
    Require(threw, "session did not enforce typed capability refusal");
}
unsigned metal_calls = 0;
struct FakeMetal final : Frontend {
    FilmScan3Geometry geometry;
    FrontendBackend identity = FrontendBackend::Metal;
    WaveletIsa isa = WaveletIsa::Auto;
    explicit FakeMetal(FilmScan3Geometry g) : geometry(g) {}
    FilmScan3Geometry Geometry() const noexcept override { return geometry; }
    FrontendBackend Backend() const noexcept override { return identity; }
    WaveletIsa Isa() const noexcept override { return isa; }
    Planes Convert(const uint8_t*, std::size_t, int, int, std::size_t) override {
        throw std::logic_error("fake Metal must never execute");
    }
    WaveletLevel Forward(const Plane&, int) override { throw std::logic_error("fake Metal must never execute"); }
    std::string DeviceName() const override { return "fake Metal contract only"; }
};
std::shared_ptr<Frontend> CountMetal(FilmScan3Geometry g) { ++metal_calls; return std::make_shared<FakeMetal>(g); }
std::shared_ptr<Frontend> CpuMasquerade(FilmScan3Geometry g) { ++metal_calls; return CpuFrontend(g, WaveletIsa::Scalar); }
std::shared_ptr<Frontend> MetalWrongGeometry(FilmScan3Geometry g) { g.width += 128; return CountMetal(g); }
std::shared_ptr<Frontend> MetalWrongCapacity(FilmScan3Geometry g) { ++g.sample_capacity_bytes; return CountMetal(g); }
std::shared_ptr<Frontend> MetalWrongIsa(FilmScan3Geometry g) {
    auto f = std::make_shared<FakeMetal>(g); f->isa = WaveletIsa::Scalar; return f;
}
std::shared_ptr<Frontend> MetalUnknown(FilmScan3Geometry g) {
    auto f = std::make_shared<FakeMetal>(g); f->identity = FrontendBackend::Unknown; return f;
}
std::shared_ptr<Frontend> MetalMissing(FilmScan3Geometry) { ++metal_calls; throw std::runtime_error("Metal init failed"); }
std::shared_ptr<Frontend> MetalNoMemory(FilmScan3Geometry) { throw std::bad_alloc(); }
std::shared_ptr<Frontend> MetalLogic(FilmScan3Geometry) { throw std::logic_error("Metal contract broken"); }
void MetalContract() {
    auto config = Small(); config.backend = SessionBackend::Metal;
    config.input_stride_bytes = 512; config.input_bytes = 512 * 24;
    EncoderDevice device; device.metal_available = true;
    device.metal_max_buffer_length_bytes = 1ull << 30;
    device.metal_recommended_working_set_bytes = 1ull << 30;
    BackendFactories factories{Count, CountMetal};
    calls = metal_calls = 0;
    auto selected = SelectBackend(config, device, factories);
    Require(selected && calls == 0 && metal_calls == 1 && selected.frontend->Backend() == FrontendBackend::Metal,
            "Metal selected wrong factory/identity");
    // Legacy scalar injection must not turn a Metal request into a CPU frontend.
    calls = 0;
    const auto legacy = SelectBackend(config, device, Count);
    Require(calls == 0 && (!legacy || legacy.frontend->Backend() == FrontendBackend::Metal),
            "legacy scalar seam used for Metal");
    for (auto factory : {MetalSessionFactory{nullptr}, CpuMasquerade, MetalWrongGeometry,
                         MetalWrongCapacity, MetalWrongIsa, MetalUnknown, MetalMissing}) {
        factories.metal = factory; config.allow_cpu_scalar_fallback = true; calls = 0;
        const auto failed = SelectBackend(config, device, factories);
        Require(!failed && !failed.frontend && failed.status == BackendSelectionStatus::InitializationFailed && calls == 0,
                "Metal initialization failed open or fell back");
    }
    for (auto factory : {MetalNoMemory, MetalLogic}) {
        factories.metal = factory; bool propagated = false;
        try { (void)SelectBackend(config, device, factories); }
        catch (const std::bad_alloc&) { propagated = factory == MetalNoMemory; }
        catch (const std::logic_error&) { propagated = factory == MetalLogic; }
        Require(propagated, "Metal non-runtime failure hidden");
    }
    factories.metal = CountMetal;
    const auto refused = [&](EncoderSessionConfig c, EncoderDevice d, CapabilityReason reason) {
        calls = metal_calls = 0; c.allow_cpu_scalar_fallback = true;
        const auto r = SelectBackend(c, d, factories);
        Require(!r && calls == 0 && metal_calls == 0 && r.effective.primary == reason,
                "Metal refusal reached a factory or changed reason");
    };
    auto c = config; c.max_sample_bytes = 8192;
    calls = metal_calls = 0;
    const auto capped = SelectBackend(c, device, factories);
    Require(capped && calls == 0 && metal_calls == 1 &&
            capped.frontend->Geometry().sample_capacity_bytes == 8192, "Metal sample cap not bound");
    c = config; c.max_sample_bytes = 8ull * 128 * 24 + 16385;
    refused(c, device, CapabilityReason::MemoryBudgetExceeded);
    c = config; c.input_stride_bytes = c.input_bytes = 0;
    refused(c, device, CapabilityReason::ValidButNotImplemented);
    auto d = device; d.metal_max_buffer_length_bytes = 0;
    refused(config, d, CapabilityReason::ValidButNotImplemented);
    d = device; d.metal_recommended_working_set_bytes = 0;
    refused(config, d, CapabilityReason::ValidButNotImplemented);
    c = config; c.width = 200; refused(c, device, CapabilityReason::FormatMathematicallyInvalid);
    c = config; c.max_sample_bytes = 1; refused(c, device, CapabilityReason::MemoryBudgetExceeded);
    c = config; c.input_bytes = 1; refused(c, device, CapabilityReason::FormatMathematicallyInvalid);
    c = config; c.require_reference_oracle = true; refused(c, device, CapabilityReason::ReferenceMissing);
    c = config; c.cpu_memory_budget_bytes = 1; refused(c, device, CapabilityReason::MemoryBudgetExceeded);
    c = config; c.gpu_memory_budget_bytes = 1; refused(c, device, CapabilityReason::MemoryBudgetExceeded);
    d = device; d.metal_recommended_working_set_bytes = 1; refused(config, d, CapabilityReason::MemoryBudgetExceeded);
    d = device; d.metal_max_buffer_length_bytes = config.input_bytes - 1; refused(config, d, CapabilityReason::MemoryBudgetExceeded);
    d = device; d.metal_max_buffer_length_bytes = ~std::uint64_t{0};
    d.metal_recommended_working_set_bytes = ~std::uint64_t{0};
    c = config; c.input_stride_bytes = 178956960; c.input_bytes = c.input_stride_bytes * 24;
    Require(static_cast<bool>(SelectBackend(c, d, factories)), "Metal uint32 extent boundary refused");
    c.input_stride_bytes += 16; c.input_bytes = c.input_stride_bytes * 24;
    refused(c, d, CapabilityReason::MemoryBudgetExceeded);
    c = config; c.input_stride_bytes = 1ull << 32; c.input_bytes = c.input_stride_bytes * 24;
    refused(c, device, CapabilityReason::MemoryBudgetExceeded);
    // Last-row extent is legal, but full padded stride*height overflows.
    // Tiny budgets/limits must not be skipped; fallback may not repair it.
    c = config; c.input_stride_bytes = 768614336404564656ull;
    c.input_bytes = 17678129737304987600ull;
    c.cpu_memory_budget_bytes = c.gpu_memory_budget_bytes = 1;
    d = device; d.metal_max_buffer_length_bytes = d.metal_recommended_working_set_bytes = 1;
    refused(c, d, CapabilityReason::MemoryBudgetExceeded);
    c.backend = SessionBackend::CpuScalar;
    refused(c, d, CapabilityReason::MemoryBudgetExceeded);
    c = config; c.width = 384; c.height = 40; c.input_stride_bytes = 1536; c.input_bytes = 1536 * 40;
    c.allow_cpu_scalar_fallback = false; calls = metal_calls = 0;
    const auto unqualified = SelectBackend(c, device, factories);
    Require(!unqualified && calls == 0 && metal_calls == 0 &&
            unqualified.effective.primary == CapabilityReason::BackendNotQualifiedForGeometry, "Metal geometry promoted");
    c = config; c.input_stride_bytes = c.input_bytes = 0; c.allow_cpu_scalar_fallback = true;
    d = {}; calls = metal_calls = 0;
    const auto fallback = SelectBackend(c, d, factories);
    Require(fallback && calls == 1 && metal_calls == 0 && fallback.frontend->Backend() == FrontendBackend::Cpu,
            "absent Metal inherited GPU requirements or reached Metal factory");
    std::cout << "PASS fake Metal binding/refusal/identity/init falsifiers (no GPU execution)\n";
}
void Contract() {
    EncoderDevice host;
    auto config = Small();
    auto result = SelectBackend(config, host, Count);
    Require(static_cast<bool>(result) && calls == 1 && result.effective.allocation_permitted, "scalar selection");
    Require(result.fallback == ScalarFallbackReason::None && result.frontend->Isa() == WaveletIsa::Scalar,
            "scalar ISA not session-bound");
    for (auto backend : {SessionBackend::Metal, SessionBackend::Cuda, SessionBackend::Vulkan}) {
        config = Small(); config.backend = backend;
        Refused(config, host, CapabilityReason::BackendNotPresent);
        config.allow_cpu_scalar_fallback = true;
        config.cpu_memory_budget_bytes = 1;
        Refused(config, host, CapabilityReason::MemoryBudgetExceeded);
        config.cpu_memory_budget_bytes = ~std::uint64_t{0};
        config.require_reference_oracle = true;
        Refused(config, host, CapabilityReason::ReferenceMissing);
        config.require_reference_oracle = false;
        result = SelectBackend(config, host);
        Require(static_cast<bool>(result) && result.fallback == ScalarFallbackReason::BackendNotPresent &&
                result.effective.effective_backend == SessionBackend::CpuScalar &&
                result.requested.reasons.backend_not_present, "explicit absent-backend fallback");
        auto present = host;
        present.metal_available = present.cuda_available = present.vulkan_available = true;
        if (backend == SessionBackend::Metal) {
            config.allow_cpu_scalar_fallback = true;
            Refused(config, present, CapabilityReason::ValidButNotImplemented);
            continue;
        }
        config.allow_cpu_scalar_fallback = false;
        Refused(config, present, CapabilityReason::BackendNotQualifiedForGeometry);
        config.allow_cpu_scalar_fallback = true;
        result = SelectBackend(config, present);
        Require(static_cast<bool>(result) && result.fallback == ScalarFallbackReason::BackendNotQualified &&
                result.requested.reasons.backend_not_qualified_for_geometry &&
                result.effective.effective_backend == SessionBackend::CpuScalar, "explicit unqualified fallback");
        config.cpu_memory_budget_bytes = 1;
        Refused(config, present, CapabilityReason::MemoryBudgetExceeded);
        config = Small(); config.backend = backend; config.allow_cpu_scalar_fallback = true;
        config.require_reference_oracle = true;
        Refused(config, present, CapabilityReason::ReferenceMissing);
        config.require_reference_oracle = false; config.gpu_memory_budget_bytes = 1;
        Refused(config, present, CapabilityReason::MemoryBudgetExceeded);
    }
    config = Small(); config.backend = static_cast<SessionBackend>(999); config.allow_cpu_scalar_fallback = true;
    Refused(config, host, CapabilityReason::FormatMathematicallyInvalid);
    config = Small(); config.backend = SessionBackend::CpuSimd;
    Refused(config, host, CapabilityReason::ValidButNotImplemented);
    config.allow_cpu_scalar_fallback = true;
    Require(static_cast<bool>(SelectBackend(config, host)), "absent SIMD explicit fallback");
    auto simd = host; simd.cpu_simd_selectable = true;
    Refused(config, simd, CapabilityReason::ValidButNotImplemented);
    config = Small(); auto absent = host; absent.cpu_scalar_available = false;
    Refused(config, absent, CapabilityReason::BackendNotPresent);
    config.backend = SessionBackend::Metal; config.allow_cpu_scalar_fallback = true;
    Refused(config, absent, CapabilityReason::BackendNotPresent);
    config = Small(); config.allow_cpu_scalar_fallback = true;
    config.backend = SessionBackend::Metal; config.width = 200;
    Refused(config, host, CapabilityReason::FormatMathematicallyInvalid);
    config = Small(); config.width = std::numeric_limits<std::int64_t>::max(); config.height = 128;
    Refused(config, host, CapabilityReason::FormatMathematicallyInvalid);
    config = Small(); config.width = 4096; config.height = 2024;
    Refused(config, host, CapabilityReason::BackendNotQualifiedForGeometry);
    config.height = 2160;
    Refused(config, host, CapabilityReason::MemoryBudgetExceeded);
    config = Small(); config.input_stride_bytes = 512; config.input_bytes = 1;
    Refused(config, host, CapabilityReason::FormatMathematicallyInvalid);
    config = Small(); config.max_sample_bytes = 1;
    Refused(config, host, CapabilityReason::MemoryBudgetExceeded);
    config = Small(); config.cpu_memory_budget_bytes = 1;
    Refused(config, host, CapabilityReason::MemoryBudgetExceeded);
    config = Small(); config.require_reference_oracle = true;
    Refused(config, host, CapabilityReason::ReferenceMissing);
    config = Small(); config.frame_rate = {24,1};
    Refused(config, host, CapabilityReason::ValidButNotImplemented);
    config = Small(); config.quality = hypercine::EncodingQuality::Medium;
    Refused(config, host, CapabilityReason::ValidButNotImplemented);
    config = Small(); config.codec = CodecFamily::LegacyFixedQ2;
    Refused(config, host, CapabilityReason::ValidButNotImplemented);
    config = Small(); config.pixel_format = SourcePixelFormat::Yuy2;
    Refused(config, host, CapabilityReason::ValidButNotImplemented);
    config = Small(); config.diagnostic = DiagnosticMode::EmbedHashesInSample;
    Refused(config, host, CapabilityReason::ValidButNotImplemented);
    for (auto factory : {ScalarFrontendFactory{nullptr}, Missing, WrongGeometry, WrongCapacity, WrongIsa}) {
        result = SelectBackend(Small(), host, factory);
        Require(!result && result.status == BackendSelectionStatus::InitializationFailed && !result.frontend,
                "bad factory did not produce typed initialization refusal");
    }
    bool propagated = false;
    try { (void)SelectBackend(Small(), host, NoMemory); } catch (const std::bad_alloc&) { propagated = true; }
    Require(propagated, "bad_alloc was hidden as availability failure");
    propagated = false;
    try { (void)SelectBackend(Small(), host, Logic); } catch (const std::logic_error&) { propagated = true; }
    Require(propagated, "logic error was hidden as availability failure");
    Require(kMaxPixels == 2097152u, "legacy fixed-Q2 limit changed");
    std::cout << "PASS typed refusals, no allocation before permission, explicit fallback and factory failures\n";
}
void Equal(const FilmScan3Frame& a, const FilmScan3Frame& b) {
    Require(a.sample == b.sample && a.tables.quant == b.tables.quant && a.tables.scale == b.tables.scale &&
            a.state.frame_index == b.state.frame_index && a.state.fs_rate_limiter == b.state.fs_rate_limiter &&
            a.state.previous_sample_bytes == b.state.previous_sample_bytes &&
            FilmScan3FullCoefficientHashesEqual(a,b), "shared writer packet/state/table/hash changed");
}
void Sequence(int width, int height, unsigned frames) {
    auto config = Small(); config.width = width; config.height = height;
    config.input_stride_bytes = static_cast<std::size_t>(width)*4+64;
    config.input_bytes = config.input_stride_bytes*height;
    config.max_sample_bytes = 8u*static_cast<std::size_t>(width)*height+16384u;
    EncoderDevice device;
    EncoderSession session(config, device);
    FilmScan3Encoder legacy({width,height,static_cast<std::size_t>(config.max_sample_bytes)}, WaveletIsa::Scalar);
    AlignedVector<uint8_t> input(static_cast<std::size_t>(config.input_bytes), 0);
    const auto stride = static_cast<std::size_t>(config.input_stride_bytes);
    // Snapshot is independent of subsequent caller config/device changes.
    config.width = 200; config.input_stride_bytes = 1; config.diagnostic = DiagnosticMode::None;
    device.cpu_scalar_available = false;
    Require(session.geometry().width == width && session.wavelet_isa() == WaveletIsa::Scalar,
            "session did not freeze geometry/ISA");
    for (unsigned frame=0;frame<frames;++frame) {
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            auto* pixel=input.data()+static_cast<std::size_t>(y)*stride+4u*x;
            pixel[0]=static_cast<uint8_t>(x+frame*17); pixel[1]=static_cast<uint8_t>(y+frame*31);
            pixel[2]=static_cast<uint8_t>((x^y)+frame*7); pixel[3]=255;
        }
        bool failed=false;
        try { (void)session.Encode(input.data(),input.size(),stride-16); }
        catch(const std::invalid_argument&) { failed=true; }
        Require(failed, "bound stride mismatch accepted");
        failed=false;
        try { (void)session.Encode(input.data()+1,input.size(),stride); }
        catch(const std::invalid_argument&) { failed=true; }
        Require(failed, "invalid storage accepted");
        Equal(session.Encode(input.data(),input.size(),stride),legacy.Encode(input.data(),input.size(),stride));
    }
    session.Reset(); legacy.Reset();
    Equal(session.Encode(input.data(),input.size(),stride),legacy.Encode(input.data(),input.size(),stride));
    auto fallback_config = session.config(); fallback_config.backend = SessionBackend::Metal;
    fallback_config.allow_cpu_scalar_fallback = true;
    EncoderSession fallback(fallback_config, EncoderDevice{});
    legacy.Reset();
    Equal(fallback.Encode(input.data(),input.size(),stride),legacy.Encode(input.data(),input.size(),stride));
    auto off_config = session.config(); off_config.diagnostic = DiagnosticMode::None;
    EncoderSession off(off_config, EncoderDevice{});
    session.Reset();
    std::promise<void> start;
    const auto ready=start.get_future().share();
    auto on_future=std::async(std::launch::async,[&]{
        ready.wait(); return session.Encode(input.data(),input.size(),stride);
    });
    auto off_future=std::async(std::launch::async,[&]{
        ready.wait(); return off.Encode(input.data(),input.size(),stride);
    });
    start.set_value();
    const auto on_frame=on_future.get();
    const auto off_frame=off_future.get();
    legacy.Reset();
    Equal(on_frame,legacy.Encode(input.data(),input.size(),stride));
    Require(on_frame.sample == off_frame.sample && off_frame.coefficient_hash == FilmScan3CoefficientHash::DiagnosticOff,
            "concurrent diagnostic session policy changed sample or was ignored");
    std::cout << "PASS session " << width << 'x' << height << " frames=" << frames
              << " padded storage/reset/fallback/hash policy/full packet-state-table-30hash parity\n";
}
void Packets() {
    auto first=std::async(std::launch::async,[]{Sequence(128,24,16);});
    auto second=std::async(std::launch::async,[]{Sequence(256,256,16);});
    first.get(); second.get();
    Sequence(1280,720,2);
    Sequence(2048,1080,2);
}
}
int main(int argc, char** argv) {
    try {
        if(argc != 2) throw std::invalid_argument("expected contract or packets");
        const std::string mode=argv[1];
        if(mode == "contract") { Contract(); MetalContract(); }
        else if(mode == "packets") Packets();
        else throw std::invalid_argument("unknown test mode");
        return 0;
    } catch(const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
