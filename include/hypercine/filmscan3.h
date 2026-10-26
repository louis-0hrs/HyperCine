// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/standalone.h"
#include <memory>

namespace hypercine { namespace standalone {
// Separate from the legacy fixed-Q2 profile. This initial profile is restricted
// to BGRA8 -> progressive YUV422 p10, GOP1. Existing constructors remain 1080p.
// Explicit CPU session geometry. Fixed for the session, including across Reset.
// 4K is experimental pending original-SDK reference qualification; 8K rejected.
struct FilmScan3Geometry {
    int width = 1920, height = 1080;
    // Zero selects the geometric bound; nonzero may only reduce that bound.
    std::size_t sample_capacity_bytes = 0;
};
void ValidateFilmScan3Geometry(FilmScan3Geometry geometry);
struct FilmScan3Tables { std::array<unsigned,36> quant{}, scale{}; };
struct FilmScan3State {
    unsigned frame_index = 0;
    int fs_rate_limiter = 4;
    std::size_t previous_sample_bytes = 0;
};
// Optional explicit variable metadata injection. The encoder always constructs
// its own records, tags, padding and UFRM. No reference sample/header is accepted.
struct FilmScan3Metadata {
    std::array<uint8_t,16> guid{{0x13,0x37,0xbe,0xef,0x20,0x26,0x40,0x20,0x8a,0xfe,0xba,0xbe,0x43,0x46,0x48,0x44}};
    std::array<char,10> date{{'2','0','2','6','-','0','1','-','0','1'}};
    std::array<char,8> time{{'0','0',':','0','0',':','0','0'}};
    std::array<char,11> timecode{{'0','0',':','0','0',':','0','0',':','0','0'}};
};
struct FilmScan3Frame {
    std::vector<uint8_t> sample;
    FilmScan3Tables tables;
    FilmScan3State state; // state used for this frame, before completion
    std::array<uint64_t,30> coefficient_fnv64{};
};
// Independently callable CPU mathematics for stage tests/future backends.
Planes ConvertFilmScan3Bgra(const uint8_t* data, std::size_t bytes, int width, int height, std::size_t stride);
int16_t QuantizeFilmScan3(int16_t value, unsigned divisor);
// Quantises a contiguous block in place. Same rule as QuantizeFilmScan3 for
// every element, including the `divisor == 1` identity. Exists so the hot loop
// can use the vector path; a scalar build falls through to the single-value
// function, so there is exactly one rounding rule in the codebase.
void QuantizeFilmScan3Block(int16_t* data, std::size_t count, unsigned divisor);
FilmScan3Tables ComputeFilmScan3Tables(int fs_rate_limiter);
// Metal requires an enabled Apple build and fails if device/pipeline creation fails.
// MetalOrCpu falls back only during initialization; dispatch failures propagate.
enum class FilmScan3Backend { Cpu, Metal, MetalOrCpu };
namespace detail { struct Frontend; struct FrontendAccess; }
class FilmScan3Encoder {
    friend struct detail::FrontendAccess;
    std::shared_ptr<detail::Frontend> frontend_;
    FilmScan3State state_;
public:
    FilmScan3Encoder() = default; // scalar CPU remains the default
    explicit FilmScan3Encoder(FilmScan3Backend backend);
    explicit FilmScan3Encoder(FilmScan3Geometry geometry); // CPU only
    // Explicit backend *and* geometry. An explicitly requested Metal backend is
    // not allowed to fall back to CPU; only MetalOrCpu may, and only during
    // initialization, in which case the CPU fallback keeps this geometry.
    FilmScan3Encoder(FilmScan3Backend backend, FilmScan3Geometry geometry);
    FilmScan3Geometry geometry() const noexcept;
    FilmScan3Frame Encode(const uint8_t* data, std::size_t bytes, std::size_t stride);
    FilmScan3Frame Encode(const uint8_t* data, std::size_t bytes, std::size_t stride,
                         const FilmScan3Metadata& metadata);
    const FilmScan3State& state() const noexcept { return state_; }
    void Reset() noexcept { state_ = FilmScan3State{}; }
};
}}
