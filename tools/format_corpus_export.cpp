// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Issue #36 stage F1: real GoPro SDK golden corpora for the compressed
// families the F0 capability matrix actually qualified (RGB_444, RGBA_4444,
// BAYER).
//
// For every case this tool writes, from a deterministic per-family source:
//   source.bin            exact source bytes handed to the SDK
//   sdk.cfhd              the unmodified packet returned by CFHD_GetSampleData
//   sdk.normalized.cfhd   the same packet with GUID/DATE/TIME/TIMC value bytes zeroed
//   decoded.bin           a decode from a fresh decoder instance in the family's
//                         highest-precision native output format
// plus a SHA-256 manifest and a TRANSFER_SHA256.tsv. Raw corpora stay outside Git.
//
// A successful export is a corpus fact, not evidence that HyperCine's own
// encoder or decoder supports the mode.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _ALLOCATOR
#define _ALLOCATOR 1
#endif

#include "CFHDDecoder.h"
#include "CFHDEncoder.h"
#include "CFHDError.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"
#include "CFHDTypes.h"
#include "format_corpus_common.h"
#include "../tests/test_vectors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

namespace hf = hypercine_format;
using ByteBuffer = HyperCineTest::AlignedVector<uint8_t>;
using EncoderOwner = std::unique_ptr<std::remove_pointer_t<CFHD_EncoderRef>, decltype(&CFHD_CloseEncoder)>;
using DecoderOwner = std::unique_ptr<std::remove_pointer_t<CFHD_DecoderRef>, decltype(&CFHD_CloseDecoder)>;
using MetadataOwner = std::unique_ptr<std::remove_pointer_t<CFHD_MetadataRef>, decltype(&CFHD_CloseMetadata)>;

constexpr char kFrozenUpstreamCommit[] = "11574d0295771edccadd17af14af74a539f924c7";
constexpr uint32_t kSourceSeed = 0x1337BEEFu;
constexpr int kMaxOfferedFormats = 64;
constexpr size_t kMaxOutputBytes = 128u * 1024u * 1024u;

enum class FamilyKind { Rgb444, Rgba4444, Bayer };

struct QualityEntry {
    CFHD_EncodingQuality value;
    const char* name;
};

constexpr std::array<QualityEntry, 6> kQualities{{
    {CFHD_ENCODING_QUALITY_LOW, "low"},
    {CFHD_ENCODING_QUALITY_MEDIUM, "medium"},
    {CFHD_ENCODING_QUALITY_HIGH, "high"},
    {CFHD_ENCODING_QUALITY_FILMSCAN1, "filmscan1"},
    {CFHD_ENCODING_QUALITY_FILMSCAN2, "filmscan2"},
    {CFHD_ENCODING_QUALITY_FILMSCAN3, "filmscan3"},
}};

const char* const kBayerFormatNames[4] = {"RGGB_RED_GRN", "GRBG_GRN_RED", "GBRG_GRN_BLU", "BGGR_BLU_GRN"};

struct SourceSpec {
    CFHD_PixelFormat format = CFHD_PIXEL_FORMAT_UNKNOWN;
    const char* label = "UNKNOWN";
    int pixel_size = 0;
    int channels = 0;
    int bits = 0;
    const char* layout = "";
};

std::vector<SourceSpec> SourcesFor(FamilyKind kind) {
    switch (kind) {
        case FamilyKind::Rgb444:
            return {
                {CFHD_PIXEL_FORMAT_RG48, "RG48", 6, 3, 16,
                 "packed RGB, native-endian uint16 per component, top-down"},
                {CFHD_PIXEL_FORMAT_RG24, "RG24", 3, 3, 8,
                 "packed B,G,R 8-bit (SDK inverted RGB24), bottom-up"},
            };
        case FamilyKind::Rgba4444:
            return {
                {CFHD_PIXEL_FORMAT_RG64, "RG64", 8, 4, 16,
                 "packed RGBA, native-endian uint16 per component, top-down"},
                {CFHD_PIXEL_FORMAT_BGRA, "BGRA", 4, 4, 8,
                 "packed B,G,R,A 8-bit, bottom-up as interpreted by the SDK"},
            };
        case FamilyKind::Bayer:
            return {
                {CFHD_PIXEL_FORMAT_BYR4, "BYR4", 2, 1, 16,
                 "raw CFA, native-endian uint16 per photosite, top-down"},
            };
    }
    return {};
}

struct FamilyDef {
    FamilyKind kind;
    CFHD_EncodedFormat encoded;
    const char* name;
};

constexpr std::array<FamilyDef, 3> kFamilies{{
    {FamilyKind::Rgb444, CFHD_ENCODED_FORMAT_RGB_444, "RGB_444"},
    {FamilyKind::Rgba4444, CFHD_ENCODED_FORMAT_RGBA_4444, "RGBA_4444"},
    {FamilyKind::Bayer, CFHD_ENCODED_FORMAT_BAYER, "BAYER"},
}};

std::vector<const char*> PatternsFor(FamilyKind kind) {
    switch (kind) {
        case FamilyKind::Rgb444:
            return {"asymmetric", "flat", "gradient", "primaries", "checker", "noise", "edge", "minmax"};
        case FamilyKind::Rgba4444:
            return {"rgb_independent", "alpha_zero", "alpha_half", "alpha_full",
                    "alpha_ramp", "alpha_edge", "alpha_noise"};
        case FamilyKind::Bayer:
            return {"flat", "black", "white", "dark", "ramp", "detail", "noise"};
    }
    return {};
}

std::string FourCharCode(uint32_t value) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((value >> shift) & 0xFFu);
        out.push_back((c >= 0x20 && c < 0x7F) ? c : '.');
    }
    return out;
}

std::string FormatLabel(uint32_t value) {
    if (value == 0) return "UNKNOWN";
    const std::string code = FourCharCode(value);
    if (code.find('.') != std::string::npos) {
        std::ostringstream hex;
        hex << "0x" << std::hex << value;
        return hex.str();
    }
    return code;
}

std::string ErrorName(CFHD_Error error) {
    switch (error) {
        case CFHD_ERROR_OKAY: return "CFHD_ERROR_OKAY";
        case CFHD_ERROR_INVALID_ARGUMENT: return "CFHD_ERROR_INVALID_ARGUMENT";
        case CFHD_ERROR_OUTOFMEMORY: return "CFHD_ERROR_OUTOFMEMORY";
        case CFHD_ERROR_BADFORMAT: return "CFHD_ERROR_BADFORMAT";
        case CFHD_ERROR_BADSCALING: return "CFHD_ERROR_BADSCALING";
        case CFHD_ERROR_BADSAMPLE: return "CFHD_ERROR_BADSAMPLE";
        case CFHD_ERROR_INTERNAL: return "CFHD_ERROR_INTERNAL";
        case CFHD_ERROR_METADATA_CLASS: return "CFHD_ERROR_METADATA_CLASS";
        case CFHD_ERROR_METADATA_UNDEFINED: return "CFHD_ERROR_METADATA_UNDEFINED";
        case CFHD_ERROR_METADATA_END: return "CFHD_ERROR_METADATA_END";
        case CFHD_ERROR_UNEXPECTED: return "CFHD_ERROR_UNEXPECTED";
        case CFHD_ERROR_BAD_RESOLUTION: return "CFHD_ERROR_BAD_RESOLUTION";
        case CFHD_ERROR_BAD_PIXEL_SIZE: return "CFHD_ERROR_BAD_PIXEL_SIZE";
        case CFHD_ERROR_NOT_FINISHED: return "CFHD_ERROR_NOT_FINISHED";
        case CFHD_ERROR_ENCODING_NOT_STARTED: return "CFHD_ERROR_ENCODING_NOT_STARTED";
        case CFHD_ERROR_METADATA_ATTACHED: return "CFHD_ERROR_METADATA_ATTACHED";
        case CFHD_ERROR_BAD_METADATA: return "CFHD_ERROR_BAD_METADATA";
        case CFHD_ERROR_THREAD_CREATE_FAILED: return "CFHD_ERROR_THREAD_CREATE_FAILED";
        case CFHD_ERROR_THREAD_WAIT_FAILED: return "CFHD_ERROR_THREAD_WAIT_FAILED";
        case CFHD_ERROR_UNKNOWN_TAG: return "CFHD_ERROR_UNKNOWN_TAG";
        case CFHD_ERROR_LICENSING: return "CFHD_ERROR_LICENSING";
        case CFHD_ERROR_CODEC_ERROR: return "CFHD_ERROR_CODEC_ERROR";
        case CFHD_ERROR_DECODE_BUFFER_SIZE: return "CFHD_ERROR_DECODE_BUFFER_SIZE";
        default: break;
    }
    std::ostringstream out;
    out << "CFHD_ERROR_" << static_cast<int>(error);
    return out.str();
}

std::string CompilerLabel() {
#if defined(_MSC_VER)
    std::ostringstream out;
    out << "MSVC " << _MSC_FULL_VER;
    return out.str();
#elif defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

std::string HostOs() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

int ComponentCount(CFHD_PixelFormat format) {
    switch (format) {
        case CFHD_PIXEL_FORMAT_RG48:
        case CFHD_PIXEL_FORMAT_RG24:
        case CFHD_PIXEL_FORMAT_WP13:
            return 3;
        case CFHD_PIXEL_FORMAT_RG64:
        case CFHD_PIXEL_FORMAT_B64A:
        case CFHD_PIXEL_FORMAT_W13A:
        case CFHD_PIXEL_FORMAT_BGRA:
            return 4;
        default:
            return 1;
    }
}

int ComponentBits(CFHD_PixelFormat format) {
    switch (format) {
        case CFHD_PIXEL_FORMAT_RG48:
        case CFHD_PIXEL_FORMAT_RG64:
        case CFHD_PIXEL_FORMAT_B64A:
        case CFHD_PIXEL_FORMAT_WP13:
        case CFHD_PIXEL_FORMAT_W13A:
        case CFHD_PIXEL_FORMAT_BYR4:
            return 16;
        default:
            return 8;
    }
}

std::string LayoutLabel(CFHD_PixelFormat format) {
    switch (format) {
        case CFHD_PIXEL_FORMAT_RG48:
            return "packed RGB, native-endian uint16 per component, top-down";
        case CFHD_PIXEL_FORMAT_RG24:
            return "packed B,G,R 8-bit (SDK inverted RGB24), bottom-up";
        case CFHD_PIXEL_FORMAT_RG64:
            return "packed RGBA, native-endian uint16 per component, top-down";
        case CFHD_PIXEL_FORMAT_B64A:
            return "packed A,R,G,B, native-endian uint16 per component, top-down";
        case CFHD_PIXEL_FORMAT_BGRA:
            return "packed B,G,R,A 8-bit, bottom-up as interpreted by the SDK";
        case CFHD_PIXEL_FORMAT_BYR4:
            return "raw CFA, native-endian uint16 per photosite, top-down";
        case CFHD_PIXEL_FORMAT_BYR2:
            return "raw CFA, 8-bit per photosite, top-down";
        default:
            return "SDK-reported layout, not re-interpreted by this tool";
    }
}

std::vector<CFHD_PixelFormat> PreferredOutputs(FamilyKind kind) {
    switch (kind) {
        case FamilyKind::Rgb444:
            return {CFHD_PIXEL_FORMAT_RG48, CFHD_PIXEL_FORMAT_RG24, CFHD_PIXEL_FORMAT_BGRA};
        case FamilyKind::Rgba4444:
            return {CFHD_PIXEL_FORMAT_RG64, CFHD_PIXEL_FORMAT_B64A, CFHD_PIXEL_FORMAT_BGRA};
        case FamilyKind::Bayer:
            return {CFHD_PIXEL_FORMAT_BYR4, CFHD_PIXEL_FORMAT_BYR2, CFHD_PIXEL_FORMAT_BGRA};
    }
    return {};
}

uint64_t SplitMix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

int Noise16(int x, int y, int channel, uint32_t seed) {
    const uint64_t mixed = SplitMix64((static_cast<uint64_t>(static_cast<uint32_t>(x)) << 40) ^
                                     (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 16) ^
                                     (static_cast<uint64_t>(static_cast<uint32_t>(channel)) << 8) ^ seed);
    return static_cast<int>((mixed >> 24) & 0xFFFFu);
}

struct Pixel16 {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 65535;
};

int ScaleX(int x, int width) {
    if (width <= 1) return 0;
    return static_cast<int>((static_cast<int64_t>(x) * 65535) / (width - 1));
}

int ScaleY(int y, int height) {
    if (height <= 1) return 0;
    return static_cast<int>((static_cast<int64_t>(y) * 65535) / (height - 1));
}

Pixel16 RgbScene(FamilyKind kind, const std::string& pattern, int x, int y, int width, int height,
                 uint32_t seed) {
    Pixel16 p;
    const int half_w = width / 2;
    const int half_h = height / 2;
    if (pattern == "flat") {
        p.r = p.g = p.b = 0x4000;
    } else if (pattern == "black") {
        p.r = p.g = p.b = 0;
    } else if (pattern == "white") {
        p.r = p.g = p.b = 0xFFFF;
    } else if (pattern == "dark") {
        p.r = p.g = p.b = 0x0400;
    } else if (pattern == "asymmetric" || pattern == "rgb_independent") {
        p.r = ScaleX(x, width);
        p.g = ScaleY(y, height);
        p.b = static_cast<int>((static_cast<int64_t>(x) * 3 + static_cast<int64_t>(y) * 7) * 65535 /
                               std::max<int64_t>(1, static_cast<int64_t>(width) * 3 + static_cast<int64_t>(height) * 7));
    } else if (pattern == "gradient") {
        p.r = ScaleX(x, width);
        p.g = ScaleY(y, height);
        p.b = (ScaleX(x, width) + ScaleY(y, height)) / 2;
    } else if (pattern == "primaries") {
        const bool right = x >= half_w;
        const bool bottom = y >= half_h;
        if (!right && !bottom) { p.r = 0xFFFF; p.g = 0; p.b = 0; }
        else if (right && !bottom) { p.r = 0; p.g = 0xFFFF; p.b = 0; }
        else if (!right && bottom) { p.r = 0; p.g = 0; p.b = 0xFFFF; }
        else { p.r = p.g = p.b = 0xFFFF; }
    } else if (pattern == "checker") {
        p.r = (((x / 8) + (y / 8)) & 1) ? 0xFFFF : 0;
        p.g = (((x / 16) + (y / 16)) & 1) ? 0xFFFF : 0;
        p.b = (((x / 4) + (y / 4)) & 1) ? 0xFFFF : 0;
    } else if (pattern == "noise" || pattern == "detail") {
        p.r = Noise16(x, y, 0, seed);
        p.g = Noise16(x, y, 1, seed);
        p.b = Noise16(x, y, 2, seed);
    } else if (pattern == "edge") {
        p.r = x < half_w ? 0 : 0xFFFF;
        p.g = x < half_w ? 0xFFFF : 0;
        p.b = x < half_w ? 0x4000 : 0xC000;
    } else if (pattern == "minmax") {
        const int block = (x / 32 + y / 32) & 1;
        p.r = block ? 0xFFFF : 0;
        p.g = block ? 0 : 0xFFFF;
        p.b = ((x + y) & 16) ? 0xFFFF : 0;
    } else if (pattern == "ramp") {
        p.r = p.g = p.b = ScaleX(x, width);
    } else {
        p.r = ScaleX(x, width);
        p.g = ScaleY(y, height);
        p.b = static_cast<int>((static_cast<int64_t>(x) + y) * 65535 /
                               std::max<int64_t>(1, static_cast<int64_t>(width) + height));
    }

    if (kind == FamilyKind::Rgba4444) {
        if (pattern == "alpha_zero") p.a = 0;
        else if (pattern == "alpha_half") p.a = 32768;
        else if (pattern == "alpha_full") p.a = 65535;
        else if (pattern == "alpha_ramp") p.a = ScaleX(x, width);
        else if (pattern == "alpha_edge") p.a = x < half_w ? 0 : 65535;
        else if (pattern == "alpha_noise") p.a = Noise16(x, y, 3, seed);
        else p.a = (ScaleX(x, width) + ScaleY(y, height)) / 2;
    }
    return p;
}

int CfaChannel(int bayer_format, int x, int y) {
    if (bayer_format < 0) bayer_format = 0;  // SDK default, BAYER_FORMAT_RED_GRN
    const bool odd_x = (x & 1) != 0;
    const bool odd_y = (y & 1) != 0;
    switch (bayer_format & 3) {
        case 0: return odd_y ? (odd_x ? 2 : 1) : (odd_x ? 1 : 0);  // RGGB
        case 1: return odd_y ? (odd_x ? 1 : 2) : (odd_x ? 0 : 1);  // GRBG
        case 2: return odd_y ? (odd_x ? 0 : 1) : (odd_x ? 2 : 1);  // GBRG
        default: return odd_y ? (odd_x ? 1 : 0) : (odd_x ? 2 : 1); // BGGR
    }
}

void FillSource(uint8_t* data, int pitch, int width, int height, const SourceSpec& spec,
                FamilyKind kind, const std::string& pattern, int bayer_format, uint32_t seed) {
    std::memset(data, 0, static_cast<size_t>(pitch) * static_cast<size_t>(height));
    std::vector<Pixel16> pixels(static_cast<size_t>(width));
    // The SDK's 8-bit RGB24 and BGRA paths read the buffer bottom-up. Store the
    // scene in that orientation so the encoded image itself stays upright.
    const bool bottom_up = spec.format == CFHD_PIXEL_FORMAT_RG24 ||
                           spec.format == CFHD_PIXEL_FORMAT_BGRA;
    for (int y = 0; y < height; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * static_cast<size_t>(pitch);
        const int scene_y = bottom_up ? height - 1 - y : y;
        for (int x = 0; x < width; ++x) {
            pixels[static_cast<size_t>(x)] = RgbScene(kind, pattern, x, scene_y, width, height, seed);
        }
        if (spec.format == CFHD_PIXEL_FORMAT_RG48) {
            uint16_t* out = reinterpret_cast<uint16_t*>(row);
            for (int x = 0; x < width; ++x) {
                out[x * 3 + 0] = static_cast<uint16_t>(pixels[x].r);
                out[x * 3 + 1] = static_cast<uint16_t>(pixels[x].g);
                out[x * 3 + 2] = static_cast<uint16_t>(pixels[x].b);
            }
        } else if (spec.format == CFHD_PIXEL_FORMAT_RG24) {
            // The SDK reads the 8-bit RGB24 layout as inverted B,G,R and bottom-up.
            for (int x = 0; x < width; ++x) {
                row[x * 3 + 0] = static_cast<uint8_t>(pixels[x].b >> 8);
                row[x * 3 + 1] = static_cast<uint8_t>(pixels[x].g >> 8);
                row[x * 3 + 2] = static_cast<uint8_t>(pixels[x].r >> 8);
            }
        } else if (spec.format == CFHD_PIXEL_FORMAT_RG64) {
            uint16_t* out = reinterpret_cast<uint16_t*>(row);
            for (int x = 0; x < width; ++x) {
                out[x * 4 + 0] = static_cast<uint16_t>(pixels[x].r);
                out[x * 4 + 1] = static_cast<uint16_t>(pixels[x].g);
                out[x * 4 + 2] = static_cast<uint16_t>(pixels[x].b);
                out[x * 4 + 3] = static_cast<uint16_t>(pixels[x].a);
            }
        } else if (spec.format == CFHD_PIXEL_FORMAT_BGRA) {
            for (int x = 0; x < width; ++x) {
                row[x * 4 + 0] = static_cast<uint8_t>(pixels[x].b >> 8);
                row[x * 4 + 1] = static_cast<uint8_t>(pixels[x].g >> 8);
                row[x * 4 + 2] = static_cast<uint8_t>(pixels[x].r >> 8);
                row[x * 4 + 3] = static_cast<uint8_t>(pixels[x].a >> 8);
            }
        } else if (spec.format == CFHD_PIXEL_FORMAT_BYR4) {
            uint16_t* out = reinterpret_cast<uint16_t*>(row);
            for (int x = 0; x < width; ++x) {
                const Pixel16& pixel = pixels[x];
                const int channel = CfaChannel(bayer_format, x, y);
                const int value = channel == 0 ? pixel.r : (channel == 1 ? pixel.g : pixel.b);
                out[x] = static_cast<uint16_t>(value);
            }
        }
    }
}

bool ReadPixel(const uint8_t* data, int pitch, int x, int y, CFHD_PixelFormat format, Pixel16* out) {
    const uint8_t* row = data + static_cast<size_t>(y) * static_cast<size_t>(pitch);
    switch (format) {
        case CFHD_PIXEL_FORMAT_RG48: {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(row) + x * 3;
            out->r = in[0]; out->g = in[1]; out->b = in[2]; out->a = 65535; return true;
        }
        case CFHD_PIXEL_FORMAT_RG24: {
            const uint8_t* in = row + x * 3;
            out->b = in[0] * 257; out->g = in[1] * 257; out->r = in[2] * 257; out->a = 65535; return true;
        }
        case CFHD_PIXEL_FORMAT_RG64: {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(row) + x * 4;
            out->r = in[0]; out->g = in[1]; out->b = in[2]; out->a = in[3]; return true;
        }
        case CFHD_PIXEL_FORMAT_B64A: {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(row) + x * 4;
            out->a = in[0]; out->r = in[1]; out->g = in[2]; out->b = in[3]; return true;
        }
        case CFHD_PIXEL_FORMAT_BGRA: {
            const uint8_t* in = row + x * 4;
            out->b = in[0] * 257; out->g = in[1] * 257; out->r = in[2] * 257; out->a = in[3] * 257; return true;
        }
        case CFHD_PIXEL_FORMAT_BYR4: {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(row) + x;
            out->r = out->g = out->b = in[0]; out->a = 65535; return true;
        }
        case CFHD_PIXEL_FORMAT_BYR2: {
            const uint8_t* in = row + x * 2;
            out->r = out->g = out->b = in[0] * 257; out->a = 65535; return true;
        }
        default:
            return false;
    }
}

std::string ComposeJsonArray(const std::vector<double>& values, int precision = 3) {
    std::ostringstream out;
    out << '[' << std::fixed << std::setprecision(precision);
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
    return out.str();
}

struct CompareStats {
    bool comparable = false;
    std::string note;
    double psnr_r = -1.0;
    double psnr_g = -1.0;
    double psnr_b = -1.0;
    double psnr_a = -1.0;
    int max_abs_diff = -1;
    double exact_ratio = 0.0;
    double alpha_exact_ratio = -1.0;
    int alpha_max_abs_diff = -1;
};

bool SameLayout(CFHD_PixelFormat a, CFHD_PixelFormat b) {
    if (a == b) return true;
    const auto is_rgb = [](CFHD_PixelFormat f) {
        return f == CFHD_PIXEL_FORMAT_RG48 || f == CFHD_PIXEL_FORMAT_RG24;
    };
    const auto is_rgba = [](CFHD_PixelFormat f) {
        return f == CFHD_PIXEL_FORMAT_RG64 || f == CFHD_PIXEL_FORMAT_B64A ||
               f == CFHD_PIXEL_FORMAT_BGRA;
    };
    // ReadPixel normalizes every supported layout to 16-bit component values,
    // so an 8-bit source is comparable with a 16-bit decode of the same family.
    // RG64 samples decode as B64A (ARGB); ReadPixel maps A,R,G,B back.
    return (is_rgb(a) && is_rgb(b)) || (is_rgba(a) && is_rgba(b));
}

CompareStats CompareFrame(const uint8_t* source, int source_pitch, const uint8_t* decoded, int decoded_pitch,
                          int width, int height, CFHD_PixelFormat source_format,
                          CFHD_PixelFormat decoded_format) {
    CompareStats stats;
    if (!SameLayout(source_format, decoded_format)) {
        stats.note = "decoded output format differs from the source layout; no pixel comparison is asserted";
        return stats;
    }
    if (source_format != CFHD_PIXEL_FORMAT_RG48 && source_format != CFHD_PIXEL_FORMAT_RG24 &&
        source_format != CFHD_PIXEL_FORMAT_RG64 && source_format != CFHD_PIXEL_FORMAT_BGRA &&
        source_format != CFHD_PIXEL_FORMAT_B64A && source_format != CFHD_PIXEL_FORMAT_BYR4) {
        stats.note = "pixel comparison not implemented for this format";
        return stats;
    }
    stats.comparable = true;
    // ReadPixel normalizes every layout to 16-bit component values, so PSNR is
    // computed on that scale for 8-bit and 16-bit layouts alike.
    const int max_value = 65535;
    double mse[4] = {0.0, 0.0, 0.0, 0.0};
    int max_diff[4] = {0, 0, 0, 0};
    uint64_t exact = 0;
    uint64_t alpha_exact = 0;
    const uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const bool has_alpha = ComponentCount(source_format) == 4;
    const int channels = ComponentCount(source_format);
    // The SDK reads its 8-bit RGB24 and BGRA sources bottom-up while decoding
    // top-down, so the same-layout comparison flips those source rows only.
    const bool flip_source = source_format == CFHD_PIXEL_FORMAT_BGRA ||
                             source_format == CFHD_PIXEL_FORMAT_RG24;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Pixel16 a;
            Pixel16 b;
            const int source_y = flip_source ? height - 1 - y : y;
            if (!ReadPixel(source, source_pitch, x, source_y, source_format, &a) ||
                !ReadPixel(decoded, decoded_pitch, x, y, decoded_format, &b)) {
                stats.comparable = false;
                stats.note = "pixel read failed";
                return stats;
            }
            const int values_a[4] = {a.r, a.g, a.b, a.a};
            const int values_b[4] = {b.r, b.g, b.b, b.a};
            bool all_equal = true;
            for (int c = 0; c < channels; ++c) {
                const int diff = std::abs(values_a[c] - values_b[c]);
                mse[c] += static_cast<double>(diff) * diff;
                if (diff > max_diff[c]) max_diff[c] = diff;
                if (diff != 0) all_equal = false;
            }
            if (all_equal) ++exact;
            if (has_alpha && values_a[3] == values_b[3]) ++alpha_exact;
        }
    }
    const double count = static_cast<double>(total);
    auto psnr = [&](int channel) {
        const double channel_mse = mse[channel] / count;
        if (channel_mse <= 0.0) return 1000.0;
        return 10.0 * std::log10((static_cast<double>(max_value) * max_value) / channel_mse);
    };
    stats.psnr_r = psnr(0);
    stats.psnr_g = psnr(1);
    stats.psnr_b = psnr(2);
    if (has_alpha) {
        stats.psnr_a = psnr(3);
        stats.alpha_exact_ratio = count > 0 ? static_cast<double>(alpha_exact) / count : 0.0;
        stats.alpha_max_abs_diff = max_diff[3];
    }
    stats.max_abs_diff = std::max({max_diff[0], max_diff[1], max_diff[2]});
    stats.exact_ratio = count > 0 ? static_cast<double>(exact) / count : 0.0;
    stats.note = "same-layout comparison against the exact source bytes";
    return stats;
}

struct EncodeResult {
    const char* stage = "open";
    bool opened = false;
    bool threw = false;
    int thrown_error = 0;
    CFHD_Error prepare_error = CFHD_ERROR_INTERNAL;
    CFHD_Error encode_error = CFHD_ERROR_INTERNAL;
    CFHD_Error sample_error = CFHD_ERROR_INTERNAL;
    size_t sdk_sample_size = 0;
    std::vector<uint8_t> sample;
};

EncodeResult EncodeCase(int width, int height, const SourceSpec& source, int32_t source_pitch,
                        CFHD_EncodedFormat encoded, CFHD_EncodingQuality quality, const uint8_t* pixels) {
    EncodeResult result;
    CFHD_EncoderRef encoder = nullptr;
    result.stage = "open";
    const CFHD_Error open_error = CFHD_OpenEncoder(&encoder, nullptr);
    EncoderOwner owner(encoder, &CFHD_CloseEncoder);
    if (open_error != CFHD_ERROR_OKAY) return result;
    result.opened = true;

    result.stage = "prepare";
    result.prepare_error = CFHD_PrepareToEncode(encoder, width, height, source.format, encoded,
                                                CFHD_ENCODING_FLAGS_NONE, quality);
    if (result.prepare_error != CFHD_ERROR_OKAY) return result;

    // The public encoder metadata path (CFHD_MetadataOpen/Add/Attach) is not
    // usable in this static SDK build: attempting to attach TAG_BAYER_FORMAT
    // crashes before EncodeSample. The Bayer corpus therefore uses the SDK
    // default CFA and records that fact; see docs/REFERENCE_CINEFORM_FORMAT_F1.md.
    result.stage = "encode";
    result.encode_error = CFHD_EncodeSample(encoder, const_cast<uint8_t*>(pixels), source_pitch);
    if (result.encode_error != CFHD_ERROR_OKAY) return result;

    result.stage = "get_sample";
    void* data = nullptr;
    size_t size = 0;
    result.sample_error = CFHD_GetSampleData(encoder, &data, &size);
    result.sdk_sample_size = size;
    if (result.sample_error == CFHD_ERROR_OKAY && data && size > 0) {
        result.sample.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }
    result.stage = "close";
    return result;
}

struct DecodeResult {
    const char* stage = "open";
    bool opened = false;
    bool threw = false;
    int thrown_error = 0;
    CFHD_Error prepare_error = CFHD_ERROR_INTERNAL;
    CFHD_Error decode_error = CFHD_ERROR_INTERNAL;
    CFHD_PixelFormat requested_format = CFHD_PIXEL_FORMAT_UNKNOWN;
    CFHD_PixelFormat actual_format = CFHD_PIXEL_FORMAT_UNKNOWN;
    int width = 0;
    int height = 0;
    int32_t pitch = 0;
    int32_t encoded_format = -1;
    int32_t channels = -1;
    int32_t display_width = -1;
    int32_t display_height = -1;
    int32_t progressive = -1;
    int32_t key_frame = -1;
    int32_t sdk_version = -1;
    int32_t encode_version = -1;
    int offered_count = 0;
    CFHD_PixelFormat offered[kMaxOfferedFormats] = {};
    int32_t bayer_format = -1;
    CFHD_Error bayer_format_error = CFHD_ERROR_INTERNAL;
    int32_t encode_curve = -1;
    int32_t decode_curve = -1;
    int32_t encode_preset = -1;
    ByteBuffer decoded;
};

void QueryInfo(CFHD_DecoderRef decoder, const uint8_t* sample, size_t size, CFHD_SampleInfoTag tag,
               int32_t* value) {
    int32_t read = -1;
    if (CFHD_GetSampleInfo(decoder, const_cast<uint8_t*>(sample), size, tag, &read, sizeof(read)) !=
        CFHD_ERROR_OKAY) {
        read = -1;
    }
    *value = read;
}

DecodeResult DecodeCase(const std::vector<uint8_t>& sample, FamilyKind kind) {
    DecodeResult result;
    CFHD_DecoderRef decoder = nullptr;
    CFHD_MetadataRef metadata = nullptr;
    MetadataOwner metadata_owner(nullptr, &CFHD_CloseMetadata);
    result.stage = "open";
    const CFHD_Error open_error = CFHD_OpenDecoder(&decoder, nullptr);
    DecoderOwner owner(decoder, &CFHD_CloseDecoder);
    if (open_error != CFHD_ERROR_OKAY) return result;
    result.opened = true;

    result.stage = "sample_info";
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_ENCODED_FORMAT, &result.encoded_format);
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_INFO_CHANNELS, &result.channels);
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_DISPLAY_WIDTH, &result.display_width);
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_DISPLAY_HEIGHT, &result.display_height);
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_PROGRESSIVE, &result.progressive);
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_KEY_FRAME, &result.key_frame);
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_SDK_VERSION, &result.sdk_version);
    QueryInfo(decoder, sample.data(), sample.size(), CFHD_SAMPLE_ENCODE_VERSION, &result.encode_version);

    result.stage = "output_formats";
    if (CFHD_GetOutputFormats(decoder, const_cast<uint8_t*>(sample.data()), sample.size(), result.offered,
                              kMaxOfferedFormats, &result.offered_count) != CFHD_ERROR_OKAY) {
        result.offered_count = 0;
    }
    const std::vector<CFHD_PixelFormat> preferred = PreferredOutputs(kind);
    CFHD_PixelFormat requested = CFHD_PIXEL_FORMAT_BGRA;
    bool chosen = false;
    for (CFHD_PixelFormat candidate : preferred) {
        for (int i = 0; i < result.offered_count; ++i) {
            if (result.offered[i] == candidate) {
                requested = candidate;
                chosen = true;
                break;
            }
        }
        if (chosen) break;
    }
    if (!chosen && result.offered_count > 0) requested = result.offered[0];
    result.requested_format = requested;

    result.stage = "prepare";
    int actual_width = 0;
    int actual_height = 0;
    CFHD_PixelFormat actual_format = CFHD_PIXEL_FORMAT_UNKNOWN;
    result.prepare_error = CFHD_PrepareToDecode(decoder, 0, 0, requested, CFHD_DECODED_RESOLUTION_FULL,
                                                CFHD_DECODING_FLAGS_NONE,
                                                const_cast<uint8_t*>(sample.data()), sample.size(),
                                                &actual_width, &actual_height, &actual_format);
    if (result.prepare_error != CFHD_ERROR_OKAY) return result;
    result.actual_format = actual_format;
    result.width = actual_width;
    result.height = actual_height;

    result.stage = "image_pitch";
    int32_t pitch = 0;
    if (CFHD_GetImagePitch(static_cast<uint32_t>(actual_width), actual_format, &pitch) == CFHD_ERROR_OKAY &&
        pitch > 0) {
        result.pitch = pitch;
    }
    const size_t needed = static_cast<size_t>(pitch) * static_cast<size_t>(actual_height);
    if (result.pitch <= 0 || needed == 0 || needed > kMaxOutputBytes) {
        result.decode_error = CFHD_ERROR_DECODE_BUFFER_SIZE;
        return result;
    }

    // The pristine decoder races its worker threads on machines with more than
    // eight cores; TAG_CPU_MAX pins the thread count (docs/UPSTREAM.md quirk 9).
    result.stage = "metadata";
    if (CFHD_OpenMetadata(&metadata) == CFHD_ERROR_OKAY) {
        metadata_owner.reset(metadata);
        CFHD_InitSampleMetadata(metadata, METADATATYPE_ORIGINAL, const_cast<uint8_t*>(sample.data()),
                                static_cast<uint32_t>(sample.size()));
        uint32_t process_flags = PROCESSING_ALL_ON;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_PROCESS_PATH, METADATATYPE_UINT32, &process_flags,
                               sizeof(process_flags));
        uint32_t cpu_max = 1;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_CPU_MAX, METADATATYPE_HIDDEN, &cpu_max,
                               sizeof(cpu_max));
        CFHD_MetadataType bayer_type = METADATATYPE_UNKNOWN;
        void* bayer_data = nullptr;
        CFHD_MetadataSize bayer_size = 0;
        result.bayer_format_error = CFHD_FindMetadata(metadata, TAG_BAYER_FORMAT, &bayer_type, &bayer_data,
                                                     &bayer_size);
        if (result.bayer_format_error == CFHD_ERROR_OKAY && bayer_data && bayer_size > 0) {
            result.bayer_format = static_cast<int32_t>(*reinterpret_cast<uint8_t*>(bayer_data));
        }
        const auto read_int_tag = [&](CFHD_MetadataTag tag, int32_t* value) {
            CFHD_MetadataType type = METADATATYPE_UNKNOWN;
            void* data = nullptr;
            CFHD_MetadataSize size = 0;
            if (CFHD_FindMetadata(metadata, tag, &type, &data, &size) == CFHD_ERROR_OKAY && data &&
                size >= 4) {
                *value = *reinterpret_cast<int32_t*>(data);
            }
        };
        read_int_tag(TAG_ENCODE_CURVE, &result.encode_curve);
        read_int_tag(TAG_DECODE_CURVE, &result.decode_curve);
        read_int_tag(TAG_ENCODE_PRESET, &result.encode_preset);
    }

    result.decoded.assign(needed, 0);
    result.stage = "decode";
    result.decode_error = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(),
                                            result.decoded.data(), result.pitch);
    result.stage = "close";
    return result;
}

struct Artifact {
    std::string path;
    std::string sha256;
    size_t bytes = 0;
};

struct CaseRecord {
    std::string case_id;
    std::string family;
    int family_code = 0;
    std::string source;
    uint32_t source_code = 0;
    std::string quality;
    int quality_code = 0;
    std::string pattern;
    int width = 0;
    int height = 0;
    int bayer_format = -1;
    std::string bayer_format_name;
    size_t source_bytes = 0;
    int32_t source_pitch = 0;
    std::string source_sha256;
    std::string source_layout;
    std::string sample_sha256;
    size_t sample_bytes = 0;
    std::string normalized_sha256;
    size_t normalized_bytes = 0;
    std::vector<hf::MaskRange> mask_ranges;
    std::string decoded_sha256;
    size_t decoded_bytes = 0;
    int decode_width = 0;
    int decode_height = 0;
    int32_t decode_pitch = 0;
    std::string requested_output;
    std::string actual_output;
    EncodeResult encode;
    DecodeResult decode;
    CompareStats compare;
    std::string status;
};

struct ArtifactLog {
    std::vector<Artifact> artifacts;
    std::set<std::string> written;

    bool Save(const std::filesystem::path& root, const std::string& relative,
              const std::vector<uint8_t>& bytes, std::string* sha_out = nullptr) {
        if (!written.insert(relative).second) {
            std::cerr << "duplicate artifact path: " << relative << '\n';
            return false;
        }
        const std::filesystem::path path = root / relative;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            std::cerr << "cannot create directory for " << relative << ": " << ec.message() << '\n';
            return false;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out || (!bytes.empty() && !out.write(reinterpret_cast<const char*>(bytes.data()),
                                                  static_cast<std::streamsize>(bytes.size())))) {
            std::cerr << "cannot write " << relative << '\n';
            return false;
        }
        const std::string sha = hf::Sha256Hex(bytes);
        if (sha.empty()) {
            std::cerr << "SHA-256 failed for " << relative << '\n';
            return false;
        }
        artifacts.push_back({relative, sha, bytes.size()});
        if (sha_out) *sha_out = sha;
        return true;
    }
};

bool WriteCase(const std::filesystem::path& root, const FamilyDef& family, const SourceSpec& source,
               const QualityEntry& quality, const std::string& pattern, int width, int height,
               int bayer_format, uint32_t seed, ArtifactLog* log, CaseRecord* record) {
    const int32_t source_pitch = [&]() {
        int32_t pitch = 0;
        if (CFHD_GetImagePitch(static_cast<uint32_t>(width), source.format, &pitch) != CFHD_ERROR_OKAY) {
            return static_cast<int32_t>(width * source.pixel_size);
        }
        return pitch > 0 ? pitch : static_cast<int32_t>(width * source.pixel_size);
    }();
    const size_t source_size = static_cast<size_t>(source_pitch) * static_cast<size_t>(height);
    ByteBuffer pixels(source_size, 0);
    FillSource(pixels.data(), source_pitch, width, height, source, family.kind, pattern, bayer_format, seed);

    std::ostringstream case_id;
    case_id << family.name << '/' << source.label << '/' << quality.name << '/' << pattern << '/' << width
            << 'x' << height;
    if (family.kind == FamilyKind::Bayer && bayer_format >= 0) case_id << "/bfmt" << bayer_format;
    record->case_id = case_id.str();
    record->family = family.name;
    record->family_code = static_cast<int>(family.encoded);
    record->source = source.label;
    record->source_code = static_cast<uint32_t>(source.format);
    record->quality = quality.name;
    record->quality_code = static_cast<int>(quality.value);
    record->pattern = pattern;
    record->width = width;
    record->height = height;
    record->bayer_format = family.kind == FamilyKind::Bayer ? bayer_format : -1;
    if (family.kind == FamilyKind::Bayer) record->bayer_format_name = kBayerFormatNames[bayer_format & 3];
    record->source_bytes = source_size;
    record->source_pitch = source_pitch;
    record->source_layout = source.layout;
    record->source_sha256 = hf::Sha256Hex(pixels.data(), pixels.size());

    std::ostringstream dir;
    dir << family.name << "/f1/" << width << 'x' << height << '/' << source.label << '/' << quality.name
        << '/' << pattern;
    if (family.kind == FamilyKind::Bayer && bayer_format >= 0) dir << "/bfmt" << bayer_format;
    const std::string base = dir.str();

    if (!log->Save(root, base + "/source.bin", std::vector<uint8_t>(pixels.begin(), pixels.end()))) {
        record->status = "WRITE_FAILED";
        return false;
    }

    try {
        record->encode = EncodeCase(width, height, source, source_pitch, family.encoded, quality.value,
                                    pixels.data());
    } catch (CFHD_Error error) {
        record->encode.threw = true;
        record->encode.thrown_error = static_cast<int>(error);
    } catch (...) {
        record->encode.threw = true;
        record->encode.thrown_error = -1;
    }

    if (record->encode.threw || record->encode.prepare_error != CFHD_ERROR_OKAY ||
        record->encode.encode_error != CFHD_ERROR_OKAY ||
        record->encode.sample_error != CFHD_ERROR_OKAY || record->encode.sample.empty()) {
        std::ostringstream status;
        status << "ENCODE_FAILED stage=" << record->encode.stage
               << " prepare=" << ErrorName(record->encode.prepare_error)
               << " encode=" << ErrorName(record->encode.encode_error)
               << " sample=" << ErrorName(record->encode.sample_error);
        record->status = status.str();
        return false;
    }

    record->sample_bytes = record->encode.sample.size();
    std::vector<uint8_t> normalized = record->encode.sample;
    record->mask_ranges = hf::MaskVolatileMetadata(&normalized);
    record->normalized_bytes = normalized.size();
    if (!log->Save(root, base + "/sdk.cfhd", record->encode.sample, &record->sample_sha256) ||
        !log->Save(root, base + "/sdk.normalized.cfhd", normalized, &record->normalized_sha256)) {
        record->status = "WRITE_FAILED";
        return false;
    }

    try {
        record->decode = DecodeCase(record->encode.sample, family.kind);
    } catch (CFHD_Error error) {
        record->decode.threw = true;
        record->decode.thrown_error = static_cast<int>(error);
    } catch (...) {
        record->decode.threw = true;
        record->decode.thrown_error = -1;
    }

    if (record->decode.threw || record->decode.prepare_error != CFHD_ERROR_OKAY ||
        record->decode.decode_error != CFHD_ERROR_OKAY) {
        std::ostringstream status;
        status << "DECODE_FAILED stage=" << record->decode.stage
               << " prepare=" << ErrorName(record->decode.prepare_error)
               << " decode=" << ErrorName(record->decode.decode_error);
        if (record->decode.threw) status << " thrown=" << record->decode.thrown_error;
        record->status = status.str();
        return false;
    }

    record->decoded_bytes = record->decode.decoded.size();
    record->decoded_sha256 = hf::Sha256Hex(record->decode.decoded.data(), record->decode.decoded.size());
    if (record->decoded_sha256.empty()) {
        record->status = "HASH_FAILED";
        return false;
    }
    if (!log->Save(root, base + "/decoded.bin",
                   std::vector<uint8_t>(record->decode.decoded.begin(), record->decode.decoded.end()))) {
        record->status = "WRITE_FAILED";
        return false;
    }
    record->decode_width = record->decode.width;
    record->decode_height = record->decode.height;
    record->decode_pitch = record->decode.pitch;
    record->requested_output = FormatLabel(static_cast<uint32_t>(record->decode.requested_format));
    record->actual_output = FormatLabel(static_cast<uint32_t>(record->decode.actual_format));
    record->compare = CompareFrame(pixels.data(), source_pitch, record->decode.decoded.data(),
                                   record->decode.pitch, width, height, source.format,
                                   record->decode.actual_format);
    record->status = "EXPORTED";
    return true;
}

std::string JsonMaskRanges(const std::vector<hf::MaskRange>& ranges) {
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i) out << ',';
        out << "{\"tag\":\"" << hf::EscapeJson(ranges[i].tag) << "\",\"offset\":" << ranges[i].offset
            << ",\"length\":" << ranges[i].length << '}';
    }
    out << ']';
    return out.str();
}

std::string JsonCase(const CaseRecord& c) {
    std::ostringstream j;
    j << "{\"case_id\":\"" << hf::EscapeJson(c.case_id) << "\",\"family\":\"" << hf::EscapeJson(c.family)
      << "\",\"family_code\":" << c.family_code << ",\"source_format\":\"" << hf::EscapeJson(c.source)
      << "\",\"source_format_code\":" << c.source_code << ",\"quality\":\"" << hf::EscapeJson(c.quality)
      << "\",\"quality_code\":" << c.quality_code << ",\"pattern\":\"" << hf::EscapeJson(c.pattern)
      << "\",\"geometry\":{\"width\":" << c.width << ",\"height\":" << c.height << '}';
    if (c.bayer_format >= 0) {
        j << ",\"bayer_format\":{\"value\":" << c.bayer_format << ",\"name\":\""
          << hf::EscapeJson(c.bayer_format_name) << "\"}";
    }
    j << ",\"source\":{\"sha256\":\"" << c.source_sha256 << "\",\"byte_length\":" << c.source_bytes
      << ",\"pitch\":" << c.source_pitch << ",\"components\":" << ComponentCount(static_cast<CFHD_PixelFormat>(c.source_code))
      << ",\"component_bits\":" << ComponentBits(static_cast<CFHD_PixelFormat>(c.source_code))
      << ",\"layout\":\"" << hf::EscapeJson(c.source_layout) << "\"}";
    j << ",\"encode\":{\"threw\":" << (c.encode.threw ? "true" : "false") << ",\"thrown_error_code\":"
      << c.encode.thrown_error << ",\"prepare_error\":\"" << ErrorName(c.encode.prepare_error)
      << "\",\"encode_error\":\"" << ErrorName(c.encode.encode_error) << "\",\"sample_error\":\""
      << ErrorName(c.encode.sample_error) << "\",\"sdk_sample_bytes\":" << c.encode.sdk_sample_size
      << ",\"sample_bytes\":" << c.sample_bytes << ",\"sample_sha256\":\"" << c.sample_sha256
      << "\",\"normalized_sha256\":\"" << c.normalized_sha256 << "\",\"normalized_byte_length\":"
      << c.normalized_bytes << ",\"volatile_metadata_masks\":" << JsonMaskRanges(c.mask_ranges) << '}';
    j << ",\"sample_info\":{\"encoded_format\":" << c.decode.encoded_format << ",\"channels\":"
      << c.decode.channels << ",\"display_width\":" << c.decode.display_width
      << ",\"display_height\":" << c.decode.display_height << ",\"progressive\":" << c.decode.progressive
      << ",\"key_frame\":" << c.decode.key_frame << ",\"sdk_version\":" << c.decode.sdk_version
      << ",\"encode_version\":" << c.decode.encode_version << ",\"bayer_format\":"
      << c.decode.bayer_format << ",\"bayer_format_error\":\"" << ErrorName(c.decode.bayer_format_error)
      << "\",\"encode_curve\":" << c.decode.encode_curve << ",\"decode_curve\":"
      << c.decode.decode_curve << ",\"encode_preset\":" << c.decode.encode_preset << '}';
    j << ",\"decode\":{\"threw\":" << (c.decode.threw ? "true" : "false") << ",\"thrown_error_code\":"
      << c.decode.thrown_error << ",\"prepare_error\":\"" << ErrorName(c.decode.prepare_error)
      << "\",\"decode_error\":\"" << ErrorName(c.decode.decode_error) << "\",\"offered_output_formats\":[";
    for (int i = 0; i < c.decode.offered_count && i < kMaxOfferedFormats; ++i) {
        if (i) j << ',';
        j << '"' << FormatLabel(static_cast<uint32_t>(c.decode.offered[i])) << '"';
    }
    j << "],\"requested_output_format\":\"" << hf::EscapeJson(c.requested_output)
      << "\",\"output_format\":\"" << hf::EscapeJson(c.actual_output) << "\",\"width\":" << c.decode_width
      << ",\"height\":" << c.decode_height << ",\"pitch\":" << c.decode_pitch
      << ",\"component_bits\":"
      << ComponentBits(static_cast<CFHD_PixelFormat>(c.decode.actual_format))
      << ",\"layout\":\"" << hf::EscapeJson(LayoutLabel(static_cast<CFHD_PixelFormat>(c.decode.actual_format)))
      << "\",\"decoded_sha256\":\"" << c.decoded_sha256 << "\",\"decoded_byte_length\":"
      << c.decoded_bytes << '}';
    j << ",\"comparison\":{\"comparable\":" << (c.compare.comparable ? "true" : "false")
      << ",\"note\":\"" << hf::EscapeJson(c.compare.note) << "\",\"psnr_db\":{";
    if (c.compare.comparable) {
        if (ComponentCount(static_cast<CFHD_PixelFormat>(c.decode.actual_format)) == 1) {
            j << "\"cfa\":" << std::fixed << std::setprecision(3) << c.compare.psnr_r;
        } else {
            j << "\"r\":" << std::fixed << std::setprecision(3) << c.compare.psnr_r << ",\"g\":"
              << c.compare.psnr_g << ",\"b\":" << c.compare.psnr_b;
            if (c.compare.psnr_a >= 0.0) j << ",\"a\":" << c.compare.psnr_a;
        }
    }
    j.unsetf(std::ios::fixed);
    j << "},\"max_abs_diff\":" << c.compare.max_abs_diff << ",\"exact_match_ratio\":" << std::setprecision(6)
      << c.compare.exact_ratio;
    if (c.compare.alpha_exact_ratio >= 0.0) {
        j << ",\"alpha_exact_ratio\":" << c.compare.alpha_exact_ratio
          << ",\"alpha_max_abs_diff\":" << c.compare.alpha_max_abs_diff;
    }
    j << "},\"status\":\"" << hf::EscapeJson(c.status) << "\"}";
    return j.str();
}

bool SaveTransferManifest(const std::filesystem::path& root, const ArtifactLog& log, Artifact* out) {
    std::vector<Artifact> sorted = log.artifacts;
    std::sort(sorted.begin(), sorted.end(), [](const Artifact& a, const Artifact& b) { return a.path < b.path; });
    std::ostringstream text;
    for (const Artifact& artifact : sorted) {
        text << artifact.sha256 << '\t' << artifact.bytes << '\t' << artifact.path << '\n';
    }
    const std::string content = text.str();
    std::ofstream file(root / "TRANSFER_SHA256.tsv", std::ios::binary | std::ios::trunc);
    if (!file || !file.write(content.data(), static_cast<std::streamsize>(content.size()))) return false;
    out->path = "TRANSFER_SHA256.tsv";
    out->bytes = content.size();
    out->sha256 = hf::Sha256Hex(reinterpret_cast<const uint8_t*>(content.data()), content.size());
    return !out->sha256.empty();
}

bool WriteManifest(const std::filesystem::path& root, const std::vector<CaseRecord>& cases,
                   const ArtifactLog& log, const Artifact& transfer, int width, int height,
                   const std::string& quality_selection, const std::string& source_selection) {
    int exported = 0;
    int encode_failed = 0;
    int decode_failed = 0;
    int write_failed = 0;
    for (const CaseRecord& c : cases) {
        if (c.status == "EXPORTED") ++exported;
        else if (c.status.rfind("ENCODE_FAILED", 0) == 0) ++encode_failed;
        else if (c.status.rfind("DECODE_FAILED", 0) == 0) ++decode_failed;
        else ++write_failed;
    }
    std::ostringstream j;
    j << "{\n\"schema\":\"hypercine.format_corpus/1\",\n\"issue\":36,\n\"stage\":\"F1\",\n"
      << "\"description\":\"Real pristine-SDK golden corpora for the compressed CineForm families the F0 matrix qualified. Raw corpora live outside Git; this manifest is the published index.\",\n"
      << "\"sdk\":{\"frozen_upstream_commit\":\"" << kFrozenUpstreamCommit
      << "\",\"usage\":\"pristine GoPro CineForm SDK, no upstream modification\"},\n"
      << "\"host\":{\"os\":\"" << HostOs() << "\",\"compiler\":\"" << hf::EscapeJson(CompilerLabel())
      << "\"},\n"
      << "\"generator\":{\"name\":\"format_corpus_export\",\"seed\":\"0x1337BEEF\","
         "\"source_generation_deterministic\":true,"
         "\"packet_reproducibility\":\"measured per run; see limits\"},\n"
      << "\"selection\":{\"width\":" << width << ",\"height\":" << height << ",\"qualities\":\""
      << hf::EscapeJson(quality_selection) << "\",\"sources\":\"" << hf::EscapeJson(source_selection)
      << "\"},\n"
      << "\"layout\":\"<family>/f1/<width>x<height>/<source>/<quality>/<pattern>/source.bin|sdk.cfhd|sdk.normalized.cfhd|decoded.bin\",\n"
      << "\"normalization\":{\"tags\":[\"GUID\",\"DATE\",\"TIME\",\"TIMC\"],\"method\":\"zero only the parsed tag payload bytes; the packet length and every other byte stay unchanged\"},\n"
      << "\"limits\":[\"No claim that HyperCine's own encoder or decoder implements these modes.\","
         "\"No visual or production fidelity claim; PSNR is reported only when the decoded output layout equals the source layout.\","
         "\"YUVA_4444, BYR5, b48r, WP13, W13A, BYR2 and yuyv are excluded because F0 measured them as SDK_UNSUPPORTED or DECODE_FAILED.\","
         "\"F2 geometry, stereo, KEYING and uncompressed flags are out of scope.\","
         "\"Raw packets embed a fresh GUID and wall-clock DATE/TIME/TIMC metadata; raw hashes never reproduce.\","
         "\"Normalized RGB_444 and RGBA_4444 packets reproduced across measured runs. Measured BAYER packets did not: the pristine encoder seeds rand() from a metadata CRC and the Bayer path uses rand() dithering, so even normalized and decoded Bayer hashes are run-dependent. Treat BAYER hashes as per-run evidence, not as a cross-run byte gate.\","
         "\"TAG_BAYER_FORMAT could not be attached through the public encoder metadata API in this build (attempt crashed before EncodeSample), so every BAYER case uses the SDK default CFA and does not prove RGGB/GRBG/GBRG/BGGR variants.\","
         "\"Raw corpus bytes are not part of the Git tree; verify transferred copies with --verify.\"],\n"
      << "\"transfer_manifest\":{\"path\":\"" << transfer.path << "\",\"sha256\":\"" << transfer.sha256
      << "\",\"byte_length\":" << transfer.bytes << "},\n"
      << "\"summary\":{\"cases\":" << cases.size() << ",\"exported\":" << exported
      << ",\"encode_failed\":" << encode_failed << ",\"decode_failed\":" << decode_failed
      << ",\"write_failed\":" << write_failed << "},\n\"cases\":[";
    for (size_t i = 0; i < cases.size(); ++i) {
        if (i) j << ',';
        j << JsonCase(cases[i]);
    }
    j << "],\n\"artifacts\":[";
    for (size_t i = 0; i < log.artifacts.size(); ++i) {
        const Artifact& a = log.artifacts[i];
        if (i) j << ',';
        j << "{\"path\":\"" << hf::EscapeJson(a.path) << "\",\"sha256\":\"" << a.sha256
          << "\",\"byte_length\":" << a.bytes << '}';
    }
    j << "]\n}\n";
    std::ofstream out(root / "manifest.json", std::ios::binary | std::ios::trunc);
    if (!out || !out.write(j.str().data(), static_cast<std::streamsize>(j.str().size()))) return false;

    std::cout << "F1 corpus summary: cases=" << cases.size() << " exported=" << exported
              << " encode_failed=" << encode_failed << " decode_failed=" << decode_failed
              << " write_failed=" << write_failed << '\n';
    return true;
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

bool VerifyDirectory(const std::filesystem::path& root) {
    const std::string manifest = ReadTextFile(root / "manifest.json");
    if (manifest.empty()) {
        std::cerr << "manifest.json not found or empty\n";
        return false;
    }
    if (manifest.find("\"schema\":\"hypercine.format_corpus/1\"") == std::string::npos) {
        std::cerr << "unsupported manifest schema\n";
        return false;
    }
    const std::regex item(
        R"re(\{\s*"path"\s*:\s*"([^"]+)"\s*,\s*"sha256"\s*:\s*"([0-9a-f]{64})"\s*,\s*"byte_length"\s*:\s*([0-9]+)\s*\})re");
    size_t checked = 0;
    for (std::sregex_iterator it(manifest.begin(), manifest.end(), item), end; it != end; ++it) {
        const std::filesystem::path path = root / (*it)[1].str();
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::cerr << "missing artifact: " << (*it)[1].str() << '\n';
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        const size_t expected_size = static_cast<size_t>(std::stoull((*it)[3].str()));
        const std::string actual = hf::Sha256Hex(bytes);
        if (bytes.size() != expected_size || actual != (*it)[2].str()) {
            std::cerr << "hash/size mismatch: " << (*it)[1].str() << '\n';
            return false;
        }
        ++checked;
    }
    if (checked == 0) {
        std::cerr << "manifest contains no artifact records\n";
        return false;
    }

    const std::regex transfer_item(
        R"re(\{\s*"path"\s*:\s*"TRANSFER_SHA256\.tsv"\s*,\s*"sha256"\s*:\s*"([0-9a-f]{64})"\s*,\s*"byte_length"\s*:\s*([0-9]+)\s*\})re");
    std::smatch transfer;
    if (std::regex_search(manifest, transfer, transfer_item)) {
        const std::string text = ReadTextFile(root / "TRANSFER_SHA256.tsv");
        if (text.empty()) {
            std::cerr << "TRANSFER_SHA256.tsv missing\n";
            return false;
        }
        if (text.size() != static_cast<size_t>(std::stoull(transfer[2].str())) ||
            hf::Sha256Hex(reinterpret_cast<const uint8_t*>(text.data()), text.size()) != transfer[1].str()) {
            std::cerr << "TRANSFER_SHA256.tsv hash/size mismatch\n";
            return false;
        }
        std::istringstream lines(text);
        std::string line;
        size_t rows = 0;
        while (std::getline(lines, line)) {
            std::istringstream row(line);
            std::string sha;
            size_t bytes = 0;
            std::string relative;
            if (!(row >> sha >> bytes >> relative)) continue;
            const std::filesystem::path path = root / relative;
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                std::cerr << "missing transfer artifact: " << relative << '\n';
                return false;
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
            if (data.size() != bytes || hf::Sha256Hex(data) != sha) {
                std::cerr << "transfer mismatch: " << relative << '\n';
                return false;
            }
            ++rows;
        }
        if (rows == 0) {
            std::cerr << "TRANSFER_SHA256.tsv has no rows\n";
            return false;
        }
        std::cout << "Verified " << checked << " manifest artifacts and " << rows
                  << " TRANSFER_SHA256.tsv rows.\n";
        return true;
    }
    std::cout << "Verified " << checked << " manifest artifacts (no transfer manifest recorded).\n";
    return true;
}

bool ParseQuality(const std::string& name, CFHD_EncodingQuality* quality) {
    for (const QualityEntry& entry : kQualities) {
        if (name == entry.name) {
            *quality = entry.value;
            return true;
        }
    }
    return false;
}

void Usage() {
    std::cout << "format_corpus_export --out DIR [options]\n"
                 "  --width N --height N        geometry (default 1920x1080)\n"
                 "  --quality NAME|all          low|medium|high|filmscan1|filmscan2|filmscan3|all\n"
                 "                              (default filmscan1,filmscan3)\n"
                 "  --family NAME               RGB_444|RGBA_4444|BAYER (default all)\n"
                 "  --source CODE               source pixel format code (default all implemented)\n"
                 "  --pattern NAME              restrict to one deterministic pattern\n"
                 "  --verify DIR                verify a transferred corpus against its manifest\n"
                 "  --list                      print the case matrix without writing\n";
}

}  // namespace

int main(int argc, char** argv) {
    int width = 1920;
    int height = 1080;
    std::filesystem::path out_path;
    std::filesystem::path verify_path;
    std::string only_family;
    std::string only_source;
    std::string only_pattern;
    std::string quality_selection = "filmscan1,filmscan3";
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--width" && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (arg == "--height" && i + 1 < argc) height = std::atoi(argv[++i]);
        else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (arg == "--quality" && i + 1 < argc) quality_selection = argv[++i];
        else if (arg == "--family" && i + 1 < argc) only_family = argv[++i];
        else if (arg == "--source" && i + 1 < argc) only_source = argv[++i];
        else if (arg == "--pattern" && i + 1 < argc) only_pattern = argv[++i];
        else if (arg == "--verify" && i + 1 < argc) verify_path = argv[++i];
        else if (arg == "--list") list_only = true;
        else { Usage(); return 2; }
    }

    if (!verify_path.empty()) {
        if (argc != 3) { Usage(); return 2; }
        return VerifyDirectory(verify_path) ? 0 : 1;
    }
    if (width <= 0 || height <= 0) { Usage(); return 2; }
    if (out_path.empty() && !list_only) { Usage(); return 2; }

    std::vector<QualityEntry> qualities;
    if (quality_selection == "all") {
        qualities.assign(kQualities.begin(), kQualities.end());
    } else {
        std::istringstream stream(quality_selection);
        std::string name;
        while (std::getline(stream, name, ',')) {
            CFHD_EncodingQuality quality;
            if (!ParseQuality(name, &quality)) {
                std::cerr << "unsupported quality: " << name << '\n';
                return 2;
            }
            for (const QualityEntry& entry : kQualities) {
                if (entry.value == quality) qualities.push_back(entry);
            }
        }
    }
    if (qualities.empty()) { Usage(); return 2; }

    std::vector<std::string> patterns_filter;
    if (!only_pattern.empty()) patterns_filter.push_back(only_pattern);

    if (!out_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(out_path, ec);
        if (ec) {
            std::cerr << "cannot create output directory: " << ec.message() << '\n';
            return 1;
        }
    }

    ArtifactLog log;
    std::vector<CaseRecord> cases;
    int listed = 0;
    for (const FamilyDef& family : kFamilies) {
        if (!only_family.empty() && only_family != family.name) continue;
        for (const SourceSpec& source : SourcesFor(family.kind)) {
            if (!only_source.empty() && only_source != source.label) continue;
            for (const char* pattern : PatternsFor(family.kind)) {
                if (!patterns_filter.empty() &&
                    std::find(patterns_filter.begin(), patterns_filter.end(), pattern) == patterns_filter.end()) {
                    continue;
                }
                for (const QualityEntry& quality : qualities) {
                    if (list_only) {
                        std::cout << family.name << '/' << source.label << '/' << quality.name << '/'
                                  << pattern << '/' << width << 'x' << height << '\n';
                        ++listed;
                        continue;
                    }
                    CaseRecord record;
                    std::cout << "case " << family.name << ' ' << source.label << ' ' << quality.name << ' '
                              << pattern << ' ' << width << 'x' << height << std::endl;
                    WriteCase(out_path, family, source, quality, pattern, width, height, -1, kSourceSeed,
                              &log, &record);
                    std::cout << "  -> " << record.status << " sample=" << record.sample_bytes
                              << " decoded=" << record.decoded_bytes << " out=" << record.actual_output
                              << '\n';
                    cases.push_back(std::move(record));
                }
            }
        }
    }
    if (list_only) {
        std::cout << "listed " << listed << " cases\n";
        return 0;
    }
    if (cases.empty()) {
        std::cerr << "no cases selected\n";
        return 2;
    }

    Artifact transfer;
    if (!SaveTransferManifest(out_path, log, &transfer)) {
        std::cerr << "failed to write TRANSFER_SHA256.tsv\n";
        return 1;
    }
    if (!WriteManifest(out_path, cases, log, transfer, width, height, quality_selection,
                       only_source.empty() ? "all implemented" : only_source)) {
        std::cerr << "failed to write manifest.json\n";
        return 1;
    }
    std::cout << "wrote " << out_path.string() << '\n';
    return 0;
}
