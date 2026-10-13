// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Research probe for Phase 3B-0. It drives the unchanged upstream encoder,
// walks tags without the production 30-band gate, and decodes with the
// upstream reference. It does not enable a new production format.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "decode_phase1_support.h"
#include "hypercine/decoder.h"
#include "hypercine/research/decode_phase2e.h"
#include "hypercine/research/decoder_diagnostics.h"
#include "../src/core/decoder_backend.h"
#include "mp4reader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace {

using HyperCineTest::AlignedVector;
using HyperCineTest::ComputeFNV1a64;
constexpr unsigned kSeed = 0x1337BEEF;
// codec.c keeps this marker private. Lowpass subbands begin with it.
constexpr unsigned kCoefficientStartCode = 0x0F0F;

uint16_t BE16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t LE32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
std::string Hex64(uint64_t v) {
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << v;
    return s.str();
}
std::string HashBytes(const void* p, size_t n) { return Hex64(ComputeFNV1a64(p, n)); }
std::string Q(std::string s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(char(c)); }
        else if (c == '\n') o += "\\n";
        else if (c < 32) o.push_back(' ');
        else o.push_back(char(c));
    }
    o.push_back('"');
    return o;
}
const char* EncodedName(int v) {
    switch (v) {
    case ENCODED_FORMAT_YUV_422: return "YUV422";
    case ENCODED_FORMAT_BAYER: return "BAYER";
    case ENCODED_FORMAT_RGB_444: return "RGB444";
    case ENCODED_FORMAT_RGBA_4444: return "RGBA4444";
    case ENCODED_FORMAT_YUVA_4444: return "YUVA4444";
    default: return "UNKNOWN";
    }
}
const char* TransformName(int v) {
    switch (v) {
    case TRANSFORM_TYPE_SPATIAL: return "spatial";
    case TRANSFORM_TYPE_FIELD: return "field";
    case TRANSFORM_TYPE_FIELDPLUS: return "fieldplus";
    case TRANSFORM_TYPE_FRAME: return "frame";
    case TRANSFORM_TYPE_INTERLACED: return "interlaced";
    default: return "other";
    }
}
const char* FrameFormatName(int v) {
    switch (v) {
    case 1: return "gray";
    case 2: return "yuv";
    case 3: return "rgb";
    case 4: return "rgba";
    default: return "other";
    }
}
std::string ErrorName(HyperCine::DecodeError e) {
    using HyperCine::DecodeError;
    switch (e) {
    case DecodeError::None: return "None";
    case DecodeError::Busy: return "Busy";
    case DecodeError::NotInitialized: return "NotInitialized";
    case DecodeError::InvalidConfig: return "InvalidConfig";
    case DecodeError::BackendUnavailable: return "BackendUnavailable";
    case DecodeError::InvalidRequest: return "InvalidRequest";
    case DecodeError::InvalidSample: return "InvalidSample";
    case DecodeError::SampleTooLarge: return "SampleTooLarge";
    case DecodeError::DeviceFailure: return "DeviceFailure";
    case DecodeError::InvalidTicket: return "InvalidTicket";
    case DecodeError::ConsumerLimit: return "ConsumerLimit";
    case DecodeError::UnsupportedEncodingMode: return "UnsupportedEncodingMode";
    }
    return "other";
}
const char* PixelName(CFHD_PixelFormat f) {
    switch (f) {
    case CFHD_PIXEL_FORMAT_YUY2: return "YUY2";
    case CFHD_PIXEL_FORMAT_2VUY: return "2VUY";
    case CFHD_PIXEL_FORMAT_YUYV: return "YUYV";
    case CFHD_PIXEL_FORMAT_V210: return "V210";
    case CFHD_PIXEL_FORMAT_YU64: return "YU64";
    case CFHD_PIXEL_FORMAT_RG24: return "RG24";
    case CFHD_PIXEL_FORMAT_BGRA: return "BGRA";
    case CFHD_PIXEL_FORMAT_BGRa: return "BGRa";
    case CFHD_PIXEL_FORMAT_RG48: return "RG48";
    case CFHD_PIXEL_FORMAT_B64A: return "B64A";
    case CFHD_PIXEL_FORMAT_RG64: return "RG64";
    case CFHD_PIXEL_FORMAT_BYR4: return "BYR4";
    case CFHD_PIXEL_FORMAT_BYR5: return "BYR5";
    case CFHD_PIXEL_FORMAT_NV12: return "NV12";
    case CFHD_PIXEL_FORMAT_YV12: return "YV12";
    case CFHD_PIXEL_FORMAT_R210: return "R210";
    case CFHD_PIXEL_FORMAT_RG30: return "RG30";
    case CFHD_PIXEL_FORMAT_AB10: return "AB10";
    case CFHD_PIXEL_FORMAT_AR10: return "AR10";
    case CFHD_PIXEL_FORMAT_DPX0: return "DPX0";
    case CFHD_PIXEL_FORMAT_CT_UCHAR: return "CT_UCHAR";
    default: return "OTHER";
    }
}
const char* RequestEncodedName(CFHD_EncodedFormat f) {
    switch (f) {
    case CFHD_ENCODED_FORMAT_YUV_422: return "YUV_422";
    case CFHD_ENCODED_FORMAT_RGB_444: return "RGB_444";
    case CFHD_ENCODED_FORMAT_RGBA_4444: return "RGBA_4444";
    case CFHD_ENCODED_FORMAT_BAYER: return "BAYER";
    case CFHD_ENCODED_FORMAT_YUVA_4444: return "YUVA_4444";
    default: return "UNKNOWN";
    }
}
const char* QualityName(int q) {
    if (q == CFHD_ENCODING_QUALITY_LOW) return "low";
    if (q == CFHD_ENCODING_QUALITY_MEDIUM) return "medium";
    if (q == CFHD_ENCODING_QUALITY_HIGH) return "high";
    if (q == CFHD_ENCODING_QUALITY_FILMSCAN1) return "filmscan1";
    if (q == CFHD_ENCODING_QUALITY_FILMSCAN2) return "filmscan2";
    if (q == CFHD_ENCODING_QUALITY_FILMSCAN3) return "filmscan3";
    if (q == int(CFHD_ENCODING_QUALITY_KEYING)) return "keying";
    return "other";
}
const char* PatternName(int p) {
    static const char* names[] = {
        "flat", "gradient", "checker", "edges", "noise", "saturated", "alpha", "chroma_split", "red_only"};
    return p >= 0 && p < 9 ? names[p] : "other";
}
std::string FlagName(uint32_t flags) {
    if (!flags) return "none";
    std::string s;
    auto add = [&](uint32_t bit, const char* name) {
        if (flags & bit) { if (!s.empty()) s += "|"; s += name; }
    };
    add(CFHD_ENCODING_FLAGS_YUV_INTERLACED, "interlaced");
    add(CFHD_ENCODING_FLAGS_YUV_2FRAME_GOP, "gop2");
    add(CFHD_ENCODING_FLAGS_YUV_601, "yuv601");
    add(CFHD_ENCODING_FLAGS_RGB_STUDIO, "rgb_studio");
    if (s.empty()) s = "other";
    return s;
}

struct BandNote {
    int channel = 0, subband = 0, level = -1, band = -1;
    int width = 0, height = 0, quant = 0, encoding = -1, codebook = 0;
    int difference = 0, peak = 0;
    int kind = 0; // 1 lowpass, 2 highpass
    uint32_t payload = 0;
};
struct Walk {
    bool ok = false;
    std::string error;
    int unconsumed = 0;
    int trailer_mismatches = 0;
    int last[128]{};
    int seen[128]{};
    std::vector<BandNote> bands;
    std::string signature;
};

Walk WalkSample(const uint8_t* data, size_t size) {
    Walk w;
    if (!data || size < 32) { w.error = "too small"; return w; }
    int channel = 0, level = -1, band = -1, subband = 0;
    int low_w = 0, low_h = 0, band_w = 0, band_h = 0, quant = 0, encoding = -1;
    int codebook = 0, difference = 0, peak = 0;
    size_t pos = 0;
    auto fail = [&](const char* message) { w.error = message; w.unconsumed = int(size - (std::min)(pos, size)); return w; };
    while (pos + 4 <= size) {
        const int raw = int16_t(BE16(data + pos));
        const unsigned tag = raw < 0 ? unsigned(-raw) : unsigned(raw);
        const unsigned value = BE16(data + pos + 2);
        pos += 4;
        if (tag < 128) { w.last[tag] = int(value); w.seen[tag] += 1; }
        if (tag == CODEC_TAG_INDEX) {
            const size_t n = size_t(value) * 4;
            if (pos + n > size) return fail("index overruns");
            pos += n;
            continue;
        }
        if ((tag & 0xff00) == CODEC_TAG_SUBBAND_SIZE) {
            const size_t chunk = (size_t((tag & 255) << 16 | value)) * 4;
            if (chunk < 4 || pos + chunk > size) return fail("subband overruns");
            const int sub_raw = int16_t(BE16(data + pos));
            const unsigned sub_tag = sub_raw < 0 ? unsigned(-sub_raw) : unsigned(sub_raw);
            const unsigned sub_val = BE16(data + pos + 2);
            BandNote note;
            note.channel = channel;
            note.subband = subband;
            note.level = level;
            note.band = band;
            note.quant = quant;
            note.encoding = encoding;
            note.codebook = codebook;
            note.difference = difference;
            note.peak = peak;
            if (sub_tag == CODEC_TAG_MARKER && sub_val == kCoefficientStartCode) {
                note.kind = 1;
                note.width = low_w;
                note.height = low_h;
                note.payload = uint32_t((std::min)(chunk - 4, size_t(low_w) * size_t(low_h) * 2));
                note.subband = 0;
                note.band = 0;
                subband = 1;
            } else if (sub_tag == CODEC_TAG_BAND_HEADER) {
                note.kind = 2;
                note.width = band_w;
                note.height = band_h;
                note.payload = chunk >= 8 ? uint32_t(chunk - 8) : 0;
                if (chunk >= 8 && pos + 4 + note.payload + 4 <= size) {
                    const uint32_t trailer = LE32(data + pos + 4 + note.payload);
                    if (trailer != 0x00003800u) ++w.trailer_mismatches;
                }
                ++subband;
            } else {
                note.kind = 0;
                note.payload = uint32_t(chunk);
            }
            w.bands.push_back(note);
            pos += chunk;
            codebook = 0;
            difference = 0;
            peak = 0;
            continue;
        }
        if (tag >= CODEC_TAG_CHUNK && tag < CODEC_TAG_CUSTOM_CHUNK24BIT) {
            const size_t n = size_t(value) * 4;
            if (pos + n > size) return fail("chunk overruns");
            pos += n;
            continue;
        }
        switch (tag) {
        case CODEC_TAG_CHANNEL: channel = int(value); subband = 0; break;
        case CODEC_TAG_WAVELET_LEVEL: level = int(value); break;
        case CODEC_TAG_BAND_NUMBER: band = int(value); break;
        case CODEC_TAG_BAND_SUBBAND: subband = int(value); break;
        case CODEC_TAG_LOWPASS_WIDTH: low_w = int(value); break;
        case CODEC_TAG_LOWPASS_HEIGHT: low_h = int(value); break;
        case CODEC_TAG_HIGHPASS_WIDTH:
        case CODEC_TAG_BAND_WIDTH: band_w = int(value); break;
        case CODEC_TAG_HIGHPASS_HEIGHT:
        case CODEC_TAG_BAND_HEIGHT: band_h = int(value); break;
        case CODEC_TAG_BAND_QUANTIZATION: quant = int(value); break;
        case CODEC_TAG_BAND_ENCODING: encoding = int(value); break;
        case CODEC_TAG_BAND_CODING_FLAGS:
            codebook = int(value & 0x0f);
            difference = int((value >> 4) & 1);
            break;
        case CODEC_TAG_PEAK_LEVEL: peak = int(value); break;
        default: break;
        }
    }
    w.ok = pos == size && w.error.empty();
    w.unconsumed = int(size - (std::min)(pos, size));
    std::ostringstream sig;
    for (size_t i = 0; i < w.bands.size(); ++i) {
        const auto& b = w.bands[i];
        if (i) sig << ";";
        sig << b.channel << ":" << b.subband << ":" << b.level << ":" << b.band
            << ":" << b.width << "x" << b.height << ":q" << b.quant
            << ":c" << b.codebook << (b.difference ? ":D" : "") << (b.peak ? ":P" : "");
    }
    w.signature = sig.str();
    return w;
}

int Component(int pattern, int x, int y, int w, int h, int c, uint32_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    const int noise = int((rng >> 24) % 220) + 16;
    const int ramp = w > 1 ? 16 + 219 * x / (w - 1) : 16;
    switch (pattern) {
    case 0: return c == 3 ? 255 : (c == 0 ? 180 : 128);
    case 1: return c == 3 ? ramp : (c == 0 ? ramp : 96 + (c == 1 ? y * 64 / (std::max)(1, h) : x * 64 / (std::max)(1, w)));
    case 2: return ((x / 8 + y / 8) & 1) ? (c == 3 ? 255 : 235) : (c == 3 ? 0 : 16);
    case 3: return x < w / 2 ? (c == 3 ? 255 : 16) : (c == 3 ? 0 : 235);
    case 4: return c == 3 ? noise : noise;
    case 5: return c == 3 ? 255 : ((x / 4) & 1 ? 16 : 235);
    case 6: return c == 3 ? ramp : 128;
    case 7: return c == 0 ? 128 : (c == 1 ? 16 : 235);
    case 8: return c == 0 ? 255 : (c == 3 ? 128 : 0);
    default: return 128;
    }
}

struct Packed { AlignedVector<uint8_t> bytes; int pitch = 0; };
Packed MakeFrame(CFHD_PixelFormat format, int w, int h, int pattern, int frame_index) {
    Packed out;
    uint32_t rng = kSeed + uint32_t(pattern) * 17u + uint32_t(frame_index) * 97u + uint32_t(w) * 3u;
    auto at = [&](int x, int y, int c, int depth) {
        int v = Component(pattern, x, y, w, h, c, rng);
        if (depth == 16) {
            if (pattern == 2 || pattern == 3 || pattern == 5) return v > 128 ? 65535 : 0;
            return v << 8;
        }
        if (depth == 10) return v * 1023 / 255;
        return v;
    };
    // Some upstream converters read a wider pixel than the declared pitch.
    // The extra bytes are never presented as image data.
    auto bytes_of = [&](int n) { out.bytes.assign(size_t(n) + 65536u, 0); };
    if (format == CFHD_PIXEL_FORMAT_YUY2 || format == CFHD_PIXEL_FORMAT_YUYV ||
        format == CFHD_PIXEL_FORMAT_2VUY || format == CFHD_PIXEL_FORMAT_CT_UCHAR) {
        out.pitch = w * 2;
        bytes_of(out.pitch * h);
        const bool uyvy = format != CFHD_PIXEL_FORMAT_YUY2 && format != CFHD_PIXEL_FORMAT_YUYV;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
            uint8_t* p = out.bytes.data() + size_t(y) * out.pitch + x * 2;
            const int Y0 = at(x, y, 0, 8), Y1 = at(x + 1, y, 0, 8), U = at(x, y, 1, 8), V = at(x, y, 2, 8);
            if (uyvy) { p[0] = uint8_t(U); p[1] = uint8_t(Y0); p[2] = uint8_t(V); p[3] = uint8_t(Y1); }
            else { p[0] = uint8_t(Y0); p[1] = uint8_t(U); p[2] = uint8_t(Y1); p[3] = uint8_t(V); }
        }
    } else if (format == CFHD_PIXEL_FORMAT_YU64) {
        out.pitch = w * 4;
        bytes_of(out.pitch * h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
            uint16_t* p = reinterpret_cast<uint16_t*>(out.bytes.data() + size_t(y) * out.pitch + x * 4);
            p[0] = uint16_t(at(x, y, 0, 16));
            p[1] = uint16_t(at(x, y, 2, 16));
            p[2] = uint16_t(at((std::min)(x + 1, w - 1), y, 0, 16));
            p[3] = uint16_t(at(x, y, 1, 16));
        }
    } else if (format == CFHD_PIXEL_FORMAT_V210) {
        const int stride_w = ((w + 47) / 48) * 48;
        out.pitch = stride_w * 8 / 3;
        bytes_of(out.pitch * h);
        for (int y = 0; y < h; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(out.bytes.data() + size_t(y) * out.pitch);
            for (int x = 0; x < stride_w; x += 6) {
                auto yv = [&](int px) { return px < w ? at(px, y, 0, 10) : 512; };
                auto uv = [&](int px, int c) { return px < w ? at(px, y, c, 10) : 512; };
                const int i = x / 6 * 4;
                row[i + 0] = uint32_t(uv(x, 1) | (yv(x) << 10) | (uv(x, 2) << 20));
                row[i + 1] = uint32_t(yv(x + 1) | (uv(x + 2, 1) << 10) | (yv(x + 2) << 20));
                row[i + 2] = uint32_t(uv(x + 2, 2) | (yv(x + 3) << 10) | (uv(x + 4, 1) << 20));
                row[i + 3] = uint32_t(yv(x + 4) | (uv(x + 4, 2) << 10) | (yv(x + 5) << 20));
            }
        }
    } else if (format == CFHD_PIXEL_FORMAT_BGRA || format == CFHD_PIXEL_FORMAT_BGRa) {
        out.pitch = w * 4;
        bytes_of(out.pitch * h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint8_t* p = out.bytes.data() + size_t(y) * out.pitch + x * 4;
            p[0] = uint8_t(at(x, y, 2, 8));
            p[1] = uint8_t(at(x, y, 1, 8));
            p[2] = uint8_t(at(x, y, 0, 8));
            p[3] = uint8_t(at(x, y, 3, 8));
        }
    } else if (format == CFHD_PIXEL_FORMAT_RG24) {
        out.pitch = w * 3;
        bytes_of(out.pitch * h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint8_t* p = out.bytes.data() + size_t(y) * out.pitch + x * 3;
            p[0] = uint8_t(at(x, y, 0, 8));
            p[1] = uint8_t(at(x, y, 1, 8));
            p[2] = uint8_t(at(x, y, 2, 8));
        }
    } else if (format == CFHD_PIXEL_FORMAT_RG48) {
        out.pitch = w * 6;
        bytes_of(out.pitch * h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint16_t* p = reinterpret_cast<uint16_t*>(out.bytes.data() + size_t(y) * out.pitch + x * 6);
            p[0] = uint16_t(at(x, y, 0, 16));
            p[1] = uint16_t(at(x, y, 1, 16));
            p[2] = uint16_t(at(x, y, 2, 16));
        }
    } else if (format == CFHD_PIXEL_FORMAT_B64A) {
        out.pitch = w * 8;
        bytes_of(out.pitch * h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint16_t* p = reinterpret_cast<uint16_t*>(out.bytes.data() + size_t(y) * out.pitch + x * 8);
            p[0] = uint16_t(at(x, y, 3, 16));
            p[1] = uint16_t(at(x, y, 0, 16));
            p[2] = uint16_t(at(x, y, 1, 16));
            p[3] = uint16_t(at(x, y, 2, 16));
        }
    } else if (format == CFHD_PIXEL_FORMAT_NV12) {
        out.pitch = w;
        bytes_of(w * h * 3 / 2);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            out.bytes[size_t(y) * w + x] = uint8_t(at(x, y, 0, 8));
        for (int y = 0; y < h / 2; ++y) for (int x = 0; x < w; x += 2) {
            uint8_t* p = out.bytes.data() + size_t(w * h) + size_t(y) * w + x;
            p[0] = uint8_t(at(x, y * 2, 2, 8));
            p[1] = uint8_t(at(x, y * 2, 1, 8));
        }
    } else if (format == CFHD_PIXEL_FORMAT_YV12) {
        out.pitch = w;
        bytes_of(w * h * 3 / 2);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            out.bytes[size_t(y) * w + x] = uint8_t(at(x, y, 0, 8));
        const size_t chroma = size_t(w / 2) * size_t(h / 2);
        for (int y = 0; y < h / 2; ++y) for (int x = 0; x < w / 2; ++x) {
            out.bytes[size_t(w * h) + size_t(y) * (w / 2) + x] = uint8_t(at(x * 2, y * 2, 1, 8));
            out.bytes[size_t(w * h) + chroma + size_t(y) * (w / 2) + x] = uint8_t(at(x * 2, y * 2, 2, 8));
        }
    } else if (format == CFHD_PIXEL_FORMAT_BYR4 || format == CFHD_PIXEL_FORMAT_BYR5) {
        out.pitch = w * 2;
        bytes_of(out.pitch * h);
        for (int i = 0; i < w * h; ++i) {
            uint32_t local = kSeed + uint32_t(i) + uint32_t(pattern) * 13u;
            reinterpret_cast<uint16_t*>(out.bytes.data())[i] = uint16_t((local * 1664525u + 1013904223u) >> 16);
        }
    } else if (format == CFHD_PIXEL_FORMAT_R210 || format == CFHD_PIXEL_FORMAT_RG30 ||
               format == CFHD_PIXEL_FORMAT_AB10 || format == CFHD_PIXEL_FORMAT_AR10 ||
               format == CFHD_PIXEL_FORMAT_DPX0) {
        out.pitch = w * 4;
        bytes_of(out.pitch * h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const uint32_t r = uint32_t(at(x, y, 0, 10));
            const uint32_t g = uint32_t(at(x, y, 1, 10));
            const uint32_t b = uint32_t(at(x, y, 2, 10));
            uint32_t word = r | (g << 10) | (b << 20);
            if (format == CFHD_PIXEL_FORMAT_R210) word = b | (g << 10) | (r << 20);
            reinterpret_cast<uint32_t*>(out.bytes.data())[size_t(y) * w + x] = word;
        }
    } else {
        out.pitch = w * 4;
        bytes_of(out.pitch * h);
    }
    return out;
}

struct Attempt {
    std::string id;
    int w = 0, h = 0, pattern = 0, frames = 1, quality = 0;
    uint32_t flags = 0;
    CFHD_PixelFormat input = CFHD_PIXEL_FORMAT_UNKNOWN;
    CFHD_EncodedFormat encoded = CFHD_ENCODED_FORMAT_UNKNOWN;
    bool keep = false;
    bool fixture = false;
    std::string note;
    int prepare = -1, encode = -1, sample_bytes = 0;
    std::string source_hash, sample_hash, research_error, production_error, production_status;
    bool research_ok = false, production_ok = false, upstream_ok = false;
    std::string upstream_error, band_sig, diff_bands, peak_bands, wavelet_map, coeff_hash, channel_means;
    std::string output_hash, output_formats, sample_info, fixture_name;
    int band_count = 0, diff_count = 0, peak_count = 0, trailer_mismatches = 0;
    int tag_precision = -1, tag_channels = -1, tag_width = -1, tag_height = -1, tag_frame_format = -1;
    int tag_transform = -1, tag_frames = -1, tag_flags = -1, tag_encoded = -1, tag_spatial = -1;
    int tag_wavelets = -1, tag_subbands = -1, tag_quality_l = -1, tag_quality_h = -1, tag_colorspace = -1;
    int tag_interlace = -1, tag_input_format = -1;
    int codec_precision = -1, codec_channels = -1, codec_progressive = -1, codec_transform = -1;
    int codec_encoded = -1, codec_frames = -1, codec_spatial = -1, codec_subbands = -1;
    int decoded_w = 0, decoded_h = 0, coeff_count = 0, coeff_min = 0, coeff_max = 0;
    int info_channels = -1, info_progressive = -1, info_encoded = -1, info_key = -1;
    std::vector<uint8_t> sample;
    std::string Json() const {
        std::ostringstream o;
        o << "{\"id\":" << Q(id)
          << ",\"request_width\":" << w << ",\"request_height\":" << h
          << ",\"input\":" << Q(PixelName(input))
          << ",\"requested_encoded\":" << Q(RequestEncodedName(encoded))
          << ",\"flags\":" << Q(FlagName(flags)) << ",\"flag_bits\":" << flags
          << ",\"quality\":" << Q(QualityName(quality)) << ",\"pattern\":" << Q(PatternName(pattern))
          << ",\"frames\":" << frames << ",\"note\":" << Q(note)
          << ",\"prepare_error\":" << prepare << ",\"encode_error\":" << encode
          << ",\"sample_bytes\":" << sample_bytes
          << ",\"source_hash\":" << Q(source_hash) << ",\"sample_hash\":" << Q(sample_hash)
          << ",\"research_ok\":" << (research_ok ? "true" : "false")
          << ",\"research_error\":" << Q(research_error)
          << ",\"band_count\":" << band_count << ",\"band_signature\":" << Q(band_sig)
          << ",\"difference_bands\":" << diff_count << ",\"difference_list\":" << Q(diff_bands)
          << ",\"peak_bands\":" << peak_count << ",\"peak_list\":" << Q(peak_bands)
          << ",\"trailer_mismatches\":" << trailer_mismatches
          << ",\"tag_precision\":" << tag_precision << ",\"tag_channels\":" << tag_channels
          << ",\"tag_width\":" << tag_width << ",\"tag_height\":" << tag_height
          << ",\"tag_frame_format\":" << tag_frame_format << ",\"tag_transform\":" << tag_transform
          << ",\"tag_frames\":" << tag_frames << ",\"tag_sample_flags\":" << tag_flags
          << ",\"tag_encoded_format\":" << tag_encoded << ",\"tag_spatial\":" << tag_spatial
          << ",\"tag_wavelets\":" << tag_wavelets << ",\"tag_subbands\":" << tag_subbands
          << ",\"tag_quality_l\":" << tag_quality_l << ",\"tag_quality_h\":" << tag_quality_h
          << ",\"tag_colorspace\":" << tag_colorspace << ",\"tag_interlace_flags\":" << tag_interlace
          << ",\"tag_input_format\":" << tag_input_format
          << ",\"production_ok\":" << (production_ok ? "true" : "false")
          << ",\"production_status\":" << Q(production_status)
          << ",\"production_error\":" << Q(production_error)
          << ",\"upstream_ok\":" << (upstream_ok ? "true" : "false")
          << ",\"upstream_error\":" << Q(upstream_error)
          << ",\"codec_precision\":" << codec_precision << ",\"codec_channels\":" << codec_channels
          << ",\"codec_progressive\":" << codec_progressive << ",\"codec_transform\":" << codec_transform
          << ",\"codec_encoded\":" << codec_encoded << ",\"codec_encoded_name\":" << Q(EncodedName(codec_encoded))
          << ",\"codec_frames\":" << codec_frames << ",\"codec_spatial\":" << codec_spatial
          << ",\"codec_subbands\":" << codec_subbands
          << ",\"decoded_width\":" << decoded_w << ",\"decoded_height\":" << decoded_h
          << ",\"output_hash\":" << Q(output_hash) << ",\"output_formats\":" << Q(output_formats)
          << ",\"sample_info\":" << Q(sample_info)
          << ",\"info_channels\":" << info_channels << ",\"info_progressive\":" << info_progressive
          << ",\"info_encoded\":" << info_encoded << ",\"info_key\":" << info_key
          << ",\"coeff_count\":" << coeff_count << ",\"coeff_min\":" << coeff_min << ",\"coeff_max\":" << coeff_max
          << ",\"coeff_hash\":" << Q(coeff_hash) << ",\"wavelet_map\":" << Q(wavelet_map)
          << ",\"channel_means\":" << Q(channel_means)
          << ",\"fixture\":" << Q(fixture_name) << "}";
        return o.str();
    }
};

int TagOr(const Walk& w, int tag) { return w.seen[tag] ? w.last[tag] : -1; }

void InspectNative(DECODER* d, Attempt& a) {
    if (!d) return;
    a.codec_precision = d->codec.precision;
    a.codec_channels = d->codec.num_channels;
    a.codec_progressive = d->codec.progressive;
    a.codec_transform = d->codec.transform_type;
    a.codec_encoded = d->codec.encoded_format;
    a.codec_frames = d->codec.num_frames;
    a.codec_spatial = d->codec.num_spatial;
    a.codec_subbands = d->codec.num_subbands;
    uint64_t hash = 14695981039346656037ULL;
    int count = 0, lo = 0, hi = 0;
    bool any = false;
    std::ostringstream map, means;
    const int channels = (std::max)(0, (std::min)(d->codec.num_channels, TRANSFORM_MAX_CHANNELS));
    for (int ch = 0; ch < channels; ++ch) {
        TRANSFORM* t = d->transform[ch];
        if (!t) continue;
        if (ch) map << "|";
        map << "ch" << ch << ":type" << int(t->type) << ":levels" << t->num_levels
            << ":spatial" << t->num_spatial << ":wavelets" << t->num_wavelets
            << ":wh" << t->width << "x" << t->height;
        for (int wi = 0; wi < TRANSFORM_MAX_WAVELETS; ++wi) {
            IMAGE* image = t->wavelet[wi];
            if (!image) continue;
            map << ";w" << wi << ":" << image->width << "x" << image->height << "b" << image->num_bands;
            for (int b = 0; b < image->num_bands && b < IMAGE_NUM_BANDS; ++b) {
                const PIXEL* row = image->band[b];
                if (!row || image->width <= 0 || image->height <= 0 || image->pitch <= 0) continue;
                const int stride = image->pitch / int(sizeof(PIXEL));
                if (stride < image->width) continue;
                long long sum = 0;
                for (int y = 0; y < image->height; ++y) for (int x = 0; x < image->width; ++x) {
                    const int sample = row[y * stride + x];
                    if (!any) { lo = hi = sample; any = true; }
                    if (sample < lo) lo = sample;
                    if (sample > hi) hi = sample;
                    ++count;
                    sum += sample;
                    hash ^= uint64_t(uint16_t(sample));
                    hash *= 1099511628211ULL;
                }
                if (b == 0) {
                    if (!means.str().empty()) means << ";";
                    means << "ch" << ch << "w" << wi << "=" << (image->width * image->height ? sum / (image->width * image->height) : 0);
                }
            }
        }
    }
    a.coeff_count = count;
    a.coeff_min = lo;
    a.coeff_max = hi;
    a.coeff_hash = any ? Hex64(hash) : "";
    a.wavelet_map = map.str();
    a.channel_means = means.str();
}

bool DecodeOne(const uint8_t* data, size_t size, CFHD_PixelFormat requested, Attempt* a, bool capture) {
    void* raw = nullptr;
    if (CFHD_OpenDecoder(&raw, nullptr) != CFHD_ERROR_OKAY || !raw) {
        if (a) a->upstream_error = "open decoder failed";
        return false;
    }
    DecodePhase0::DecoderHandle guard(raw);
    int32_t info[8]{};
    CFHD_SampleInfoTag tags[] = {
        CFHD_SAMPLE_INFO_CHANNELS, CFHD_SAMPLE_DISPLAY_WIDTH, CFHD_SAMPLE_DISPLAY_HEIGHT,
        CFHD_SAMPLE_KEY_FRAME, CFHD_SAMPLE_PROGRESSIVE, CFHD_SAMPLE_ENCODED_FORMAT,
        CFHD_SAMPLE_SDK_VERSION, CFHD_SAMPLE_ENCODE_VERSION};
    std::ostringstream info_text;
    for (int i = 0; i < 8; ++i) {
        if (CFHD_GetSampleInfo(raw, const_cast<uint8_t*>(data), size, tags[i], &info[i], sizeof(info[i])) == CFHD_ERROR_OKAY) {
            if (i) info_text << ",";
            info_text << info[i];
        } else info_text << (i ? ",na" : "na");
    }
    if (a && capture) {
        a->sample_info = info_text.str();
        a->info_channels = info[0];
        a->info_key = info[3];
        a->info_progressive = info[4];
        a->info_encoded = info[5];
    }
    int width = 0, height = 0;
    CFHD_PixelFormat actual = CFHD_PIXEL_FORMAT_UNKNOWN;
    const CFHD_Error prepared = CFHD_PrepareToDecode(raw, 0, 0, requested, CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(data), size, &width, &height, &actual);
    if (prepared != CFHD_ERROR_OKAY) {
        if (a && capture) a->upstream_error = "prepare " + std::to_string(int(prepared));
        return false;
    }
    int32_t pitch = 0;
    if (CFHD_GetImagePitch(width, actual, &pitch) != CFHD_ERROR_OKAY || pitch <= 0) {
        if (a && capture) a->upstream_error = "pitch";
        return false;
    }
    AlignedVector<uint8_t> image(size_t(pitch) * size_t(height));
    auto* native = DecodePhase0::NativeDecoder(raw);
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(kSeed);
    const CFHD_Error decoded = CFHD_DecodeSample(raw, const_cast<uint8_t*>(data), size, image.data(), pitch);
    if (decoded != CFHD_ERROR_OKAY) {
        if (a && capture) a->upstream_error = "decode " + std::to_string(int(decoded));
        return false;
    }
    if (a && capture) {
        a->upstream_ok = true;
        a->upstream_error.clear();
        a->decoded_w = width;
        a->decoded_h = height;
        a->output_hash = HashBytes(image.data(), image.size());
        a->output_formats = std::string(PixelName(actual)) + ":" + a->output_hash;
        InspectNative(native, *a);
    }
    return true;
}

Attempt Run(const Attempt& request) {
    Attempt a = request;
    if (a.input == CFHD_PIXEL_FORMAT_RG30 || a.input == CFHD_PIXEL_FORMAT_R210 ||
        a.input == CFHD_PIXEL_FORMAT_DPX0 || a.input == CFHD_PIXEL_FORMAT_AR10 ||
        a.input == CFHD_PIXEL_FORMAT_AB10 || a.input == CFHD_PIXEL_FORMAT_RG64 ||
        a.input == CFHD_PIXEL_FORMAT_NV12 || a.input == CFHD_PIXEL_FORMAT_YV12 ||
        a.encoded == CFHD_ENCODED_FORMAT_YUVA_4444) {
        a.note += " encode skipped: this SDK build has no safe converter and the default encoder case opens a modal dialog";
        a.prepare = -2;
        return a;
    }
    void* raw = nullptr;
    a.prepare = int(CFHD_OpenEncoder(&raw, nullptr));
    if (a.prepare != CFHD_ERROR_OKAY || !raw) { a.note += " open failed"; return a; }
    DecodePhase0::Encoder guard(raw);
    a.prepare = int(CFHD_PrepareToEncode(raw, a.w, a.h, a.input, a.encoded, a.flags,
        CFHD_EncodingQuality(a.quality)));
    if (a.prepare != CFHD_ERROR_OKAY) return a;
    std::srand(kSeed);
    uint64_t source_hash = 14695981039346656037ULL;
    for (int frame = 0; frame < a.frames; ++frame) {
        Packed packed;
        if (a.id == "yuy2_interlaced_reconfirm") {
            auto official = HyperCineTest::GenerateTestVectorYUY2(
                HyperCineTest::PATTERN_PSEUDO_RANDOM_NOISE, a.w, a.h);
            packed.bytes.assign(official.begin(), official.end());
            packed.pitch = a.w * 2;
            a.note += " source=GenerateTestVectorYUY2 PATTERN_PSEUDO_RANDOM_NOISE";
        } else {
            packed = MakeFrame(a.input, a.w, a.h, a.pattern, frame);
        }
        source_hash ^= ComputeFNV1a64(packed.bytes.data(), packed.bytes.size());
        source_hash *= 1099511628211ULL;
        a.encode = int(CFHD_EncodeSample(raw, packed.bytes.data(), packed.pitch));
        if (a.encode != CFHD_ERROR_OKAY) return a;
    }
    a.source_hash = Hex64(source_hash);
    void* data = nullptr;
    size_t size = 0;
    a.encode = int(CFHD_GetSampleData(raw, &data, &size));
    if (a.encode != CFHD_ERROR_OKAY || !data || !size) {
        a.note += " empty sample";
        return a;
    }
    a.sample.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    try {
        DecodePhase0::Bytes normalized(a.sample.begin(), a.sample.end());
        DecodePhase0::NormalizeMetadata(normalized);
        a.sample.assign(normalized.begin(), normalized.end());
    } catch (const std::exception& ex) { a.note += std::string(" metadata:") + ex.what(); }
    a.sample_bytes = int(a.sample.size());
    a.sample_hash = HashBytes(a.sample.data(), a.sample.size());
    const Walk walk = WalkSample(a.sample.data(), a.sample.size());
    a.research_ok = walk.ok;
    a.research_error = walk.error;
    a.band_count = int(walk.bands.size());
    a.band_sig = walk.signature;
    a.trailer_mismatches = walk.trailer_mismatches;
    a.tag_precision = TagOr(walk, CODEC_TAG_PRECISION);
    a.tag_channels = TagOr(walk, CODEC_TAG_NUM_CHANNELS);
    a.tag_width = TagOr(walk, CODEC_TAG_FRAME_WIDTH);
    a.tag_height = TagOr(walk, CODEC_TAG_FRAME_HEIGHT);
    a.tag_frame_format = TagOr(walk, CODEC_TAG_FRAME_FORMAT);
    a.tag_transform = TagOr(walk, CODEC_TAG_TRANSFORM_TYPE);
    a.tag_frames = TagOr(walk, CODEC_TAG_NUM_FRAMES);
    a.tag_flags = TagOr(walk, CODEC_TAG_SAMPLE_FLAGS);
    a.tag_encoded = TagOr(walk, CODEC_TAG_ENCODED_FORMAT);
    a.tag_spatial = TagOr(walk, CODEC_TAG_NUM_SPATIAL);
    a.tag_wavelets = TagOr(walk, CODEC_TAG_NUM_WAVELETS);
    a.tag_subbands = TagOr(walk, CODEC_TAG_NUM_SUBBANDS);
    a.tag_quality_l = TagOr(walk, CODEC_TAG_QUALITY_L);
    a.tag_quality_h = TagOr(walk, CODEC_TAG_QUALITY_H);
    a.tag_colorspace = TagOr(walk, CODEC_TAG_ENCODED_COLORSPACE);
    a.tag_interlace = TagOr(walk, CODEC_TAG_INTERLACED_FLAGS);
    a.tag_input_format = TagOr(walk, CODEC_TAG_INPUT_FORMAT);
    std::ostringstream diffs, peaks;
    for (const auto& band : walk.bands) {
        if (band.difference) {
            if (a.diff_count) diffs << ",";
            diffs << band.channel << ":" << band.subband;
            ++a.diff_count;
        }
        if (band.peak) {
            if (a.peak_count) peaks << ",";
            peaks << band.channel << ":" << band.subband << "@" << band.peak;
            ++a.peak_count;
        }
    }
    a.diff_bands = diffs.str();
    a.peak_bands = peaks.str();
    std::vector<HyperCine::GpuEntropyBandDescriptor> parsed;
    HyperCine::DecodeError status = HyperCine::DecodeError::InvalidSample;
    a.production_ok = HyperCine::ParseCineFormSampleValidated(
        a.sample.data(), a.sample.size(), 24, parsed, &a.production_error, &status);
    a.production_status = ErrorName(a.production_ok ? HyperCine::DecodeError::None : status);
    CFHD_PixelFormat outputs[32]{};
    int output_count = 0;
    void* listing = nullptr;
    if (CFHD_OpenDecoder(&listing, nullptr) == CFHD_ERROR_OKAY) {
        CFHD_GetOutputFormats(listing, a.sample.data(), a.sample.size(), outputs, 32, &output_count);
        if (output_count < 0) output_count = 0;
        if (output_count > 32) output_count = 32;
        std::ostringstream names;
        for (int i = 0; i < output_count; ++i) {
            if (i) names << ",";
            names << PixelName(outputs[i]);
        }
        a.output_formats = names.str();
        CFHD_CloseDecoder(listing);
    }
    const CFHD_PixelFormat prefer[] = {
        CFHD_PIXEL_FORMAT_BGRA, CFHD_PIXEL_FORMAT_YU64, CFHD_PIXEL_FORMAT_RG48,
        CFHD_PIXEL_FORMAT_B64A, CFHD_PIXEL_FORMAT_BYR4};
    for (CFHD_PixelFormat format : prefer) {
        if (DecodeOne(a.sample.data(), a.sample.size(), format, &a, true)) break;
    }
    if (!a.keep) a.sample.clear();
    return a;
}

std::string ProfileKey(const Attempt& a) {
    std::ostringstream s;
    s << EncodedName(a.codec_encoded) << "-p" << a.codec_precision
      << "-ch" << a.codec_channels << "-" << TransformName(a.codec_transform)
      << (a.codec_progressive ? "-prog" : "-field")
      << "-f" << a.codec_frames << "-bands" << a.band_count
      << "-fmt" << a.tag_frame_format;
    return s.str();
}

void WriteArray(const std::filesystem::path& path, const std::vector<std::string>& rows) {
    std::ofstream out(path);
    out << "[\n";
    for (size_t i = 0; i < rows.size(); ++i) out << rows[i] << (i + 1 == rows.size() ? "\n" : ",\n");
    out << "]\n";
}

struct Study {
    std::string text;
};

Study YU64Study(const std::vector<uint8_t>& sample, int width, int height) {
    Study study;
    std::ostringstream o;
    o << "{\"sample_bytes\":" << sample.size() << ",\"width\":" << width << ",\"height\":" << height;
    void* raw = nullptr;
    if (sample.empty() || CFHD_OpenDecoder(&raw, nullptr) != CFHD_ERROR_OKAY) {
        o << ",\"status\":\"no-sample\"}";
        study.text = o.str();
        return study;
    }
    DecodePhase0::DecoderHandle guard(raw);
    int dw = 0, dh = 0;
    CFHD_PixelFormat actual = CFHD_PIXEL_FORMAT_UNKNOWN;
    const auto prepared = CFHD_PrepareToDecode(raw, 0, 0, CFHD_PIXEL_FORMAT_YU64, CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(), &dw, &dh, &actual);
    int32_t pitch = 0;
    o << ",\"prepare\":" << int(prepared) << ",\"actual\":" << Q(PixelName(actual));
    if (prepared != CFHD_ERROR_OKAY || CFHD_GetImagePitch(dw, actual, &pitch) != CFHD_ERROR_OKAY) {
        o << ",\"status\":\"prepare-failed\"}";
        study.text = o.str();
        return study;
    }
    AlignedVector<uint8_t> packed(size_t(pitch) * size_t(dh));
    auto* native = DecodePhase0::NativeDecoder(raw);
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(kSeed);
    const auto decoded = CFHD_DecodeSample(raw, const_cast<uint8_t*>(sample.data()), sample.size(), packed.data(), pitch);
    o << ",\"decode\":" << int(decoded) << ",\"precision\":" << native->codec.precision
      << ",\"progressive\":" << native->codec.progressive;
    if (decoded != CFHD_ERROR_OKAY || native->codec.num_channels < 3 ||
        !native->transform[0] || !native->transform[0]->wavelet[0] ||
        !native->transform[0]->wavelet[1] || !native->transform[0]->wavelet[2]) {
        o << ",\"status\":\"inverse-unavailable\"}";
        study.text = o.str();
        return study;
    }
    auto ref = HyperCine::DecodePhase1::ComputeCpuReferenceIntermediates(native);
    const int precision = native->codec.precision > 0 ? native->codec.precision : 10;
    const int upshift = 16 - precision;
    const int limit = (1 << precision) - 1;
    size_t words = 0, over = 0, match_shift = 0, match_clamp = 0, both = 0, neither = 0;
    size_t packed_65535 = 0, packed_65472 = 0, packed_words = 0, plane_oob = 0;
    int max_plane = 0, max_packed = 0;
    auto plane_at = [&](int channel, int x, int y, bool& ok) {
        ok = y >= 0 && y < ref.plane_height[channel] && x >= 0 && x < ref.plane_width[channel];
        if (!ok) return 0;
        return int(ref.plane[channel][size_t(y) * size_t(ref.plane_width[channel]) + size_t(x)]);
    };
    for (int y = 0; y < dh; ++y) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(packed.data() + size_t(y) * size_t(pitch));
        const int row_words = pitch / int(sizeof(uint16_t));
        const int active = (std::min)(row_words, dw * 2);
        for (int i = 0; i < active; ++i) {
            const int word = int(row[i]);
            ++packed_words;
            if (word > max_packed) max_packed = word;
            packed_65535 += word == 65535;
            packed_65472 += word == 65472;
        }
        for (int x = 0; x + 1 < ref.plane_width[0] && x + 1 < dw; x += 2) {
            // Little-endian YU64 group is Y0, V, Y1, U. Decoder planar[1] is the V
            // source and planar[2] is the U source (convert.c ConvertPlanarYUVToYU64).
            bool ok0 = false, ok1 = false, ok2 = false, ok3 = false;
            const int samples[4] = {
                plane_at(0, x, y, ok0), plane_at(1, x / 2, y, ok1),
                plane_at(0, x + 1, y, ok2), plane_at(2, x / 2, y, ok3)};
            const bool oks[4] = {ok0, ok1, ok2, ok3};
            for (int i = 0; i < 4; ++i) {
                if (!oks[i]) { ++plane_oob; continue; }
                const int sample = samples[i];
                if (sample > max_plane) max_plane = sample;
                if (sample > limit) ++over;
                int shifted = sample << upshift;
                if (shifted > 65535) shifted = 65535;
                if (shifted < 0) shifted = 0;
                int clamped = sample;
                if (clamped < 0) clamped = 0;
                if (clamped > limit) clamped = limit;
                clamped <<= upshift;
                const int word = int(row[(x / 2) * 4 + i]);
                const bool m_shift = word == shifted;
                const bool m_clamp = word == clamped;
                ++words;
                match_shift += m_shift;
                match_clamp += m_clamp;
                both += m_shift && m_clamp;
                neither += !m_shift && !m_clamp;
            }
        }
    }
    // ConvertPlanarYUVToYU64 shifts first. A packed word of 65535 is that path
    // observed in bytes. A word of 65472 also matches a reconstructed sample that
    // was already limited to the coded maximum before the packer ran.
    const char* canonical = "unresolved";
    if (packed_65535 > 0 && match_shift == words && match_clamp < words) canonical = "shift_then_saturate_uint16";
    else if (over && packed_65535 == 0 && match_clamp == words && match_shift < words)
        canonical = "packed_bytes_match_preclamp_then_shift_on_this_fixture";
    else if (match_shift == words && match_clamp == words) canonical = "both_agree_on_this_sample";
    else canonical = "mixed_or_incomplete";
    o << ",\"decoded_width\":" << dw << ",\"decoded_height\":" << dh
      << ",\"words\":" << words << ",\"plane_samples_above_precision\":" << over
      << ",\"plane_out_of_range\":" << plane_oob
      << ",\"max_reconstructed_sample\":" << max_plane
      << ",\"packed_words\":" << packed_words << ",\"max_packed_word\":" << max_packed
      << ",\"packed_word_65535\":" << packed_65535 << ",\"packed_word_65472\":" << packed_65472
      << ",\"word_order\":\"Y0,V,Y1,U little-endian uint16\""
      << ",\"match_shift_then_saturate\":" << match_shift
      << ",\"match_saturate_then_shift\":" << match_clamp
      << ",\"match_both\":" << both << ",\"match_neither\":" << neither
      << ",\"upstream_function\":\"ConvertPlanarYUVToYU64\""
      << ",\"upstream_source\":\"upstream/Codec/convert.c\""
      << ",\"canonical\":" << Q(canonical)
      << ",\"hypercine_helper\":\"tests/decode_phase1_support.h ConvertPlanarToYU64 saturates to (1<<precision)-1 before the shift\""
      << ",\"status\":\"measured\"}";
    study.text = o.str();
    return study;
}

std::string GpuReconfirm(const std::vector<uint8_t>& sample) {
    std::ostringstream o;
    o << "{\"bytes\":" << sample.size();
    if (sample.size() < 32) { o << ",\"status\":\"missing\"}"; return o.str(); }
    HyperCine::DecodeError status = HyperCine::DecodeError::InvalidSample;
    std::string reason;
    std::vector<HyperCine::GpuEntropyBandDescriptor> bands;
    const bool production = HyperCine::ParseCineFormSampleValidated(sample.data(), sample.size(), 24, bands, &reason, &status);
    o << ",\"production_parse\":" << (production ? "true" : "false")
      << ",\"production_status\":" << Q(ErrorName(status)) << ",\"production_error\":" << Q(reason);
    HyperCine::Decoder decoder;
    const auto init = decoder.Initialize();
    o << ",\"production_init\":" << Q(ErrorName(init));
    if (init == HyperCine::DecodeError::None) {
        HyperCine::DecodeError queued = HyperCine::DecodeError::None;
        auto ticket = decoder.QueueDecode(sample.data(), sample.size(), {}, &queued);
        auto done = decoder.WaitForDecode(ticket);
        o << ",\"queue_error\":" << Q(ErrorName(queued)) << ",\"wait_error\":" << Q(ErrorName(done.error))
          << ",\"wait_state\":" << int(done.state);
        decoder.ReleaseOutput(ticket);
    }
    void* raw = nullptr;
    if (CFHD_OpenDecoder(&raw, nullptr) != CFHD_ERROR_OKAY) { o << ",\"status\":\"no-upstream\"}"; return o.str(); }
    DecodePhase0::DecoderHandle guard(raw);
    int w = 0, h = 0;
    CFHD_PixelFormat actual = CFHD_PIXEL_FORMAT_UNKNOWN;
    if (CFHD_PrepareToDecode(raw, 0, 0, CFHD_PIXEL_FORMAT_BGRA, CFHD_DECODED_RESOLUTION_FULL,
            CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(), &w, &h, &actual) != CFHD_ERROR_OKAY) {
        o << ",\"status\":\"upstream-prepare-failed\"}";
        return o.str();
    }
    int32_t pitch = 0;
    CFHD_GetImagePitch(w, actual, &pitch);
    AlignedVector<uint8_t> image(size_t(pitch) * size_t((std::max)(h, 0)));
    auto* native = DecodePhase0::NativeDecoder(raw);
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(kSeed);
    if (CFHD_DecodeSample(raw, const_cast<uint8_t*>(sample.data()), sample.size(), image.data(), pitch) != CFHD_ERROR_OKAY ||
        !native->transform[0] || !native->transform[0]->wavelet[2]) {
        o << ",\"status\":\"upstream-decode-failed\"}";
        return o.str();
    }
    auto cpu = HyperCine::DecodePhase1::ExtractBandsFromNative(native);
    o << ",\"cpu_bands\":" << cpu.size();
    HyperCine::CudaDecoderPipelinePhase2E legacy, hybrid;
    const bool legacy_ok = legacy.Initialize(w, h, 1, 10, 8, 4);
    const bool hybrid_ok = hybrid.Initialize(w, h, 1, 10, 8, 4);
    o << ",\"legacy_init\":" << (legacy_ok ? "true" : "false") << ",\"hybrid_init\":" << (hybrid_ok ? "true" : "false");
    if (legacy_ok && hybrid_ok && cpu.size() == 30) {
        hybrid.SetPipelineMode(HyperCine::Phase2EPipelineMode::Phase2FCHybridFullFrame);
        const auto legacy_ticket = legacy.QueueDecode(sample.data(), sample.size());
        const auto hybrid_ticket = hybrid.QueueDecode(sample.data(), sample.size());
        const bool legacy_done = legacy_ticket && legacy.WaitForDecode(legacy_ticket);
        const bool hybrid_done = hybrid_ticket && hybrid.WaitForDecode(hybrid_ticket);
        int max_error = 0, mismatches = 0, compared = 0;
        if (legacy_done && hybrid_done) {
            for (int b = 0; b < 30; ++b) {
                std::vector<int16_t> left, right;
                int bw = 0, bh = 0, pitch_bytes = 0;
                if (!legacy.DownloadBandCoefficientsToHost(legacy_ticket, b, left, &bw, &bh, &pitch_bytes) ||
                    !hybrid.DownloadBandCoefficientsToHost(hybrid_ticket, b, right)) {
                    ++mismatches;
                    continue;
                }
                const auto& ref = cpu[size_t(b)];
                const int stride = pitch_bytes / 2;
                for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) {
                    const int gpu = left[size_t(y) * stride + x];
                    const int other = right[size_t(y) * stride + x];
                    const int gold = ref.host_data[size_t(y) * (ref.pitch / 2) + x];
                    const int delta = std::abs(gpu - gold);
                    if (delta > max_error) max_error = delta;
                    if (gpu != gold || gpu != other) ++mismatches;
                    ++compared;
                }
            }
        }
        legacy.ReleaseOutput(legacy_ticket);
        hybrid.ReleaseOutput(hybrid_ticket);
        o << ",\"legacy_done\":" << (legacy_done ? "true" : "false")
          << ",\"hybrid_done\":" << (hybrid_done ? "true" : "false")
          << ",\"compared\":" << compared << ",\"mismatches\":" << mismatches
          << ",\"max_abs_error\":" << max_error;
    }
    o << ",\"status\":\"measured\"}";
    return o.str();
}

struct RunPack { const Attempt* in; Attempt* out; };
void RunThunk(void* opaque) {
    auto* pack = static_cast<RunPack*>(opaque);
    *pack->out = Run(*pack->in);
}

}  // namespace

extern "C" int HyperCineSehCall(void (*fn)(void*), void* arg, unsigned long* code);

int main(int argc, char** argv) {
    const std::filesystem::path results = argc > 1 ? argv[1] : "benchmarks/results";
    const std::filesystem::path fixtures = argc > 2 ? argv[2] : "tests/fixtures/phase3b0";
    const std::filesystem::path root = argc > 3 ? argv[3] : ".";
    std::filesystem::create_directories(results);
    std::filesystem::create_directories(fixtures);
    struct Axis { const char* name; CFHD_PixelFormat input; CFHD_EncodedFormat encoded; uint32_t flags; int frames; bool safe; };
    const Axis axes[] = {
        {"yuy2_yuv_prog", CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"yuy2_yuv_intl", CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_YUV_INTERLACED, 1, true},
        {"yuy2_yuv_gop2", CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_YUV_2FRAME_GOP, 2, true},
        {"yuy2_yuv_intl_gop2", CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422,
            CFHD_ENCODING_FLAGS_YUV_INTERLACED | CFHD_ENCODING_FLAGS_YUV_2FRAME_GOP, 2, true},
        {"yuy2_yuv_601", CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_YUV_601, 1, true},
        {"2vuy_yuv_prog", CFHD_PIXEL_FORMAT_2VUY, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"yu64_yuv_prog", CFHD_PIXEL_FORMAT_YU64, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"yu64_yuv_intl", CFHD_PIXEL_FORMAT_YU64, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_YUV_INTERLACED, 1, true},
        {"v210_yuv_prog", CFHD_PIXEL_FORMAT_V210, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"v210_yuv_intl", CFHD_PIXEL_FORMAT_V210, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_YUV_INTERLACED, 1, true},
        {"nv12_yuv_prog", CFHD_PIXEL_FORMAT_NV12, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, false},
        {"yv12_yuv_prog", CFHD_PIXEL_FORMAT_YV12, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, false},
        {"ct_yuv_prog", CFHD_PIXEL_FORMAT_CT_UCHAR, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"rg24_rgb", CFHD_PIXEL_FORMAT_RG24, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"rg24_yuv", CFHD_PIXEL_FORMAT_RG24, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"rg24_rgba", CFHD_PIXEL_FORMAT_RG24, CFHD_ENCODED_FORMAT_RGBA_4444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"bgra_rgb", CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"bgra_rgba", CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_RGBA_4444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"bgra_yuv", CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"bgra_rgb_studio", CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_RGB_STUDIO, 1, true},
        {"bgra_rgb_intl_flag", CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_YUV_INTERLACED, 1, true},
        {"bgra_bayer", CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_BAYER, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"bgra_yuva", CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUVA_4444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"rg48_rgb", CFHD_PIXEL_FORMAT_RG48, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"rg48_yuv", CFHD_PIXEL_FORMAT_RG48, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"b64a_rgba", CFHD_PIXEL_FORMAT_B64A, CFHD_ENCODED_FORMAT_RGBA_4444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"b64a_rgb", CFHD_PIXEL_FORMAT_B64A, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"b64a_yuv", CFHD_PIXEL_FORMAT_B64A, CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"byr4_bayer", CFHD_PIXEL_FORMAT_BYR4, CFHD_ENCODED_FORMAT_BAYER, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"byr5_bayer", CFHD_PIXEL_FORMAT_BYR5, CFHD_ENCODED_FORMAT_BAYER, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"yuy2_rgb", CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, 1, true},
        {"rg30_rgb_skipped", CFHD_PIXEL_FORMAT_RG30, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, 1, false},
        {"r210_rgb_skipped", CFHD_PIXEL_FORMAT_R210, CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, 1, false},
        {"rg64_rgba_skipped", CFHD_PIXEL_FORMAT_RG64, CFHD_ENCODED_FORMAT_RGBA_4444, CFHD_ENCODING_FLAGS_NONE, 1, false},
    };
    const int qualities[] = {
        CFHD_ENCODING_QUALITY_LOW, CFHD_ENCODING_QUALITY_MEDIUM, CFHD_ENCODING_QUALITY_HIGH,
        CFHD_ENCODING_QUALITY_FILMSCAN1, CFHD_ENCODING_QUALITY_FILMSCAN2, CFHD_ENCODING_QUALITY_FILMSCAN3};
    const int patterns[] = {0, 1, 2, 3, 4, 5, 6};
    std::vector<Attempt> planned;
    auto add = [&](Attempt item) { planned.push_back(std::move(item)); };
    for (const auto& axis : axes) {
        for (int quality : qualities) {
            Attempt item;
            item.id = std::string(axis.name) + "_64_" + QualityName(quality) + "_gradient";
            item.w = axis.input == CFHD_PIXEL_FORMAT_BYR4 || axis.input == CFHD_PIXEL_FORMAT_BYR5 ? 128 : 64;
            item.h = item.w;
            item.pattern = 1;
            item.frames = axis.frames;
            item.quality = quality;
            item.flags = axis.flags;
            item.input = axis.input;
            item.encoded = axis.encoded;
            item.fixture = quality == CFHD_ENCODING_QUALITY_FILMSCAN1 && axis.safe;
            item.keep = item.fixture;
            if (!axis.safe && quality != CFHD_ENCODING_QUALITY_FILMSCAN1) continue;
            add(item);
        }
        if (!axis.safe) continue;
        if (std::string(axis.name) == "bgra_rgba") {
            Attempt key = planned.back();
            key.id = "bgra_rgba_64_keying_alpha";
            key.quality = int(CFHD_ENCODING_QUALITY_KEYING);
            key.pattern = 6;
            key.fixture = true;
            key.keep = true;
            add(key);
        }
    }
    const char* deep_names[] = {"yuy2_yuv_prog", "yuy2_yuv_intl", "bgra_rgb", "bgra_rgba", "rg48_rgb", "yu64_yuv_prog", "byr4_bayer"};
    for (const char* name : deep_names) {
        const Axis* axis = nullptr;
        for (const auto& candidate : axes) if (std::string(candidate.name) == name) axis = &candidate;
        if (!axis) continue;
        for (int pattern : patterns) {
            if (pattern == 1) continue;
            for (int quality : {int(CFHD_ENCODING_QUALITY_FILMSCAN1), int(CFHD_ENCODING_QUALITY_FILMSCAN3)}) {
                Attempt item;
                item.id = std::string(name) + "_64_" + QualityName(quality) + "_" + PatternName(pattern);
                item.w = axis->input == CFHD_PIXEL_FORMAT_BYR4 ? 128 : 64;
                item.h = item.w;
                item.pattern = pattern;
                item.frames = axis->frames;
                item.quality = quality;
                item.flags = axis->flags;
                item.input = axis->input;
                item.encoded = axis->encoded;
                add(item);
            }
        }
    }
    const int sizes[][2] = {{128, 64}, {128, 128}, {48, 48}, {64, 40}, {32, 32}};
    for (auto size : sizes) {
        for (const char* name : {"yuy2_yuv_prog", "yuy2_yuv_intl", "bgra_rgb", "bgra_rgba"}) {
            const Axis* axis = nullptr;
            for (const auto& candidate : axes) if (std::string(candidate.name) == name) axis = &candidate;
            Attempt item;
            item.id = std::string(name) + "_" + std::to_string(size[0]) + "x" + std::to_string(size[1]);
            item.w = size[0];
            item.h = size[1];
            item.pattern = 1;
            item.frames = 1;
            item.quality = CFHD_ENCODING_QUALITY_FILMSCAN1;
            item.flags = axis->flags;
            item.input = axis->input;
            item.encoded = axis->encoded;
            add(item);
        }
    }
    const char* hd_names[] = {"yuy2_yuv_prog", "yuy2_yuv_intl", "yu64_yuv_prog", "bgra_rgb", "bgra_rgba", "rg48_rgb", "b64a_rgba", "v210_yuv_prog"};
    for (const char* name : hd_names) {
        const Axis* axis = nullptr;
        for (const auto& candidate : axes) if (std::string(candidate.name) == name) axis = &candidate;
        Attempt item;
        item.id = std::string(name) + "_1920x1080_filmscan1_gradient";
        item.w = 1920;
        item.h = 1080;
        item.pattern = 1;
        item.frames = 1;
        item.quality = CFHD_ENCODING_QUALITY_FILMSCAN1;
        item.flags = axis->flags;
        item.input = axis->input;
        item.encoded = axis->encoded;
        item.keep = std::string(name) == "yuy2_yuv_intl" || std::string(name) == "yu64_yuv_prog";
        add(item);
    }
    {
        Attempt item;
        item.id = "yu64_overshoot_edges";
        item.w = 128; item.h = 64; item.pattern = 3; item.frames = 1;
        item.quality = CFHD_ENCODING_QUALITY_FILMSCAN3;
        item.input = CFHD_PIXEL_FORMAT_YU64;
        item.encoded = CFHD_ENCODED_FORMAT_YUV_422;
        item.keep = true;
        add(item);
        item.id = "yuy2_interlaced_reconfirm";
        item.w = 1920; item.h = 1080; item.pattern = 4;
        item.quality = CFHD_ENCODING_QUALITY_FILMSCAN3;
        item.flags = CFHD_ENCODING_FLAGS_YUV_INTERLACED;
        item.input = CFHD_PIXEL_FORMAT_YUY2;
        item.keep = true;
        add(item);
        item.id = "channel_yuy2_chroma";
        item.w = 64; item.h = 64; item.pattern = 7; item.flags = 0;
        item.quality = CFHD_ENCODING_QUALITY_FILMSCAN1;
        item.input = CFHD_PIXEL_FORMAT_YUY2;
        item.encoded = CFHD_ENCODED_FORMAT_YUV_422;
        item.keep = false;
        add(item);
        item.id = "channel_rg24_red";
        item.pattern = 8;
        item.input = CFHD_PIXEL_FORMAT_RG24;
        item.encoded = CFHD_ENCODED_FORMAT_RGB_444;
        add(item);
        item.id = "channel_bgra_alpha";
        item.pattern = 6;
        item.input = CFHD_PIXEL_FORMAT_BGRA;
        item.encoded = CFHD_ENCODED_FORMAT_RGBA_4444;
        add(item);
    }

    std::vector<Attempt> done;
    done.reserve(planned.size());
    std::set<std::string> written;
    int index = 0;
    for (const auto& request : planned) {
        std::cout << "begin " << request.id << "\n" << std::flush;
        Attempt result = request;
        unsigned long exception_code = 0;
        RunPack pack{&request, &result};
        if (HyperCineSehCall(&RunThunk, &pack, &exception_code)) {
            result = request;
            result.note += " access violation 0x" + Hex64(exception_code);
            result.encode = -3;
            result.prepare = -3;
        }
        std::cout << "end " << request.id << " encode=" << result.encode
                  << " bytes=" << result.sample_bytes << "\n" << std::flush;
        if (result.fixture && result.sample_bytes > 0 && result.sample_bytes <= 256 * 1024 && result.upstream_ok) {
            const std::string key = ProfileKey(result);
            if (!written.count(key)) {
                written.insert(key);
                const std::string filename = key + ".cfhd";
                std::ofstream file(fixtures / filename, std::ios::binary);
                file.write(reinterpret_cast<const char*>(result.sample.data()), result.sample.size());
                result.fixture_name = filename;
            }
        }
        if ((++index % 25) == 0) std::cout << "probed " << index << "/" << planned.size() << "\n" << std::flush;
        if (!result.keep) result.sample.clear();
        done.push_back(std::move(result));
    }

    std::vector<std::string> rows, profiles, fixtures_json, matrix;
    std::map<std::string, int> profile_index;
    int success = 0, failed = 0;
    for (const auto& item : done) {
        rows.push_back(item.Json());
        if (item.sample_bytes > 0 && item.encode == CFHD_ERROR_OKAY) ++success;
        else ++failed;
        if (!item.upstream_ok) continue;
        const std::string key = ProfileKey(item);
        if (profile_index.insert({key, int(profiles.size())}).second) profiles.push_back(item.Json());
        if (!item.fixture_name.empty()) fixtures_json.push_back(item.Json());
    }
    struct Want { const char* name; int encoded, precision, progressive, channels; };
    const Want wants[] = {
        {"YUV422 progressive 8", ENCODED_FORMAT_YUV_422, 8, 1, 3},
        {"YUV422 progressive 10", ENCODED_FORMAT_YUV_422, 10, 1, 3},
        {"YUV422 progressive 12", ENCODED_FORMAT_YUV_422, 12, 1, 3},
        {"YUV422 interlaced 8", ENCODED_FORMAT_YUV_422, 8, 0, 3},
        {"YUV422 interlaced 10", ENCODED_FORMAT_YUV_422, 10, 0, 3},
        {"YUV422 interlaced 12", ENCODED_FORMAT_YUV_422, 12, 0, 3},
        {"RGB444 progressive 8", ENCODED_FORMAT_RGB_444, 8, 1, 3},
        {"RGB444 progressive 10", ENCODED_FORMAT_RGB_444, 10, 1, 3},
        {"RGB444 progressive 12", ENCODED_FORMAT_RGB_444, 12, 1, 3},
        {"RGBA4444 progressive 8", ENCODED_FORMAT_RGBA_4444, 8, 1, 4},
        {"RGBA4444 progressive 10", ENCODED_FORMAT_RGBA_4444, 10, 1, 4},
        {"RGBA4444 progressive 12", ENCODED_FORMAT_RGBA_4444, 12, 1, 4},
    };
    for (const auto& want : wants) {
        const Attempt* found = nullptr;
        int count = 0, production = 0, upstream = 0, diffs = 0, peaks = 0;
        for (const auto& item : done) {
            if (item.codec_encoded != want.encoded || item.codec_precision != want.precision ||
                item.codec_progressive != want.progressive || item.codec_channels != want.channels ||
                item.codec_frames != 1) continue;
            ++count;
            production += item.production_ok;
            upstream += item.upstream_ok;
            diffs += item.diff_count > 0;
            peaks += item.peak_count > 0;
            if (!found) found = &item;
        }
        std::ostringstream row;
        row << "{\"profile\":" << Q(want.name) << ",\"emitted\":" << count
            << ",\"production_parse_pass\":" << production << ",\"upstream_decode_pass\":" << upstream
            << ",\"samples_with_difference_coding\":" << diffs << ",\"samples_with_peaks\":" << peaks
            << ",\"example\":" << (found ? found->Json() : "null") << "}";
        matrix.push_back(row.str());
    }
    WriteArray(results / "decode_phase3b0_encoder_probe.json", rows);
    WriteArray(results / "decode_phase3b0_profiles.json", profiles);
    WriteArray(results / "decode_phase3b0_fixture_inventory.json", fixtures_json);
    WriteArray(results / "decode_phase3b0_capability_matrix.json", matrix);

    std::vector<std::string> parsers;
    for (const auto& item : done) {
        if (item.sample_bytes <= 0) continue;
        std::ostringstream row;
        row << "{\"id\":" << Q(item.id) << ",\"production_ok\":" << (item.production_ok ? "true" : "false")
            << ",\"status\":" << Q(item.production_status) << ",\"error\":" << Q(item.production_error)
            << ",\"research_ok\":" << (item.research_ok ? "true" : "false")
            << ",\"bands\":" << item.band_count << ",\"precision\":" << item.codec_precision
            << ",\"encoded\":" << Q(EncodedName(item.codec_encoded))
            << ",\"progressive\":" << item.codec_progressive << "}";
        parsers.push_back(row.str());
    }
    WriteArray(results / "decode_phase3b0_parser_matrix.json", parsers);

    const Attempt* yu64 = nullptr;
    const Attempt* interlaced = nullptr;
    for (const auto& item : done) {
        if (item.id == "yu64_overshoot_edges") yu64 = &item;
        if (item.id == "yuy2_interlaced_reconfirm") interlaced = &item;
    }
    const auto yu = YU64Study(yu64 ? yu64->sample : std::vector<uint8_t>{}, yu64 ? yu64->w : 0, yu64 ? yu64->h : 0);
    {
        std::ofstream out(results / "decode_phase3b0_yu64.json");
        out << yu.text << "\n";
    }
    const std::string gpu = GpuReconfirm(interlaced ? interlaced->sample : std::vector<uint8_t>{});
    {
        std::ofstream out(results / "decode_phase3b0_interlaced_map.json");
        out << "{\"reconfirm\":" << gpu << ",\"probe\":" << (interlaced ? interlaced->Json() : "null") << "}\n";
    }

    std::vector<std::string> external;
    for (const char* name : {"upstream/Example/videos/CFHDsample422.AVI", "upstream/Example/videos/CFHDsample444.AVI"}) {
        const auto path = root / name;
        std::ostringstream row;
        row << "{\"path\":" << Q(name);
        void* handle = std::filesystem::exists(path) ? OpenAVISource(const_cast<char*>(path.string().c_str()), AVI_TRAK_TYPE, AVI_TRAK_SUBTYPE) : nullptr;
        if (!handle) { row << ",\"status\":\"absent\"}"; external.push_back(row.str()); continue; }
        const uint32_t frames = GetNumberPayloads(handle);
        uint32_t* payload = nullptr;
        const uint32_t bytes = frames ? GetPayloadSize(handle, 0) : 0;
        payload = frames ? GetPayload(handle, payload, 0) : nullptr;
        row << ",\"frames\":" << frames << ",\"first_bytes\":" << bytes;
        if (payload && bytes) {
            Attempt inspected;
            inspected.id = name;
            const Walk walk = WalkSample(reinterpret_cast<uint8_t*>(payload), bytes);
            inspected.research_ok = walk.ok;
            inspected.band_count = int(walk.bands.size());
            inspected.band_sig = walk.signature;
            inspected.tag_precision = TagOr(walk, CODEC_TAG_PRECISION);
            inspected.tag_encoded = TagOr(walk, CODEC_TAG_ENCODED_FORMAT);
            inspected.tag_flags = TagOr(walk, CODEC_TAG_SAMPLE_FLAGS);
            inspected.tag_transform = TagOr(walk, CODEC_TAG_TRANSFORM_TYPE);
            inspected.tag_frame_format = TagOr(walk, CODEC_TAG_FRAME_FORMAT);
            inspected.sample_bytes = int(bytes);
            DecodeOne(reinterpret_cast<uint8_t*>(payload), bytes, CFHD_PIXEL_FORMAT_BGRA, &inspected, true);
            std::string error;
            HyperCine::DecodeError status = HyperCine::DecodeError::InvalidSample;
            std::vector<HyperCine::GpuEntropyBandDescriptor> parsed;
            inspected.production_ok = HyperCine::ParseCineFormSampleValidated(
                reinterpret_cast<uint8_t*>(payload), bytes, 24, parsed, &error, &status);
            inspected.production_status = ErrorName(status);
            inspected.production_error = error;
            row << ",\"first\":" << inspected.Json();
        }
        CloseSource(handle);
        row << "}";
        external.push_back(row.str());
    }
    WriteArray(results / "decode_phase3b0_external.json", external);

    const bool yuv10 = std::any_of(done.begin(), done.end(), [](const Attempt& item) {
        return item.codec_encoded == ENCODED_FORMAT_YUV_422 && item.codec_precision == 10 &&
               item.codec_progressive == 1 && item.production_ok && item.upstream_ok;
    });
    const bool yuv_i = std::any_of(done.begin(), done.end(), [](const Attempt& item) {
        return item.id == "yuy2_interlaced_reconfirm" && item.sample_bytes > 0 && item.upstream_ok &&
               item.production_ok && item.production_status == "None";
    });
    std::ofstream completion(results / "decode_phase3b0_completion.json");
    completion << "{\"attempts\":" << done.size() << ",\"encoded\":" << success << ",\"not_encoded\":" << failed
               << ",\"unique_upstream_profiles\":" << profiles.size()
               << ",\"fixtures\":" << fixtures_json.size()
               << ",\"progressive_yuv422_10_production\":" << (yuv10 ? "true" : "false")
               << ",\"interlaced_reconfirm\":" << (yuv_i ? "true" : "false")
               << ",\"yu64\":" << yu.text << "}\n";
    std::cout << "attempts " << done.size() << " encoded " << success << " profiles " << profiles.size()
              << " fixtures " << fixtures_json.size() << " yuv10 " << yuv10 << " interlaced " << yuv_i << "\n";
    return yuv10 && yuv_i ? 0 : 1;
}
