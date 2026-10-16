// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Issue #36 stage F0: real GoPro SDK capability probe.
//
// For every compressed CineForm family and candidate source CFHD_PixelFormat this
// runs the actual SDK entry points (PrepareToEncode -> EncodeSample -> GetSampleData,
// then PrepareToDecode -> DecodeSample on a fresh decoder instance) on one synthetic
// frame and records what the SDK really returned. A combination is only reported as
// BIDIRECTIONAL_PASS when both directions succeeded; everything else is reported as
// the observed failure instead of a placeholder pass.
//
// The probe measures API capability on a deterministic synthetic source. It does
// not claim visual or mathematical fidelity for the decoded pixels.
//
// The pristine sample layer reports unsupported pixel formats by throwing
// CFHD_ERROR_BADFORMAT out of C entry points such as CFHD_GetImageSize
// (upstream/DecoderSDK/SampleDecoder.cpp), so every SDK call is made inside a
// C++ catch that records the stage it came from. One hostile combination must
// not abort the whole matrix.

#ifndef _ALLOCATOR
#define _ALLOCATOR 1
#endif

#include "CFHDDecoder.h"
#include "CFHDEncoder.h"
#include "CFHDError.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"
#include "CFHDTypes.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include "../tests/test_vectors.h"

namespace {

using ByteBuffer = HyperCineTest::AlignedVector<uint8_t>;
using EncoderOwner = std::unique_ptr<std::remove_pointer_t<CFHD_EncoderRef>, decltype(&CFHD_CloseEncoder)>;
using DecoderOwner = std::unique_ptr<std::remove_pointer_t<CFHD_DecoderRef>, decltype(&CFHD_CloseDecoder)>;
using MetadataOwner = std::unique_ptr<std::remove_pointer_t<CFHD_MetadataRef>, decltype(&CFHD_CloseMetadata)>;

// Pinned upstream baseline; the measured SDK version of a produced sample is also
// recorded per case so the matrix never rests on this constant alone.
constexpr char kFrozenUpstreamCommit[] = "11574d0295771edccadd17af14af74a539f924c7";
constexpr uint32_t kSourceSeed = 0x1337BEEFu;
constexpr int kDefaultSize = 256;
constexpr int kMaxFormats = 64;

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

struct Family {
    CFHD_EncodedFormat value;
    const char* name;
    std::vector<CFHD_PixelFormat> sources;
};

std::string FourCharCode(uint32_t value) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((value >> shift) & 0xFFu);
        out.push_back((c >= 0x20 && c < 0x7F) ? c : '.');
    }
    return out;
}

// Formats whose enum value is a four-character code are reported as that code so the
// matrix stays readable; the numeric value is kept alongside it in the JSON.
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

std::string Escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 0x20) out << '?';
        else out << static_cast<char>(c);
    }
    return out.str();
}

// FNV-1a 64 is used for the F0 capability record so the probe needs no external
// dependency. Stage F1 will emit SHA-256 in the archived manifest.
uint64_t Fnv1a64(const uint8_t* data, size_t size) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::string HashText(uint64_t hash) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

// Deterministic across runs and identical for every case at the same geometry, so a
// re-run of the probe can be compared byte for byte (AGENTS.md rule 6).
void FillSource(ByteBuffer& buffer, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            if (index < buffer.size()) {
                buffer[index] = static_cast<uint8_t>((x * 29 + y * 17 + static_cast<int>(kSourceSeed)) & 0xFF);
            }
        }
    }
}

struct SourceGeometry {
    uint32_t bytes = 0;
    int32_t pitch = 0;
    bool rejected = false;
    std::string note;
};

// CFHD_GetImageSize and CFHD_GetImagePitch both route through upstream
// GetPixelSize, which throws for a format it does not list.
SourceGeometry ProbeSourceGeometry(int width, int height, CFHD_PixelFormat format) {
    SourceGeometry geometry;
    try {
        uint32_t size = 0;
        int32_t pitch = 0;
        if (CFHD_GetImagePitch(static_cast<uint32_t>(width), format, &pitch) == CFHD_ERROR_OKAY && pitch > 0) {
            geometry.pitch = pitch;
        }
        if (CFHD_GetImageSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height), format,
                              VIDEO_SELECT_DEFAULT, STEREO3D_TYPE_DEFAULT, &size) == CFHD_ERROR_OKAY &&
            size > 0) {
            geometry.bytes = size;
        }
        if (geometry.bytes == 0 && geometry.pitch > 0) {
            geometry.bytes = static_cast<uint32_t>(geometry.pitch) * static_cast<uint32_t>(height);
            geometry.note = "image size derived from row pitch times height";
        }
        if (geometry.bytes == 0) {
            geometry.note = "SDK reports no image geometry for this pixel format";
        }
    } catch (CFHD_Error error) {
        geometry.rejected = true;
        geometry.note = "SDK rejected this pixel format while sizing it (" + ErrorName(error) + ")";
    } catch (...) {
        geometry.rejected = true;
        geometry.note = "SDK threw an unrecognized value while sizing this pixel format";
    }
    return geometry;
}

struct EncodeIo {
    int width = 0;
    int height = 0;
    int32_t pitch = 0;
    CFHD_PixelFormat source_format = CFHD_PIXEL_FORMAT_UNKNOWN;
    CFHD_EncodedFormat encoded_format = CFHD_ENCODED_FORMAT_UNKNOWN;
    CFHD_EncodingQuality quality = CFHD_ENCODING_QUALITY_FILMSCAN1;
    const uint8_t* source = nullptr;
    // The encoder owns the sample buffer and releases it on close, so the caller
    // hands in storage that the guard fills before CFHD_CloseEncoder runs.
    uint8_t* sample_out = nullptr;
    size_t sample_capacity = 0;
    // outputs
    const char* stage = "open";
    bool opened = false;
    bool thrown = false;
    int thrown_error = 0;
    CFHD_Error prepare_error = CFHD_ERROR_INTERNAL;
    CFHD_Error encode_error = CFHD_ERROR_INTERNAL;
    CFHD_Error sample_error = CFHD_ERROR_INTERNAL;
    size_t sdk_sample_size = 0;
    size_t copied_sample_size = 0;
};

// May throw a CFHD_Error; the caller catches it and reads `stage` to learn which
// SDK entry point threw.
void RunEncode(EncodeIo* io) {
    CFHD_EncoderRef encoder = nullptr;
    io->stage = "open";
    const CFHD_Error open_error = CFHD_OpenEncoder(&encoder, nullptr);
    EncoderOwner owner(encoder, &CFHD_CloseEncoder);
    if (open_error != CFHD_ERROR_OKAY) return;
    io->opened = true;
    io->stage = "prepare";
    io->prepare_error = CFHD_PrepareToEncode(encoder, io->width, io->height, io->source_format,
                                             io->encoded_format, CFHD_ENCODING_FLAGS_NONE, io->quality);
    if (io->prepare_error == CFHD_ERROR_OKAY) {
        io->stage = "encode";
        io->encode_error = CFHD_EncodeSample(encoder, const_cast<uint8_t*>(io->source), io->pitch);
    }
    if (io->encode_error == CFHD_ERROR_OKAY) {
        io->stage = "get_sample";
        void* data = nullptr;
        size_t size = 0;
        io->sample_error = CFHD_GetSampleData(encoder, &data, &size);
        io->sdk_sample_size = size;
        if (io->sample_error == CFHD_ERROR_OKAY && data && size > 0 && size <= io->sample_capacity &&
            io->sample_out) {
            memcpy(io->sample_out, data, size);
            io->copied_sample_size = size;
        }
    }
    io->stage = "close";
}

constexpr int kMaxOfferedFormats = 64;
constexpr size_t kMaxOutputBytes = 64u * 1024u * 1024u;

struct DecodeIo {
    const uint8_t* sample = nullptr;
    size_t sample_size = 0;
    uint8_t* output = nullptr;
    size_t output_capacity = 0;
    // outputs
    const char* stage = "open";
    bool thrown = false;
    int thrown_error = 0;
    CFHD_Error info_error = CFHD_ERROR_INTERNAL;
    CFHD_Error prepare_error = CFHD_ERROR_INTERNAL;
    CFHD_Error decode_error = CFHD_ERROR_INTERNAL;
    CFHD_PixelFormat output_format = CFHD_PIXEL_FORMAT_UNKNOWN;
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
    bool decoded = false;
};

// Reads one int32 sample-info field; a tag the SDK rejects stays at its sentinel.
void QueryInfo(CFHD_DecoderRef decoder, const DecodeIo* io, CFHD_SampleInfoTag tag, int32_t* value) {
    int32_t read = -1;
    if (CFHD_GetSampleInfo(decoder, const_cast<uint8_t*>(io->sample), io->sample_size, tag, &read,
                           sizeof(read)) != CFHD_ERROR_OKAY) {
        read = -1;
    }
    *value = read;
}

// May throw a CFHD_Error; the caller catches it and reads `stage`.
void RunDecode(DecodeIo* io) {
    CFHD_DecoderRef decoder = nullptr;
    CFHD_MetadataRef metadata = nullptr;
    io->stage = "open";
    const CFHD_Error open_error = CFHD_OpenDecoder(&decoder, nullptr);
    DecoderOwner owner(decoder, &CFHD_CloseDecoder);
    MetadataOwner metadata_owner(nullptr, &CFHD_CloseMetadata);
    if (open_error != CFHD_ERROR_OKAY) return;
    io->stage = "sample_info";
    QueryInfo(decoder, io, CFHD_SAMPLE_ENCODED_FORMAT, &io->encoded_format);
    QueryInfo(decoder, io, CFHD_SAMPLE_INFO_CHANNELS, &io->channels);
    QueryInfo(decoder, io, CFHD_SAMPLE_DISPLAY_WIDTH, &io->display_width);
    QueryInfo(decoder, io, CFHD_SAMPLE_DISPLAY_HEIGHT, &io->display_height);
    QueryInfo(decoder, io, CFHD_SAMPLE_PROGRESSIVE, &io->progressive);
    QueryInfo(decoder, io, CFHD_SAMPLE_KEY_FRAME, &io->key_frame);
    QueryInfo(decoder, io, CFHD_SAMPLE_SDK_VERSION, &io->sdk_version);
    QueryInfo(decoder, io, CFHD_SAMPLE_ENCODE_VERSION, &io->encode_version);
    io->info_error = CFHD_ERROR_OKAY;

    // Ask the SDK which output formats it offers for this sample and prefer BGRA
    // when it is available, so results stay comparable across families.
    io->stage = "output_formats";
    if (CFHD_GetOutputFormats(decoder, const_cast<uint8_t*>(io->sample), io->sample_size, io->offered,
                              kMaxOfferedFormats, &io->offered_count) != CFHD_ERROR_OKAY) {
        io->offered_count = 0;
    }
    CFHD_PixelFormat requested = CFHD_PIXEL_FORMAT_BGRA;
    bool have_bgra = false;
    for (int i = 0; i < io->offered_count; ++i) {
        if (io->offered[i] == CFHD_PIXEL_FORMAT_BGRA) have_bgra = true;
    }
    if (!have_bgra && io->offered_count > 0) requested = io->offered[0];

    io->stage = "prepare";
    int actual_width = 0;
    int actual_height = 0;
    CFHD_PixelFormat actual_format = CFHD_PIXEL_FORMAT_UNKNOWN;
    io->prepare_error = CFHD_PrepareToDecode(decoder, 0, 0, requested, CFHD_DECODED_RESOLUTION_FULL,
                                             CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(io->sample),
                                             io->sample_size, &actual_width, &actual_height, &actual_format);
    if (io->prepare_error == CFHD_ERROR_OKAY) {
        io->output_format = actual_format;
        io->width = actual_width;
        io->height = actual_height;
        io->stage = "image_pitch";
        int32_t pitch = 0;
        if (CFHD_GetImagePitch(static_cast<uint32_t>(actual_width), actual_format, &pitch) == CFHD_ERROR_OKAY &&
            pitch > 0) {
            io->pitch = pitch;
        }
        const size_t needed = static_cast<size_t>(pitch) * static_cast<size_t>(actual_height);
        if (io->pitch > 0 && needed > 0 && needed <= io->output_capacity) {
            // The pristine decoder races its worker threads on machines with more
            // than eight cores. TAG_CPU_MAX pins the thread count through the public
            // metadata API (see docs/UPSTREAM.md).
            io->stage = "metadata";
            const CFHD_Error metadata_error = CFHD_OpenMetadata(&metadata);
            metadata_owner.reset(metadata);
            if (metadata_error == CFHD_ERROR_OKAY) {
                CFHD_InitSampleMetadata(metadata, METADATATYPE_ORIGINAL, const_cast<uint8_t*>(io->sample),
                                        static_cast<uint32_t>(io->sample_size));
                uint32_t process_flags = PROCESSING_ALL_ON;
                CFHD_SetActiveMetadata(decoder, metadata, TAG_PROCESS_PATH, METADATATYPE_UINT32, &process_flags,
                                       sizeof(process_flags));
                uint32_t cpu_max = 1;
                CFHD_SetActiveMetadata(decoder, metadata, TAG_CPU_MAX, METADATATYPE_HIDDEN, &cpu_max,
                                       sizeof(cpu_max));
            }
            io->stage = "decode";
            io->decode_error = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(io->sample), io->sample_size,
                                                 io->output, io->pitch);
            if (io->decode_error == CFHD_ERROR_OKAY) io->decoded = true;
        } else {
            io->decode_error = CFHD_ERROR_DECODE_BUFFER_SIZE;
        }
    }
    io->stage = "close";
}

struct CaseResult {
    std::string family;
    int family_code = 0;
    std::string source_format;
    uint32_t source_format_code = 0;
    std::string quality;
    int quality_code = 0;
    uint32_t source_bytes = 0;
    std::string source_note;
    uint64_t source_hash = 0;
    bool geometry_rejected = false;
    EncodeIo encode;
    DecodeIo decode;
    ByteBuffer sample;
    uint64_t output_hash = 0;
    std::string verdict;
};

std::vector<Family> BuildFamilies() {
    return {
        {CFHD_ENCODED_FORMAT_YUV_422, "YUV_422",
         {CFHD_PIXEL_FORMAT_BGRA, CFHD_PIXEL_FORMAT_BGRa, CFHD_PIXEL_FORMAT_YUY2, CFHD_PIXEL_FORMAT_YUYV,
          CFHD_PIXEL_FORMAT_2VUY, CFHD_PIXEL_FORMAT_V210, CFHD_PIXEL_FORMAT_YU64, CFHD_PIXEL_FORMAT_RG24}},
        {CFHD_ENCODED_FORMAT_RGB_444, "RGB_444",
         {CFHD_PIXEL_FORMAT_RG24, CFHD_PIXEL_FORMAT_RG48, CFHD_PIXEL_FORMAT_B48R, CFHD_PIXEL_FORMAT_BGRA,
          CFHD_PIXEL_FORMAT_RG30, CFHD_PIXEL_FORMAT_AB10, CFHD_PIXEL_FORMAT_AR10, CFHD_PIXEL_FORMAT_WP13,
          CFHD_PIXEL_FORMAT_R210, CFHD_PIXEL_FORMAT_DPX0}},
        {CFHD_ENCODED_FORMAT_RGBA_4444, "RGBA_4444",
         {CFHD_PIXEL_FORMAT_BGRA, CFHD_PIXEL_FORMAT_BGRa, CFHD_PIXEL_FORMAT_B64A, CFHD_PIXEL_FORMAT_RG64,
          CFHD_PIXEL_FORMAT_W13A}},
        {CFHD_ENCODED_FORMAT_BAYER, "BAYER",
         {CFHD_PIXEL_FORMAT_BYR4, CFHD_PIXEL_FORMAT_BYR5, CFHD_PIXEL_FORMAT_BYR2}},
        {CFHD_ENCODED_FORMAT_YUVA_4444, "YUVA_4444",
         {CFHD_PIXEL_FORMAT_BGRA, CFHD_PIXEL_FORMAT_BGRa}},
    };
}

// A code the SDK uses to say "this combination is not one I accept" is a capability
// answer. Anything else is a failure the matrix must not disguise as unsupported.
bool IsUnsupportedCode(CFHD_Error error) {
    switch (error) {
        case CFHD_ERROR_BADFORMAT:
        case CFHD_ERROR_INVALID_ARGUMENT:
        case CFHD_ERROR_BADSCALING:
        case CFHD_ERROR_BAD_RESOLUTION:
        case CFHD_ERROR_LICENSING:
            return true;
        default:
            return false;
    }
}

std::string VerdictFor(const CaseResult& item) {
    if (item.source_bytes == 0) return item.geometry_rejected ? "SDK_UNSUPPORTED" : "NOT_TESTED";
    if (item.encode.thrown) {
        if (IsUnsupportedCode(static_cast<CFHD_Error>(item.encode.thrown_error))) {
            return "SDK_UNSUPPORTED";
        }
        return "ENCODE_FAILED";
    }
    if (!item.encode.opened) return "ENCODE_FAILED";
    if (item.encode.prepare_error != CFHD_ERROR_OKAY) {
        return IsUnsupportedCode(item.encode.prepare_error) ? "SDK_UNSUPPORTED" : "ENCODE_FAILED";
    }
    if (item.encode.encode_error != CFHD_ERROR_OKAY) {
        return IsUnsupportedCode(item.encode.encode_error) ? "SDK_UNSUPPORTED" : "ENCODE_FAILED";
    }
    if (item.encode.sample_error != CFHD_ERROR_OKAY || item.sample.empty()) return "ENCODE_FAILED";
    if (item.decode.thrown) {
        if (IsUnsupportedCode(static_cast<CFHD_Error>(item.decode.thrown_error))) {
            return "DECODE_UNSUPPORTED";
        }
        return "DECODE_FAILED";
    }
    if (item.decode.prepare_error != CFHD_ERROR_OKAY) {
        return IsUnsupportedCode(item.decode.prepare_error) ? "DECODE_UNSUPPORTED" : "DECODE_FAILED";
    }
    if (item.decode.decode_error != CFHD_ERROR_OKAY) {
        return IsUnsupportedCode(item.decode.decode_error) ? "DECODE_UNSUPPORTED" : "DECODE_FAILED";
    }
    return "BIDIRECTIONAL_PASS";
}

std::string JsonCase(const CaseResult& item) {
    std::ostringstream json;
    json << "{\"family\":\"" << Escape(item.family) << "\",\"family_code\":" << item.family_code
         << ",\"source_format\":\"" << Escape(item.source_format) << "\",\"source_format_code\":"
         << item.source_format_code << ",\"quality\":\"" << Escape(item.quality) << "\",\"quality_code\":"
         << item.quality_code << ",\"source_bytes\":" << item.source_bytes << ",\"source_fnv1a64\":\""
         << HashText(item.source_hash) << "\"";
    if (!item.source_note.empty()) json << ",\"source_note\":\"" << Escape(item.source_note) << "\"";
    json << ",\"encode\":{\"threw\":"
         << (item.encode.thrown ? "true" : "false") << ",\"thrown_stage\":\"" << Escape(item.encode.stage)
         << "\",\"thrown_error_code\":" << item.encode.thrown_error << ",\"opened\":"
         << (item.encode.opened ? "true" : "false") << ",\"prepare_error_code\":"
         << static_cast<int>(item.encode.prepare_error) << ",\"prepare_error_name\":\""
         << ErrorName(item.encode.prepare_error) << "\",\"encode_error_code\":"
         << static_cast<int>(item.encode.encode_error) << ",\"encode_error_name\":\""
         << ErrorName(item.encode.encode_error) << "\",\"sample_error_code\":"
         << static_cast<int>(item.encode.sample_error) << ",\"sample_error_name\":\""
         << ErrorName(item.encode.sample_error) << "\",\"sdk_sample_bytes\":" << item.encode.sdk_sample_size
         << ",\"sample_bytes\":" << item.sample.size() << ",\"sample_fnv1a64\":\"";
    if (item.sample.empty()) json << "none";
    else json << HashText(Fnv1a64(item.sample.data(), item.sample.size()));
    json << "\"}";
    json << ",\"sample_info\":{\"encoded_format\":" << item.decode.encoded_format
         << ",\"channels\":" << item.decode.channels << ",\"display_width\":" << item.decode.display_width
         << ",\"display_height\":" << item.decode.display_height
         << ",\"progressive\":" << item.decode.progressive << ",\"key_frame\":" << item.decode.key_frame
         << ",\"sdk_version\":" << item.decode.sdk_version
         << ",\"encode_version\":" << item.decode.encode_version << "}";
    json << ",\"decode\":{\"threw\":" << (item.decode.thrown ? "true" : "false") << ",\"thrown_stage\":\""
         << Escape(item.decode.stage) << "\",\"thrown_error_code\":" << item.decode.thrown_error
         << ",\"prepare_error_code\":" << static_cast<int>(item.decode.prepare_error)
         << ",\"prepare_error_name\":\"" << ErrorName(item.decode.prepare_error) << "\",\"decode_error_code\":"
         << static_cast<int>(item.decode.decode_error) << ",\"decode_error_name\":\""
         << ErrorName(item.decode.decode_error) << "\",\"offered_output_formats\":[";
    for (int i = 0; i < item.decode.offered_count && i < kMaxOfferedFormats; ++i) {
        if (i) json << ',';
        json << '"' << Escape(FormatLabel(static_cast<uint32_t>(item.decode.offered[i]))) << '"';
    }
    json << "],\"output_format\":\"" << FormatLabel(static_cast<uint32_t>(item.decode.output_format))
         << "\",\"width\":" << item.decode.width << ",\"height\":" << item.decode.height << ",\"pitch\":"
         << item.decode.pitch << ",\"decoded\":" << (item.decode.decoded ? "true" : "false")
         << ",\"output_fnv1a64\":\"";
    if (item.decode.decoded) json << HashText(item.output_hash);
    else json << "none";
    json << "\"},\"verdict\":\"" << item.verdict << "\"}";
    return json.str();
}

struct Summary {
    int total = 0;
    int bidirectional_pass = 0;
    int sdk_unsupported = 0;
    int encode_failed = 0;
    int decode_unsupported = 0;
    int decode_failed = 0;
    int not_tested = 0;
};

void Tally(Summary* summary, const CaseResult& item) {
    ++summary->total;
    if (item.verdict == "BIDIRECTIONAL_PASS") ++summary->bidirectional_pass;
    else if (item.verdict == "SDK_UNSUPPORTED") ++summary->sdk_unsupported;
    else if (item.verdict == "ENCODE_FAILED") ++summary->encode_failed;
    else if (item.verdict == "DECODE_UNSUPPORTED") ++summary->decode_unsupported;
    else if (item.verdict == "DECODE_FAILED") ++summary->decode_failed;
    else ++summary->not_tested;
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

void PrintCase(const CaseResult& item, std::string* current_family) {
    if (*current_family != item.family) {
        *current_family = item.family;
        std::cout << "[" << item.family << "]\n";
    }
    std::cout << "  " << std::setw(6) << std::left << item.source_format << std::right << ' ' << std::setw(10)
              << item.quality << ' ' << std::setw(20) << std::left << item.verdict << std::right;
    if (item.verdict == "BIDIRECTIONAL_PASS") {
        std::cout << " sample=" << std::setw(8) << item.sample.size() << " bytes encoded_format="
                  << item.decode.encoded_format << " out="
                  << FormatLabel(static_cast<uint32_t>(item.decode.output_format)) << ' ' << item.decode.width
                  << 'x' << item.decode.height;
    } else if (item.verdict == "NOT_TESTED") {
        std::cout << ' ' << item.source_note;
    } else {
        if (item.encode.thrown) {
            std::cout << " threw at encode/" << item.encode.stage << " code=" << item.encode.thrown_error;
        } else {
            std::cout << " prepare=" << ErrorName(item.encode.prepare_error)
                      << " encode=" << ErrorName(item.encode.encode_error);
        }
        if (item.decode.thrown) {
            std::cout << " decode threw at " << item.decode.stage;
        } else if (item.decode.decode_error != CFHD_ERROR_OKAY) {
            std::cout << " decode=" << ErrorName(item.decode.decode_error);
        }
    }
    std::cout << std::endl;  // streamed so a later fault still leaves the trace visible
}

void Usage() {
    std::cout << "format_corpus_probe [--width N] [--height N] [--out FILE]\n"
                 "  Runs the issue #36 stage F0 capability probe; --out writes the matrix JSON.\n"
                 "  The readable summary is always printed.\n";
}

}  // namespace

int main(int argc, char** argv) {
    int width = kDefaultSize;
    int height = kDefaultSize;
    std::filesystem::path out_path;
    std::string only_family;
    std::string only_source;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--width" && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (arg == "--height" && i + 1 < argc) height = std::atoi(argv[++i]);
        else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (arg == "--family" && i + 1 < argc) only_family = argv[++i];
        else if (arg == "--source" && i + 1 < argc) only_source = argv[++i];
        else { Usage(); return 2; }
    }
    if (width <= 0 || height <= 0) { Usage(); return 2; }

    std::vector<std::string> declared_input_formats;
    {
        CFHD_EncoderRef encoder = nullptr;
        const CFHD_Error open_error = CFHD_OpenEncoder(&encoder, nullptr);
        EncoderOwner owner(encoder, &CFHD_CloseEncoder);
        if (open_error == CFHD_ERROR_OKAY) {
            CFHD_PixelFormat declared[kMaxFormats] = {};
            int count = 0;
            if (CFHD_GetInputFormats(encoder, declared, kMaxFormats, &count) == CFHD_ERROR_OKAY) {
                for (int i = 0; i < count && i < kMaxFormats; ++i) {
                    declared_input_formats.push_back(FormatLabel(static_cast<uint32_t>(declared[i])));
                }
            }
        }
    }

    std::vector<CaseResult> cases;
    // Reused across cases so the 64 MiB buffers are not reallocated ~200 times.
    ByteBuffer output(kMaxOutputBytes, 0);
    ByteBuffer sample_buffer(kMaxOutputBytes, 0);
    std::string current_family;
    const std::vector<Family> families = BuildFamilies();
    for (const Family& family : families) {
        if (!only_family.empty() && only_family != family.name) continue;
        for (CFHD_PixelFormat source_format : family.sources) {
            const std::string source_label = FormatLabel(static_cast<uint32_t>(source_format));
            if (!only_source.empty() && only_source != source_label) continue;
            std::string source_note;
            const SourceGeometry geometry = ProbeSourceGeometry(width, height, source_format);
            const uint32_t source_size = geometry.bytes;
            int32_t source_pitch = geometry.pitch > 0 ? geometry.pitch : static_cast<int32_t>(width) * 4;
            source_note = geometry.note;
            ByteBuffer source;
            if (source_size > 0) {
                const size_t rows = static_cast<size_t>(source_pitch) * static_cast<size_t>(height);
                source.assign(rows > source_size ? rows : source_size, 0);
                FillSource(source, width, height);
            }
            const uint64_t source_hash = source.empty() ? 0 : Fnv1a64(source.data(), source.size());
            for (const QualityEntry& quality : kQualities) {
                CaseResult item;
                item.family = family.name;
                item.family_code = static_cast<int>(family.value);
                item.source_format = FormatLabel(static_cast<uint32_t>(source_format));
                item.source_format_code = static_cast<uint32_t>(source_format);
                item.quality = quality.name;
                item.quality_code = static_cast<int>(quality.value);
                item.source_bytes = source_size;
                item.source_note = source_size == 0 ? source_note : std::string();
                item.source_hash = source_hash;
                item.geometry_rejected = geometry.rejected;
                if (source_size > 0) {
                    item.encode.width = width;
                    item.encode.height = height;
                    item.encode.pitch = source_pitch;
                    item.encode.source_format = source_format;
                    item.encode.encoded_format = family.value;
                    item.encode.quality = quality.value;
                    item.encode.source = source.data();
                    item.encode.sample_out = sample_buffer.data();
                    item.encode.sample_capacity = sample_buffer.size();
                    try {
                        RunEncode(&item.encode);
                    } catch (CFHD_Error thrown) {
                        item.encode.thrown = true;
                        item.encode.thrown_error = static_cast<int>(thrown);
                    } catch (...) {
                        item.encode.thrown = true;
                        item.encode.thrown_error = -1;
                    }
                    if (item.encode.copied_sample_size > 0) {
                        item.sample.assign(sample_buffer.data(),
                                           sample_buffer.data() + item.encode.copied_sample_size);
                    }
                    if (!item.sample.empty()) {
                        item.decode.sample = item.sample.data();
                        item.decode.sample_size = item.sample.size();
                        item.decode.output = output.data();
                        item.decode.output_capacity = output.size();
                        try {
                            RunDecode(&item.decode);
                        } catch (CFHD_Error thrown) {
                            item.decode.thrown = true;
                            item.decode.thrown_error = static_cast<int>(thrown);
                        } catch (...) {
                            item.decode.thrown = true;
                            item.decode.thrown_error = -1;
                        }
                        if (item.decode.decoded) {
                            const size_t used = static_cast<size_t>(item.decode.pitch) *
                                                static_cast<size_t>(item.decode.height);
                            item.output_hash = Fnv1a64(output.data(), used);
                        }
                    }
                }
                item.verdict = VerdictFor(item);
                PrintCase(item, &current_family);
                cases.push_back(std::move(item));
            }
        }
    }

    Summary summary;
    for (const CaseResult& item : cases) Tally(&summary, item);

    std::cout << "F0 capability probe " << width << 'x' << height << ", source seed 0x" << std::hex << kSourceSeed
              << std::dec << "\n";
    std::cout << "declared encoder input formats:";
    for (const std::string& name : declared_input_formats) std::cout << ' ' << name;
    std::cout << "\n\n" << std::endl;

    std::cout << "\ntotal=" << summary.total << " bidirectional_pass=" << summary.bidirectional_pass
              << " sdk_unsupported=" << summary.sdk_unsupported << " encode_failed=" << summary.encode_failed
              << " decode_unsupported=" << summary.decode_unsupported
              << " decode_failed=" << summary.decode_failed << " not_tested=" << summary.not_tested << '\n';

    if (!out_path.empty()) {
        std::error_code ec;
        if (!out_path.parent_path().empty()) std::filesystem::create_directories(out_path.parent_path(), ec);
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "cannot write " << out_path.string() << '\n';
            return 1;
        }
        out << "{\n\"schema\":\"hypercine.format_capability_probe/1\",\n\"issue\":36,\n\"stage\":\"F0\",\n"
            << "\"geometry\":{\"width\":" << width << ",\"height\":" << height << "},\n"
            << "\"source_pattern\":\"deterministic per-pixel byte ramp over the first width*height bytes\",\n"
            << "\"source_seed\":\"0x1337BEEF\",\n"
            << "\"determinism\":{\"decoded_output_hashes_reproduce\":true,"
            << "\"raw_sample_hashes_reproduce\":false,"
            << "\"reason\":\"the pristine encoder stamps wall-clock date/time metadata into every "
               "sample, so the raw packet hash differs between runs; stage F1 normalizes the "
               "GUID/DATE/TIME/TIMC value bytes\"},\n"
            << "\"sdk\":{\"frozen_upstream_commit\":\"" << kFrozenUpstreamCommit << "\"},\n"
            << "\"host\":{\"os\":\"" << HostOs() << "\",\"compiler\":\"" << Escape(CompilerLabel()) << "\"},\n"
            << "\"declared_encoder_input_formats\":[";
        for (size_t i = 0; i < declared_input_formats.size(); ++i) {
            if (i) out << ',';
            out << '"' << Escape(declared_input_formats[i]) << '"';
        }
        out << "],\n\"cases\":[";
        for (size_t i = 0; i < cases.size(); ++i) {
            if (i) out << ',';
            out << JsonCase(cases[i]);
        }
        out << "],\n\"summary\":{\"total\":" << summary.total
            << ",\"bidirectional_pass\":" << summary.bidirectional_pass
            << ",\"sdk_unsupported\":" << summary.sdk_unsupported
            << ",\"encode_failed\":" << summary.encode_failed
            << ",\"decode_unsupported\":" << summary.decode_unsupported
            << ",\"decode_failed\":" << summary.decode_failed
            << ",\"not_tested\":" << summary.not_tested << "}\n}\n";
        std::cout << "wrote " << out_path.string() << '\n';
    }
    return 0;
}
