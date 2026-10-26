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
// Diagnostic side channel only. These 30 FNV-64 values are not written into
// the CFHD sample and do not feed rate control. Absent is the default of a
// frame that Encode did not produce. Full means every slot was hashed.
// DiagnosticOff means this session explicitly skipped hashing: the array then
// holds nonzero tombstones, never a silent all-zero fingerprint.
enum class FilmScan3CoefficientHash : uint8_t { Absent = 0, Full = 1, DiagnosticOff = 2 };
struct FilmScan3Frame {
    std::vector<uint8_t> sample;
    FilmScan3Tables tables;
    FilmScan3State state; // state used for this frame, before completion
    std::array<uint64_t,30> coefficient_fnv64{};
    FilmScan3CoefficientHash coefficient_hash = FilmScan3CoefficientHash::Absent;
};
// Bit-exact gates must call this. Equal zero arrays, or equal tombstones, are
// not a pass: both sides have to declare Full before the 30 values are compared.
inline bool FilmScan3FullCoefficientHashesEqual(const FilmScan3Frame& a, const FilmScan3Frame& b) noexcept {
    return a.coefficient_hash == FilmScan3CoefficientHash::Full
        && b.coefficient_hash == FilmScan3CoefficientHash::Full
        && a.coefficient_fnv64 == b.coefficient_fnv64;
}
inline const char* FilmScan3CoefficientHashName(FilmScan3CoefficientHash hash) noexcept {
    switch (hash) {
    case FilmScan3CoefficientHash::Full: return "full";
    case FilmScan3CoefficientHash::DiagnosticOff: return "diagnostic_off";
    case FilmScan3CoefficientHash::Absent: return "absent";
    }
    return "absent";
}
// Independently callable CPU mathematics for stage tests/future backends.
Planes ConvertFilmScan3Bgra(const uint8_t* data, std::size_t bytes, int width, int height, std::size_t stride);
int16_t QuantizeFilmScan3(int16_t value, unsigned divisor);
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
    // Per session, not process-global. Default is full hashing so existing
    // bit-exact gates keep real fingerprints. false skips the 30 hashes for
    // later frames and marks them DiagnosticOff. Sample bytes, quantization,
    // entropy, rate state and metadata are unchanged. DiagnosticOff is stamped
    // only after the sample is finalized, so a failed Encode does not consume
    // a tombstone serial or mint a hash session.
    void SetCoefficientHashing(bool enabled) noexcept;
    bool coefficient_hashing() const noexcept { return coefficient_hashing_; }
private:
    void StampDiagnosticOff(FilmScan3Frame& result);
    bool coefficient_hashing_ = true;
    uint32_t hash_session_ = 0;
    uint32_t diagnostic_serial_ = 1;
};
}}
