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
// Internal stage boundary. Implementations throw before returning incomplete data.
struct Frontend {
    virtual ~Frontend() = default;
    virtual Planes Convert(const uint8_t*, std::size_t, int, int, std::size_t) = 0;
    virtual WaveletLevel Forward(const Plane&, int) = 0;
    virtual std::string DeviceName() const = 0;
};
struct MetalTiming {
    double color_gpu_ms=0, wavelet_gpu_ms=0, upload_ms=0, readback_ms=0;
    double submit_ms=0, wait_ms=0;
    std::size_t peak_buffer_bytes=0, peak_device_allocated_bytes=0;
    unsigned color_dispatches=0, wavelet_dispatches=0;
};
std::shared_ptr<Frontend> CpuFrontend();
std::shared_ptr<Frontend> MetalFrontend(); // throws if disabled/device/pipeline unavailable
MetalTiming ReadMetalTiming(const Frontend&);
// Failure injection for deterministic fallback tests; no process-global switch.
using MetalFactory = std::shared_ptr<Frontend>(*)();
std::shared_ptr<Frontend> SelectFrontend(FilmScan3Backend, MetalFactory = MetalFrontend);
struct FrontendAccess {
    static void Use(FilmScan3Encoder& encoder, std::shared_ptr<Frontend> frontend) {
        if (!frontend) throw std::invalid_argument("null FilmScan3 frontend");
        encoder.frontend_=std::move(frontend);
    }
};
void ValidateFilmScan3Input(const uint8_t*, std::size_t, int, int, std::size_t);
void ValidateWaveletInput(const Plane&, int);
}}}
