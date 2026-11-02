// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Bounded, opt-in Windows probe of original GoPro SDK geometry handling.
// Each case runs in a child process because this is a reference-SDK experiment.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <malloc.h>

#include "CFHDDecoder.h"
#include "CFHDEncoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kSeed = 0x1337BEEFu;
constexpr DWORD kCaseTimeoutMs = 15u * 60u * 1000u;
constexpr std::uint64_t kReserveBytes = 1024ull * 1024ull * 1024ull;

struct Geometry {
    int width;
    int height;
};

constexpr std::array<Geometry, 4> kGeometries{{
    {3840, 2160},
    {7680, 4320},
    {12288, 8040},
    {17520, 8040},
}};

constexpr std::array<const char*, 3> kPatterns{{"flat", "gradient", "noise"}};

struct EncoderCloser {
    void operator()(void* handle) const noexcept {
        if (handle) (void)CFHD_CloseEncoder(handle);
    }
};

struct DecoderCloser {
    void operator()(void* handle) const noexcept {
        if (handle) (void)CFHD_CloseDecoder(handle);
    }
};

struct AlignedFree {
    void operator()(std::uint8_t* pointer) const noexcept {
        if (pointer) _aligned_free(pointer);
    }
};

using EncoderPtr = std::unique_ptr<void, EncoderCloser>;
using DecoderPtr = std::unique_ptr<void, DecoderCloser>;
using AlignedBytes = std::unique_ptr<std::uint8_t, AlignedFree>;

const char* ErrorClass(CFHD_Error error) {
    if (error == CFHD_ERROR_OUTOFMEMORY || error == CFHD_ERROR_DECODE_BUFFER_SIZE)
        return "RESOURCE_EXCEEDED";
    if (error == CFHD_ERROR_INVALID_ARGUMENT || error == CFHD_ERROR_BADFORMAT ||
        error == CFHD_ERROR_BAD_RESOLUTION || error == CFHD_ERROR_BAD_PIXEL_SIZE)
        return "UNSUPPORTED_GEOMETRY";
    return "SDK_ERROR";
}

std::string Hex(const std::uint8_t* bytes, std::size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) out << std::setw(2) << unsigned(bytes[i]);
    return out.str();
}

bool Sha256(const std::uint8_t* data, std::size_t size, std::string* digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD returned = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                   nullptr, 0);
    if (status < 0) return false;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                               &returned, 0);
    if (status < 0 || objectBytes == 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<std::uint8_t> object(objectBytes);
    status = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0);
    if (status >= 0) {
        std::size_t offset = 0;
        while (offset < size && status >= 0) {
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(size - offset, 1u << 20));
            status = BCryptHashData(hash, const_cast<PUCHAR>(data + offset), chunk, 0);
            offset += chunk;
        }
    }
    std::array<std::uint8_t, 32> result{};
    if (status >= 0)
        status = BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return false;
    *digest = Hex(result.data(), result.size());
    return true;
}

std::uint32_t NextRandom(std::uint32_t* state) {
    std::uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

void FillInput(std::uint8_t* frame, int width, int height, const char* pattern) {
    std::uint32_t state = kSeed ^ static_cast<std::uint32_t>(width) ^
                          (static_cast<std::uint32_t>(height) << 1);
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (std::string(pattern) == "flat") {
        for (std::size_t i = 0; i < pixels; ++i) {
            frame[i * 4 + 0] = 32;
            frame[i * 4 + 1] = 96;
            frame[i * 4 + 2] = 160;
            frame[i * 4 + 3] = 255;
        }
        return;
    }
    if (std::string(pattern) == "gradient") {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
                frame[offset + 0] = static_cast<std::uint8_t>((x * 255ll) / (width - 1));
                frame[offset + 1] = static_cast<std::uint8_t>((y * 255ll) / (height - 1));
                frame[offset + 2] = static_cast<std::uint8_t>((x + 3ll * y) & 255);
                frame[offset + 3] = 255;
            }
        }
        return;
    }
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t value = NextRandom(&state);
        frame[i * 4 + 0] = static_cast<std::uint8_t>(value);
        frame[i * 4 + 1] = static_cast<std::uint8_t>(value >> 8);
        frame[i * 4 + 2] = static_cast<std::uint8_t>(value >> 16);
        frame[i * 4 + 3] = 255;
    }
}

bool AvailablePhysicalMemory(std::uint64_t* available) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return false;
    *available = status.ullAvailPhys;
    return true;
}

std::uint64_t PeakWorkingSet() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters)))
        return 0;
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
}

void PrintRecord(const Geometry& geometry, const char* pattern, const char* result,
                 const char* stage, int sdkError, const std::string& inputHash,
                 const std::string& sampleHash, const std::string& decodedHash,
                 std::size_t sampleBytes, int actualWidth, int actualHeight,
                 int actualFormat, int pitch, int sdkVersion, int encoderVersion,
                 double prepareMs, double encodeMs, double prepareDecodeMs,
                 double decodeMs) {
    std::cout << "{\"width\":" << geometry.width
              << ",\"height\":" << geometry.height
              << ",\"pattern\":\"" << pattern << "\",\"seed\":" << kSeed
              << ",\"result\":\"" << result << "\",\"stage\":\"" << stage
              << "\",\"sdk_error\":" << sdkError
              << ",\"input_sha256\":\"" << inputHash
              << "\",\"sample_sha256\":\"" << sampleHash
              << "\",\"decoded_sha256\":\"" << decodedHash
              << "\",\"sample_bytes\":" << sampleBytes
              << ",\"decoded_width\":" << actualWidth
              << ",\"decoded_height\":" << actualHeight
              << ",\"decoded_format\":" << actualFormat
              << ",\"decoded_pitch\":" << pitch
              << ",\"sdk_version\":" << sdkVersion
              << ",\"encoder_version\":" << encoderVersion
              << ",\"prepare_ms\":" << prepareMs
              << ",\"encode_ms\":" << encodeMs
              << ",\"prepare_decode_ms\":" << prepareDecodeMs
              << ",\"decode_ms\":" << decodeMs
              << ",\"peak_working_set_bytes\":" << PeakWorkingSet() << "}\n";
}

int RunOneCase(const Geometry& geometry, const char* pattern) {
    const std::uint64_t inputBytes64 = static_cast<std::uint64_t>(geometry.width) *
                                       geometry.height * 4ull;
    const std::uint64_t requiredHeadroom = inputBytes64 * 3ull + kReserveBytes;
    std::uint64_t available = 0;
    if (inputBytes64 > std::numeric_limits<std::size_t>::max() ||
        geometry.width > std::numeric_limits<int>::max() / 4 ||
        !AvailablePhysicalMemory(&available) || available < requiredHeadroom) {
        PrintRecord(geometry, pattern, "RESOURCE_EXCEEDED", "preflight", -1, "", "", "",
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        return 0;
    }

    CFHD_EncoderRef encoderRaw = nullptr;
    const CFHD_Error openEncoder = CFHD_OpenEncoder(&encoderRaw, nullptr);
    EncoderPtr encoder(encoderRaw);
    if (openEncoder != CFHD_ERROR_OKAY || !encoder) {
        PrintRecord(geometry, pattern, "SDK_ERROR", "open_encoder", openEncoder, "", "", "",
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        return 0;
    }

    const int pitch = geometry.width * 4;
    const auto prepareStart = Clock::now();
    const CFHD_Error prepare = CFHD_PrepareToEncode(
        encoder.get(), geometry.width, geometry.height, CFHD_PIXEL_FORMAT_BGRA,
        CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE,
        CFHD_ENCODING_QUALITY_FILMSCAN3);
    const double prepareMs = std::chrono::duration<double, std::milli>(Clock::now() - prepareStart).count();
    if (prepare != CFHD_ERROR_OKAY) {
        PrintRecord(geometry, pattern, ErrorClass(prepare), "prepare_encode", prepare,
                    "", "", "", 0, 0, 0, 0, 0, 0, 0, prepareMs, 0, 0, 0);
        return 0;
    }

    AlignedBytes input(static_cast<std::uint8_t*>(_aligned_malloc(
        static_cast<std::size_t>(inputBytes64), 32)));
    if (!input) {
        PrintRecord(geometry, pattern, "RESOURCE_EXCEEDED", "allocate_input", -1,
                    "", "", "", 0, 0, 0, 0, 0, 0, 0, prepareMs, 0, 0, 0);
        return 0;
    }
    FillInput(input.get(), geometry.width, geometry.height, pattern);
    std::string inputHash;
    if (!Sha256(input.get(), static_cast<std::size_t>(inputBytes64), &inputHash)) {
        PrintRecord(geometry, pattern, "HARNESS_ERROR", "hash_input", -1,
                    "", "", "", 0, 0, 0, 0, 0, 0, 0, prepareMs, 0, 0, 0);
        return 2;
    }

    const auto encodeStart = Clock::now();
    const CFHD_Error encode = CFHD_EncodeSample(encoder.get(), input.get(), pitch);
    const double encodeMs = std::chrono::duration<double, std::milli>(Clock::now() - encodeStart).count();
    if (encode != CFHD_ERROR_OKAY) {
        PrintRecord(geometry, pattern, ErrorClass(encode), "encode_sample", encode,
                    inputHash, "", "", 0, 0, 0, 0, 0, 0, 0, prepareMs, encodeMs, 0, 0);
        return 0;
    }

    void* sample = nullptr;
    std::size_t sampleBytes = 0;
    const CFHD_Error getSample = CFHD_GetSampleData(encoder.get(), &sample, &sampleBytes);
    if (getSample != CFHD_ERROR_OKAY || !sample || sampleBytes == 0) {
        PrintRecord(geometry, pattern, ErrorClass(getSample), "get_sample", getSample,
                    inputHash, "", "", sampleBytes, 0, 0, 0, 0, 0, 0,
                    prepareMs, encodeMs, 0, 0);
        return 0;
    }
    std::string sampleHash;
    if (!Sha256(static_cast<const std::uint8_t*>(sample), sampleBytes, &sampleHash)) {
        PrintRecord(geometry, pattern, "HARNESS_ERROR", "hash_sample", -1,
                    inputHash, "", "", sampleBytes, 0, 0, 0, 0, 0, 0,
                    prepareMs, encodeMs, 0, 0);
        return 2;
    }

    CFHD_DecoderRef decoderRaw = nullptr;
    const CFHD_Error openDecoder = CFHD_OpenDecoder(&decoderRaw, nullptr);
    DecoderPtr decoder(decoderRaw);
    if (openDecoder != CFHD_ERROR_OKAY || !decoder) {
        PrintRecord(geometry, pattern, "SDK_ERROR", "open_decoder", openDecoder,
                    inputHash, sampleHash, "", sampleBytes, 0, 0, 0, 0, 0, 0,
                    prepareMs, encodeMs, 0, 0);
        return 0;
    }

    int sdkVersion = 0;
    int encoderVersion = 0;
    (void)CFHD_GetSampleInfo(decoder.get(), sample, sampleBytes, CFHD_SAMPLE_SDK_VERSION,
                             &sdkVersion, sizeof(sdkVersion));
    (void)CFHD_GetSampleInfo(decoder.get(), sample, sampleBytes, CFHD_SAMPLE_ENCODE_VERSION,
                             &encoderVersion, sizeof(encoderVersion));

    int actualWidth = 0;
    int actualHeight = 0;
    CFHD_PixelFormat actualFormat = CFHD_PIXEL_FORMAT_UNKNOWN;
    const auto prepareDecodeStart = Clock::now();
    const CFHD_Error prepareDecode = CFHD_PrepareToDecode(
        decoder.get(), 0, 0, CFHD_PIXEL_FORMAT_BGRA, CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE, sample, sampleBytes, &actualWidth, &actualHeight,
        &actualFormat);
    const double prepareDecodeMs = std::chrono::duration<double, std::milli>(Clock::now() - prepareDecodeStart).count();
    if (prepareDecode != CFHD_ERROR_OKAY) {
        PrintRecord(geometry, pattern, "SDK_DECODE_FAILED", "prepare_decode", prepareDecode,
                    inputHash, sampleHash, "", sampleBytes, actualWidth, actualHeight,
                    actualFormat, 0, sdkVersion, encoderVersion, prepareMs, encodeMs,
                    prepareDecodeMs, 0);
        return 0;
    }

    std::int32_t decodePitch = 0;
    const CFHD_Error getPitch = CFHD_GetImagePitch(static_cast<std::uint32_t>(actualWidth),
                                                   actualFormat, &decodePitch);
    if (getPitch != CFHD_ERROR_OKAY || actualWidth <= 0 || actualHeight <= 0 ||
        decodePitch <= 0 || static_cast<std::uint64_t>(decodePitch) * actualHeight >
                                std::numeric_limits<std::size_t>::max()) {
        PrintRecord(geometry, pattern, "SDK_DECODE_FAILED", "decoded_geometry", getPitch,
                    inputHash, sampleHash, "", sampleBytes, actualWidth, actualHeight,
                    actualFormat, decodePitch, sdkVersion, encoderVersion, prepareMs,
                    encodeMs, prepareDecodeMs, 0);
        return 0;
    }
    const std::size_t outputBytes = static_cast<std::size_t>(decodePitch) * actualHeight;
    AlignedBytes output(static_cast<std::uint8_t*>(_aligned_malloc(outputBytes, 32)));
    if (!output) {
        PrintRecord(geometry, pattern, "RESOURCE_EXCEEDED", "allocate_output", -1,
                    inputHash, sampleHash, "", sampleBytes, actualWidth, actualHeight,
                    actualFormat, decodePitch, sdkVersion, encoderVersion, prepareMs,
                    encodeMs, prepareDecodeMs, 0);
        return 0;
    }
    std::memset(output.get(), 0xcd, outputBytes);
    const auto decodeStart = Clock::now();
    const CFHD_Error decode = CFHD_DecodeSample(decoder.get(), sample, sampleBytes,
                                                output.get(), decodePitch);
    const double decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - decodeStart).count();
    std::string decodedHash;
    if (decode == CFHD_ERROR_OKAY && actualFormat == CFHD_PIXEL_FORMAT_BGRA &&
        actualWidth == geometry.width && actualHeight == geometry.height) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD objectBytes = 0;
        DWORD returned = 0;
        NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                       nullptr, 0);
        if (status >= 0)
            status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                       reinterpret_cast<PUCHAR>(&objectBytes),
                                       sizeof(objectBytes), &returned, 0);
        std::vector<std::uint8_t> hashObject(objectBytes);
        if (status >= 0)
            status = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectBytes,
                                      nullptr, 0, 0);
        const std::size_t visibleRow = static_cast<std::size_t>(actualWidth) * 4;
        for (int row = 0; row < actualHeight && status >= 0; ++row) {
            status = BCryptHashData(hash, output.get() + static_cast<std::size_t>(row) *
                                   decodePitch, static_cast<ULONG>(visibleRow), 0);
        }
        std::array<std::uint8_t, 32> result{};
        if (status >= 0)
            status = BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (status >= 0) decodedHash = Hex(result.data(), result.size());
    }

    const bool exactDimensions = actualWidth == geometry.width && actualHeight == geometry.height;
    const bool decoded = decode == CFHD_ERROR_OKAY;
    const char* result = decoded && exactDimensions && actualFormat == CFHD_PIXEL_FORMAT_BGRA
                             ? "SDK_ENCODE_DECODE_PASS"
                             : (decoded ? "SDK_DIMENSION_OR_FORMAT_MISMATCH" : "SDK_DECODE_FAILED");
    PrintRecord(geometry, pattern, result, "complete", decode, inputHash, sampleHash,
                decodedHash, sampleBytes, actualWidth, actualHeight, actualFormat,
                decodePitch, sdkVersion, encoderVersion, prepareMs, encodeMs,
                prepareDecodeMs, decodeMs);
    return 0;
}

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(character);
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

bool RunChild(const Geometry& geometry, const char* pattern, bool* harnessFailure) {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH) return false;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = QuoteArgument(modulePath) + L" --case " +
                           std::to_wstring(geometry.width) + L" " +
                           std::to_wstring(geometry.height) + L" " +
                           std::wstring(pattern, pattern + std::strlen(pattern));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(modulePath, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        return false;
    }

    const DWORD waited = WaitForSingleObject(process.hProcess, kCaseTimeoutMs);
    DWORD exitCode = 0;
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
        exitCode = ERROR_TIMEOUT;
    } else if (waited == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(process.hProcess, &exitCode);
    } else {
        TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
        exitCode = ERROR_PROCESS_ABORTED;
    }

    std::string output;
    std::array<char, 4096> buffer{};
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &bytesRead, nullptr) && bytesRead > 0)
        output.append(buffer.data(), bytesRead);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (waited == WAIT_TIMEOUT) {
        std::cout << "{\"width\":" << geometry.width << ",\"height\":" << geometry.height
                  << ",\"pattern\":\"" << pattern
                  << "\",\"result\":\"TIMEOUT\",\"timeout_ms\":"
                  << kCaseTimeoutMs << "}\n";
        return true;
    }
    if (exitCode >= 0xc0000000u) {
        std::cout << "{\"width\":" << geometry.width << ",\"height\":" << geometry.height
                  << ",\"pattern\":\"" << pattern
                  << "\",\"result\":\"NATIVE_EXCEPTION\",\"exception_code\":"
                  << exitCode << "}\n";
        return true;
    }
    if (exitCode != 0) {
        std::cout << "{\"width\":" << geometry.width << ",\"height\":" << geometry.height
                  << ",\"pattern\":\"" << pattern
                  << "\",\"result\":\"HARNESS_CHILD_FAILURE\",\"exit_code\":"
                  << exitCode << "}\n";
        *harnessFailure = true;
        return true;
    }
    std::cout << output;
    return true;
}

bool ParseInt(const wchar_t* text, int* value) {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (!text[0] || !end || *end != L'\0' || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max())
        return false;
    *value = static_cast<int>(parsed);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--case") {
        int width = 0;
        int height = 0;
        if (!ParseInt(argv[2], &width) || !ParseInt(argv[3], &height)) return 2;
        const bool validGeometry = std::any_of(kGeometries.begin(), kGeometries.end(),
            [width, height](const Geometry& candidate) {
                return candidate.width == width && candidate.height == height;
            });
        std::string pattern;
        for (const wchar_t* character = argv[4]; *character; ++character) {
            if (*character > 0x7f) return 2;
            pattern.push_back(static_cast<char>(*character));
        }
        const bool validPattern = std::find(kPatterns.begin(), kPatterns.end(), pattern) !=
                                  kPatterns.end();
        if (!validGeometry || !validPattern) return 2;
        return RunOneCase({width, height}, pattern.c_str());
    }
    if (argc != 1) {
        std::cerr << "usage: test_issue48_sdk_geometry_smoke [--case width height flat|gradient|noise]\n";
        return 2;
    }

    std::cout << "{\"probe\":\"issue48_original_sdk_geometry\",\"seed\":" << kSeed
              << ",\"timeout_ms\":" << kCaseTimeoutMs
              << ",\"geometries\":4,\"patterns_per_geometry\":3}\n";
    bool harnessFailure = false;
    for (const Geometry& geometry : kGeometries) {
        for (const char* pattern : kPatterns) {
            std::cout << "{\"case_start\":true,\"width\":" << geometry.width
                      << ",\"height\":" << geometry.height
                      << ",\"pattern\":\"" << pattern << "\"}\n" << std::flush;
            if (!RunChild(geometry, pattern, &harnessFailure)) {
                std::cerr << "unable to launch isolated child case\n";
                return 2;
            }
        }
    }
    return harnessFailure ? 2 : 0;
}
