// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/filmscan3.h"
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
namespace hypercine { namespace standalone { namespace detail {
struct FrontendFrame {
    // Tiny valid placeholders avoid changing the portable Plane allocation API.
    Planes color{Plane(1,1),Plane(1,1),Plane(1,1)};
    using Channel = std::array<WaveletLevel,3>;
    static WaveletLevel EmptyLevel() { return {{Plane(1,1),Plane(1,1),Plane(1,1),Plane(1,1)}}; }
    static Channel EmptyChannel() { return {EmptyLevel(),EmptyLevel(),EmptyLevel()}; }
    std::array<Channel,3> levels{EmptyChannel(),EmptyChannel(),EmptyChannel()};
};
struct HostTiming {
    double quant_ms=0, coefficient_hash_ms=0, entropy_ms=0, framing_ms=0;
};
// Internal stage boundary. Implementations throw before returning incomplete data.
enum class FrontendBackend { Unknown, Cpu, Metal, Cuda };
struct Frontend {
    virtual ~Frontend() = default;
    // Internal identity is checked by session selection, independently of ISA.
    virtual FrontendBackend Backend() const noexcept { return FrontendBackend::Unknown; }
    virtual FilmScan3Geometry Geometry() const noexcept { return {}; }
    virtual Planes Convert(const uint8_t*, std::size_t, int, int, std::size_t) = 0;
    virtual WaveletLevel Forward(const Plane&, int) = 0;
    virtual std::string DeviceName() const = 0;
    // Wavelet ISA this session actually dispatches to, resolved once at
    // construction. A caller can read it instead of inferring the kernel from a
    // timing measurement, which cannot work because both kernels are bit-exact.
    virtual WaveletIsa Isa() const noexcept { return WaveletIsa::Auto; }
    // Full audit retains color and intermediate LL storage. Production consumers
    // require only L3 LL + nine highpass bands per channel; other LL layouts remain.
    virtual FrontendFrame Transform(const uint8_t*, std::size_t, int, int, std::size_t, bool audit=false);
    virtual bool ProfileHost() const { return false; }
    virtual void RecordHostTiming(const HostTiming&) {}
};
struct MetalTiming {
    double color_gpu_ms=0, wavelet_gpu_ms=0, upload_ms=0, readback_ms=0;
    double submit_ms=0, wait_ms=0;
    std::size_t peak_buffer_bytes=0, peak_device_allocated_bytes=0;
    unsigned color_dispatches=0, wavelet_dispatches=0;
    unsigned command_buffers=0, waits=0, buffer_allocations=0, upload_copies=0, readback_copies=0;
    std::size_t uploaded_bytes=0, readback_bytes=0;
    double allocation_ms=0, batch_gpu_ms=0;
    HostTiming host;
};
std::shared_ptr<Frontend> CpuFrontend();
std::shared_ptr<Frontend> CpuFrontend(FilmScan3Geometry);
// Session-bound ISA policy: the requested value is resolved exactly once here, so
// the session keeps its kernel even if the process-global NEON default changes.
std::shared_ptr<Frontend> CpuFrontend(FilmScan3Geometry, WaveletIsa);
// Internal test modes exercise the parity gate itself; neither is a production
// selection API and both must make the complete-sample comparison fail.
enum class CudaTestMode { Normal, SkipKernel, AlterOutput };
std::shared_ptr<Frontend> CudaFrontend(FilmScan3Geometry, CudaTestMode = CudaTestMode::Normal);
// Geometry-only production entry to the same CUDA frontend. Keeps fault-injection
// modes out of the session factory table; throws when CUDA is not built.
std::shared_ptr<Frontend> CudaSessionFrontend(FilmScan3Geometry);
std::shared_ptr<Frontend> MetalFrontend(); // throws if disabled/device/pipeline unavailable
// Geometry-carrying Metal frontend. Same failure contract: it throws rather than
// degrading, so an explicitly requested Metal backend cannot silently become CPU.
std::shared_ptr<Frontend> MetalFrontend(FilmScan3Geometry);
MetalTiming ReadMetalTiming(const Frontend&);
// One-shot deterministic failure *after real GPU completion*, before readback.
// This tests state rollback/reuse without submitting unsafe commands to the GPU.
void FailNextMetalCompletion(Frontend&);
// Failure injection for deterministic fallback tests; no process-global switch.
using MetalFactory = std::shared_ptr<Frontend>(*)();
std::shared_ptr<Frontend> SelectFrontend(FilmScan3Backend, MetalFactory = MetalFrontend);
// Geometry-aware selection. MetalOrCpu keeps the requested geometry in its CPU
// fallback instead of reverting to the 1080p default.
std::shared_ptr<Frontend> SelectFrontend(FilmScan3Backend, FilmScan3Geometry);
struct FrontendAccess {
    static void Use(FilmScan3Encoder& encoder, std::shared_ptr<Frontend> frontend) {
        if (!frontend) throw std::invalid_argument("null FilmScan3 frontend");
        encoder.frontend_=std::move(frontend);
    }
    // Test seam for tombstone identity. Not a production control: the public
    // gate still rejects anything that is not Full on both sides.
    static uint32_t DiagnosticSerial(const FilmScan3Encoder& encoder) noexcept { return encoder.diagnostic_serial_; }
    static uint32_t HashSession(const FilmScan3Encoder& encoder) noexcept { return encoder.hash_session_; }
    static void SetDiagnosticSerial(FilmScan3Encoder& encoder, uint32_t serial) noexcept { encoder.diagnostic_serial_ = serial; }
    static void SetHashSession(FilmScan3Encoder& encoder, uint32_t session) noexcept { encoder.hash_session_ = session; }
};
void ValidateFilmScan3Input(const uint8_t*, std::size_t, int, int, std::size_t);
void ValidateWaveletInput(const Plane&, int);
}}}
