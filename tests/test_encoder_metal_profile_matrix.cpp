// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #324 Metal half: CPU scalar session vs Metal session, 1920x1080,
// frames 0..19, all six regular profiles. Each frame compares full packet
// bytes, 36 quant entries, 36 scale entries, fs_rate_limiter, fs_overbitrate,
// previous_sample_bytes, the exact frame index, tag -80 and all 30 coefficient
// hashes. Frames 8 and 17 are named coverage points. Low/Medium/High must also
// reach overbitrate > 1 and those frames take the same field comparison.
//
// Non-Darwin hosts cannot execute Metal. They print the reason and return 77
// so CTest records Skipped. That skip is not a pass. The original SDK is not
// linked here; that axis is not a PASS either. Not a SMPTE/VC-5 claim.
#include "../src/core/encoder_backend_selection.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hypercine;
using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;

namespace {
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr unsigned kFrames = 20;

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

std::string JoinInts(const std::vector<int>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << values[i];
    }
    return out.str();
}

bool UsesOverbitrateCounter(EncodingQuality quality) {
    return quality == EncodingQuality::Low || quality == EncodingQuality::Medium ||
           quality == EncodingQuality::High;
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

EncoderSessionConfig MakeConfig(EncodingQuality quality, SessionBackend backend) {
    EncoderSessionConfig config;
    config.quality = quality;
    config.backend = backend;
    config.width = kWidth;
    config.height = kHeight;
    config.input_stride_bytes = static_cast<std::uint64_t>(kWidth) * 4u + 64u;
    config.input_bytes = config.input_stride_bytes * static_cast<std::uint64_t>(kHeight);
    config.max_sample_bytes = 8u * static_cast<std::uint64_t>(kWidth) * static_cast<std::uint64_t>(kHeight) + 16384u;
    config.diagnostic = DiagnosticMode::CoefficientFnv64;
    return config;
}

void Fill(AlignedVector<std::uint8_t>& input, std::uint64_t stride, unsigned frame) {
    for (int y = 0; y < kHeight; ++y) for (int x = 0; x < kWidth; ++x) {
        auto* pixel = input.data() + static_cast<std::size_t>(y) * stride + 4u * static_cast<unsigned>(x);
        pixel[0] = static_cast<std::uint8_t>(x + frame * 17);
        pixel[1] = static_cast<std::uint8_t>(y + frame * 31);
        pixel[2] = static_cast<std::uint8_t>((x ^ y) + frame * 7);
        pixel[3] = 255;
    }
}

void CompareFrame(const FilmScan3Frame& cpu, const FilmScan3Frame& metal, unsigned frame,
                   std::size_t carried, EncodingQuality quality) {
    const auto name = std::string(EncodingQualityName(quality)) + " frame " + std::to_string(frame);
    Require(cpu.state.frame_index == frame, name + " CPU frame index is not the requested index");
    Require(metal.state.frame_index == frame, name + " Metal frame index is not the requested index");
    Require(cpu.state.previous_sample_bytes == carried, name + " CPU previous_sample_bytes broke the chain");
    Require(metal.state.previous_sample_bytes == carried, name + " Metal previous_sample_bytes broke the chain");
    Require(cpu.state.fs_rate_limiter == metal.state.fs_rate_limiter, name + " fs_rate_limiter differs");
    Require(cpu.state.fs_overbitrate == metal.state.fs_overbitrate, name + " fs_overbitrate differs");
    Require(cpu.tables.quant.size() == 36 && metal.tables.quant.size() == 36, name + " quant table length");
    Require(cpu.tables.scale.size() == 36 && metal.tables.scale.size() == 36, name + " scale table length");
    for (std::size_t slot = 0; slot < 36; ++slot) {
        Require(cpu.tables.quant[slot] == metal.tables.quant[slot], name + " quant slot " + std::to_string(slot));
        Require(cpu.tables.scale[slot] == metal.tables.scale[slot], name + " scale slot " + std::to_string(slot));
    }
    Require(!cpu.sample.empty() && cpu.sample == metal.sample, name + " packet bytes differ");
    const unsigned stamp = static_cast<unsigned>(static_cast<int>(quality));
    Require(HeaderQualityStamp(cpu.sample) == stamp, name + " CPU tag -80 does not name the profile");
    Require(HeaderQualityStamp(metal.sample) == stamp, name + " Metal tag -80 does not name the profile");
    Require(cpu.coefficient_fnv64.size() == 30 && metal.coefficient_fnv64.size() == 30, name + " hash length");
    Require(FilmScan3FullCoefficientHashesEqual(cpu, metal), name + " coefficient hash declaration");
    for (std::size_t slot = 0; slot < 30; ++slot)
        Require(cpu.coefficient_fnv64[slot] == metal.coefficient_fnv64[slot],
                name + " coefficient hash " + std::to_string(slot));
}

struct ProfileRecord {
    EncodingQuality quality = EncodingQuality::Low;
    std::array<unsigned, 36> quant{};
    bool frame8_compared = false;
    bool frame17_compared = false;
    unsigned overbitrate_gt1_frames = 0;
    int max_overbitrate = 0;
    std::size_t max_packet_bytes = 0;
    std::vector<int> over_trace;
    std::vector<int> limiter_trace;
    unsigned wavelet_dispatches = 0;
};

void RunProfile(EncodingQuality quality, const EncoderDevice& metal_device, ProfileRecord& record) {
    const auto cpu_config = MakeConfig(quality, SessionBackend::CpuScalar);
    const auto metal_config = MakeConfig(quality, SessionBackend::Metal);
    EncoderSession cpu(cpu_config, EncoderDevice{});
    EncoderSession metal(metal_config, metal_device);
    Require(cpu.selection().effective.effective_backend == SessionBackend::CpuScalar &&
            cpu.wavelet_isa() == WaveletIsa::Scalar, "CPU session was not scalar");
    Require(metal.selection().effective.effective_backend == SessionBackend::Metal &&
            metal.selection().fallback == ScalarFallbackReason::None &&
            metal.selection().frontend->Backend() == FrontendBackend::Metal,
            "Metal session fell back or lost its frontend");
    AlignedVector<std::uint8_t> input(static_cast<std::size_t>(cpu_config.input_bytes), 0xa5);
    std::size_t carried = 0;
    record.quality = quality;
    std::cout << "PROFILE name=" << EncodingQualityName(quality)
              << " id=" << static_cast<int>(quality)
              << " geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0..19\n";
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        Fill(input, cpu_config.input_stride_bytes, frame);
        const auto before = ReadMetalTiming(*metal.selection().frontend);
        const auto cpu_frame = cpu.Encode(input.data(), input.size(), cpu_config.input_stride_bytes);
        const auto metal_frame = metal.Encode(input.data(), input.size(), metal_config.input_stride_bytes);
        const auto after = ReadMetalTiming(*metal.selection().frontend);
        Require(after.color_dispatches > before.color_dispatches &&
                after.wavelet_dispatches > before.wavelet_dispatches &&
                after.command_buffers > before.command_buffers && after.waits > before.waits,
                std::string(EncodingQualityName(quality)) + " did not execute real Metal work on frame " +
                    std::to_string(frame));
        const unsigned wavelet = after.wavelet_dispatches - before.wavelet_dispatches;
        record.wavelet_dispatches += wavelet;
        CompareFrame(cpu_frame, metal_frame, frame, carried, quality);
        carried = cpu_frame.sample.size();
        if (frame == 0) record.quant = cpu_frame.tables.quant;
        record.over_trace.push_back(cpu_frame.state.fs_overbitrate);
        record.limiter_trace.push_back(cpu_frame.state.fs_rate_limiter);
        if (cpu_frame.state.fs_overbitrate > record.max_overbitrate)
            record.max_overbitrate = cpu_frame.state.fs_overbitrate;
        if (cpu_frame.sample.size() > record.max_packet_bytes)
            record.max_packet_bytes = cpu_frame.sample.size();
        if (frame == 8) record.frame8_compared = true;
        if (frame == 17) record.frame17_compared = true;
        const bool over_gt1 = cpu_frame.state.fs_overbitrate > 1;
        if (over_gt1) ++record.overbitrate_gt1_frames;
        std::cout << "FRAME profile=" << EncodingQualityName(quality)
                  << " index=" << frame
                  << " packet_bytes=" << cpu_frame.sample.size()
                  << " limiter=" << cpu_frame.state.fs_rate_limiter
                  << " overbitrate=" << cpu_frame.state.fs_overbitrate
                  << " previous_sample_bytes=" << cpu_frame.state.previous_sample_bytes
                  << " stamp=" << HeaderQualityStamp(cpu_frame.sample)
                  << " metal_wavelet_dispatches=" << wavelet
                  << " packet_equal=1 quant_equal=1 scale_equal=1 limiter_equal=1"
                  << " overbitrate_equal=1 index_equal=1 hash_equal=1 equal=cpu_metal\n"
                  << "QUANT profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " values=" << Join(cpu_frame.tables.quant) << '\n'
                  << "SCALE profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " values=" << Join(cpu_frame.tables.scale) << '\n'
                  << "HASHES profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " count=30 values=" << JoinHashes(cpu_frame.coefficient_fnv64) << '\n';
        if (frame == 8 || frame == 17 || over_gt1) {
            std::cout << "CHECKED profile=" << EncodingQualityName(quality)
                      << " index=" << frame
                      << " packet_bytes=" << cpu_frame.sample.size()
                      << " limiter=" << cpu_frame.state.fs_rate_limiter
                      << " overbitrate=" << cpu_frame.state.fs_overbitrate
                      << " metal_wavelet_dispatches=" << wavelet
                      << " fields=packet,quant36,scale36,limiter,overbitrate,index,hash30"
                      << " equal=cpu_metal\n";
        }
    }
    Require(record.over_trace.size() == kFrames, std::string(EncodingQualityName(quality)) + " did not compare 20 frames");
    Require(record.frame8_compared, std::string(EncodingQualityName(quality)) + " frame 8 was not compared");
    Require(record.frame17_compared, std::string(EncodingQualityName(quality)) + " frame 17 was not compared");
    std::cout << "PARITY profile=" << EncodingQualityName(quality)
              << " cpu_metal_frames=" << kFrames
              << " status=match fields=packet,quant36,scale36,limiter,overbitrate,overrate_quant,index,hash30"
              << " frame8=compared frame17=compared"
              << " overbitrate_gt1_frames=" << record.overbitrate_gt1_frames
              << " max_overbitrate=" << record.max_overbitrate
              << " metal_wavelet_dispatches=" << record.wavelet_dispatches
              << " over_trace=" << JoinInts(record.over_trace)
              << " limiter_trace=" << JoinInts(record.limiter_trace) << '\n';
}
}  // namespace

int main() try {
#if !defined(__APPLE__)
    std::cout << "SKIP encoder_metal_profile_matrix"
              << " reason=host_is_not_darwin"
              << " metal_sdk_unavailable"
              << " frames_requested=" << kFrames
              << " indices=0..19"
              << " not_executed=1"
              << " counted_as_pass=no\n";
    return 77;
#endif
    const EncodingQuality profiles[] = {
        EncodingQuality::Low, EncodingQuality::Medium, EncodingQuality::High,
        EncodingQuality::FilmScan1, EncodingQuality::FilmScan2, EncodingQuality::FilmScan3};
    for (const auto quality : profiles) {
        Require(ProfileAdmitted(SessionBackend::CpuScalar, quality), "CPU scalar dropped a regular profile");
        Require(ProfileAdmitted(SessionBackend::Metal, quality), "Metal matrix profile is not admitted");
    }
    Require(!ProfileAdmitted(SessionBackend::Metal, static_cast<EncodingQuality>(0)), "Metal admitted quality 0");
    Require(!ProfileAdmitted(SessionBackend::Metal, static_cast<EncodingQuality>(7)), "Metal admitted quality 7");
    const auto device = ProbeMetalEncoderDevice();
    Require(device.metal_available && device.metal_max_buffer_length_bytes != 0 &&
            device.metal_recommended_working_set_bytes != 0,
            "real Metal device facts unavailable");
    std::cout << "PROFILE_MATRIX backend=metal geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0..19\n";
    std::vector<ProfileRecord> records;
    unsigned wavelet_dispatches = 0;
    for (const auto quality : profiles) {
        ProfileRecord record;
        RunProfile(quality, device, record);
        wavelet_dispatches += record.wavelet_dispatches;
        records.push_back(record);
    }
    Require(records.size() == 6, "profile matrix did not run six profiles");
    std::set<std::array<unsigned, 36>> distinct;
    for (const auto& record : records) distinct.insert(record.quant);
    Require(distinct.size() == 6, "six profiles did not produce six distinct quant tables");
    std::cout << "GPU_EXECUTION backend=metal wavelet_dispatches=" << wavelet_dispatches
              << " frames_executed=" << (records.size() * kFrames) << '\n';
    std::cout << "DISCRIMINATOR distinct_frame0_quant_tables=" << distinct.size()
              << " backend=metal frame_index=0\n";
    std::cout << "SDK_COMPARE status=not_compared"
              << " reason=sdk_free_standalone_encoder_is_linked_original_sdk_is_not"
              << " and_no_source_bound_original_sdk_packet_fixture_is_compiled_in"
              << " counted_as_pass=no\n";
    std::ostringstream coverage_gap;
    bool coverage_ok = true;
    for (const auto& record : records) {
        std::cout << "COVERAGE profile=" << EncodingQualityName(record.quality)
                  << " frames=" << kFrames
                  << " frame8=" << (record.frame8_compared ? "compared" : "missing")
                  << " frame17=" << (record.frame17_compared ? "compared" : "missing")
                  << " overbitrate_gt1_frames=" << record.overbitrate_gt1_frames
                  << " max_overbitrate=" << record.max_overbitrate << '\n';
        if (!UsesOverbitrateCounter(record.quality)) continue;
        if (record.overbitrate_gt1_frames == 0 || record.max_overbitrate <= 1) {
            coverage_ok = false;
            coverage_gap << EncodingQualityName(record.quality)
                         << " max_overbitrate=" << record.max_overbitrate << "; ";
        }
    }
    if (!coverage_ok) {
        throw std::runtime_error(
            "required overbitrate>1 state was not observed on low/medium/high; " + coverage_gap.str());
    }
    std::cout << "PASS metal profile matrix 6/6 cpu-vs-metal frames=" << kFrames
              << " sdk=not_compared\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
