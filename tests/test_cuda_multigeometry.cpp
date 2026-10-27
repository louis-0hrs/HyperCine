// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// A2 multigeometry + one-session sequence gate for the SDK-free CUDA frontend.
#include "hypercine/filmscan3.h"
#include "hypercine/wavelet.h"
#include "../src/core/filmscan3_backend.h"
#include "../src/cuda/filmscan3_cuda_stats.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace hypercine::standalone;
namespace fs = std::filesystem;

namespace {
void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

std::string Hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::uint64_t Fnv64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

AlignedVector<std::uint8_t> MakeInput(int width, int height, int frame, std::uint32_t salt) {
    AlignedVector<std::uint8_t> input(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    std::uint32_t state = 0x33c0ffeeu ^ (static_cast<std::uint32_t>(frame) * 0x9e3779b9u) ^ salt
        ^ (static_cast<std::uint32_t>(width) * 0x85ebca6bu) ^ static_cast<std::uint32_t>(height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            state = state * 1664525u + 1013904223u;
            const std::uint8_t noise = static_cast<std::uint8_t>(state >> 24);
            const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
            input[at + 0] = static_cast<std::uint8_t>((x * 13 + noise) & 255);
            input[at + 1] = static_cast<std::uint8_t>((y * 9 + (noise >> 1)) & 255);
            input[at + 2] = static_cast<std::uint8_t>(((x + y + frame * 17) * 7 + (noise >> 2)) & 255);
            input[at + 3] = 255;
        }
    }
    return input;
}

bool SameState(const FilmScan3State& a, const FilmScan3State& b) {
    return a.frame_index == b.frame_index && a.fs_rate_limiter == b.fs_rate_limiter
        && a.previous_sample_bytes == b.previous_sample_bytes;
}

struct StageDiff {
    bool found = false;
    int channel = 0, level = 0, band = 0, x = 0, y = 0;
    std::int16_t cpu = 0, gpu = 0;
};

StageDiff ComparePlane(const Plane& cpu, const Plane& gpu, int channel, int level, int band) {
    if (cpu.width != gpu.width || cpu.height != gpu.height)
        return {true, channel, level, band, 0, 0, 0, 0};
    for (int y = 0; y < cpu.height; ++y) {
        for (int x = 0; x < cpu.width; ++x) {
            const auto a = cpu.pixels[static_cast<std::size_t>(y) * cpu.stride + x];
            const auto b = gpu.pixels[static_cast<std::size_t>(y) * gpu.stride + x];
            if (a != b) return {true, channel, level, band, x, y, a, b};
        }
    }
    return {};
}

StageDiff FirstStageDifference(const detail::FrontendFrame& cpu, const detail::FrontendFrame& gpu) {
    for (int channel = 0; channel < 3; ++channel) {
        auto difference = ComparePlane(cpu.levels[channel][2].bands[0], gpu.levels[channel][2].bands[0], channel, 3, 0);
        if (difference.found) return difference;
        for (int level = 2; level >= 0; --level) {
            for (int band = 1; band < 4; ++band) {
                difference = ComparePlane(cpu.levels[channel][level].bands[band], gpu.levels[channel][level].bands[band],
                                          channel, level, band);
                if (difference.found) return difference;
            }
        }
    }
    return {};
}

std::string HashList(const FilmScan3Frame& frame) {
    std::ostringstream out;
    for (int i = 0; i < 30; ++i) {
        if (i) out << ',';
        out << Hex64(frame.coefficient_fnv64[static_cast<std::size_t>(i)]);
    }
    return out.str();
}

struct Meta {
    unsigned width_tags = 0, height_tags = 0, frame_tags = 0, ufrm = 0;
    unsigned width = 0, height = 0, frame_tag = 0, ufrm_value = 0;
};

std::uint16_t U16(const std::vector<std::uint8_t>& sample, std::size_t at) {
    Require(at + 2 <= sample.size(), "truncated CFHD tag");
    return static_cast<std::uint16_t>((sample[at] << 8) | sample[at + 1]);
}
std::uint32_t U32(const std::vector<std::uint8_t>& sample, std::size_t at) {
    return (static_cast<std::uint32_t>(U16(sample, at)) << 16) | U16(sample, at + 2);
}

void NoteUfrm(Meta& meta, const std::uint8_t* data, std::size_t size) {
    const char key[] = {'U', 'F', 'R', 'M'};
    for (std::size_t i = 0; i + 12 <= size; ++i) {
        if (std::memcmp(data + i, key, 4) != 0) continue;
        if (data[i + 4] != 4 || data[i + 5] != 0 || data[i + 6] != 0 || data[i + 7] != 'L') continue;
        const unsigned value = static_cast<unsigned>(data[i + 8])
            | (static_cast<unsigned>(data[i + 9]) << 8)
            | (static_cast<unsigned>(data[i + 10]) << 16)
            | (static_cast<unsigned>(data[i + 11]) << 24);
        ++meta.ufrm;
        meta.ufrm_value = value;
    }
}

Meta ReadMeta(const std::vector<std::uint8_t>& sample) {
    Require(sample.size() >= 20 && U32(sample, 0) == 0x10009 && U32(sample, 4) == 0x20003, "CFHD intra header");
    Meta meta;
    for (std::size_t at = 20; at + 4 <= sample.size();) {
        const unsigned raw = U16(sample, at);
        const unsigned tag = raw >= 32768 ? 65536u - raw : raw;
        const unsigned value = U16(sample, at + 2);
        at += 4;
        if (tag == 0x4002) {
            Require(value <= (sample.size() - at) / 4, "metadata extent");
            NoteUfrm(meta, sample.data() + at, static_cast<std::size_t>(value) * 4u);
            at += static_cast<std::size_t>(value) * 4u;
            continue;
        }
        if (tag == 20) { ++meta.width_tags; meta.width = value; }
        if (tag == 21) { ++meta.height_tags; meta.height = value; }
        if (tag == 69) { ++meta.frame_tags; meta.frame_tag = value; }
        const unsigned type = tag & 0xff00u;
        if (type == 0x2000 || type == 0x2100 || type == 0x2200) {
            const auto words = ((tag & 255u) << 16) | value;
            Require(words <= (sample.size() - at) / 4, "chunk extent");
            if (type == 0x2000) at += static_cast<std::size_t>(words) * 4u;
        }
    }
    return meta;
}

void CheckMeta(const std::vector<std::uint8_t>& sample, int width, int height, unsigned frame_index) {
    const Meta meta = ReadMeta(sample);
    Require(meta.width_tags >= 1 && meta.width == static_cast<unsigned>(width), "packet width tag");
    Require(meta.height_tags >= 1 && meta.height == static_cast<unsigned>(height), "packet height tag");
    Require(meta.frame_tags == 1 && meta.frame_tag == frame_index + 1, "packet frame-counter tag");
    Require(meta.ufrm == 1 && meta.ufrm_value == frame_index, "packet UFRM metadata");
}

struct Session {
    FilmScan3Encoder cpu;
    FilmScan3Encoder gpu;
    std::shared_ptr<detail::Frontend> cpu_frontend;
    std::shared_ptr<detail::Frontend> gpu_frontend;
    explicit Session(FilmScan3Geometry geometry, detail::CudaTestMode mode = detail::CudaTestMode::Normal)
        : cpu(geometry), gpu(geometry),
          cpu_frontend(detail::CpuFrontend(geometry)),
          gpu_frontend(detail::CudaFrontend(geometry, mode)) {
        detail::FrontendAccess::Use(gpu, gpu_frontend);
    }
};

struct Compared {
    FilmScan3Frame cpu;
    FilmScan3Frame gpu;
    StageDiff stage{};
    bool audited = false;
    std::uint64_t launches_before = 0;
    std::uint64_t launches_after = 0;
};

Compared EncodePair(Session& session, const AlignedVector<std::uint8_t>& input, int width, int height, bool audit) {
    Compared out;
    out.launches_before = detail::CudaSpatialDispatchCount(*session.gpu_frontend);
    const auto* bytes = input.data();
    const auto size = input.size();
    const auto stride = static_cast<std::size_t>(width) * 4u;
    if (audit) {
        const auto cpu_audit = session.cpu_frontend->Transform(bytes, size, width, height, stride, true);
        const auto gpu_audit = session.gpu_frontend->Transform(bytes, size, width, height, stride, true);
        out.stage = FirstStageDifference(cpu_audit, gpu_audit);
        out.audited = true;
    }
    out.cpu = session.cpu.Encode(bytes, size, stride);
    out.gpu = session.gpu.Encode(bytes, size, stride);
    out.launches_after = detail::CudaSpatialDispatchCount(*session.gpu_frontend);
    return out;
}

void RequireDeviceIsCuda(const Session& session, const std::string& device_name) {
    const auto name = session.gpu_frontend->DeviceName();
    Require(name.find("CUDA") != std::string::npos, "CUDA frontend did not identify itself: " + name);
    Require(name.find(device_name) != std::string::npos, "frontend device does not match cudaGetDeviceProperties: " + name);
    Require(name.find("CPU scalar") == std::string::npos, "CPU frontend was selected");
}

std::string Tool(const char* env, const char* fallback) {
    if (const char* value = std::getenv(env); value && *value) return value;
    if (fs::exists(fallback)) return fallback;
    return {};
}

// Run an external tool and return its combined output.
//
// The previous version used fork/execv/pipe/read, which is POSIX-only: the target
// is registered unconditionally in the CUDA-enabled standalone block, so on native
// Windows/MSVC with CUDA ON it did not compile (C1083 on <sys/wait.h>). The
// repository has no subprocess abstraction and the core codec may not gain a
// dependency for one test, so the portable floor is used: a shell redirect into a
// temporary file, read back with the standard library.
//
// The arguments are quoted and the shell metacharacters that matter for a redirect
// cannot appear unquoted. The tool path is the environment variable when that
// variable is set and non-empty; otherwise Tool() uses its fallback argument if
// that file exists. The path is never taken from remote input.
std::string ShellQuote(const std::string& value) {
    std::string quoted = "\"";
    for (const char c : value) {
        if (c == '"') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

int SystemStatusToExitCode(int status) {
#ifdef _WIN32
    // On Windows std::system returns the child's exit code directly.
    return status;
#else
    // On POSIX it returns a wait status; decode it the same way WEXITSTATUS does.
    if (status == -1) return -1;
    if ((status & 0x7f) == 0) return (status >> 8) & 0xff;
    return 128 + (status & 0x7f);  // terminated by a signal
#endif
}

struct ChildOutput {
    int exit_code = -1;
    std::string text;
};

ChildOutput RunTool(const std::vector<std::string>& args, const fs::path& capture_path) {
    std::string command;
    for (const auto& arg : args) {
        if (!command.empty()) command += ' ';
        command += ShellQuote(arg);
    }
    // Merge stderr so a failing tool's diagnosis is not lost on one platform only.
    command += " > " + ShellQuote(capture_path.string()) + " 2>&1";
#ifdef _WIN32
    // cmd.exe strips the first/last quotes of a command beginning with a
    // quoted executable. With a redirect, the whole command needs another
    // pair of quotes; otherwise even ffprobe -version fails to launch.
    command = "\"" + command + "\"";
#endif
    const int status = std::system(command.c_str());
    ChildOutput out;
    out.exit_code = SystemStatusToExitCode(status);
    std::ifstream stream(capture_path, std::ios::binary);
    if (stream) {
        out.text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    return out;
}

std::string Capture(const std::vector<std::string>& args, const fs::path& capture_path) {
    const auto out = RunTool(args, capture_path);
    if (out.exit_code != 0) {
        throw std::runtime_error(args[0] + " failed with exit code " +
                                 std::to_string(out.exit_code) + ":\n" + out.text);
    }
    return out.text;
}

void Append(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}
void AppendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void AppendI32(std::vector<std::uint8_t>& out, std::int32_t value) { AppendU32(out, static_cast<std::uint32_t>(value)); }
std::vector<std::uint8_t> Chunk(const char* tag, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    Append(out, tag, 4);
    AppendU32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    if (data.size() & 1u) out.push_back(0);
    return out;
}

std::vector<std::uint8_t> Avi(const std::vector<std::uint8_t>& packet, int width, int height) {
    std::vector<std::uint8_t> avih;
    const std::uint32_t avih_words[] = {40000, 0, 0, 0, 1, 0, 1, static_cast<std::uint32_t>(packet.size()),
                                        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 0, 0, 0, 0};
    for (std::uint32_t word : avih_words) AppendU32(avih, word);
    std::vector<std::uint8_t> strh;
    Append(strh, "vids", 4);
    Append(strh, "CFHD", 4);
    AppendU32(strh, 0);
    AppendU16(strh, 0);
    AppendU16(strh, 0);
    const std::uint32_t strh_words[] = {0, 1, 25, 0, 1, static_cast<std::uint32_t>(packet.size()), 0xffffffffu, 0};
    for (std::uint32_t word : strh_words) AppendU32(strh, word);
    AppendU16(strh, 0);
    AppendU16(strh, 0);
    AppendU16(strh, static_cast<std::uint16_t>(width));
    AppendU16(strh, static_cast<std::uint16_t>(height));
    std::vector<std::uint8_t> strf;
    AppendU32(strf, 40);
    AppendI32(strf, width);
    AppendI32(strf, height);
    AppendU16(strf, 1);
    AppendU16(strf, 24);
    Append(strf, "CFHD", 4);
    AppendU32(strf, static_cast<std::uint32_t>(packet.size()));
    AppendI32(strf, 0);
    AppendI32(strf, 0);
    AppendU32(strf, 0);
    AppendU32(strf, 0);
    std::vector<std::uint8_t> strl_body;
    Append(strl_body, "strl", 4);
    const auto strh_chunk = Chunk("strh", strh);
    const auto strf_chunk = Chunk("strf", strf);
    strl_body.insert(strl_body.end(), strh_chunk.begin(), strh_chunk.end());
    strl_body.insert(strl_body.end(), strf_chunk.begin(), strf_chunk.end());
    std::vector<std::uint8_t> hdrl_body;
    Append(hdrl_body, "hdrl", 4);
    const auto avih_chunk = Chunk("avih", avih);
    const auto strl_chunk = Chunk("LIST", strl_body);
    hdrl_body.insert(hdrl_body.end(), avih_chunk.begin(), avih_chunk.end());
    hdrl_body.insert(hdrl_body.end(), strl_chunk.begin(), strl_chunk.end());
    std::vector<std::uint8_t> movi_body;
    Append(movi_body, "movi", 4);
    const auto frame = Chunk("00dc", packet);
    movi_body.insert(movi_body.end(), frame.begin(), frame.end());
    std::vector<std::uint8_t> riff_body;
    Append(riff_body, "AVI ", 4);
    const auto hdrl = Chunk("LIST", hdrl_body);
    const auto movi = Chunk("LIST", movi_body);
    riff_body.insert(riff_body.end(), hdrl.begin(), hdrl.end());
    riff_body.insert(riff_body.end(), movi.begin(), movi.end());
    return Chunk("RIFF", riff_body);
}

std::string Field(const std::string& text, const std::string& key) {
    const auto at = text.find(key + "=");
    Require(at != std::string::npos, "ffprobe missing " + key + " in: " + text);
    const auto start = at + key.size() + 1;
    const auto end = text.find('\n', start);
    std::string value = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // ffprobe prints CRLF on Windows; a raw CR is not part of the field value.
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return value;
}

void WriteFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    Require(static_cast<bool>(stream), "cannot write " + path.string());
}

std::vector<std::uint8_t> ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(static_cast<bool>(stream), "cannot read " + path.string());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// Directory this call created. create_directory is atomic: an existing path is
// skipped and never deleted. Ownership is that successful create, not the
// candidate string. A clock tick, a process-local counter and the attempt index
// only spread names. Concurrent processes and leftovers from an earlier failed
// run stay untouched.
fs::path OwnedGeometryDirectory(int width, int height) {
    static std::atomic<unsigned> sequence{0};
    const auto base = fs::temp_directory_path();
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto candidate = base / ("hypercine-cuda-geometry-" + std::to_string(tick) + "-" +
                                       std::to_string(++sequence) + "-" + std::to_string(attempt) + "-" +
                                       std::to_string(width) + "x" + std::to_string(height));
        std::error_code ec;
        if (fs::create_directory(candidate, ec))
            return candidate;
        if (!ec || ec == std::errc::file_exists)
            continue;
        throw fs::filesystem_error("cannot create isolated CUDA geometry directory", candidate, ec);
    }
    throw std::runtime_error("could not create exclusive CUDA geometry directory");
}


// No CUDA device or FFmpeg installation is required for this subprocess/line
// ending contract. On Windows the command script and capture file both have
// spaces in their paths: this reproduces the native cmd.exe quoting failure.
int RunToolContract() {
    try {
        Require(Field("codec_name=cfhd\r\nwidth=1920\r\npix_fmt=yuv422p10le\r\n", "codec_name") == "cfhd",
                "CRLF codec field");
        Require(Field("codec_name=cfhd\r\nwidth=1920\r\npix_fmt=yuv422p10le\r\n", "width") == "1920",
                "CRLF width field");
        Require(Field("codec_name=cfhd\r\nwidth=1920\r\npix_fmt=yuv422p10le\r\n", "pix_fmt") == "yuv422p10le",
                "CRLF pixel-format field");
        Require(Field("width=1920\n", "width") == "1920", "LF field");

        const fs::path owned = OwnedGeometryDirectory(0, 0);
        const fs::path folder = owned / "path with spaces";
        fs::create_directory(folder);
        const fs::path capture = folder / "capture with spaces.txt";
        std::string executable;
#ifdef _WIN32
        const fs::path script = folder / "mock ffprobe.cmd";
        std::ofstream stub(script, std::ios::binary);
        Require(static_cast<bool>(stub), "cannot create subprocess fixture");
        stub << "@echo off\r\necho ffprobe version mock\r\n";
        stub.close();
        Require(static_cast<bool>(stub), "cannot write subprocess fixture");
        executable = script.string();
#else
        // The POSIX contract also covers an argument and capture path with
        // spaces. The Windows-specific executable-path case is above.
        executable = "/bin/echo";
#endif
        const auto output = Capture({executable, "ffprobe version mock"}, capture);
        Require(output.find("ffprobe version mock") != std::string::npos,
                "quoted subprocess did not return the expected output");
        std::error_code cleanup_ignored;
        fs::remove_all(owned, cleanup_ignored);
        std::cout << "PASS cuda subprocess/CRLF contract\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL cuda subprocess/CRLF contract: " << error.what() << "\n";
        return 1;
    }
}

// Independent FFmpeg decode. Checks container-reported codec, dimensions and
// pix_fmt, then the raw yuv422p10le byte count and CPU/CUDA pixel equality.
void DecodePair(const std::string& ffmpeg, const std::string& ffprobe, int width, int height,
                const std::vector<std::uint8_t>& cpu, const std::vector<std::uint8_t>& gpu) {
    const fs::path owned = OwnedGeometryDirectory(width, height);
    try {
        WriteFile(owned / "cpu.avi", Avi(cpu, width, height));
        WriteFile(owned / "gpu.avi", Avi(gpu, width, height));
        for (const char* side : {"cpu", "gpu"}) {
            const auto probe = Capture({ffprobe, "-v", "error", "-select_streams", "v:0",
                                        "-show_entries", "stream=codec_name,width,height,pix_fmt",
                                        "-of", "default=noprint_wrappers=1", (owned / (std::string(side) + ".avi")).string()},
                                       owned / (std::string(side) + ".probe.txt"));
            Require(Field(probe, "codec_name") == "cfhd", side + probe);
            Require(Field(probe, "width") == std::to_string(width), side + probe);
            Require(Field(probe, "height") == std::to_string(height), side + probe);
            Require(Field(probe, "pix_fmt") == "yuv422p10le", side + probe);
            Capture({ffmpeg, "-v", "error", "-xerror", "-y", "-i", (owned / (std::string(side) + ".avi")).string(),
                     "-frames:v", "1", "-pix_fmt", "yuv422p10le", "-f", "rawvideo",
                     (owned / (std::string(side) + ".yuv")).string()},
                    owned / (std::string(side) + ".decode.txt"));
        }
        const auto cpu_raw = ReadFile(owned / "cpu.yuv");
        const auto gpu_raw = ReadFile(owned / "gpu.yuv");
        const auto expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        Require(cpu_raw.size() == expected && gpu_raw.size() == expected, "decoded byte count");
        Require(cpu_raw == gpu_raw, "CPU and CUDA FFmpeg decodes differ");
        // Success path only. `owned` is the directory this call created; a failed
        // decode leaves it in place, and nothing else is removed.
        std::error_code cleanup_ignored;
        fs::remove_all(owned, cleanup_ignored);
    } catch (...) {
        std::cerr << "FFMPEG artifacts left in the temporary directory\n";
        throw;
    }
}

int RunGeometry(int width, int height, const std::string& device_name, const std::string& ffmpeg, const std::string& ffprobe) {
    const bool want_ffmpeg = (width == 1920 && height == 1080) || (width == 3840 && height == 2160);
    try {
        Session session({width, height, 0});
        RequireDeviceIsCuda(session, device_name);
        const auto input = MakeInput(width, height, 0, 0x4c315445u);
        const auto compared = EncodePair(session, input, width, height, true);
        const auto launches = compared.launches_after - compared.launches_before;
        Require(!compared.stage.found, "pre-quantized coefficients differ");
        Require(FilmScan3FullCoefficientHashesEqual(compared.cpu, compared.gpu), "coefficient hashes are not Full and equal");
        Require(compared.cpu.sample == compared.gpu.sample, "CFHD packets differ");
        Require(SameState(compared.cpu.state, compared.gpu.state), "frame state differs");
        Require(SameState(session.cpu.state(), session.gpu.state()), "encoder state differs");
        Require(launches == 12, "expected 12 CUDA spatial launches (6 audit + 6 encode), got " + std::to_string(launches));
        CheckMeta(compared.gpu.sample, width, height, 0);
        std::string ffmpeg_result = "not-required";
        if (want_ffmpeg) {
            Require(!ffmpeg.empty() && !ffprobe.empty(), "ffmpeg/ffprobe required for this geometry");
            DecodePair(ffmpeg, ffprobe, width, height, compared.cpu.sample, compared.gpu.sample);
            ffmpeg_result = "PASS";
        }
        std::cout << "ROW geometry=" << width << 'x' << height
                  << " parity=PASS packet=PASS hashes=30/30 full"
                  << " cuda_launches=" << launches
                  << " packet_bytes=" << compared.cpu.sample.size()
                  << " packet_fnv64=" << Hex64(Fnv64(compared.cpu.sample))
                  << " ffmpeg=" << ffmpeg_result
                  << " device=\"" << session.gpu_frontend->DeviceName() << "\"\n"
                  << "HASHES " << width << 'x' << height << ' ' << HashList(compared.cpu) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "ROW geometry=" << width << 'x' << height << " parity=FAIL packet=FAIL ffmpeg="
                  << (want_ffmpeg ? "FAIL" : "not-required") << " detail=\"" << error.what() << "\"\n";
        return 1;
    }
}

int RunSequence(const std::string& device_name) {
    constexpr int width = 1920, height = 1080, frames = 16;
    try {
        Session session({width, height, 0});
        RequireDeviceIsCuda(session, device_name);
        std::vector<std::vector<std::uint8_t>> saved;
        bool limiter_moved = false;
        for (int frame = 0; frame < frames; ++frame) {
            const auto input = MakeInput(width, height, frame, 0x53514e43u);
            const auto compared = EncodePair(session, input, width, height, true);
            Require(!compared.stage.found, "sequence stage mismatch at frame " + std::to_string(frame));
            Require(FilmScan3FullCoefficientHashesEqual(compared.cpu, compared.gpu), "sequence hash mismatch");
            Require(compared.cpu.sample == compared.gpu.sample, "sequence packet mismatch");
            Require(SameState(compared.cpu.state, compared.gpu.state), "sequence frame state mismatch");
            Require(SameState(session.cpu.state(), session.gpu.state()), "sequence encoder state mismatch");
            Require(compared.cpu.state.frame_index == static_cast<unsigned>(frame), "frame counter");
            Require(session.cpu.state().frame_index == static_cast<unsigned>(frame + 1), "post-frame counter");
            CheckMeta(compared.gpu.sample, width, height, static_cast<unsigned>(frame));
            if (compared.cpu.state.fs_rate_limiter != 4) limiter_moved = true;
            saved.push_back(compared.cpu.sample);
            std::cout << "FRAME " << frame
                      << " index=" << compared.cpu.state.frame_index
                      << " limiter=" << compared.cpu.state.fs_rate_limiter
                      << " previous=" << compared.cpu.state.previous_sample_bytes
                      << " bytes=" << compared.cpu.sample.size()
                      << " packet_fnv64=" << Hex64(Fnv64(compared.cpu.sample))
                      << " hashes=30/30 " << HashList(compared.cpu) << "\n";
        }
        const auto launches = detail::CudaSpatialDispatchCount(*session.gpu_frontend);
        Require(launches == static_cast<std::uint64_t>(frames) * 12u, "sequence launch count " + std::to_string(launches));
        Require(limiter_moved, "rate limiter never left 4; sequence did not exercise rate state");
        const auto after = session.gpu.state();
        session.cpu.Reset();
        session.gpu.Reset();
        Require(session.cpu.state().frame_index == 0 && session.gpu.state().frame_index == 0, "reset frame counter");
        Require(session.cpu.state().fs_rate_limiter == 4 && session.gpu.state().fs_rate_limiter == 4, "reset limiter");
        Require(session.cpu.state().previous_sample_bytes == 0 && session.gpu.state().previous_sample_bytes == 0, "reset rate memory");
        for (int frame = 0; frame < frames; ++frame) {
            const auto input = MakeInput(width, height, frame, 0x53514e43u);
            const auto repeated = EncodePair(session, input, width, height, false);
            Require(repeated.cpu.sample == repeated.gpu.sample, "reset repeat packet mismatch");
            Require(repeated.cpu.sample == saved[static_cast<std::size_t>(frame)], "reset repeat is not deterministic");
            Require(FilmScan3FullCoefficientHashesEqual(repeated.cpu, repeated.gpu), "reset repeat hashes");
            Require(SameState(repeated.cpu.state, repeated.gpu.state), "reset repeat state");
        }
        std::cout << "ROW sequence=16x1920x1080 parity=PASS packet=PASS metadata=PASS rate_state=PASS"
                  << " reset=PASS deterministic_repeat=PASS cuda_launches=" << launches
                  << " end_index=" << after.frame_index << " end_limiter=" << after.fs_rate_limiter << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "ROW sequence=16x1920x1080 parity=FAIL detail=\"" << error.what() << "\"\n";
        return 1;
    }
}

struct ParallelResult {
    std::string error;
    std::vector<std::vector<std::uint8_t>> packets;
    FilmScan3State end{};
};

void RunOneSession(int frames, std::uint32_t salt, const std::string& device_name, ParallelResult& out) {
    try {
        constexpr int width = 1920, height = 1080;
        Session session({width, height, 0});
        RequireDeviceIsCuda(session, device_name);
        for (int frame = 0; frame < frames; ++frame) {
            const auto input = MakeInput(width, height, frame, salt);
            const auto* bytes = input.data();
            const auto stride = static_cast<std::size_t>(width) * 4u;
            const auto cpu = session.cpu.Encode(bytes, input.size(), stride);
            const auto gpu = session.gpu.Encode(bytes, input.size(), stride);
            Require(cpu.sample == gpu.sample, "parallel packet mismatch");
            Require(FilmScan3FullCoefficientHashesEqual(cpu, gpu), "parallel hashes");
            Require(SameState(cpu.state, gpu.state) && SameState(session.cpu.state(), session.gpu.state()), "parallel state");
            Require(cpu.state.frame_index == static_cast<unsigned>(frame), "parallel frame counter");
            CheckMeta(gpu.sample, width, height, static_cast<unsigned>(frame));
            out.packets.push_back(cpu.sample);
        }
        out.end = session.gpu.state();
    } catch (const std::exception& error) {
        out.error = error.what();
    }
}

int RunParallel(const std::string& device_name) {
    try {
        const auto before = detail::ProcessCudaSpatialDispatchTotal();
        ParallelResult first, second;
        std::thread a(RunOneSession, 4, 0xA11u, device_name, std::ref(first));
        std::thread b(RunOneSession, 4, 0xB22u, device_name, std::ref(second));
        a.join();
        b.join();
        Require(first.error.empty(), "session A: " + first.error);
        Require(second.error.empty(), "session B: " + second.error);
        Require(first.packets.size() == 4 && second.packets.size() == 4, "parallel frame count");
        Require(first.packets != second.packets, "parallel sessions produced the same packets");
        Require(first.end.frame_index == 4 && second.end.frame_index == 4, "parallel frame counters crossed");
        const auto launches = detail::ProcessCudaSpatialDispatchTotal() - before;
        Require(launches == 48, "parallel launch count " + std::to_string(launches));
        std::cout << "ROW parallel_sessions=2 frames=4 geometry=1920x1080 parity=PASS packet=PASS"
                  << " isolated=PASS cuda_launches=" << launches << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "ROW parallel_sessions=2 parity=FAIL detail=\"" << error.what() << "\"\n";
        return 1;
    }
}

int RunMidframe(const std::string& device_name) {
    constexpr int width = 1920, height = 1080;
    try {
        Session session({width, height, 0});
        RequireDeviceIsCuda(session, device_name);
        const auto first_input = MakeInput(width, height, 0, 0x45525231u);
        const auto first = EncodePair(session, first_input, width, height, false);
        Require(first.cpu.sample == first.gpu.sample, "pre-failure packet");
        const auto committed = session.gpu.state();
        const auto launches_after_first = detail::CudaSpatialDispatchCount(*session.gpu_frontend);
        Require(launches_after_first == 6, "first frame launches");
        detail::ArmCudaMidframeFailureAfter(*session.gpu_frontend, launches_after_first + 2);
        const auto second_input = MakeInput(width, height, 1, 0x45525231u);
        bool threw = false;
        std::string what;
        try {
            session.gpu.Encode(second_input.data(), second_input.size(), static_cast<std::size_t>(width) * 4u);
        } catch (const std::exception& error) {
            threw = true;
            what = error.what();
        }
        detail::ArmCudaMidframeFailureAfter(*session.gpu_frontend, 0);
        Require(threw, "mid-frame CUDA error did not fail the encode");
        Require(what.find("CUDA FilmScan3 mid-frame failure:") != std::string::npos, what);
        Require(what.find("code=2") != std::string::npos, "expected cudaErrorMemoryAllocation code=2, got: " + what);
        Require(SameState(session.gpu.state(), committed), "failed frame committed encoder state");
        Require(cudaPeekAtLastError() == cudaSuccess, "CUDA last-error leaked past the failed frame");
        const auto launches_after_fail = detail::CudaSpatialDispatchCount(*session.gpu_frontend);
        Require(launches_after_fail == launches_after_first + 2, "failure was not mid-frame");
        const auto cpu_second = session.cpu.Encode(second_input.data(), second_input.size(), static_cast<std::size_t>(width) * 4u);
        const auto gpu_second = session.gpu.Encode(second_input.data(), second_input.size(), static_cast<std::size_t>(width) * 4u);
        Require(cpu_second.sample == gpu_second.sample, "frame after CUDA error does not match CPU");
        Require(FilmScan3FullCoefficientHashesEqual(cpu_second, gpu_second), "hashes after CUDA error");
        Require(SameState(session.cpu.state(), session.gpu.state()), "state diverged after CUDA error");
        Require(session.gpu.state().frame_index == 2, "frame counter skipped or stalled");
        std::cout << "ROW midframe=FIRED state_unchanged=PASS next_frame=PASS detail=\"" << what << "\"\n";
        std::cout << "NEGATIVE midframe-cuda-error=FIRED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "ROW midframe=FAIL detail=\"" << error.what() << "\"\n";
        std::cout << "NEGATIVE midframe-cuda-error=DID_NOT_FIRE\n";
        return 1;
    }
}

int RunNegative(const char* name, detail::CudaTestMode mode, bool expect_launches) {
    constexpr int width = 256, height = 256;
    try {
        Session session({width, height, 0}, mode);
        const auto input = MakeInput(width, height, 0, 0x4e454731u);
        const auto compared = EncodePair(session, input, width, height, true);
        const auto launches = compared.launches_after - compared.launches_before;
        const bool stages_differ = compared.stage.found;
        const bool packets_differ = compared.cpu.sample != compared.gpu.sample;
        const bool hashes_differ = !FilmScan3FullCoefficientHashesEqual(compared.cpu, compared.gpu);
        if (expect_launches) Require(launches == 12, "alter-output did not launch CUDA");
        else Require(launches == 0, "skip-kernel still counted a CUDA launch");
        if (!(stages_differ && packets_differ && hashes_differ)) {
            std::cout << "NEGATIVE " << name << "=DID_NOT_FIRE stages=" << stages_differ
                      << " packets=" << packets_differ << " hashes=" << hashes_differ
                      << " launches=" << launches << "\n";
            return 1;
        }
        std::cout << "NEGATIVE " << name << "=FIRED"
                  << " channel=" << compared.stage.channel << " level=" << compared.stage.level
                  << " band=" << compared.stage.band << " x=" << compared.stage.x << " y=" << compared.stage.y
                  << " cpu=" << compared.stage.cpu << " gpu=" << compared.stage.gpu
                  << " launches=" << launches << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "NEGATIVE " << name << "=DID_NOT_FIRE detail=\"" << error.what() << "\"\n";
        return 1;
    }
}

int RunPrequantNegative() {
    constexpr int width = 256, height = 256;
    try {
        Session session({width, height, 0});
        const auto input = MakeInput(width, height, 3, 0x50524551u);
        const auto* bytes = input.data();
        const auto stride = static_cast<std::size_t>(width) * 4u;
        const auto cpu_audit = session.cpu_frontend->Transform(bytes, input.size(), width, height, stride, true);
        auto gpu_audit = session.gpu_frontend->Transform(bytes, input.size(), width, height, stride, true);
        Require(!FirstStageDifference(cpu_audit, gpu_audit).found, "clean prequant stages already differ");
        auto& pixel = gpu_audit.levels[0][2].bands[3].pixels;
        Require(!pixel.empty(), "no prequant coefficient to alter");
        pixel[0] = static_cast<std::int16_t>(pixel[0] ^ 0x0001);
        const auto difference = FirstStageDifference(cpu_audit, gpu_audit);
        const auto cpu = session.cpu.Encode(bytes, input.size(), stride);
        const auto gpu = session.gpu.Encode(bytes, input.size(), stride);
        const bool packet_equal = cpu.sample == gpu.sample;
        const bool hashes_equal = FilmScan3FullCoefficientHashesEqual(cpu, gpu);
        if (!difference.found || !packet_equal || !hashes_equal) {
            std::cout << "NEGATIVE alter-pre-quantized-stage=DID_NOT_FIRE"
                      << " stage=" << difference.found << " packet_equal=" << packet_equal
                      << " hashes_equal=" << hashes_equal << "\n";
            return 1;
        }
        std::cout << "NEGATIVE alter-pre-quantized-stage=FIRED"
                  << " channel=" << difference.channel << " level=" << difference.level
                  << " band=" << difference.band << " cpu=" << difference.cpu << " gpu=" << difference.gpu
                  << " packet_still_equal=1 hashes_still_full=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "NEGATIVE alter-pre-quantized-stage=DID_NOT_FIRE detail=\"" << error.what() << "\"\n";
        return 1;
    }
}
} // namespace

int main(int argc, char** argv) {
    bool gpu_required = false;
    bool tool_contract = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu-required") == 0) gpu_required = true;
        else if (std::strcmp(argv[i], "--tool-contract") == 0) tool_contract = true;
        else {
            std::cerr << "usage: test_cuda_multigeometry [--gpu-required | --tool-contract]\n";
            return 2;
        }
    }
    if (gpu_required && tool_contract) {
        std::cerr << "--gpu-required and --tool-contract cannot be combined\n";
        return 2;
    }
    if (tool_contract) return RunToolContract();
    int device_count = 0;
    const auto count_status = cudaGetDeviceCount(&device_count);
    if (count_status != cudaSuccess || device_count < 1) {
        if (gpu_required) {
            std::cerr << "FAIL: --gpu-required and no CUDA device (" << cudaGetErrorString(count_status) << ")\n";
            return 1;
        }
        std::cerr << "SKIP77: CUDA GPU required (" << cudaGetErrorString(count_status) << ")\n";
        return 77;
    }
    cudaDeviceProp device{};
    if (cudaGetDeviceProperties(&device, 0) != cudaSuccess) {
        std::cerr << "FAIL: cudaGetDeviceProperties\n";
        return 1;
    }
    if (!HyperCine_IsCUDAAvailable()) {
        std::cerr << "FAIL: CUDA device exists but HyperCine_IsCUDAAvailable is false\n";
        return 1;
    }
    std::size_t free_bytes = 0, total_bytes = 0;
    cudaMemGetInfo(&free_bytes, &total_bytes);
    int runtime = 0, driver = 0;
    cudaRuntimeGetVersion(&runtime);
    cudaDriverGetVersion(&driver);
    std::cout << "GPU: " << device.name << " compute " << device.major << '.' << device.minor << "\n"
              << "VRAM_CUDA: " << (free_bytes / (1024 * 1024)) << " MiB free / " << (total_bytes / (1024 * 1024))
              << " MiB total\n"
              << "CUDA runtime/driver: " << runtime << '/' << driver << "\n"
              << "GPU_REQUIRED: enforced device=\"" << device.name << "\" launches_must_be_nonzero=1"
              << " cpu_fallback=excluded\n";

    // Reachable fallbacks, not absent paths: used only when the matching variable
    // is unset or empty and the file exists. "/usr/bin/ffmpeg" and "/usr/bin/ffprobe".
    const auto ffmpeg = Tool("HYPERCINE_FFMPEG_EXECUTABLE", "/usr/bin/ffmpeg");
    const auto ffprobe = Tool("HYPERCINE_FFPROBE_EXECUTABLE", "/usr/bin/ffprobe");
    const std::string device_name = device.name;
    int failures = 0;
    for (const auto [width, height] : {std::pair{128, 24}, {256, 256}, {1280, 720}, {1920, 1080}, {2048, 1080}, {3840, 2160}})
        failures += RunGeometry(width, height, device_name, ffmpeg, ffprobe);
    failures += RunNegative("skip-kernel", detail::CudaTestMode::SkipKernel, false);
    failures += RunNegative("alter-output", detail::CudaTestMode::AlterOutput, true);
    failures += RunPrequantNegative();
    failures += RunSequence(device_name);
    failures += RunParallel(device_name);
    failures += RunMidframe(device_name);
    if (failures) {
        std::cout << "FAIL cuda multigeometry failures=" << failures << "\n";
        return 1;
    }
    std::cout << "PASS cuda multigeometry\n";
    return 0;
}
