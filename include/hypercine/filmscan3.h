// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/standalone.h"

namespace hypercine { namespace standalone {
// Separate from the legacy fixed-Q2 profile. This initial profile is restricted
// to the corpus-backed 1920x1080 BGRA8 -> progressive YUV422 p10, GOP1 domain.
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
FilmScan3Tables ComputeFilmScan3Tables(int fs_rate_limiter);
class FilmScan3Encoder {
    FilmScan3State state_;
public:
    FilmScan3Frame Encode(const uint8_t* data, std::size_t bytes, std::size_t stride);
    FilmScan3Frame Encode(const uint8_t* data, std::size_t bytes, std::size_t stride,
                         const FilmScan3Metadata& metadata);
    const FilmScan3State& state() const noexcept { return state_; }
    void Reset() noexcept { state_ = FilmScan3State{}; }
};
}}
