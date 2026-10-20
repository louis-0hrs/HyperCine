// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
}

#include "VideoBuffers.h"
#include "SampleMetadata.h"

#define private public
#define protected public
#include "SampleEncoder.h"
#undef private
#undef protected

#include "hypercine/canonical.h"
#include "hypercine/entropy_parallel.h"
#include "hypercine/phase2a.h"
#include "hypercine/phase2c.h"

#include "bitstream.h"
#include "decoder.h"
#include "diagnostic_decode_guard.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>

namespace {

struct DecodeResult {
    bool opened = false;
    CFHD_Error open_error = CFHD_ERROR_OKAY;
    CFHD_Error prepare_error = CFHD_ERROR_OKAY;
    CFHD_Error decode_error = CFHD_ERROR_OKAY;
    int decoded_width = 0;
    int decoded_height = 0;
    int pitch = 0;
    CFHD_PixelFormat format = CFHD_PIXEL_FORMAT_UNKNOWN;
    bool geometry_rejected = false;
};

bool Decode(const std::vector<uint8_t>& sample, int width, int height, bool strict_diagnostic_geometry,
            std::vector<uint8_t>* image, DecodeResult* result) {
    CFHD_DecoderRef decoder = nullptr;
    result->open_error = CFHD_OpenDecoder(&decoder, nullptr);
    if (result->open_error != CFHD_ERROR_OKAY) return false;
    result->opened = true;
    int aw = 0, ah = 0;
    CFHD_PixelFormat fmt = CFHD_PIXEL_FORMAT_UNKNOWN;
    result->prepare_error = CFHD_PrepareToDecode(decoder, 0, 0, CFHD_PIXEL_FORMAT_BGRA, CFHD_DECODED_RESOLUTION_FULL,
                                                 CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(),
                                                 &aw, &ah, &fmt);
    if (result->prepare_error != CFHD_ERROR_OKAY) {
        CFHD_CloseDecoder(decoder);
        return false;
    }
    result->decoded_width = aw;
    result->decoded_height = ah;
    result->format = fmt;
    int32_t pitch = 0;
    const CFHD_Error pitch_error =
        CFHD_GetImagePitch(static_cast<uint32_t>(aw > 0 ? aw : width), fmt, &pitch);
    result->pitch = pitch;
    if (strict_diagnostic_geometry &&
        !hypercine::test::IsAcceptedDiagnosticDecodeGeometry(aw, ah, fmt, pitch_error, pitch)) {
        // Reject an unexpected SDK prepare result before allocating storage or
        // entering the decoder. The exact 1920x1080 BGRA extent is bounded, so
        // no unchecked pitch x height multiplication reaches the allocator.
        result->geometry_rejected = true;
        CFHD_CloseDecoder(decoder);
        return false;
    }
    if (pitch_error != CFHD_ERROR_OKAY || pitch <= 0) pitch = width * 4;
    result->pitch = pitch;
    image->assign(static_cast<size_t>(pitch) * static_cast<size_t>(ah > 0 ? ah : height), 0);
    CFHD_MetadataRef metadata = nullptr;
    CFHD_OpenMetadata(&metadata);
    if (metadata) {
        CFHD_InitSampleMetadata(metadata, METADATATYPE_ORIGINAL, const_cast<uint8_t*>(sample.data()),
                                static_cast<uint32_t>(sample.size()));
        unsigned int processflags = PROCESSING_ALL_ON;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_PROCESS_PATH, METADATATYPE_UINT32, &processflags, sizeof(processflags));
    }
    result->decode_error = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(),
                                             image->data(), pitch);
    if (metadata) CFHD_CloseMetadata(metadata);
    CFHD_CloseDecoder(decoder);
    return result->decode_error == CFHD_ERROR_OKAY;
}

struct SampleHeaderInfo {
    bool parsed = false;
    int error = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_number = 0;
    uint32_t encode_quality = 0;
    uint32_t encoder_version = 0;
    int encoded_format = 0;
    int input_format = 0;
    int progressive = 0;
    int interlaced_flags = 0;
    size_t parsed_bytes = 0;
};

SampleHeaderInfo ParseSample(const std::vector<uint8_t>& packet) {
    SampleHeaderInfo info;
    BITSTREAM stream;
    std::memset(&stream, 0, sizeof(stream));
    InitBitstreamBuffer(&stream, const_cast<uint8_t*>(packet.data()), packet.size(), BITSTREAM_ACCESS_READ);
    SAMPLE_HEADER header;
    std::memset(&header, 0, sizeof(header));
    header.find_lowpass_bands = 1;
    info.parsed = ParseSampleHeader(&stream, &header);
    info.error = header.error;
    info.width = header.width;
    info.height = header.height;
    info.frame_number = header.frame_number;
    info.encode_quality = header.encode_quality;
    info.encoder_version = header.encoder_version;
    info.encoded_format = static_cast<int>(header.encoded_format);
    info.input_format = static_cast<int>(header.input_format);
    info.progressive = header.hdr_progressive;
    info.interlaced_flags = header.interlaced_flags;
    const uint8_t* position = GetBitstreamPosition(&stream);
    if (position) info.parsed_bytes = static_cast<size_t>(position - packet.data());
    return info;
}

void PrintSampleHeader(const char* label, const std::vector<uint8_t>& packet) {
    const SampleHeaderInfo info = ParseSample(packet);
    std::cout << label << " parsed=" << (info.parsed ? 1 : 0)
              << " error=" << info.error
              << " width=" << info.width << " height=" << info.height
              << " frame=" << info.frame_number
              << " quality=" << info.encode_quality
              << " encoder_version=0x" << std::hex << info.encoder_version << std::dec
              << " encoded_format=" << info.encoded_format
              << " input_format=" << info.input_format
              << " progressive=" << info.progressive
              << " interlaced_flags=" << info.interlaced_flags
              << " parsed_bytes=" << info.parsed_bytes << "\n";
}

void PrintPacket(const char* label, const std::vector<uint8_t>& packet) {
    std::cout << label << " size=" << packet.size()
              << " sha256=" << hypercine::Sha256Hex(packet.data(), packet.size()) << "\n";
}

void PrintContext(const std::vector<uint8_t>& packet, size_t offset) {
    const size_t begin = offset > 16 ? offset - 16 : 0;
    const size_t end = std::min(packet.size(), offset + 17);
    std::cout << "  context[" << begin << ".." << end << ")=";
    for (size_t i = begin; i < end; ++i) {
        if (i == offset) std::cout << "[";
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(packet[i]);
        if (i == offset) std::cout << "]";
    }
    std::cout << std::dec << std::setfill(' ') << "\n";
}

void PrintPacketDiffs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t diffs = 0;
    const size_t common = std::min(a.size(), b.size());
    std::cout << "packet size delta=" << (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size()) << "\n";
    for (size_t i = 0; i < common; ++i) {
        if (a[i] == b[i]) continue;
        if (diffs < 32) {
            std::cout << "packet diff offset=" << i
                      << " host=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(a[i])
                      << " device=0x" << std::setw(2) << static_cast<int>(b[i])
                      << std::dec << std::setfill(' ') << "\n";
            if (diffs < 8) {
                PrintContext(a, i);
                PrintContext(b, i);
            }
        }
        ++diffs;
    }
    std::cout << "packet differing bytes=" << diffs << "\n";
}

void PrintDecode(const char* label, const DecodeResult& result, const std::vector<uint8_t>& image) {
    std::cout << label << " open=" << static_cast<int>(result.open_error)
              << " prepare=" << static_cast<int>(result.prepare_error)
              << " decode=" << static_cast<int>(result.decode_error)
              << " width=" << result.decoded_width << " height=" << result.decoded_height
              << " pitch=" << result.pitch << " format=" << static_cast<int>(result.format)
              << " bytes=" << image.size()
              << " sha256=" << hypercine::Sha256Hex(image.data(), image.size()) << "\n";
}

void PrintPixelDiffs(const std::vector<uint8_t>& a, int a_pitch,
                     const std::vector<uint8_t>& b, int b_pitch, int width, int height) {
    if (width <= 0 || height <= 0 || a_pitch <= 0 || b_pitch <= 0 ||
        static_cast<size_t>(a_pitch) < static_cast<size_t>(width) * 4u ||
        static_cast<size_t>(b_pitch) < static_cast<size_t>(width) * 4u) {
        std::cout << "pixel comparison unavailable (invalid BGRA geometry/pitch)\n";
        return;
    }
    const size_t rows = static_cast<size_t>(height);
    if (a.size() < rows * static_cast<size_t>(a_pitch) || b.size() < rows * static_cast<size_t>(b_pitch)) {
        std::cout << "pixel comparison unavailable (image smaller than height x pitch)\n";
        return;
    }
    size_t differing_bytes = 0;
    size_t differing_pixels = 0;
    bool first = true;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool pixel_diff = false;
            for (int c = 0; c < 4; ++c) {
                const size_t off_a = static_cast<size_t>(y) * static_cast<size_t>(a_pitch) + static_cast<size_t>(x) * 4u + static_cast<size_t>(c);
                const size_t off_b = static_cast<size_t>(y) * static_cast<size_t>(b_pitch) + static_cast<size_t>(x) * 4u + static_cast<size_t>(c);
                if (a[off_a] == b[off_b]) continue;
                ++differing_bytes;
                pixel_diff = true;
                if (first) {
                    std::cout << "first differing pixel x=" << x << " y=" << y << " channel=" << c
                              << " host=" << static_cast<int>(a[off_a])
                              << " device=" << static_cast<int>(b[off_b]) << "\n";
                    first = false;
                }
            }
            if (pixel_diff) ++differing_pixels;
        }
    }
    std::cout << "image differing pixels=" << differing_pixels << " bytes=" << differing_bytes << "\n";
}

double Psnr(const std::vector<uint8_t>& src, const std::vector<uint8_t>& dec, int width, int height, int pitch) {
    double best = 0;
    for (int flip = 0; flip < 2; ++flip) {
        double sse = 0;
        int count = 0;
        for (int y = 0; y < height; ++y) {
            const int ry = flip ? (height - 1 - y) : y;
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int a = src[(static_cast<size_t>(y) * width + x) * 4 + c];
                    const int b = dec[static_cast<size_t>(ry) * pitch + static_cast<size_t>(x) * 4 + c];
                    const double d = static_cast<double>(a - b);
                    sse += d * d;
                    ++count;
                }
            }
        }
        const double score = sse <= 0 ? 99.0 : 10.0 * std::log10((255.0 * 255.0) / (sse / count));
        if (score > best) best = score;
    }
    return best;
}

bool Expect(const hypercine::Phase2Run& run, const char* name) {
    if (!run.success || !run.byte_match || run.coeff_mismatches != 0 || run.expand_mismatches != 0 || run.bitstream.empty()) {
        std::cerr << name << " failed: " << run.error
                  << " byte_match=" << run.byte_match
                  << " coeff=" << run.coeff_mismatches << "/" << run.coeff_compared
                  << " expand=" << run.expand_mismatches << "\n";
        return false;
    }
    std::cout << name << " ok fps=" << run.sustained_fps
              << " latency=" << run.mean_latency_ms
              << " d2h_bytes=" << run.d2h_bytes
              << " int8_bands=" << (run.frame_times.empty() ? 0 : run.frame_times.back().int8_bands) << "\n";
    return true;
}

bool WritePacket(const std::string& directory, const char* name, const std::vector<uint8_t>& packet) {
    const std::string path = directory + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
    return out.good();
}

bool ReadPacket(const char* path, std::vector<uint8_t>* packet) {
    // This is a 1080p synthetic-sample diagnostic, not a general-purpose
    // decoder for untrusted external packet sizes.
    constexpr std::streamoff max_capture_bytes = 2 * 1024 * 1024;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streampos end = in.tellg();
    if (end <= std::streampos(0) || end > std::streampos(max_capture_bytes))
        return false;
    const auto bytes = static_cast<size_t>(static_cast<std::streamoff>(end));
    packet->resize(bytes);
    in.seekg(0, std::ios::beg);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(packet->data()),
                                     static_cast<std::streamsize>(bytes)));
}

int DecodePacketFile(const char* path) {
    std::vector<uint8_t> packet;
    if (!ReadPacket(path, &packet)) {
        std::cerr << "cannot read packet file\n";
        return 1;
    }
    PrintPacket("packet", packet);
    PrintSampleHeader("packet header", packet);
    const SampleHeaderInfo header = ParseSample(packet);
    if (!header.parsed || header.error || header.width != 1920 || header.height != 1080) {
        std::cerr << "packet is not the expected 1920x1080 synthetic fixture\n";
        return 1;
    }
    std::vector<uint8_t> image;
    DecodeResult result;
    const bool ok = Decode(packet, 0, 0, true, &image, &result);
    PrintDecode("packet decode", result, image);
    if (!ok || result.decoded_width != 1920 || result.decoded_height != 1080 ||
        result.pitch < 1920 * 4 ||
        image.size() < static_cast<size_t>(result.pitch) * 1080u) {
        if (result.geometry_rejected) {
            std::cerr << "SDK prepare geometry rejected before allocation/decode\n";
        } else {
            std::cerr << "packet decode failed or returned invalid BGRA geometry\n";
        }
        return 1;
    }
    if (ok) {
        const int w = result.decoded_width;
        const int h = result.decoded_height;
        const auto frame = hypercine::MakeFrame(w, h, hypercine::CanonicalPattern::Canonical);
        std::cout << "packet decode psnr " << Psnr(frame, image, w, h, result.pitch) << " dB\n";
        bool first = true;
        size_t differing_pixels = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                bool pixel_diff = false;
                for (int c = 0; c < 4; ++c) {
                    const size_t source = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u + static_cast<size_t>(c);
                    const size_t decoded = static_cast<size_t>(y) * static_cast<size_t>(result.pitch) + static_cast<size_t>(x) * 4u + static_cast<size_t>(c);
                    if (frame[source] == image[decoded]) continue;
                    pixel_diff = true;
                    if (first) {
                        std::cout << "first source difference x=" << x << " y=" << y << " channel=" << c
                                  << " source=" << static_cast<int>(frame[source])
                                  << " decoded=" << static_cast<int>(image[decoded]) << "\n";
                        first = false;
                    }
                }
                if (pixel_diff) ++differing_pixels;
            }
        }
        std::cout << "source differing pixels=" << differing_pixels << "\n";
    }
    return ok ? 0 : 1;
}

int RunGeometryGuardSelfTest() {
    struct Case {
        const char* name;
        int width;
        int height;
        CFHD_PixelFormat format;
        CFHD_Error pitch_error;
        int32_t pitch;
        bool accepted;
    };
    const Case cases[] = {
        {"accepted_1920x1080_bgra", 1920, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 7680, true},
        {"reject_width_1921", 1921, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 7680, false},
        {"reject_width_0", 0, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 7680, false},
        {"reject_height_1081", 1920, 1081, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 7680, false},
        {"reject_height_0", 1920, 0, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 7680, false},
        {"reject_yuy2_format", 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, CFHD_ERROR_OKAY, 7680, false},
        {"reject_unknown_format", 1920, 1080, CFHD_PIXEL_FORMAT_UNKNOWN, CFHD_ERROR_OKAY, 7680, false},
        {"reject_pitch_error", 1920, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_INVALID_ARGUMENT, 7680, false},
        {"reject_pitch_short", 1920, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 7679, false},
        {"reject_pitch_zero", 1920, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 0, false},
        {"reject_pitch_negative", 1920, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, -7680, false},
        {"reject_pitch_unpadded_extra", 1920, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ERROR_OKAY, 7696, false},
    };
    int failures = 0;
    for (const Case& test : cases) {
        const bool accepted = hypercine::test::IsAcceptedDiagnosticDecodeGeometry(
            test.width, test.height, test.format, test.pitch_error, test.pitch);
        std::cout << "geometry guard case=" << test.name
                  << " accepted=" << (accepted ? 1 : 0)
                  << " expected=" << (test.accepted ? 1 : 0) << "\n";
        if (accepted != test.accepted) ++failures;
    }
    std::cout << "geometry guard cases=" << (sizeof(cases) / sizeof(cases[0]))
              << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--decode") return DecodePacketFile(argv[2]);
    if (argc == 2 && std::string(argv[1]) == "--self-test-geometry-guard") return RunGeometryGuardSelfTest();

    const int width = 1920;
    const int height = 1080;
    const int pitch = width * 4;
    auto frame = hypercine::MakeFrame(width, height, hypercine::CanonicalPattern::Canonical);
    CFHD_EncoderRef encoder_ref = nullptr;
    if (CFHD_OpenEncoder(&encoder_ref, nullptr) != CFHD_ERROR_OKAY) return 1;
    if (CFHD_PrepareToEncode(encoder_ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) return 1;
    if (CFHD_EncodeSample(encoder_ref, frame.data(), pitch) != CFHD_ERROR_OKAY) return 1;
    CSampleEncoder* sample = reinterpret_cast<CSampleEncoder*>(encoder_ref);
    auto* enc = reinterpret_cast<encoder*>(sample->m_encoder);
    auto* transforms = reinterpret_cast<transform**>(sample->m_transformArray);

    if (!hypercine::RefreshUpstreamPyramid(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                           sample->m_scratchBuffer, sample->m_scratchBufferSize)) {
        std::cerr << "pyramid refresh failed\n";
        return 1;
    }
    const auto refs = hypercine::EnumerateBands(transforms, sample->m_channelCount);
    ENCODER* raw = sample->m_encoder;
    TRANSFORM** raw_tf = sample->m_transformArray;
    std::vector<std::vector<uint8_t>> storage(refs.size());
    std::vector<hypercine::EncodedPiece> pieces(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto& ref = refs[i];
        IMAGE* wavelet = nullptr;
        for (int k = 0; k < raw_tf[ref.channel]->num_wavelets; ++k) {
            if (raw_tf[ref.channel]->wavelet[k] && raw_tf[ref.channel]->wavelet[k]->level == ref.level) {
                wavelet = raw_tf[ref.channel]->wavelet[k];
            }
        }
        if (!wavelet) return 1;
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        const PIXEL* src = wavelet->band[ref.band];
        int vmin = src[0];
        int vmax = src[0];
        for (int y = 0; y < ref.height; ++y) {
            const PIXEL* row = src + static_cast<size_t>(y) * stride;
            for (int x = 0; x < ref.width; ++x) {
                vmin = std::min(vmin, static_cast<int>(row[x]));
                vmax = std::max(vmax, static_cast<int>(row[x]));
            }
        }
        hypercine::ExternalCoeff coeff;
        coeff.channel = ref.channel;
        coeff.level = ref.level;
        coeff.band = ref.band;
        coeff.subband = ref.subband;
        coeff.lowpass = ref.lowpass;
        coeff.width = ref.width;
        coeff.height = ref.height;
        coeff.value_min = vmin;
        coeff.value_max = vmax;
        const bool as_int8 = !ref.lowpass && vmin >= -128 && vmax <= 127;
        if (as_int8) {
            storage[i].resize(static_cast<size_t>(ref.width) * ref.height);
            for (int y = 0; y < ref.height; ++y) {
                const PIXEL* row = src + static_cast<size_t>(y) * stride;
                for (int x = 0; x < ref.width; ++x) {
                    const int value = row[x];
                    if (value < -128 || value > 127) {
                        std::cerr << "int8 pack would saturate\n";
                        return 1;
                    }
                    storage[i][static_cast<size_t>(y) * ref.width + x] = static_cast<uint8_t>(static_cast<int8_t>(value));
                }
            }
            coeff.elem_bytes = 1;
        } else {
            storage[i].resize(static_cast<size_t>(ref.width) * ref.height * 2u);
            auto* dst = reinterpret_cast<int16_t*>(storage[i].data());
            for (int y = 0; y < ref.height; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * ref.width,
                            src + static_cast<size_t>(y) * stride,
                            static_cast<size_t>(ref.width) * 2u);
            }
            coeff.elem_bytes = 2;
        }
        coeff.data = storage[i].data();
        if (!hypercine::EncodeOneExternalBand(enc, transforms, coeff, &pieces[i])) {
            std::cerr << "external band encode failed channel " << ref.channel << " level " << ref.level << "\n";
            return 1;
        }
    }
    auto entropy = hypercine::AssembleEncodedPieces(enc, transforms, sample->m_channelCount, pieces, true);
    if (!entropy.success || !entropy.byte_match) {
        std::cerr << "external entropy mismatch at " << entropy.mismatch_offset << " " << entropy.error << "\n";
        return 1;
    }
    std::cout << "external int8/int16 entropy matches EncodeQuantizedGroup\n";

    hypercine::Phase2Config cfg;
    cfg.frames = 1;
    cfg.warmup = 1;
    cfg.workers = 8;
    cfg.pipeline_depth = 1;
    cfg.verify = true;
    cfg.input = hypercine::Phase2Input::HostBgra;
    cfg.transport = hypercine::Phase2Transport::FullInt16;
    auto full = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                     sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(full, "host full int16")) return 1;

    cfg.transport = hypercine::Phase2Transport::Int16Stream;
    cfg.pipeline_depth = 2;
    auto streamed = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                         sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(streamed, "host int16 stream")) return 1;

    cfg.transport = hypercine::Phase2Transport::CompactStream;
    auto compact = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                        sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(compact, "host compact stream")) return 1;
    if (!compact.frame_times.empty() && compact.frame_times.back().int8_bands <= 0) {
        std::cerr << "canonical frame produced no int8 bands\n";
        return 1;
    }

    cfg.input = hypercine::Phase2Input::DeviceBgra;
    auto resident = hypercine::RunPhase2(enc, transforms, sample->m_channelCount, frame.data(), pitch,
                                         sample->m_scratchBuffer, sample->m_scratchBufferSize, cfg);
    if (!Expect(resident, "device compact stream")) return 1;
    if (resident.h2d_bytes != 0) {
        std::cerr << "device-resident path copied " << resident.h2d_bytes << " bytes to the device\n";
        return 1;
    }
    if (resident.bitstream.size() != compact.bitstream.size()) {
        std::cerr << "device-resident sample size " << resident.bitstream.size()
                  << " differs from host compact " << compact.bitstream.size() << "\n";
        return 1;
    }
    size_t bitstream_diffs = 0;
    for (size_t i = 0; i < resident.bitstream.size(); ++i) {
        if (resident.bitstream[i] != compact.bitstream[i]) ++bitstream_diffs;
    }
    PrintPacket("host compact packet", compact.bitstream);
    PrintSampleHeader("host compact header", compact.bitstream);
    PrintPacket("device compact packet", resident.bitstream);
    PrintSampleHeader("device compact header", resident.bitstream);
    PrintPacketDiffs(compact.bitstream, resident.bitstream);
    if (const char* capture = std::getenv("HYPERCINE_PHASE2A_CAPTURE_DIR")) {
        // Atomically reserve a *new* capture directory. A second invocation
        // must fail rather than truncating an immutable prior packet pair.
        std::error_code create_error;
        if (!std::filesystem::create_directory(capture, create_error)) {
            std::cerr << "capture directory must be new and writable\n";
            return 1;
        }
        if (WritePacket(capture, "phase2a_host.cfhd", compact.bitstream) &&
            WritePacket(capture, "phase2a_device.cfhd", resident.bitstream)) {
            std::cout << "captured immutable packets under HYPERCINE_PHASE2A_CAPTURE_DIR\n";
        } else {
            std::cerr << "packet capture failed\n";
            return 1;
        }
    }

    std::vector<uint8_t> host_image, device_image;
    DecodeResult host_decode, device_decode;
    const bool host_ok = Decode(compact.bitstream, width, height, false, &host_image, &host_decode);
    const bool device_ok = Decode(resident.bitstream, width, height, false, &device_image, &device_decode);
    PrintDecode("host compact decode", host_decode, host_image);
    PrintDecode("device compact decode", device_decode, device_image);
    if (host_ok && device_ok) {
        PrintPixelDiffs(host_image, host_decode.pitch, device_image, device_decode.pitch, width, height);
    }
    if (!host_ok || !device_ok) {
        std::cerr << "device-resident decode differs from host compact (" << bitstream_diffs
                  << " bitstream bytes):"
                  << " host_decode=" << (host_ok ? "ok" : "failed")
                  << " device_decode=" << (device_ok ? "ok" : "failed") << "\n";
        return 1;
    }
    if (host_decode.pitch != device_decode.pitch) {
        std::cerr << "device-resident decode differs from host compact (" << bitstream_diffs
                  << " bitstream bytes): pitch " << host_decode.pitch
                  << " vs " << device_decode.pitch << "\n";
        return 1;
    }
    if (host_image != device_image) {
        std::cerr << "device-resident decode differs from host compact (" << bitstream_diffs
                  << " bitstream bytes): decoded pixels differ\n";
        return 1;
    }
    std::cout << "device and host compact decode identically (" << bitstream_diffs
              << " bitstream bytes differ, sample size " << compact.bitstream.size() << ")\n";

    std::vector<uint8_t> image;
    DecodeResult compact_decode;
    if (!Decode(compact.bitstream, width, height, false, &image, &compact_decode)) {
        PrintDecode("host compact decode repeat", compact_decode, image);
        std::cerr << "upstream decoder rejected compact bitstream\n";
        return 1;
    }
    PrintDecode("host compact decode repeat", compact_decode, image);
    const double psnr = Psnr(frame, image, width, height, compact_decode.pitch);
    std::cout << "decoded psnr " << psnr << " dB\n";
    if (psnr < 35.0) {
        std::cerr << "psnr below 35 dB\n";
        return 1;
    }
    CFHD_CloseEncoder(encoder_ref);
    std::cout << "phase 2a tests passed\n";
    return 0;
}
