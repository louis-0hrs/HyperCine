// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #324/#330: CPU scalar session vs qualification-only CUDA writer.
// This test does not admit new production session profiles.
// Twenty consecutive frames (indices 0..19, not a zip of a shorter trace).
// Each frame is compared field by field: full packet bytes, all 36 quant
// entries, all 36 scale entries, fs_rate_limiter, fs_overbitrate (the overrate
// selection is the quant table those counters choose), previous_sample_bytes,
// the exact frame index and all 30 coefficient hashes.
// Frames 8 and 17 are named coverage points. Low/Medium/High must also reach
// a frame whose overbitrate counter is greater than 1; that frame is compared
// with the same fields, not summarized away.
//
// The original SDK is not linked by this binary and no source-bound SDK packet
// fixture is compiled in. That axis is reported as not compared and is not a
// PASS. This is not a product qualification and not a SMPTE/VC-5 claim.
#include "../src/core/encoder_backend_selection.h"
#include "../src/cuda/filmscan3_cuda_stats.h"

#include <cuda_runtime.h>

#include <array>
#include <climits>
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
constexpr unsigned kFrames = 20;  // indices 0..19 exactly
constexpr unsigned kSpatialLaunchesPerFrame = 6;

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

void Fill(AlignedVector<std::uint8_t>& input, const EncoderSessionConfig& config, unsigned frame, bool noise) {
    // Noise is the deterministic LCG from seed 0x1337BEEF. The gradient alone
    // never pushes Medium or High through the 120% bitrate step, so it cannot
    // demand an overbitrate counter above 1.
    std::uint32_t state = 0x1337BEEFu ^ (frame * 0x9E3779B9u);
    for (int y = 0; y < config.height; ++y) {
        for (int x = 0; x < config.width; ++x) {
            auto* pixel = input.data() + static_cast<std::size_t>(y) * config.input_stride_bytes + 4u * static_cast<unsigned>(x);
            if (noise) {
                state = state * 1664525u + 1013904223u;
                pixel[0] = static_cast<std::uint8_t>(state);
                pixel[1] = static_cast<std::uint8_t>(state >> 8);
                pixel[2] = static_cast<std::uint8_t>(state >> 16);
            } else {
                pixel[0] = static_cast<std::uint8_t>(x + frame * 17);
                pixel[1] = static_cast<std::uint8_t>(y + frame * 31);
                pixel[2] = static_cast<std::uint8_t>((x ^ y) + frame * 7);
            }
            pixel[3] = 255;
        }
    }
}

// True when the previous packet is above 120% of the profile ceiling at 30 fps.
// The arithmetic follows the encoder: bit count via int32, times the frame rate
// in float, then the integer 12/10 step. This is the band where a counter that
// is already positive must move above 1. It is not an original-SDK oracle.
bool PreviousAbove120(EncodingQuality quality, std::size_t previous_bytes) {
    if (previous_bytes == 0 || !UsesOverbitrateCounter(quality)) return false;
    const std::int64_t upper = 130000000LL + (static_cast<std::int64_t>(static_cast<int>(quality)) - 2) * 20000000LL;
    const std::int64_t bits = static_cast<std::int64_t>(previous_bytes) * 8;
    if (bits > INT_MAX) return true;
    const int current = static_cast<int>(static_cast<float>(static_cast<std::int32_t>(bits)) * 30.0f);
    return static_cast<std::int64_t>(current) > upper * 12 / 10;
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
    const char* pattern = "gradient";
    EncodingQuality quality = EncodingQuality::Low;
    std::array<unsigned, 36> quant{};
    std::array<unsigned, 36> scale{};
    bool frame8_compared = false;
    bool frame17_compared = false;
    unsigned overbitrate_gt1_frames = 0;
    unsigned frames_above_120 = 0;
    int max_overbitrate = 0;
    std::size_t max_packet_bytes = 0;
    std::vector<int> over_trace;
    std::vector<int> limiter_trace;
    std::vector<unsigned> band_violations;
};

void ReportSdkNotCompared();

// records == nullptr runs the pass for parity only; the recorded pass is the one
// that has to reach overbitrate > 1, so the profile total stays six.
void RunProfile(const char* pattern, bool noise, EncodingQuality quality, const EncoderDevice& cuda_device,
                std::vector<ProfileRecord>* records_out, std::uint64_t& spatial_dispatches) {
    const auto cpu_config = MakeConfig(quality, SessionBackend::CpuScalar);
    const auto cuda_config = MakeConfig(quality, SessionBackend::Cuda);
    EncoderDevice cpu_device;
    EncoderSession cpu(cpu_config, cpu_device);
    // Qualification uses the existing internal writer seam, not permission to
    // broaden the production profile/backend admission matrix.
    const auto capability = CheckEncoderCapability(cuda_config, cuda_device);
    const bool admitted = quality == EncodingQuality::FilmScan3;
    Require(capability.profile_admitted == admitted, "CUDA profile admission changed during measurement");
    Require(admitted ? capability.allocation_permitted :
            (!capability.allocation_permitted &&
             capability.primary == CapabilityReason::ProfileNotQualifiedForBackend),
            "CUDA measurement lost the typed production refusal");
    const FilmScan3Geometry geometry{kWidth, kHeight, 0};
    const auto cuda_frontend = CudaSessionFrontend(geometry);
    Require(cuda_frontend && cuda_frontend->Backend() == FrontendBackend::Cuda,
            "measurement factory did not create a CUDA frontend");
    FilmScan3Encoder cuda(geometry, WaveletIsa::Scalar);
    FrontendAccess::Use(cuda, cuda_frontend);
    cuda.SetSessionQuality(quality);
    cuda.SetCoefficientHashing(true);
    std::cout << "MEASUREMENT_ONLY backend=cuda profile=" << EncodingQualityName(quality)
              << " session_admitted=" << (admitted ? 1 : 0) << " fallback=none\n";
    Require(cpu.selection().effective.effective_backend == SessionBackend::CpuScalar &&
            cpu.wavelet_isa() == WaveletIsa::Scalar, "CPU session was not scalar");
    Require(cpu.config().quality == quality && cuda.session_quality() == quality,
            "session dropped the requested profile");

    AlignedVector<std::uint8_t> input(static_cast<std::size_t>(cpu_config.input_bytes), 0xa5);
    std::size_t carried = 0;
    ProfileRecord record;
    record.pattern = pattern;
    record.quality = quality;
    std::cout << "PROFILE pattern=" << pattern
              << " name=" << EncodingQualityName(quality)
              << " id=" << static_cast<int>(quality)
              << " geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0..19\n";
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        Fill(input, cpu_config, frame, noise);
        const bool above_120 = PreviousAbove120(quality, carried);
        const auto before = CudaSpatialDispatchCount(*cuda_frontend);
        const auto cpu_frame = cpu.Encode(input.data(), input.size(), cpu_config.input_stride_bytes);
        const auto cuda_frame = cuda.Encode(input.data(), input.size(), cuda_config.input_stride_bytes);
        const auto after = CudaSpatialDispatchCount(*cuda_frontend);
        const auto launches = after - before;
        Require(launches == kSpatialLaunchesPerFrame,
                std::string(EncodingQualityName(quality)) + " CUDA spatial launch count frame " + std::to_string(frame));
        spatial_dispatches += launches;
        CompareFrame(cpu_frame, cuda_frame, frame, carried, quality);
        carried = cpu_frame.sample.size();
        if (frame == 0) {
            record.quant = cpu_frame.tables.quant;
            record.scale = cpu_frame.tables.scale;
        }
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
        if (above_120) ++record.frames_above_120;
        if (above_120 && !over_gt1) record.band_violations.push_back(frame);
        std::cout << "FRAME profile=" << EncodingQualityName(quality)
                  << " index=" << frame
                  << " packet_bytes=" << cpu_frame.sample.size()
                  << " limiter=" << cpu_frame.state.fs_rate_limiter
                  << " overbitrate=" << cpu_frame.state.fs_overbitrate
                  << " previous_sample_bytes=" << cpu_frame.state.previous_sample_bytes
                  << " stamp=" << HeaderQualityStamp(cpu_frame.sample)
                  << " dispatch=" << launches
                  << " above_120=" << (above_120 ? 1 : 0)
                  << " packet_equal=1 quant_equal=1 scale_equal=1 limiter_equal=1"
                  << " overbitrate_equal=1 index_equal=1 hash_equal=1 equal=cpu_cuda\n"
                  << "QUANT profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " values=" << JoinQuant(cpu_frame.tables.quant) << '\n'
                  << "SCALE profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " values=" << JoinQuant(cpu_frame.tables.scale) << '\n'
                  << "HASHES profile=" << EncodingQualityName(quality) << " index=" << frame
                  << " count=30 values=" << JoinHashes(cpu_frame.coefficient_fnv64) << '\n';
        if (frame == 8 || frame == 17 || over_gt1 || above_120) {
            std::cout << "CHECKED pattern=" << pattern
                      << " profile=" << EncodingQualityName(quality)
                      << " index=" << frame
                      << " frame8=" << (frame == 8 ? 1 : 0)
                      << " frame17=" << (frame == 17 ? 1 : 0)
                      << " overbitrate_gt1=" << (over_gt1 ? 1 : 0)
                      << " above_120=" << (above_120 ? 1 : 0)
                      << " packet_bytes=" << cpu_frame.sample.size()
                      << " limiter=" << cpu_frame.state.fs_rate_limiter
                      << " overbitrate=" << cpu_frame.state.fs_overbitrate
                      << " previous_sample_bytes=" << cpu_frame.state.previous_sample_bytes
                      << " quant_slot1=" << cpu_frame.tables.quant[1]
                      << " scale_slot1=" << cpu_frame.tables.scale[1]
                      << " fields=packet,quant36,scale36,limiter,overbitrate,index,hash30"
                      << " equal=cpu_cuda\n";
        }
    }
    Require(record.over_trace.size() == kFrames, std::string(EncodingQualityName(quality)) + " did not compare 20 frames");
    Require(record.frame8_compared, std::string(EncodingQualityName(quality)) + " frame 8 was not compared");
    Require(record.frame17_compared, std::string(EncodingQualityName(quality)) + " frame 17 was not compared");
    std::cout << "PARITY pattern=" << pattern
              << " profile=" << EncodingQualityName(quality)
              << " cpu_cuda_frames=" << kFrames
              << " status=match fields=packet,quant36,scale36,limiter,overbitrate,overrate_quant,index,hash30"
              << " frame8=compared frame17=compared"
              << " overbitrate_gt1_frames=" << record.overbitrate_gt1_frames
              << " frames_above_120=" << record.frames_above_120
              << " max_overbitrate=" << record.max_overbitrate
              << " over_trace=" << JoinInts(record.over_trace)
              << " limiter_trace=" << JoinInts(record.limiter_trace) << '\n';
    if (records_out != nullptr) records_out->push_back(record);
}

void ReportSdkNotCompared() {
    std::cout << "SDK_COMPARE status=not_compared"
              << " reason=sdk_free_standalone_encoder_is_linked_original_sdk_is_not"
              << " and_no_source_bound_original_sdk_packet_fixture_is_compiled_in"
              << " counted_as_pass=no\n";
}
}  // namespace

int main() try {
    const EncodingQuality profiles[] = {
        EncodingQuality::Low, EncodingQuality::Medium, EncodingQuality::High,
        EncodingQuality::FilmScan1, EncodingQuality::FilmScan2, EncodingQuality::FilmScan3};
    for (const auto quality : profiles) {
        const auto capability = CheckEncoderCapability(MakeConfig(quality, SessionBackend::CpuScalar), EncoderDevice{});
        Require(capability.profile_admitted && capability.allocation_permitted,
                "CPU scalar dropped a regular profile");
    }
    for (const auto invalid : {0, 7}) {
        const auto capability = CheckEncoderCapability(
            MakeConfig(static_cast<EncodingQuality>(invalid), SessionBackend::Cuda), EncoderDevice{});
        Require(capability.primary == CapabilityReason::FormatMathematicallyInvalid &&
                !capability.allocation_permitted, "CUDA admitted an invalid profile enum");
    }

    const auto device = ProbeCudaEncoderDevice();
    Require(device.cuda_available && device.cuda_hardware_observed && device.cuda_total_global_memory_bytes != 0,
            "real CUDA device facts unavailable");
    cudaDeviceProp properties{};
    Require(cudaGetDeviceProperties(&properties, 0) == cudaSuccess, "cudaGetDeviceProperties failed");
    Require(properties.totalGlobalMem == device.cuda_total_global_memory_bytes,
            "probe memory does not match cudaGetDeviceProperties");
    std::cout << "DEVICE name=" << properties.name
              << " compute=" << properties.major << '.' << properties.minor
              << " memory_bytes=" << properties.totalGlobalMem
              << " multiprocessors=" << properties.multiProcessorCount << '\n';
    std::cout << "PROFILE_MATRIX geometry=" << kWidth << 'x' << kHeight
              << " frames=" << kFrames << " indices=0..19"
              << " device=" << properties.name
              << " device_memory_bytes=" << device.cuda_total_global_memory_bytes << '\n';

    std::vector<ProfileRecord> records;
    std::uint64_t spatial_dispatches = 0;
    // Two passes per profile: the plain gradient proves parity on the pattern the
    // older four-frame test used, the seeded noise pass is the one that drives
    // the rate counter past 1 and is the pass that is recorded.
    std::uint64_t passes = 0;
    for (const auto quality : profiles) {
        RunProfile("gradient", false, quality, device, nullptr, spatial_dispatches);
        ++passes;
        RunProfile("noise", true, quality, device, &records, spatial_dispatches);
        ++passes;
    }
    Require(records.size() == 6, "profile matrix did not run six profiles");
    const std::uint64_t expected_dispatches =
        passes * kFrames * kSpatialLaunchesPerFrame;
    Require(spatial_dispatches == expected_dispatches, "spatial dispatch total does not match executed frames");
    std::cout << "GPU_EXECUTION device=" << properties.name
              << " compute=" << properties.major << '.' << properties.minor
              << " spatial_dispatches=" << spatial_dispatches
              << " frames_executed=" << (passes * kFrames)
              << " launches_per_frame=" << kSpatialLaunchesPerFrame << '\n';

    std::set<std::array<unsigned, 36>> distinct;
    for (const auto& record : records) distinct.insert(record.quant);
    Require(distinct.size() == 6, "six profiles did not produce six distinct quant tables");
    std::cout << "DISCRIMINATOR distinct_frame0_quant_tables=" << distinct.size()
              << " profiles=6 geometry=" << kWidth << 'x' << kHeight << " frame_index=0\n";

    ReportSdkNotCompared();

    std::ostringstream coverage_gap;
    bool coverage_ok = true;
    for (const auto& record : records) {
        const bool needs_over = UsesOverbitrateCounter(record.quality);
        std::cout << "COVERAGE profile=" << EncodingQualityName(record.quality)
                  << " frames=" << kFrames
                  << " frame8=" << (record.frame8_compared ? "compared" : "missing")
                  << " frame17=" << (record.frame17_compared ? "compared" : "missing")
                  << " overbitrate_gt1_frames=" << record.overbitrate_gt1_frames
                  << " max_overbitrate=" << record.max_overbitrate
                  << " max_packet_bytes=" << record.max_packet_bytes
                  << " over_trace=" << JoinInts(record.over_trace) << '\n';
        if (!needs_over) {
            std::cout << "COVERAGE profile=" << EncodingQualityName(record.quality)
                      << " overbitrate_counter=not_used_quality_above_high status=reported_not_required\n";
            continue;
        }
        if (record.overbitrate_gt1_frames == 0 || record.max_overbitrate <= 1) {
            coverage_ok = false;
            coverage_gap << EncodingQualityName(record.quality)
                         << " max_overbitrate=" << record.max_overbitrate
                         << " trace=" << JoinInts(record.over_trace)
                         << " max_packet_bytes=" << record.max_packet_bytes << "; ";
            std::cout << "COVERAGE_FAIL profile=" << EncodingQualityName(record.quality)
                      << " required_state=overbitrate_gt1 observed=no\n";
        } else {
            std::cout << "COVERAGE_OK profile=" << EncodingQualityName(record.quality)
                      << " required_state=overbitrate_gt1 frames=" << record.overbitrate_gt1_frames
                      << " max_overbitrate=" << record.max_overbitrate << '\n';
        }
    }
    if (!coverage_ok) {
        throw std::runtime_error(
            "required overbitrate>1 state was not observed on low/medium/high; " + coverage_gap.str());
    }
    std::cout << "PASS cuda profile matrix 6/6 cpu-vs-cuda frames=" << kFrames
              << " sdk=not_compared\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
