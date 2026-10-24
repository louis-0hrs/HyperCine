// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "decode_fieldplus.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace HyperCine;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

uint16_t ReadBE16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>((uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
}

uint16_t AbsTag(uint16_t raw) {
    return raw < 0x8000u ? raw : static_cast<uint16_t>(0x10000u - raw);
}

size_t FindHeaderTag(const std::vector<uint8_t>& bytes, uint16_t wanted, size_t end) {
    size_t pos = 0;
    while (pos + 4 <= end) {
        const uint16_t tag = AbsTag(ReadBE16(bytes, pos));
        const uint16_t value = ReadBE16(bytes, pos + 2);
        if (tag == wanted) return pos;
        if (tag == 2) {
            pos += 4 + size_t(value) * 4;
            continue;
        }
        if ((tag & 0xff00u) == 0x2000u || (tag >= 0x4000u && tag < 0x6000u)) break;
        pos += 4;
    }
    throw std::runtime_error("header tag not found");
}

size_t FindPreviousTag(const std::vector<uint8_t>& bytes, size_t before, uint16_t wanted) {
    size_t pos = before;
    while (pos >= 4) {
        pos -= 4;
        if (AbsTag(ReadBE16(bytes, pos)) == wanted) return pos;
    }
    throw std::runtime_error("preceding tag not found");
}

void SetValue(std::vector<uint8_t>& bytes, size_t tag_offset, uint16_t value) {
    bytes[tag_offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[tag_offset + 3] = static_cast<uint8_t>(value);
}

void SetTag(std::vector<uint8_t>& bytes, size_t tag_offset, uint16_t tag) {
    bytes[tag_offset] = static_cast<uint8_t>(tag >> 8);
    bytes[tag_offset + 1] = static_cast<uint8_t>(tag);
}

std::vector<uint8_t> Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    Require(bool(in), "fixture open");
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    Require(length > 0, "fixture size");
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    in.read(reinterpret_cast<char*>(bytes.data()), length);
    Require(bool(in), "fixture read");
    return bytes;
}

void CheckFrozenMapping(const FieldPlusSampleDescriptor& parsed) {
    static constexpr int owners[17] = {5,5,5,5,4,4,4,3,3,3,3,1,1,1,0,0,0};
    static constexpr int bands[17] = {0,1,2,3,1,2,3,0,1,2,3,1,2,3,1,2,3};
    static constexpr int divisors[17] = {8,8,8,8,4,4,4,4,4,4,4,2,2,2,2,2,2};
    static constexpr FieldPlusTemporalRole roles[17] = {
        FieldPlusTemporalRole::GopLow, FieldPlusTemporalRole::GopLow,
        FieldPlusTemporalRole::GopLow, FieldPlusTemporalRole::GopLow,
        FieldPlusTemporalRole::GopLow, FieldPlusTemporalRole::GopLow,
        FieldPlusTemporalRole::GopLow, FieldPlusTemporalRole::GopHigh,
        FieldPlusTemporalRole::GopHigh, FieldPlusTemporalRole::GopHigh,
        FieldPlusTemporalRole::GopHigh, FieldPlusTemporalRole::Frame1Detail,
        FieldPlusTemporalRole::Frame1Detail, FieldPlusTemporalRole::Frame1Detail,
        FieldPlusTemporalRole::Frame0Detail, FieldPlusTemporalRole::Frame0Detail,
        FieldPlusTemporalRole::Frame0Detail
    };

    for (int ch = 0; ch < kFieldPlusChannelCount; ++ch) {
        for (int local = 0; local < kFieldPlusChunksPerChannel; ++local) {
            const int physical = ch * kFieldPlusChunksPerChannel + local;
            const auto& chunk = parsed.chunks[size_t(physical)];
            Require(chunk.physical_index == physical, "physical chunk index");
            Require(chunk.channel == ch && chunk.channel_chunk_index == local, "channel chunk ordering");
            if (local == 11) {
                Require(chunk.kind == FieldPlusChunkKind::TemporalHighControl, "control chunk kind");
                Require(chunk.logical_subband == 255, "control chunk logical index");
                Require(chunk.chunk_bytes == 8 && chunk.payload_bytes == 0, "control chunk has no entropy payload");
                Require(chunk.wavelet_owner == 2 && chunk.band_owner == 1, "control chunk target");
                Require(parsed.control_chunk_indices[size_t(ch)] == physical, "control index table");
                continue;
            }
            const int logical = local < 11 ? local : local - 1;
            const auto& band = parsed.bands[size_t(ch * kFieldPlusLogicalBandsPerChannel + logical)];
            const auto& d = band.entropy;
            Require(chunk.kind == FieldPlusChunkKind::Coefficients, "coefficient chunk kind");
            Require(chunk.logical_subband == logical, "logical/physical mapping");
            Require(band.logical_subband == logical && band.physical_chunk == physical, "band descriptor order");
            Require(band.wavelet_owner == owners[logical] && band.band_owner == bands[logical], "wavelet/band ownership");
            Require(band.temporal_role == roles[logical], "temporal role mapping");
            Require(d.channel == ch && d.subband == logical, "entropy routing identity");
            if (logical == 0) {
                Require(d.method == BandEncodingMethod::LowPass16S, "logical subband 0 raw lowpass method");
            } else if (logical == 7) {
                Require(d.method == BandEncodingMethod::Raw16, "logical subband 7 raw16 method");
                Require(d.payload_bytes == size_t(d.width) * size_t(d.height) * sizeof(int16_t),
                    "raw16 payload is exactly the coefficient plane");
            } else {
                Require(d.method == BandEncodingMethod::RunLengthVLC, "remaining coefficient bands use VLC");
            }
            const int channel_width = ch == 0 ? parsed.width : parsed.width / 2;
            Require(d.width == channel_width / divisors[logical], "band width geometry");
            Require(d.height == parsed.height / divisors[logical], "band height geometry");
            Require(chunk.payload_offset == d.payload_offset && chunk.payload_bytes == d.payload_bytes,
                "chunk payload view matches entropy descriptor");
        }
    }
}

void CheckRejectsMalformed(const std::vector<uint8_t>& sample, size_t control_chunk_offset) {
    auto mutated = sample;
    // The subband-size value is measured in 4-byte units. Expanding the empty
    // control from 8 to 12 bytes must fail instead of becoming entropy input.
    Require(control_chunk_offset + 4 <= mutated.size(), "control size field bounds");
    mutated[control_chunk_offset + 2] = 0;
    mutated[control_chunk_offset + 3] = 3;
    FieldPlusSampleDescriptor ignored;
    DecodeError status = DecodeError::None;
    Require(!ParseProgressiveFieldPlusSampleValidated(mutated.data(), mutated.size(), 24, ignored, nullptr, &status),
        "non-empty control chunk rejection");
    Require(status == DecodeError::InvalidSample, "malformed control error status");

    status = DecodeError::None;
    Require(!ParseProgressiveFieldPlusSampleValidated(sample.data(), sample.size() - 1, 24, ignored, nullptr, &status),
        "truncated sample rejection");
}

void CheckRejectsFieldPlusMalformed(const std::vector<uint8_t>& sample,
                                    const FieldPlusSampleDescriptor& parsed) {
    auto reject = [&](std::vector<uint8_t> mutated, const char* what,
                      DecodeError expected = DecodeError::InvalidSample) {
        FieldPlusSampleDescriptor ignored;
        DecodeError status = DecodeError::None;
        const bool ok = ParseProgressiveFieldPlusSampleValidated(
            mutated.data(), mutated.size(), 24, ignored, nullptr, &status);
        Require(!ok, what);
        Require(status == expected, "malformed FIELDPLUS error class");
    };

    // Missing control marker, duplicate/mispositioned index=255, and bad size.
    auto missing_control = sample;
    SetTag(missing_control, parsed.chunks[11].chunk_offset + 4, 4);
    SetValue(missing_control, parsed.chunks[11].chunk_offset + 4, 0x0f0f);
    reject(std::move(missing_control), "missing control chunk rejection");

    auto duplicate_control = sample;
    const size_t sb10 = FindPreviousTag(duplicate_control, parsed.chunks[10].chunk_offset, 51);
    SetValue(duplicate_control, sb10, 255);
    reject(std::move(duplicate_control), "duplicate/mispositioned control rejection");

    auto bad_control_target = sample;
    const size_t control_subband = FindPreviousTag(bad_control_target, parsed.chunks[11].chunk_offset, 51);
    SetValue(bad_control_target, control_subband, 10);
    reject(std::move(bad_control_target), "invalid control target rejection");

    auto bad_payload_bounds = sample;
    bad_payload_bounds[parsed.chunks[8].chunk_offset + 2] = 0x7f;
    bad_payload_bounds[parsed.chunks[8].chunk_offset + 3] = 0xff;
    reject(std::move(bad_payload_bounds), "out-of-bounds chunk rejection");

    auto wrong_logical_index = sample;
    const size_t sb8 = FindPreviousTag(wrong_logical_index, parsed.chunks[8].chunk_offset, 51);
    SetValue(wrong_logical_index, sb8, 7);
    reject(std::move(wrong_logical_index), "wrong logical subband rejection");

    auto wrong_wavelet_owner = sample;
    const size_t wavelet8 = FindPreviousTag(wrong_wavelet_owner, parsed.chunks[8].chunk_offset, 38);
    SetValue(wrong_wavelet_owner, wavelet8, 5);
    reject(std::move(wrong_wavelet_owner), "wrong wavelet owner rejection");

    auto wrong_count = sample;
    const auto& last = parsed.chunks.back();
    const size_t last_end = last.chunk_offset + 4 + last.chunk_bytes;
    Require(last_end <= wrong_count.size(), "last physical chunk bounds");
    wrong_count.erase(wrong_count.begin() + static_cast<std::ptrdiff_t>(last.chunk_offset),
        wrong_count.begin() + static_cast<std::ptrdiff_t>(last_end));
    reject(std::move(wrong_count), "wrong physical chunk count rejection");

    const size_t header_end = parsed.chunks[0].chunk_offset;
    for (const auto& item : std::array<std::pair<uint16_t, uint16_t>, 4>{{
            {11, 1}, {10, 0}, {70, 12}, {12, 2}}}) {
        auto changed_header = sample;
        SetValue(changed_header, FindHeaderTag(changed_header, item.first, header_end), item.second);
        reject(std::move(changed_header), "inconsistent FIELDPLUS header rejection",
            DecodeError::UnsupportedEncodingMode);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: test_decode_phase3b2a_parser <repo-root>");
        const std::filesystem::path root(argv[1]);
        const auto sample = Load(root / "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd");
        FieldPlusSampleDescriptor parsed;
        std::string error;
        DecodeError status = DecodeError::InvalidSample;
        const bool parsed_ok = ParseProgressiveFieldPlusSampleValidated(sample.data(), sample.size(), 24, parsed, &error, &status);
        Require(parsed_ok, error.empty() ? "progressive FIELDPLUS parse" : error.c_str());
        Require(status == DecodeError::None, "successful parser status");
        Require(parsed.width == 64 && parsed.height == 64 && parsed.coded_precision == 10, "profile geometry and precision");
        Require(parsed.channel_count == 3 && parsed.frame_count == 2 && parsed.transform_type == 2, "profile topology");
        Require(parsed.wavelet_count == 6 && parsed.logical_subbands_per_channel == 17 && parsed.progressive,
            "progressive FIELDPLUS profile flags");
        CheckFrozenMapping(parsed);
        CheckRejectsMalformed(sample, parsed.chunks[11].chunk_offset);
        CheckRejectsFieldPlusMalformed(sample, parsed);

        const auto interlaced = Load(root / "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-field-f2-bands54-fmt-1.cfhd");
        status = DecodeError::None;
        Require(!ParseProgressiveFieldPlusSampleValidated(interlaced.data(), interlaced.size(), 24, parsed, nullptr, &status),
            "interlaced FIELDPLUS remains unsupported");
        Require(status == DecodeError::UnsupportedEncodingMode, "interlaced FIELDPLUS error status");

        std::cout << "progressive FIELDPLUS parser: 54 physical chunks, 51 coefficient bands, 3 controls passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
