// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #46: per-profile CPU scalar session vs CUDA session on one input.
// Compares full packet bytes, all 36 quant/scale entries, rate state
// (fs_rate_limiter, fs_overbitrate, previous_sample_bytes), the exact frame
// index and all 30 coefficient hashes. Frame indices are the requested
// sequence 0..kFrames-1, not a zip of a shorter trace.
//
// This is an internal packet-parity gate. It is not a product qualification,
// not an original-SDK stage claim and not a SMPTE/VC-5 conformance claim.
#include "../src/core/encoder_backend_selection.h"
#include "../src/cuda/filmscan3_cuda_stats.h"

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
constexpr unsigned kFrames = 4;  // indices 0, 1, 2, 3 exactly

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::string Hex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string JoinQuant(const std::array<unsigned, 36>& table) {
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

void Fill(AlignedVector<std::uint8_t>& input, const EncoderSessionConfig& config, unsigned frame) {
    for (int y = 0; y < config.height; ++y) {
        for (int x = 0; x < config.width; ++x) {
            auto* pixel = input.data() + static_cast<std::size_t>(y) * config.input_stride_bytes + 4u * static_cast<unsigned>(x);
            pixel[0] = static_cast<std::uint8_t>(x + frame * 17);
            pixel[1] = static_cast<std::uint8_t>(y + frame * 31);
            pixel[2] = static_cast<std::uint8_t>((x ^ y) + frame * 7);
            pixel[3] = 255;
        }
    }
}

// Tag -80 is the session quality enum. A writer that ignores the profile and
// hard-codes 6 still matches CPU against CUDA, so equality alone cannot see it.
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

void CompareFrame(const FilmScan3Frame& cpu, const FilmScan3Frame& cuda, unsigned frame,
                   std::size_t carried_bytes, EncodingQuality quality) {
    const auto name = std::string(EncodingQualityName(quality)) + " frame " + std::to_string(frame);
    Require(cpu.state.frame_index == frame, name + " CPU frame index is not the requested index");
    Require(cuda.state.frame_index == frame, name + " CUDA frame index is not the requested index");
    Require(cpu.state.previous_sample_bytes == carried_bytes, name + " CPU previous_sample_bytes broke the chain");
    Require(cuda.state.previous_sample_bytes == carried_bytes, name + " CUDA previous_sample_bytes broke the chain");
    Require(cpu.state.fs_rate_limiter == cuda.state.fs_rate_limiter, name + " fs_rate_limiter differs");
    Require(cpu.state.fs_overbitrate == cuda.state.fs_overbitrate, name + " fs_overbitrate differs");
    Require(cpu.tables.quant.size() == 36 && cuda.tables.quant.size() == 36, name + " quant table length");
    Require(cpu.tables.scale.size() == 36 && cuda.tables.scale.size() == 36, name + " scale table length");
    for (std::size_t slot = 0; slot < 36; ++slot) {
        Require(cpu.tables.quant[slot] == cuda.tables.quant[slot],
                name + " quant slot " + std::to_string(slot));
        Require(cpu.tables.scale[slot] == cuda.tables.scale[slot],
                name + " scale slot " + std::to_string(slot));
    }
    Require(!cpu.sample.empty() && cpu.sample == cuda.sample, name + " packet bytes differ");
    const unsigned stamp = static_cast<unsigned>(static_cast<int>(quality));
    Require(HeaderQualityStamp(cpu.sample) == stamp, name + " CPU tag -80 does not name the profile");
    Require(HeaderQualityStamp(cuda.sample) == stamp, name + " CUDA tag -80 does not name the profile");
    Require(cpu.coefficient_fnv64.size() == 30 && cuda.coefficient_fnv64.size() == 30, name + " hash length");
    Require(FilmScan3FullCoefficientHashesEqual(cpu, cuda), name + " coefficient hash declaration");
    for (std::size_t slot = 0; slot < 30; ++slot) {
        Require(cpu.coefficient_fnv64[slot] == cuda.coefficient_fnv64[slot],
                name + " coefficient hash " + std::to_string(slot));
    }
}

struct ProfileRecord {
    EncodingQuality quality;
    std::array<unsigned, 36> quant{};
    std::array<unsigned, 36> scale{};
};

void RunProfile(EncodingQuality quality, const EncoderDevice& cuda_device, std::vector<ProfileRecord>& records) {
    const auto cpu_config = MakeConfig(quality, SessionBackend::CpuScalar);
    const auto cuda_config = MakeConfig(quality, SessionBackend::Cuda);
    EncoderDevice cpu_device;
    EncoderSession cpu(cpu_config, cpu_device);
    EncoderSession cuda(cuda_config, cuda_device);
    Require(cpu.selection().effective.effective_backend == SessionBackend::CpuScalar &&
            cpu.wavelet_isa() == WaveletIsa::Scalar, "CPU session was not scalar");
    Require(cuda.selection().effective.effective_backend == SessionBackend::Cuda &&
            cuda.selection().fallback == ScalarFallbackReason::None &&
            cuda.selection().frontend->Backend() == FrontendBackend::Cuda,
            "CUDA session fell back or lost its frontend");
    Require(cpu.config().quality == quality && cuda.config().quality == quality,
            "session dropped the requested profile");

    AlignedVector<std::uint8_t> input(static_cast<std::size_t>(cpu_config.input_bytes), 0xa5);
    std::size_t carried = 0;
    ProfileRecord record;
    record.quality = quality;
    std::cout << "PROFILE name=" << EncodingQualityName(quality)
              << " id=" << static_cast<int>(quality)
              << " geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0,1,2,3\n";
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        Fill(input, cpu_config, frame);
        const auto before = CudaSpatialDispatchCount(*cuda.selection().frontend);
        const auto cpu_frame = cpu.Encode(input.data(), input.size(), cpu_config.input_stride_bytes);
        const auto cuda_frame = cuda.Encode(input.data(), input.size(), cuda_config.input_stride_bytes);
        const auto after = CudaSpatialDispatchCount(*cuda.selection().frontend);
        Require(after - before == 6u, std::string(EncodingQualityName(quality)) + " CUDA spatial launch count");
        CompareFrame(cpu_frame, cuda_frame, frame, carried, quality);
        carried = cpu_frame.sample.size();
        if (frame == 0) {
            record.quant = cpu_frame.tables.quant;
            record.scale = cpu_frame.tables.scale;
        }
        std::cout << "FRAME profile=" << EncodingQualityName(quality)
                  << " index=" << frame
                  << " packet_bytes=" << cpu_frame.sample.size()
                  << " limiter=" << cpu_frame.state.fs_rate_limiter
                  << " overbitrate=" << cpu_frame.state.fs_overbitrate
                  << " previous_sample_bytes=" << cpu_frame.state.previous_sample_bytes
                  << " stamp=" << HeaderQualityStamp(cpu_frame.sample)
                  << " dispatch=6 equal=cpu_cuda\n"
                  << "QUANT profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " values=" << JoinQuant(cpu_frame.tables.quant) << '\n'
                  << "SCALE profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " values=" << JoinQuant(cpu_frame.tables.scale) << '\n'
                  << "HASHES profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " count=30 values=" << JoinHashes(cpu_frame.coefficient_fnv64) << '\n';
    }
    records.push_back(record);
    std::cout << "PASS profile=" << EncodingQualityName(quality)
              << " cpu_cuda_frames=" << kFrames << " full-packet/36quant/36scale/rate/index/30hash\n";
}
}  // namespace

int main() try {
    const EncodingQuality profiles[] = {
        EncodingQuality::Low, EncodingQuality::Medium, EncodingQuality::High,
        EncodingQuality::FilmScan1, EncodingQuality::FilmScan2, EncodingQuality::FilmScan3};
    for (const auto quality : profiles) {
        Require(ProfileAdmitted(SessionBackend::CpuScalar, quality), "CPU scalar dropped a regular profile");
        Require(ProfileAdmitted(SessionBackend::Cuda, quality), "CUDA matrix profile is not admitted");
    }
    for (const auto quality : profiles) {
        Require(ProfileAdmitted(SessionBackend::Metal, quality),
                "Metal matrix profile is not admitted");
    }
    Require(!ProfileAdmitted(SessionBackend::Cuda, static_cast<EncodingQuality>(0)), "CUDA admitted quality 0");
    Require(!ProfileAdmitted(SessionBackend::Cuda, static_cast<EncodingQuality>(7)), "CUDA admitted quality 7");
    Require(!ProfileAdmitted(SessionBackend::Metal, static_cast<EncodingQuality>(7)), "Metal admitted quality 7");

    const auto device = ProbeCudaEncoderDevice();
    Require(device.cuda_available && device.cuda_hardware_observed && device.cuda_total_global_memory_bytes != 0,
            "real CUDA device facts unavailable");
    std::cout << "PROFILE_MATRIX geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0,1,2,3"
              << " device_memory_bytes=" << device.cuda_total_global_memory_bytes << '\n';

    std::vector<ProfileRecord> records;
    for (const auto quality : profiles) RunProfile(quality, device, records);
    Require(records.size() == 6, "profile matrix did not run six profiles");
    std::set<std::array<unsigned, 36>> distinct;
    for (const auto& record : records) distinct.insert(record.quant);
    Require(distinct.size() == 6, "six profiles did not produce six distinct quant tables");
    std::cout << "DISCRIMINATOR distinct_frame0_quant_tables=" << distinct.size()
              << " profiles=6 geometry=" << kWidth << 'x' << kHeight << " frame_index=0\n";
    std::cout << "PASS cuda profile matrix 6/6 cpu-vs-cuda full packet parity\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
