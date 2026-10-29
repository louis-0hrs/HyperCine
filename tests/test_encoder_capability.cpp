// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../src/core/encoder_session_config.h"
#include "hypercine/filmscan3.h"
#include "reference_frame_generator.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hypercine::standalone;

namespace {
void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

EncoderDevice Host() {
    EncoderDevice device;
    device.cpu_scalar_available = true;
    device.cpu_simd_selectable = false;
    device.metal_available = false;
    device.cuda_available = false;
    device.vulkan_available = false;
    // This host has an A2. The flag must not qualify a CUDA encoder.
    device.cuda_hardware_observed = true;
    return device;
}

EncoderSessionConfig Film(std::int64_t width, std::int64_t height) {
    EncoderSessionConfig config;
    config.codec = CodecFamily::FilmScan3;
    config.quality = hypercine::EncodingQuality::FilmScan3;
    config.pixel_format = SourcePixelFormat::Bgra8;
    config.width = width;
    config.height = height;
    config.frame_rate = {30, 1};
    config.gop = GopMode::IntraGop1;
    config.progressive = true;
    config.backend = SessionBackend::CpuScalar;
    config.diagnostic = DiagnosticMode::CoefficientFnv64;
    return config;
}

EncoderCapabilityResult Check(const char* name, const EncoderSessionConfig& config, const EncoderDevice& device,
                              CapabilityReason primary) {
    const auto result = CheckEncoderCapability(config, device);
    std::cout << "CAPABILITY " << name
              << " primary=" << CapabilityReasonName(result.primary)
              << " allocation=" << result.allocation_permitted
              << " validator=" << result.geometry_validator_passed
              << " validator_invoked=" << result.geometry_validator_invoked
              << " pixel_budget=" << result.pixel_budget_ok
              << " envelope=" << result.outside_qualified_dimension_envelope
              << " evidence=" << result.evidence_record
              << " unqualified=" << result.reasons.backend_not_qualified_for_geometry
              << " memory=" << result.reasons.memory_budget_exceeded
              << " unimplemented=" << result.reasons.valid_but_not_implemented
              << " missing_backend=" << result.reasons.backend_not_present
              << " format=" << result.reasons.format_mathematically_invalid
              << " profile_unqualified=" << result.reasons.profile_not_qualified_for_backend
              << " reference_missing=" << result.reasons.reference_missing
              << " reference_qualified=" << result.reference_qualified
              << " fallback=" << result.fell_back_to_cpu_scalar
              << " experimental=" << result.reasons.experimentally_supported
              << " detail=" << result.detail << '\n';
    Require(result.primary == primary, std::string(name) + " primary");
    Require(!result.reference_qualified, std::string(name) + " must not claim a reference");
    Require(result.reasons.reference_missing, std::string(name) + " reference axis");
    if (result.primary != CapabilityReason::ExperimentallySupported)
        Require(!result.allocation_permitted, std::string(name) + " allocation was not refused");
    return result;
}

unsigned Read16(const std::vector<std::uint8_t>& sample, std::size_t at) {
    Require(at + 2 <= sample.size(), "truncated tag");
    return (static_cast<unsigned>(sample[at]) << 8) | sample[at + 1];
}
std::uint32_t Read32(const std::vector<std::uint8_t>& sample, std::size_t at) {
    return (static_cast<std::uint32_t>(Read16(sample, at)) << 16) | Read16(sample, at + 2);
}

void InspectPacket(const std::vector<std::uint8_t>& sample, int width, int height) {
    Require(Read32(sample, 0) == 0x10009 && Read32(sample, 4) == 0x20003, "intra index");
    unsigned low = 0, high = 0, levels = 0, geometry = 0, channel = 0, wavelet = 3;
    std::size_t first = 0;
    for (std::size_t at = 20; at < sample.size();) {
        const unsigned raw = Read16(sample, at);
        const unsigned tag = raw >= 32768 ? 65536 - raw : raw;
        const unsigned value = Read16(sample, at + 2);
        at += 4;
        if (tag == 0x4002) {
            Require(value <= (sample.size() - at) / 4, "metadata extent");
            at += 4u * value;
            continue;
        }
        if (tag == 68) first = at;
        if (tag == 62) { channel = value; Require(channel < 3, "channel"); }
        if (tag == 38) { wavelet = value; Require(wavelet >= 1 && wavelet <= 3, "wavelet"); }
        const unsigned channel_width = channel ? static_cast<unsigned>(width / 2) : static_cast<unsigned>(width);
        if (tag == 20) { ++geometry; Require(value == static_cast<unsigned>(width), "width tag"); }
        if (tag == 21 || tag == 85) { ++geometry; Require(value == static_cast<unsigned>(height), "height tag"); }
        if (tag == 27) Require(value == channel_width / 8, "LL width");
        if (tag == 28) Require(value == static_cast<unsigned>(height / 8), "LL height");
        if (tag == 41 || tag == 49) Require(value == (channel_width >> wavelet), "HP width");
        if (tag == 42 || tag == 50) Require(value == static_cast<unsigned>(height >> wavelet), "HP height");
        const unsigned type = tag & 0xff00;
        if (type == 0x2000 || type == 0x2100 || type == 0x2200) {
            const auto words = ((tag & 255u) << 16) | value;
            Require(words <= (sample.size() - at) / 4, "chunk extent");
            const auto end = at + 4u * words;
            if (type == 0x2200) Require(end == sample.size(), "sample extent");
            if (type == 0x2100) ++levels;
            if (type == 0x2000) {
                Require(end >= at + 8, "short band");
                if (Read32(sample, at) == 0x40f0f) {
                    ++low;
                    Require(Read32(sample, end - 4) == 0x41b4b, "LL end");
                } else {
                    ++high;
                    Require(Read32(sample, at) == 0x370000 && Read32(sample, end - 4) == 0x380000, "HP framing");
                }
                at = end;
            }
        }
    }
    Require(geometry == 3 && low == 3 && high == 27 && levels == 9, "complete geometry/bands");
    Require(first + Read32(sample, 8) + Read32(sample, 12) + Read32(sample, 16) + 20 == sample.size(), "channel index");
}

void Append32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}
void Append16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void Chunk(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& data) {
    out.insert(out.end(), tag, tag + 4);
    Append32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    if (data.size() & 1u) out.push_back(0);
}

std::vector<std::uint8_t> Avi(const std::vector<std::uint8_t>& packet, int width, int height) {
    const auto size = static_cast<std::uint32_t>(packet.size());
    std::vector<std::uint8_t> avih;
    for (std::uint32_t value : {40000u, 0u, 0u, 0u, 1u, 0u, 1u, size,
                                static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 0u, 0u, 0u, 0u})
        Append32(avih, value);
    std::vector<std::uint8_t> strh;
    strh.insert(strh.end(), {'v', 'i', 'd', 's', 'C', 'F', 'H', 'D'});
    Append32(strh, 0); Append16(strh, 0); Append16(strh, 0);
    for (std::uint32_t value : {0u, 1u, 25u, 0u, 1u, size, 0xffffffffu, 0u}) Append32(strh, value);
    Append16(strh, 0); Append16(strh, 0);
    Append16(strh, static_cast<std::uint16_t>(width));
    Append16(strh, static_cast<std::uint16_t>(height));
    std::vector<std::uint8_t> strf;
    Append32(strf, 40);
    Append32(strf, static_cast<std::uint32_t>(width));
    Append32(strf, static_cast<std::uint32_t>(height));
    Append16(strf, 1); Append16(strf, 24);
    strf.insert(strf.end(), {'C', 'F', 'H', 'D'});
    Append32(strf, size); Append32(strf, 0); Append32(strf, 0); Append32(strf, 0); Append32(strf, 0);
    std::vector<std::uint8_t> hdrl_payload = {'h', 'd', 'r', 'l'};
    std::vector<std::uint8_t> avih_chunk, strh_chunk, strf_chunk, strl_chunk;
    Chunk(avih_chunk, "avih", avih);
    Chunk(strh_chunk, "strh", strh);
    Chunk(strf_chunk, "strf", strf);
    std::vector<std::uint8_t> strl_payload = {'s', 't', 'r', 'l'};
    strl_payload.insert(strl_payload.end(), strh_chunk.begin(), strh_chunk.end());
    strl_payload.insert(strl_payload.end(), strf_chunk.begin(), strf_chunk.end());
    Chunk(strl_chunk, "LIST", strl_payload);
    hdrl_payload.insert(hdrl_payload.end(), avih_chunk.begin(), avih_chunk.end());
    hdrl_payload.insert(hdrl_payload.end(), strl_chunk.begin(), strl_chunk.end());
    std::vector<std::uint8_t> hdrl;
    Chunk(hdrl, "LIST", hdrl_payload);
    std::vector<std::uint8_t> frame;
    Chunk(frame, "00dc", packet);
    std::vector<std::uint8_t> movi_payload = {'m', 'o', 'v', 'i'};
    movi_payload.insert(movi_payload.end(), frame.begin(), frame.end());
    std::vector<std::uint8_t> movi;
    Chunk(movi, "LIST", movi_payload);
    std::vector<std::uint8_t> riff_payload = {'A', 'V', 'I', ' '};
    riff_payload.insert(riff_payload.end(), hdrl.begin(), hdrl.end());
    riff_payload.insert(riff_payload.end(), movi.begin(), movi.end());
    std::vector<std::uint8_t> avi;
    Chunk(avi, "RIFF", riff_payload);
    return avi;
}

// Each test run owns a fresh, atomically created scratch directory.
// The previous fixed "hypercine-capability-evidence" temp path could be a
// pre-existing user directory/symlink and its named .cfhd/.yuv files could
// be overwritten. Never reuse or clean a directory this test did not create.
std::filesystem::path FreshScratch() {
    const auto base = std::filesystem::temp_directory_path();
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = base / ("hypercine-capability-" + std::to_string(tick) +
                                  "-" + std::to_string(attempt));
        std::error_code ec;
        if (std::filesystem::create_directory(path, ec))
            return path;
        if (ec) throw std::filesystem::filesystem_error("cannot create isolated test directory", path, ec);
    }
    throw std::runtime_error("could not create exclusive capability test directory");
}

// Tool paths are configurable so the test can run on any host. Hardcoding
// /usr/bin/ffmpeg would fail on Windows and on any distribution that puts it
// elsewhere -- the test would then look broken rather than unavailable.
std::string Ffmpeg() {
    if (const char* env = std::getenv("HYPERCINE_FFMPEG_EXECUTABLE")) return env;
    return "ffmpeg";
}
std::string Ffprobe() {
    if (const char* env = std::getenv("HYPERCINE_FFPROBE_EXECUTABLE")) return env;
    return "ffprobe";
}

// Shell quoting is platform-specific and this bit me on the Windows runner:
// POSIX single quotes are literal characters to cmd.exe, so the redirect was
// parsed as an argument and ffmpeg -version died with exit 1. Quoting every path
// through one helper keeps that mistake from coming back per call site.
#if defined(_WIN32)
std::string Quote(const std::string& text) { return "\"" + text + "\""; }
#else
std::string Quote(const std::string& text) { return "'" + text + "'"; }
#endif
std::string Quote(const std::filesystem::path& path) { return Quote(path.string()); }

std::string Run(const std::string& command, const std::filesystem::path& err) {
    // .out and the stderr file are built as strings and quoted once, rather than
    // assembling quote characters inline at each call site.
    const auto out_path = err.string() + ".out";
    const auto err_path = err.string();
    auto wrapped = command + " >" + Quote(out_path) + " 2>" + Quote(err_path);
#if defined(_WIN32)
    // cmd.exe strips the outer quotes when /C starts with a quoted executable.
    // Keep a separate outer pair around the entire command, including redirects.
    wrapped = "\"" + wrapped + "\"";
#endif
    const int rc = std::system(wrapped.c_str());
    std::string output, error;
    if (std::ifstream in(err.string() + ".out"); in) output.assign(std::istreambuf_iterator<char>(in), {});
    if (std::ifstream in(err); in) error.assign(std::istreambuf_iterator<char>(in), {});
    if (rc != 0) throw std::runtime_error(command + " -> " + std::to_string(rc) + " " + error);
    return output;
}

std::string Field(const std::string& text, const char* key) {
    const auto needle = std::string(key) + "=";
    const auto at = text.find(needle);
    Require(at != std::string::npos, std::string("missing ffprobe ") + key);
    const auto end = text.find_first_of("\r\n", at);
    return text.substr(at + needle.size(), end == std::string::npos ? std::string::npos : end - (at + needle.size()));
}

void EncodeAndDecode(const std::filesystem::path& dir, int width, int height) {
    const auto stride = static_cast<std::size_t>(width) * 4u;
    Require(stride % 16u == 0u, "test stride alignment");
    auto config = Film(width, height);
    config.input_stride_bytes = stride;
    config.input_bytes = stride * static_cast<std::size_t>(height);
    const auto capability = Check((std::to_string(width) + "x" + std::to_string(height)).c_str(),
                                  config, Host(), CapabilityReason::ExperimentallySupported);
    Require(capability.allocation_permitted, "experimental attempt refused");
    Require(capability.geometry_validator_passed && capability.pixel_budget_ok, "accepted geometry");
    AlignedVector<std::uint8_t> input(config.input_bytes);
    reference_fixture::GeneratePhase3AFrame(width, height, 0, reference_fixture::Phase3APattern::FlatBlack,
                                            input.data(), static_cast<int>(stride), 0x1337beef);
    FilmScan3Encoder first({width, height});
    FilmScan3Encoder second({width, height});
    const auto a = first.Encode(input.data(), input.size(), stride);
    const auto b = second.Encode(input.data(), input.size(), stride);
    Require(a.sample == b.sample, "repeat encode was not deterministic");
    InspectPacket(a.sample, width, height);
    if (width == 1920 && height == 1080) {
        FilmScan3Encoder legacy;
        Require(legacy.geometry().width == 1920 && legacy.geometry().height == 1080, "default geometry");
        Require(legacy.Encode(input.data(), input.size(), stride).sample == a.sample, "default constructor bytes");
        Require(a.sample.size() == 185648u, "1080p flat golden size changed");
    }
    const auto stem = dir / (std::to_string(width) + "x" + std::to_string(height));
    {
        std::ofstream out(stem.string() + ".cfhd", std::ios::binary);
        out.write(reinterpret_cast<const char*>(a.sample.data()), static_cast<std::streamsize>(a.sample.size()));
        Require(static_cast<bool>(out), "cfhd write");
    }
    const auto avi_bytes = Avi(a.sample, width, height);
    {
        std::ofstream out(stem.string() + ".avi", std::ios::binary);
        out.write(reinterpret_cast<const char*>(avi_bytes.data()), static_cast<std::streamsize>(avi_bytes.size()));
        Require(static_cast<bool>(out), "avi write");
    }
    const auto probed = Run(Quote(Ffprobe()) +
                            " -v error -count_frames -select_streams v:0 "
                            "-show_entries stream=width,height,pix_fmt,nb_read_frames -of default=nw=1 " +
                            Quote(stem.string() + ".avi"),
                            stem.string() + ".ffprobe");
    Require(Field(probed, "width") == std::to_string(width), "ffprobe width");
    Require(Field(probed, "height") == std::to_string(height), "ffprobe height");
    Require(Field(probed, "pix_fmt") == "yuv422p10le", "ffprobe pix_fmt");
    Require(Field(probed, "nb_read_frames") == "1", "ffprobe frame count");
    const auto yuv = stem.string() + ".yuv";
    Run(Quote(Ffmpeg()) + " -v error -xerror -y -i " + Quote(stem.string() + ".avi") +
        " -frames:v 1 -pix_fmt yuv422p10le -f rawvideo " + Quote(yuv),
        stem.string() + ".ffmpeg");
    const auto decoded = std::filesystem::file_size(yuv);
    const auto expect = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4u;
    Require(decoded == expect, "decoded byte count");
    std::cout << "CODEC_PASS " << width << "x" << height
              << " bytes=" << a.sample.size()
              << " stride=" << stride
              << " ffprobe=" << width << "x" << height
              << " pix_fmt=yuv422p10le frames=1"
              << " decoded_bytes=" << decoded << '\n';
}
}

int main() {
    try {
        Require(kMaxPlaneElements == 3840u * 2160u, "pixel budget constant");
        Require(4096ull * 2024ull <= kMaxPlaneElements, "4096x2024 arithmetic");
        Require(4096ull * 2160ull > kMaxPlaneElements, "4096x2160 arithmetic");
        ValidateFilmScan3Geometry({4096, 2024});
        bool budget = false;
        try { ValidateFilmScan3Geometry({4096, 2160}); }
        catch (const std::length_error&) { budget = true; }
        Require(budget, "4096x2160 did not hit the validator budget");

        const auto host = Host();
        const struct { int w, h; } pass[] = {{128, 24}, {256, 256}, {1280, 720}, {1920, 1080}, {2048, 1080}, {3840, 2160}};
        for (const auto& item : pass) {
            const auto name = std::to_string(item.w) + "x" + std::to_string(item.h) + "-unbound";
            const auto result = Check(name.c_str(), Film(item.w, item.h), host, CapabilityReason::ExperimentallySupported);
            Require(result.allocation_permitted && result.evidence_record, "evidence attempt");
        }

        auto edge = Film(4096, 2024);
        auto edge_result = Check("4096x2024", edge, host, CapabilityReason::BackendNotQualifiedForGeometry);
        Require(edge_result.geometry_validator_passed && edge_result.pixel_budget_ok, "2024 is inside the budget");
        Require(!edge_result.evidence_record && edge_result.reasons.backend_not_qualified_for_geometry, "2024 qualification");
        Require(!edge_result.reasons.memory_budget_exceeded && !edge_result.outside_qualified_dimension_envelope, "2024 budget axis");
        std::cout << "NO_CODEC_PASS 4096x2024 reason=capability refused before encode\n";

        auto over = Check("4096x2160", Film(4096, 2160), host, CapabilityReason::MemoryBudgetExceeded);
        Require(!over.geometry_validator_passed && !over.pixel_budget_ok && over.reasons.memory_budget_exceeded, "2160 budget");
        Require(!over.outside_qualified_dimension_envelope && !over.reasons.backend_not_qualified_for_geometry, "2160 is not an envelope miss");

        auto eight = Check("7680x4320", Film(7680, 4320), host, CapabilityReason::BackendNotQualifiedForGeometry);
        Require(eight.outside_qualified_dimension_envelope && !eight.pixel_budget_ok, "8K axes");
        Require(eight.geometry_validator_invoked && !eight.geometry_validator_passed,
                "8K validator must fail closed despite wavelet-aligned shape");
        Require(!eight.allocation_permitted, "8K may not allocate");
        Require(eight.reasons.memory_budget_exceeded && eight.reasons.backend_not_qualified_for_geometry, "8K separate flags");
        Require(!eight.reasons.format_mathematically_invalid, "8K is wavelet-aligned in our current pipeline");
        auto twelve = Check("12288x8040", Film(12288, 8040), host, CapabilityReason::BackendNotQualifiedForGeometry);
        Require(twelve.outside_qualified_dimension_envelope && twelve.reasons.memory_budget_exceeded &&
                !twelve.geometry_validator_passed && !twelve.allocation_permitted,
                "12K is structurally aligned but outside current budget/qualification");

        Check("zero-width", Film(0, 1080), host, CapabilityReason::FormatMathematicallyInvalid);
        Check("negative-width", Film(-1920, 1080), host, CapabilityReason::FormatMathematicallyInvalid);
        Check("negative-height", Film(1920, -8), host, CapabilityReason::FormatMathematicallyInvalid);
        auto bad_height = Check("1920x1084", Film(1920, 1084), host, CapabilityReason::FormatMathematicallyInvalid);
        Require(bad_height.reasons.format_mathematically_invalid && !bad_height.geometry_validator_passed, "incompatible height");
        Check("200x24", Film(200, 24), host, CapabilityReason::FormatMathematicallyInvalid);
        Check("128x16", Film(128, 16), host, CapabilityReason::FormatMathematicallyInvalid);

        auto short_buffer = Film(1920, 1080);
        short_buffer.input_stride_bytes = 7680;
        short_buffer.input_bytes = 100;
        auto short_result = Check("short-input", short_buffer, host, CapabilityReason::FormatMathematicallyInvalid);
        Require(short_result.geometry_validator_passed && short_result.reasons.format_mathematically_invalid, "short buffer vs geometry");

        auto stride_overflow = Film(1920, 1080);
        stride_overflow.input_stride_bytes = ~std::uint64_t{0} - 15u;
        stride_overflow.input_bytes = ~std::uint64_t{0};
        auto stride_result = Check("stride-overflow", stride_overflow, host, CapabilityReason::FormatMathematicallyInvalid);
        Require(stride_result.geometry_validator_passed, "stride overflow still has a valid shape");

        auto product_overflow = Film(std::int64_t{1} << 62, 128);
        auto product_result = Check("product-overflow", product_overflow, host, CapabilityReason::FormatMathematicallyInvalid);
        Require(!product_result.geometry_validator_invoked && product_result.reasons.format_mathematically_invalid, "overflow must not call the int validator");

        auto huge = Check("int-domain-huge", Film(2147483520, 2147483640), host, CapabilityReason::BackendNotQualifiedForGeometry);
        Require(huge.geometry_validator_invoked && huge.outside_qualified_dimension_envelope && !huge.pixel_budget_ok, "huge aligned geometry");

        auto sample_floor = Film(1920, 1080);
        sample_floor.max_sample_bytes = 32;
        auto floor_result = Check("sample-floor", sample_floor, host, CapabilityReason::MemoryBudgetExceeded);
        Require(floor_result.geometry_validator_passed && floor_result.evidence_record && !floor_result.allocation_permitted, "sample floor");

        auto sample_ceiling = Film(1920, 1080);
        sample_ceiling.max_sample_bytes = 8ull * 1920ull * 1080ull + 16384ull + 1ull;
        Check("sample-ceiling", sample_ceiling, host, CapabilityReason::MemoryBudgetExceeded);

        auto cpu_budget = Film(1920, 1080);
        cpu_budget.cpu_memory_budget_bytes = 4096;
        auto cpu_result = Check("cpu-budget", cpu_budget, host, CapabilityReason::MemoryBudgetExceeded);
        Require(cpu_result.evidence_record && !cpu_result.reasons.backend_not_qualified_for_geometry, "budget is not loss of evidence");

        auto cuda = Film(1920, 1080);
        cuda.backend = SessionBackend::Cuda;
        auto cuda_result = Check("cuda-a2", cuda, host, CapabilityReason::BackendNotPresent);
        Require(cuda_result.reasons.backend_not_present && !cuda_result.allocation_permitted, "A2 does not qualify CUDA");
        // Presence without bound storage and a probed device size stays fail-closed.
        auto cuda_present = host; cuda_present.cuda_available = true;
        auto cuda_unbound = Check("cuda-unbound", cuda, cuda_present, CapabilityReason::ValidButNotImplemented);
        Require(cuda_unbound.requested_backend_present && !cuda_unbound.allocation_permitted, "CUDA presence alone allocated");
        cuda.input_stride_bytes = 1920u * 4u; cuda.input_bytes = cuda.input_stride_bytes * 1080u;
        auto cuda_no_memory = Check("cuda-no-device-memory", cuda, cuda_present, CapabilityReason::ValidButNotImplemented);
        Require(cuda_no_memory.requested_backend_present && !cuda_no_memory.allocation_permitted, "CUDA without probed memory allocated");

        auto metal = Film(1920, 1080);
        metal.backend = SessionBackend::Metal;
        Check("metal-absent", metal, host, CapabilityReason::BackendNotPresent);
        auto metal_device = host;
        metal_device.metal_available = true;
        auto metal_built = Check("metal-missing-limits-storage", metal, metal_device, CapabilityReason::ValidButNotImplemented);
        Require(metal_built.requested_backend_present && metal_built.geometry_validator_passed, "present is not qualified");
        metal_device.metal_max_buffer_length_bytes = 1ull << 30;
        metal_device.metal_recommended_working_set_bytes = 1ull << 30;
        metal.input_stride_bytes = 1920u * 4u;
        metal.input_bytes = metal.input_stride_bytes * 1080u;
        metal.gpu_memory_budget_bytes = 1;
        auto metal_memory = Check("metal-gpu-budget", metal, metal_device, CapabilityReason::MemoryBudgetExceeded);
        Require(!metal_memory.reasons.backend_not_qualified_for_geometry && metal_memory.reasons.memory_budget_exceeded, "gpu axes");

        for (const auto& item : pass) {
            auto c = Film(item.w, item.h); c.backend = SessionBackend::Metal;
            c.input_stride_bytes = static_cast<std::uint64_t>(item.w) * 4u + 64u;
            c.input_bytes = c.input_stride_bytes * item.h;
            const auto ceiling = 36ull * item.w * item.h + 1024u * 1024u + c.input_bytes;
            c.cpu_memory_budget_bytes = c.gpu_memory_budget_bytes = ceiling;
            auto d = metal_device;
            d.metal_recommended_working_set_bytes = ceiling;
            d.metal_max_buffer_length_bytes = c.input_bytes;
            const auto r = Check("metal-bounded-matrix", c, d, CapabilityReason::ExperimentallySupported);
            Require(r.allocation_permitted && r.evidence_record && r.effective_backend == SessionBackend::Metal,
                    "Metal matrix did not preserve backend");
            --c.cpu_memory_budget_bytes;
            Check("metal-host-ceiling-minus-one", c, d, CapabilityReason::MemoryBudgetExceeded);
            ++c.cpu_memory_budget_bytes; --c.gpu_memory_budget_bytes;
            Check("metal-gpu-ceiling-minus-one", c, d, CapabilityReason::MemoryBudgetExceeded);
            ++c.gpu_memory_budget_bytes; --d.metal_recommended_working_set_bytes;
            Check("metal-working-set-minus-one", c, d, CapabilityReason::MemoryBudgetExceeded);
            ++d.metal_recommended_working_set_bytes; --d.metal_max_buffer_length_bytes;
            Check("metal-single-buffer-minus-one", c, d, CapabilityReason::MemoryBudgetExceeded);
            d = metal_device;
            // Metal limits are not CUDA limits and vice versa: a CUDA request with
            // Metal-shaped device facts but no probed CUDA memory stays refused.
            c.backend = SessionBackend::Cuda; d.cuda_available = true;
            Check("cuda-without-probe", c, d, CapabilityReason::ValidButNotImplemented);
            for (const auto backend : {SessionBackend::Cuda, SessionBackend::Vulkan}) {
                c.backend = backend; d.cuda_available = d.vulkan_available = true;
                if (backend == SessionBackend::Vulkan)
                    Check("vulkan-still-unqualified", c, d, CapabilityReason::BackendNotQualifiedForGeometry);
            }
        }
        auto metal_unbound = Film(128, 24); metal_unbound.backend = SessionBackend::Metal;
        metal_unbound.allow_cpu_scalar_fallback = true;
        Check("metal-unbound-no-fallback", metal_unbound, metal_device, CapabilityReason::ValidButNotImplemented);
        Check("metal-absent-unbound-fallback", metal_unbound, host, CapabilityReason::ExperimentallySupported);
        metal_unbound.cpu_memory_budget_bytes = 1;
        Check("metal-absent-fallback-host-budget", metal_unbound, host, CapabilityReason::MemoryBudgetExceeded);

        auto simd = Film(1920, 1080);
        simd.backend = SessionBackend::CpuSimd;
        Check("cpu-simd", simd, host, CapabilityReason::ValidButNotImplemented);
        simd.allow_cpu_scalar_fallback = true;
        auto simd_fallback = Check("cpu-simd-fallback", simd, host, CapabilityReason::ExperimentallySupported);
        Require(simd_fallback.fell_back_to_cpu_scalar && simd_fallback.reasons.backend_not_present, "simd fallback keeps the missing-backend axis");

        for (const auto quality : {hypercine::EncodingQuality::Low, hypercine::EncodingQuality::Medium,
                                   hypercine::EncodingQuality::High, hypercine::EncodingQuality::FilmScan1,
                                   hypercine::EncodingQuality::FilmScan2, hypercine::EncodingQuality::FilmScan3}) {
            auto profile = Film(1920, 1080);
            profile.quality = quality;
            const auto admitted = Check(hypercine::EncodingQualityName(quality), profile, host,
                                        CapabilityReason::ExperimentallySupported);
            Require(admitted.allocation_permitted && ProfileAdmitted(SessionBackend::CpuScalar, quality),
                    "CPU scalar did not admit a regular profile");
        }
        auto invalid_quality = Film(1920, 1080);
        invalid_quality.quality = static_cast<hypercine::EncodingQuality>(0);
        Check("quality-zero", invalid_quality, host, CapabilityReason::FormatMathematicallyInvalid);
        invalid_quality.quality = static_cast<hypercine::EncodingQuality>(7);
        Check("quality-seven", invalid_quality, host, CapabilityReason::FormatMathematicallyInvalid);
        auto cuda_profiles = host;
        cuda_profiles.cuda_available = true;
        cuda_profiles.cuda_hardware_observed = true;
        cuda_profiles.cuda_total_global_memory_bytes = 1ull << 34;
        auto metal_profiles = host;
        metal_profiles.metal_available = true;
        metal_profiles.metal_max_buffer_length_bytes = 1ull << 30;
        metal_profiles.metal_recommended_working_set_bytes = 1ull << 30;
        for (const auto quality : {hypercine::EncodingQuality::Low, hypercine::EncodingQuality::Medium,
                                   hypercine::EncodingQuality::High, hypercine::EncodingQuality::FilmScan1,
                                   hypercine::EncodingQuality::FilmScan2, hypercine::EncodingQuality::FilmScan3}) {
            auto cuda_profile = Film(1920, 1080);
            cuda_profile.backend = SessionBackend::Cuda;
            cuda_profile.quality = quality;
            cuda_profile.allow_cpu_scalar_fallback = true;
            cuda_profile.input_stride_bytes = 1920u * 4u;
            cuda_profile.input_bytes = cuda_profile.input_stride_bytes * 1080u;
            const std::string cuda_name = std::string("cuda-") + hypercine::EncodingQualityName(quality);
            const auto cuda_admitted = Check(cuda_name.c_str(), cuda_profile,
                                             cuda_profiles, CapabilityReason::ExperimentallySupported);
            Require(cuda_admitted.allocation_permitted && cuda_admitted.effective_backend == SessionBackend::Cuda &&
                    !cuda_admitted.fell_back_to_cpu_scalar && ProfileAdmitted(SessionBackend::Cuda, quality),
                    "measured CUDA profile was not admitted on CUDA");
            auto metal_profile = cuda_profile;
            metal_profile.backend = SessionBackend::Metal;
            const std::string metal_name = std::string("metal-") + hypercine::EncodingQualityName(quality);
            const auto metal_admitted = Check(metal_name.c_str(), metal_profile,
                                             metal_profiles, CapabilityReason::ExperimentallySupported);
            Require(metal_admitted.allocation_permitted && metal_admitted.effective_backend == SessionBackend::Metal &&
                    !metal_admitted.fell_back_to_cpu_scalar && ProfileAdmitted(SessionBackend::Metal, quality),
                    "measured Metal profile was not admitted on Metal");
        }
        auto yuy2 = Film(1920, 1080);
        yuy2.pixel_format = SourcePixelFormat::Yuy2;
        Check("yuy2", yuy2, host, CapabilityReason::ValidButNotImplemented);
        auto rgb444 = Film(1920, 1080);
        rgb444.codec = CodecFamily::Rgb444;
        Check("rgb444", rgb444, host, CapabilityReason::ValidButNotImplemented);
        auto legacy = Film(1920, 1080);
        legacy.codec = CodecFamily::LegacyFixedQ2;
        auto legacy_result = Check("legacy-q2", legacy, host, CapabilityReason::ValidButNotImplemented);
        Require(!legacy_result.geometry_validator_invoked, "legacy must not use the FilmScan3 validator");
        auto gop2 = Film(1920, 1080);
        gop2.gop = GopMode::Gop2;
        Check("gop2", gop2, host, CapabilityReason::ValidButNotImplemented);
        auto interlaced = Film(1920, 1080);
        interlaced.progressive = false;
        Check("interlaced", interlaced, host, CapabilityReason::ValidButNotImplemented);
        auto embed = Film(1920, 1080);
        embed.diagnostic = DiagnosticMode::EmbedHashesInSample;
        Check("embed-hash", embed, host, CapabilityReason::ValidButNotImplemented);
        // Fail closed on declared frame rates not emitted by the present
        // FilmScan3 metadata-less writer. #262 pins its 30fps TIMC behaviour.
        // The capability must not turn a 'valid rational' into a promise that
        // this encoder has already implemented that requested timebase.
        auto rate = Film(1920, 1080);
        Check("timebase-30-1", rate, host, CapabilityReason::ExperimentallySupported);
        for (const auto candidate : {Rational{24, 1}, Rational{25, 1},
                                     Rational{50, 1}, Rational{60000, 1001},
                                     Rational{60, 2}}) {
            rate.frame_rate = candidate;
            const auto rejected = Check("timebase-not-implemented", rate, host,
                                        CapabilityReason::ValidButNotImplemented);
            Require(rejected.reasons.valid_but_not_implemented &&
                    !rejected.allocation_permitted && rejected.geometry_validator_passed,
                    "a valid unsupported requested timebase was advertised as available");
        }
        rate.frame_rate = {30, 0};
        Check("bad-rate", rate, host, CapabilityReason::FormatMathematicallyInvalid);
        rate.frame_rate = {0, 1};
        Check("zero-numerator", rate, host, CapabilityReason::FormatMathematicallyInvalid);

        auto other = Check("384x40", Film(384, 40), host, CapabilityReason::BackendNotQualifiedForGeometry);
        Require(other.geometry_validator_passed && other.pixel_budget_ok && !other.evidence_record, "other in-budget geometry");

        auto oracle = Film(1920, 1080);
        oracle.require_reference_oracle = true;
        auto oracle_result = Check("require-oracle", oracle, host, CapabilityReason::ReferenceMissing);
        Require(oracle_result.evidence_record && !oracle_result.allocation_permitted, "oracle blocks experimental");

        auto cuda_fallback = Film(1280, 720);
        cuda_fallback.backend = SessionBackend::Cuda;
        cuda_fallback.allow_cpu_scalar_fallback = true;
        auto fallback = Check("cuda-fallback", cuda_fallback, host, CapabilityReason::ExperimentallySupported);
        Require(fallback.fell_back_to_cpu_scalar && fallback.effective_backend == SessionBackend::CpuScalar, "explicit cuda fallback");

        const auto scratch = FreshScratch();
        const auto version = Run(Quote(Ffmpeg()) + " -version",
            scratch / "ffmpeg-version");
        const auto newline = version.find('\n');
        std::cout << "FFMPEG " << version.substr(0, newline == std::string::npos ? version.size() : newline) << '\n';
        const auto dir = scratch / "samples";
        Require(std::filesystem::create_directory(dir), "exclusive sample directory");
        for (const auto& item : pass) EncodeAndDecode(dir, item.w, item.h);
        std::cout << "PASS capability contract and encoded matrix\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}