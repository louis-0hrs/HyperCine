// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #46 Metal half: same 1920x1080 frames 0..3 as the CUDA profile matrix.
// Compares an explicit Metal encoder with an explicit CPU scalar encoder.
// This measures packets. It does not admit a profile; ProfileAdmitted stays
// fail-closed until a green run of this binary is recorded.
// Not a product qualification and not an original-SDK or SMPTE claim.
#include "../src/core/encoder_backend_selection.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace hypercine;
using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;

namespace {
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr unsigned kFrames = 4;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::string Hex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string Join(const std::array<unsigned, 36>& table) {
    std::ostringstream out;
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (i) out << ',';
        out << table[i];
    }
    return out.str();
}

std::string JoinHashes(const std::array<std::uint64_t, 30>& hashes) {
    std::ostringstream out;
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        if (i) out << ',';
        out << Hex(hashes[i]);
    }
    return out.str();
}

unsigned HeaderQualityStamp(const std::vector<std::uint8_t>& sample) {
    std::size_t at = 0;
    while (at + 4 <= sample.size()) {
        const int tag = static_cast<std::int16_t>((sample[at] << 8) | sample[at + 1]);
        const unsigned value = (static_cast<unsigned>(sample[at + 2]) << 8) | sample[at + 3];
        if (tag == -80) return value;
        if (tag < 0) {
            const int code = -tag;
            const int group = code & 0xff00;
            if (group == 0x2000) {
                at += 4 + static_cast<std::size_t>(((code & 255) << 16) | value) * 4;
                continue;
            }
            if (group == 0x4000) {
                at += 4 + static_cast<std::size_t>(value) * 4;
                continue;
            }
            if (group == 0x2100 || group == 0x2200) {
                at += 4;
                continue;
            }
        }
        at += 4;
    }
    throw std::runtime_error("emitted packet carries no quality stamp");
}

void CompareFrame(const FilmScan3Frame& cpu, const FilmScan3Frame& metal, unsigned frame,
                   std::size_t carried, EncodingQuality quality) {
    const auto name = std::string(EncodingQualityName(quality)) + " frame " + std::to_string(frame);
    Require(cpu.state.frame_index == frame && metal.state.frame_index == frame,
            name + " frame index is not the requested index");
    Require(cpu.state.previous_sample_bytes == carried && metal.state.previous_sample_bytes == carried,
            name + " previous_sample_bytes broke the chain");
    Require(cpu.state.fs_rate_limiter == metal.state.fs_rate_limiter, name + " fs_rate_limiter differs");
    Require(cpu.state.fs_overbitrate == metal.state.fs_overbitrate, name + " fs_overbitrate differs");
    for (std::size_t slot = 0; slot < 36; ++slot) {
        Require(cpu.tables.quant[slot] == metal.tables.quant[slot], name + " quant slot " + std::to_string(slot));
        Require(cpu.tables.scale[slot] == metal.tables.scale[slot], name + " scale slot " + std::to_string(slot));
    }
    Require(!cpu.sample.empty() && cpu.sample == metal.sample, name + " packet bytes differ");
    const unsigned stamp = static_cast<unsigned>(static_cast<int>(quality));
    Require(HeaderQualityStamp(cpu.sample) == stamp && HeaderQualityStamp(metal.sample) == stamp,
            name + " tag -80 does not name the profile");
    Require(FilmScan3FullCoefficientHashesEqual(cpu, metal), name + " coefficient hash declaration");
    for (std::size_t slot = 0; slot < 30; ++slot)
        Require(cpu.coefficient_fnv64[slot] == metal.coefficient_fnv64[slot],
                name + " coefficient hash " + std::to_string(slot));
}

void RunProfile(EncodingQuality quality, std::vector<std::array<unsigned, 36>>& quants) {
    const FilmScan3Geometry geometry{kWidth, kHeight, 8u * static_cast<std::size_t>(kWidth) * kHeight + 16384u};
    FilmScan3Encoder cpu(geometry, WaveletIsa::Scalar);
    FilmScan3Encoder metal(FilmScan3Backend::Metal, geometry);
    cpu.SetSessionQuality(quality);
    metal.SetSessionQuality(quality);
    cpu.SetCoefficientHashing(true);
    metal.SetCoefficientHashing(true);
    Require(cpu.wavelet_isa() == WaveletIsa::Scalar, "CPU side was not scalar");
    Require(metal.wavelet_isa() == WaveletIsa::Auto, "Metal side did not keep the GPU ISA convention");
    const std::size_t stride = static_cast<std::size_t>(kWidth) * 4u + 64u;
    const std::size_t bytes = stride * static_cast<std::size_t>(kHeight);
    AlignedVector<std::uint8_t> input(bytes, 0xa5);
    std::size_t carried = 0;
    std::cout << "PROFILE name=" << EncodingQualityName(quality)
              << " id=" << static_cast<int>(quality)
              << " geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0,1,2,3\n";
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        for (int y = 0; y < kHeight; ++y) for (int x = 0; x < kWidth; ++x) {
            auto* pixel = input.data() + static_cast<std::size_t>(y) * stride + 4u * static_cast<unsigned>(x);
            pixel[0] = static_cast<std::uint8_t>(x + frame * 17);
            pixel[1] = static_cast<std::uint8_t>(y + frame * 31);
            pixel[2] = static_cast<std::uint8_t>((x ^ y) + frame * 7);
            pixel[3] = 255;
        }
        const auto cpu_frame = cpu.Encode(input.data(), input.size(), stride);
        const auto metal_frame = metal.Encode(input.data(), input.size(), stride);
        CompareFrame(cpu_frame, metal_frame, frame, carried, quality);
        carried = cpu_frame.sample.size();
        if (frame == 0) quants.push_back(cpu_frame.tables.quant);
        std::cout << "FRAME profile=" << EncodingQualityName(quality)
                  << " index=" << frame
                  << " packet_bytes=" << cpu_frame.sample.size()
                  << " limiter=" << cpu_frame.state.fs_rate_limiter
                  << " overbitrate=" << cpu_frame.state.fs_overbitrate
                  << " previous_sample_bytes=" << cpu_frame.state.previous_sample_bytes
                  << " stamp=" << HeaderQualityStamp(cpu_frame.sample)
                  << " equal=cpu_metal\n"
                  << "QUANT profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " values=" << Join(cpu_frame.tables.quant) << '\n'
                  << "HASHES profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " count=30 values=" << JoinHashes(cpu_frame.coefficient_fnv64) << '\n';
    }
    std::cout << "PASS profile=" << EncodingQualityName(quality)
              << " cpu_metal_frames=" << kFrames << " full-packet/36quant/36scale/rate/index/30hash\n";
}
}  // namespace

int main() try {
    const auto device = ProbeMetalEncoderDevice();
    Require(device.metal_available && device.metal_max_buffer_length_bytes != 0 &&
            device.metal_recommended_working_set_bytes != 0,
            "real Metal device facts unavailable");
    // Admission is intentionally not opened by this binary. A green log is the
    // measurement; ProfileAdmitted changes only in a later reviewed commit.
    for (const auto quality : {EncodingQuality::Low, EncodingQuality::Medium, EncodingQuality::High,
                               EncodingQuality::FilmScan1, EncodingQuality::FilmScan2})
        Require(!ProfileAdmitted(SessionBackend::Metal, quality),
                "Metal admission moved without a separate reviewed commit");
    Require(ProfileAdmitted(SessionBackend::Metal, EncodingQuality::FilmScan3), "FilmScan3 Metal row missing");
    std::cout << "PROFILE_MATRIX backend=metal geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0,1,2,3\n";
    std::vector<std::array<unsigned, 36>> quants;
    for (const auto quality : {EncodingQuality::Low, EncodingQuality::Medium, EncodingQuality::High,
                               EncodingQuality::FilmScan1, EncodingQuality::FilmScan2, EncodingQuality::FilmScan3})
        RunProfile(quality, quants);
    Require(quants.size() == 6, "profile matrix did not run six profiles");
    std::set<std::array<unsigned, 36>> distinct(quants.begin(), quants.end());
    Require(distinct.size() == 6, "six profiles did not produce six distinct quant tables");
    std::cout << "DISCRIMINATOR distinct_frame0_quant_tables=6 backend=metal\n";
    std::cout << "PASS metal profile matrix 6/6 cpu-vs-metal full packet parity\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
