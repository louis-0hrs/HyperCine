// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Research-only FIELDPLUS temporal arithmetic and upstream frame oracle.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "decode_phase0_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void ReconstructSampleFrameToBuffer(DECODER* decoder, int frame,
                                                  uint8_t* output, int pitch);

namespace {
using DecodePhase0::Bytes;
constexpr uint32_t kSeed = 0x1337BEEFu;
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t HashAdd(uint64_t h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= kFnvPrime; }
    return h;
}
std::string Hex(uint64_t v) {
    std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << v; return s.str();
}
std::string Quote(const std::string& s) {
    std::ostringstream o; o << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << char(c);
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c < 32) o << ' ';
        else o << char(c);
    }
    o << '"'; return o.str();
}
void Check(CFHD_Error e, const char* op) { DecodePhase0::Check(e, op); }

int16_t Sat16(int value) {
    return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, value)));
}
int16_t ArithmeticHalf(int16_t value) {
    const int v = value;
    return static_cast<int16_t>(v >= 0 ? v / 2 : -((-v + 1) / 2));
}
struct TemporalPair { int16_t frame0; int16_t frame1; };
TemporalPair InverseTemporal(int16_t low, int16_t high) {
    return {ArithmeticHalf(Sat16(int(low) - int(high))),
            ArithmeticHalf(Sat16(int(low) + int(high)))};
}

void RunMathTests() {
    struct Case { int16_t low, high, f0, f1; const char* name; };
    const Case cases[] = {
        {0, 0, 0, 0, "zero"}, {8, 2, 3, 5, "positive"},
        {-8, -2, -3, -5, "negative"}, {-1, 0, -1, -1, "negative_odd_round_down"},
        {0, 1, -1, 0, "negative_half_boundary"}, {1, 0, 0, 0, "positive_half_boundary"},
        {30000, -30000, 16383, 0, "subtraction_saturates"},
        {30000, 30000, 0, 16383, "addition_saturates"},
        {-30000, -30000, 0, -16384, "negative_addition_saturates"},
        {-30000, 30000, -16384, 0, "negative_subtraction_saturates"},
        {32767, -1, 16383, 16383, "near_limit"},
    };
    for (const auto& c : cases) {
        const auto p = InverseTemporal(c.low, c.high);
        if (p.frame0 != c.f0 || p.frame1 != c.f1)
            throw std::runtime_error(std::string("temporal arithmetic mismatch: ") + c.name);
    }
    const auto fwd0 = Sat16(12000 + 14000);
    const auto fwd1 = Sat16(14000 - 12000);
    const auto restored = InverseTemporal(fwd0, fwd1);
    if (restored.frame0 != 12000 || restored.frame1 != 14000)
        throw std::runtime_error("forward/inverse temporal vector mismatch");
    std::cout << "temporal math cases " << (sizeof(cases) / sizeof(cases[0]))
              << " plus forward/inverse vector passed\n";
}

struct ChunkFacts {
    int chunk_count = 0;
    int index255_count = 0;
    std::vector<std::string> sentinels;
    std::vector<std::string> difference;
    std::vector<std::string> codebooks;
    std::vector<std::string> peaks;
    std::vector<std::string> records;
};
uint16_t BE16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
ChunkFacts InspectChunks(const Bytes& sample) {
    ChunkFacts f;
    // The first channel is implicit in the sample header; later channels carry
    // explicit channel tags. This matches the upstream parser's initial state.
    int channel = 0, subband = 0, channel_chunk = 0;
    int codebook = 0, difference = 0, peak = 0;
    size_t pos = 0;
    auto add = [](std::vector<std::string>& v, const std::string& x) {
        if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
    };
    while (pos + 4 <= sample.size()) {
        const int16_t raw = int16_t(BE16(sample.data() + pos));
        const unsigned tag = raw < 0 ? unsigned(-int(raw)) : unsigned(raw);
        const unsigned value = BE16(sample.data() + pos + 2);
        pos += 4;
        if ((tag & 0xff00) == CODEC_TAG_SUBBAND_SIZE) {
            const size_t n = (size_t((tag & 255) << 16 | value)) * 4;
            if (n > sample.size() - pos) throw std::runtime_error("subband length overruns sample");
            const int16_t inner_raw = int16_t(BE16(sample.data() + pos));
            const unsigned inner_tag = inner_raw < 0 ? unsigned(-int(inner_raw)) : unsigned(inner_raw);
            const bool lowpass = inner_tag == CODEC_TAG_MARKER;
            ++f.chunk_count;
            if (subband == 255) { ++f.index255_count; add(f.sentinels, std::to_string(channel)); }
            if (difference) add(f.difference, std::to_string(channel) + ":" + std::to_string(subband));
            if (peak) add(f.peaks, std::to_string(channel) + ":" + std::to_string(subband) + "@" + std::to_string(peak));
            add(f.codebooks, std::to_string(codebook));
            const size_t payload = subband == 255 ? 0 : (n > (lowpass ? 4u : 8u) ? n - (lowpass ? 4u : 8u) : 0);
            std::ostringstream chunk;
            chunk << "{\"physical_chunk_index\":" << (f.chunk_count - 1)
                  << ",\"channel\":" << channel << ",\"channel_chunk_index\":" << channel_chunk
                  << ",\"subband_index\":" << subband << ",\"chunk_bytes\":" << n
                  << ",\"payload_bytes\":" << payload << ",\"payload_kind\":"
                  << Quote(subband == 255 ? "empty temporal highpass control" : (lowpass ? "lowpass coefficient payload" : "highpass entropy payload"))
                  << ",\"codebook\":" << codebook << ",\"difference_coded\":" << (difference ? "true" : "false")
                  << ",\"peak_level\":" << peak << '}';
            f.records.push_back(chunk.str());
            ++channel_chunk;
            if (subband == 255) subband = 11;
            else if (lowpass) subband = 1;
            else ++subband;
            codebook = difference = peak = 0;
            pos += n;
            continue;
        }
        if (tag == CODEC_TAG_INDEX) { const size_t n = size_t(value) * 4; if (n > sample.size() - pos) throw std::runtime_error("index overruns sample"); pos += n; continue; }
        if (tag >= CODEC_TAG_CHUNK && tag < CODEC_TAG_CUSTOM_CHUNK24BIT) {
            const size_t n = size_t(value) * 4; if (n > sample.size() - pos) throw std::runtime_error("chunk overruns sample"); pos += n; continue;
        }
        if (tag == CODEC_TAG_CHANNEL) { channel = int(value); subband = 0; channel_chunk = 0; }
        else if (tag == CODEC_TAG_BAND_SUBBAND) subband = int(value);
        else if (tag == CODEC_TAG_BAND_CODING_FLAGS) { codebook = int(value & 0x0f); difference = int((value >> 4) & 1); }
        else if (tag == CODEC_TAG_PEAK_LEVEL) peak = int(value);
    }
    if (pos != sample.size()) throw std::runtime_error("unconsumed sample bytes");
    return f;
}

Bytes MakeYUY2(int w, int h, int mode, int frame) {
    Bytes b(size_t(w) * size_t(h) * 2, 0);
    uint32_t rng = kSeed ^ (uint32_t(mode) * 0x9e3779b9u) ^ (uint32_t(frame) * 0x85ebca6bu);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        auto pixel = [&](int px) {
            int Y = 0, U = 128, V = 128;
            switch (mode) {
            case 0: { // Strongly asymmetric dark sparse -> bright textured.
                if (frame == 0) Y = ((px > w / 8 && px < w / 4 && y > h / 3 && y < h / 2) ? 210 : 24);
                else { rng = rng * 1664525u + 1013904223u; Y = 150 + int((rng >> 24) % 90); if (((px / 4 + y / 4) & 1) != 0) Y = 235; }
                U = frame == 0 ? 48 : 220; V = frame == 0 ? 208 : 40;
                break;
            }
            case 1: { // A deterministic translated box and a moving edge.
                const int bx = (frame ? w * 5 / 8 : w * 3 / 8);
                const bool box = px >= bx && px < bx + (std::max)(8, w / 8) && y >= h / 3 && y < (2 * h) / 3;
                Y = box ? 232 : (px > w / 2 + frame * 3 ? 64 : 28);
                U = 112 + frame * 48; V = 144 - frame * 40;
                break;
            }
            case 2: { // Isolate strong temporal U/V changes from luma.
                Y = 30 + ((px * 3 + y * 5) % 190);
                U = frame ? 240 : 16; V = frame ? 16 : 240;
                break;
            }
            default: { // Interlaced field parity and GOP frame both differ.
                const int parity = y & 1;
                Y = 32 + parity * 112 + frame * 48 + ((px / 8 + y / 2) & 1) * 40;
                U = 24 + parity * 176 + frame * 20;
                V = 232 - parity * 160 - frame * 16;
                break;
            }
            }
            return std::array<int, 3>{Y, U, V};
        };
        const auto p0 = pixel(x), p1 = pixel((std::min)(x + 1, w - 1));
        const size_t i = (size_t(y) * w + size_t(x)) * 2;
        b[i] = uint8_t(p0[0]); b[i + 1] = uint8_t((p0[1] + p1[1]) / 2);
        b[i + 2] = uint8_t(p1[0]); b[i + 3] = uint8_t((p0[2] + p1[2]) / 2);
    }
    return b;
}

struct Encoded {
    Bytes sample;
    std::string source_hash;
};
Encoded EncodePair(int w, int h, bool interlaced, int quality, int pattern) {
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr), "open encoder");
    DecodePhase0::Encoder encoder(raw);
    const uint32_t flags = CFHD_ENCODING_FLAGS_YUV_2FRAME_GOP |
        (interlaced ? CFHD_ENCODING_FLAGS_YUV_INTERLACED : 0u);
    Check(CFHD_PrepareToEncode(raw, w, h, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422,
                               flags, CFHD_EncodingQuality(quality)), "prepare FIELDPLUS encoder");
    uint64_t source = kFnvOffset;
    for (int frame = 0; frame < 2; ++frame) {
        auto input = MakeYUY2(w, h, pattern, frame);
        source = HashAdd(source, input.data(), input.size());
        std::srand(kSeed);
        Check(CFHD_EncodeSample(raw, input.data(), w * 2), "encode FIELDPLUS frame");
    }
    void* data = nullptr; size_t size = 0; Check(CFHD_GetSampleData(raw, &data, &size), "get FIELDPLUS sample");
    if (!data || size == 0) throw std::runtime_error("encoder returned empty FIELDPLUS sample");
    Encoded result{Bytes(size), Hex(source)};
    std::memcpy(result.sample.data(), data, size);
    DecodePhase0::NormalizeMetadata(result.sample);
    return result;
}

struct PlaneHashes { std::string y, u, v; };
PlaneHashes HashYU64(const uint8_t* image, int pitch, int w, int h) {
    uint64_t y = kFnvOffset, u = kFnvOffset, v = kFnvOffset;
    auto word = [&](uint64_t& hash, uint16_t value) {
        const uint8_t bytes[2] = {uint8_t(value), uint8_t(value >> 8)}; hash = HashAdd(hash, bytes, 2);
    };
    for (int row = 0; row < h; ++row) {
        const auto* p = reinterpret_cast<const uint16_t*>(image + size_t(row) * pitch);
        for (int x = 0; x < w; x += 2) {
            const size_t i = size_t(x) * 2;
            word(y, p[i]); word(v, p[i + 1]); word(y, p[i + 2]); word(u, p[i + 3]);
        }
    }
    return {Hex(y), Hex(u), Hex(v)};
}
std::string HashImageRows(const uint8_t* image, int pitch, int row_bytes, int h) {
    uint64_t hash = kFnvOffset;
    for (int y = 0; y < h; ++y) hash = HashAdd(hash, image + size_t(y) * pitch, size_t(row_bytes));
    return Hex(hash);
}

struct FrameCapture {
    int width = 0, height = 0, pitch = 0;
    std::array<std::string, 2> bgra;
    std::array<PlaneHashes, 2> yu64;
    std::string coefficient_banks;
};
std::string CaptureBands(DECODER* d) {
    std::ostringstream o; bool first = true;
    for (int ch = 0; ch < d->codec.num_channels && ch < TRANSFORM_MAX_CHANNELS; ++ch) {
        TRANSFORM* t = d->transform[ch]; if (!t) continue;
        for (int wi = 0; wi < TRANSFORM_MAX_WAVELETS; ++wi) {
            IMAGE* image = t->wavelet[wi]; if (!image) continue;
            for (int band = 0; band < image->num_bands && band < IMAGE_NUM_BANDS; ++band) {
                if (!image->band[band] || image->pitch <= 0) continue;
                uint64_t hash = kFnvOffset;
                const int stride = image->pitch / int(sizeof(PIXEL));
                for (int y = 0; y < image->height; ++y)
                    hash = HashAdd(hash, image->band[band] + size_t(y) * stride,
                                   size_t(image->width) * sizeof(PIXEL));
                if (!first) o << ','; first = false;
                o << "{\"channel\":" << ch << ",\"wavelet\":" << wi << ",\"band\":" << band
                  << ",\"width\":" << image->width << ",\"height\":" << image->height
                  << ",\"signed_i16_fnv1a64\":" << Quote(Hex(hash)) << '}';
            }
        }
    }
    return o.str();
}

FrameCapture CaptureFrames(const Bytes& sample) {
    FrameCapture cap;
    void* raw = nullptr; Check(CFHD_OpenDecoder(&raw, nullptr), "open reference decoder");
    DecodePhase0::DecoderHandle decoder(raw);
    CFHD_PixelFormat actual = CFHD_PIXEL_FORMAT_UNKNOWN;
    Check(CFHD_PrepareToDecode(raw, 0, 0, CFHD_PIXEL_FORMAT_BGRA, CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(),
        &cap.width, &cap.height, &actual), "prepare BGRA reference");
    if (actual != CFHD_PIXEL_FORMAT_BGRA) throw std::runtime_error("upstream did not provide BGRA reference");
    Check(CFHD_GetImagePitch(cap.width, actual, &cap.pitch), "BGRA pitch");
    HyperCineTest::AlignedVector<uint8_t> bgra(size_t(cap.pitch) * cap.height);
    auto* native = DecodePhase0::NativeDecoder(raw);
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(kSeed);
    Check(CFHD_DecodeSample(raw, const_cast<uint8_t*>(sample.data()), sample.size(), bgra.data(), cap.pitch),
          "decode BGRA frame 0");
    cap.coefficient_banks = CaptureBands(native);
    cap.bgra[0] = HashImageRows(bgra.data(), cap.pitch, cap.width * 4, cap.height);
    ReconstructSampleFrameToBuffer(native, 1, bgra.data(), cap.pitch);
    cap.bgra[1] = HashImageRows(bgra.data(), cap.pitch, cap.width * 4, cap.height);

    void* raw_yuv = nullptr; Check(CFHD_OpenDecoder(&raw_yuv, nullptr), "open YU64 reference decoder");
    DecodePhase0::DecoderHandle yuv_decoder(raw_yuv);
    int yuv_w = 0, yuv_h = 0; CFHD_PixelFormat yuv_format = CFHD_PIXEL_FORMAT_UNKNOWN;
    Check(CFHD_PrepareToDecode(raw_yuv, 0, 0, CFHD_PIXEL_FORMAT_YU64, CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(),
        &yuv_w, &yuv_h, &yuv_format), "prepare YU64 reference");
    if (yuv_w != cap.width || yuv_h != cap.height || yuv_format != CFHD_PIXEL_FORMAT_YU64)
        throw std::runtime_error("unexpected YU64 reference geometry/format");
    int32_t yuv_pitch = 0; Check(CFHD_GetImagePitch(yuv_w, yuv_format, &yuv_pitch), "YU64 pitch");
    HyperCineTest::AlignedVector<uint8_t> yuv(size_t(yuv_pitch) * yuv_h);
    auto* native_yuv = DecodePhase0::NativeDecoder(raw_yuv);
    native_yuv->cfhddata.cpu_limit = 1;
    native_yuv->thread_cntrl.limit = 1;
    native_yuv->thread_cntrl.set_thread_params = 1;
    native_yuv->thread_cntrl.capabilities = (native_yuv->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(kSeed);
    Check(CFHD_DecodeSample(raw_yuv, const_cast<uint8_t*>(sample.data()), sample.size(), yuv.data(), yuv_pitch),
          "decode YU64 frame 0");
    cap.yu64[0] = HashYU64(yuv.data(), yuv_pitch, yuv_w, yuv_h);
    ReconstructSampleFrameToBuffer(native_yuv, 1, yuv.data(), yuv_pitch);
    cap.yu64[1] = HashYU64(yuv.data(), yuv_pitch, yuv_w, yuv_h);
    return cap;
}

std::string StringArray(const std::vector<std::string>& v) {
    std::ostringstream o; o << '[';
    for (size_t i = 0; i < v.size(); ++i) { if (i) o << ','; o << Quote(v[i]); }
    o << ']'; return o.str();
}
void WriteArray(const std::filesystem::path& path, const std::vector<std::string>& rows) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << "{\"license\":\"MIT OR Apache-2.0\",\"data\":[\n";
    for (size_t i = 0; i < rows.size(); ++i) out << rows[i] << (i + 1 == rows.size() ? "\n" : ",\n");
    out << "]}\n";
}
std::string ProfileRecord(const std::string& id, const Encoded& e, int quality,
                          const char* quality_name, bool interlaced) {
    const auto facts = InspectChunks(e.sample);
    std::ostringstream o;
    o << "{\"id\":" << Quote(id) << ",\"width\":64,\"height\":64,\"frames_per_sample\":2"
      << ",\"profile\":" << Quote(interlaced ? "FIELDPLUS_interlaced" : "FIELDPLUS_progressive")
      << ",\"quality\":" << Quote(quality_name) << ",\"quality_value\":" << quality
      << ",\"encoder_flags\":" << (2 | (interlaced ? 1 : 0))
      << ",\"source_fnv1a64\":" << Quote(e.source_hash)
      << ",\"sample_bytes\":" << e.sample.size() << ",\"chunk_count\":" << facts.chunk_count
      << ",\"index255_count\":" << facts.index255_count
      << ",\"sentinel_channels\":" << StringArray(facts.sentinels)
      << ",\"difference_bands\":" << StringArray(facts.difference)
      << ",\"codebooks\":" << StringArray(facts.codebooks)
      << ",\"peak_bands\":" << StringArray(facts.peaks) << ",\"chunks\":[";
    for (size_t i = 0; i < facts.records.size(); ++i) { if (i) o << ','; o << facts.records[i]; }
    o << "]}";
    return o.str();
}
std::string ReferenceRecord(const std::string& id, const std::string& profile, const std::string& file,
                            const Bytes& sample, const FrameCapture& c) {
    std::ostringstream o;
    o << "{\"id\":" << Quote(id) << ",\"profile\":" << Quote(profile)
      << ",\"sample_path\":" << Quote(file)
      << ",\"sample_fnv1a64\":" << Quote(Hex(HashAdd(kFnvOffset, sample.data(), sample.size())))
      << ",\"sample_bytes\":" << sample.size() << ",\"width\":" << c.width << ",\"height\":" << c.height
      << ",\"frames_per_sample\":2,\"oracle\":\"unchanged upstream CPU decoder; frame 0 public decode, frame 1 internal frame-index renderer\""
      << ",\"hash\":\"FNV-1a 64-bit over active rows/words; seed 14695981039346656037\""
      << ",\"frames\":[";
    for (int f = 0; f < 2; ++f) {
        if (f) o << ',';
        o << "{\"frame_index\":" << f << ",\"bgra_fnv1a64\":" << Quote(c.bgra[f])
          << ",\"yu64_deinterleaved_output_words\":{\"y_fnv1a64\":" << Quote(c.yu64[f].y)
          << ",\"u_fnv1a64\":" << Quote(c.yu64[f].u) << ",\"v_fnv1a64\":" << Quote(c.yu64[f].v) << "}}";
    }
    o << "],\"signed_coefficient_bands_after_frame0_decode\":[" << c.coefficient_banks << "]}";
    return o.str();
}

Bytes ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    in.seekg(0, std::ios::end); const auto n = in.tellg(); in.seekg(0, std::ios::beg);
    if (n <= 0) throw std::runtime_error("empty file " + p.string());
    Bytes b(static_cast<size_t>(n), uint8_t{}); in.read(reinterpret_cast<char*>(b.data()), n);
    if (!in) throw std::runtime_error("failed reading " + p.string());
    return b;
}

void Collect(const std::filesystem::path& root, const std::filesystem::path& outdir) {
    std::filesystem::create_directories(outdir);
    std::vector<std::string> profiles;
    const int qualities[] = {CFHD_ENCODING_QUALITY_LOW, CFHD_ENCODING_QUALITY_MEDIUM,
        CFHD_ENCODING_QUALITY_HIGH, CFHD_ENCODING_QUALITY_FILMSCAN1, CFHD_ENCODING_QUALITY_FILMSCAN3};
    const char* qnames[] = {"Low", "Medium", "High", "FilmScan1", "FilmScan3"};
    for (int interlaced = 0; interlaced < 2; ++interlaced) for (int qi = 0; qi < 5; ++qi) {
        const auto e = EncodePair(64, 64, interlaced != 0, qualities[qi], interlaced ? 3 : 0);
        const std::string profile_name = std::string(interlaced ? "interlaced_" : "progressive_") + qnames[qi];
        { std::ofstream f(outdir / (profile_name + ".cfhd"), std::ios::binary); f.write(reinterpret_cast<const char*>(e.sample.data()), std::streamsize(e.sample.size())); }
        profiles.push_back(ProfileRecord(profile_name, e, qualities[qi], qnames[qi], interlaced != 0));
    }
    WriteArray(outdir / "decode_phase3b2r_profiles.json", profiles);

    std::vector<std::string> refs;
    std::vector<std::string> canonical_chunks;
    const std::array<std::pair<const char*, const char*>, 2> committed = {{
        {"YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd", "FIELDPLUS_progressive"},
        {"YUV422-p10-ch3-fieldplus-field-f2-bands54-fmt-1.cfhd", "FIELDPLUS_interlaced"}}};
    for (const auto& item : committed) {
        const auto file = root / "tests" / "fixtures" / "phase3b0" / item.first;
        const auto sample = ReadFile(file);
        const auto chunks = InspectChunks(sample);
        std::ostringstream chunk_record;
        chunk_record << "{\"fixture\":" << Quote(item.first) << ",\"chunk_count\":" << chunks.chunk_count
                     << ",\"index255_count\":" << chunks.index255_count << ",\"chunks\":[";
        for (size_t i = 0; i < chunks.records.size(); ++i) { if (i) chunk_record << ','; chunk_record << chunks.records[i]; }
        chunk_record << "]}";
        canonical_chunks.push_back(chunk_record.str());
        refs.push_back(ReferenceRecord(item.first, item.second, item.first, sample, CaptureFrames(sample)));
    }
    WriteArray(outdir / "decode_phase3b2r_chunks.json", canonical_chunks);
    const struct { const char* id; bool interlaced; int pattern; int w; int h; int quality; } generated[] = {
        {"temporal_asymmetric_64_progressive", false, 0, 64, 64, CFHD_ENCODING_QUALITY_FILMSCAN3},
        {"motion_64_progressive", false, 1, 64, 64, CFHD_ENCODING_QUALITY_FILMSCAN3},
        {"chroma_temporal_64_progressive", false, 2, 64, 64, CFHD_ENCODING_QUALITY_FILMSCAN3},
        {"field_parity_temporal_asymmetric_64_interlaced", true, 3, 64, 64, CFHD_ENCODING_QUALITY_FILMSCAN3},
        {"motion_1080p_progressive", false, 1, 1920, 1080, CFHD_ENCODING_QUALITY_FILMSCAN3},
        {"field_parity_temporal_asymmetric_1080p_interlaced", true, 3, 1920, 1080, CFHD_ENCODING_QUALITY_FILMSCAN3},
    };
    std::vector<std::string> practical;
    for (const auto& g : generated) {
        const auto e = EncodePair(g.w, g.h, g.interlaced, g.quality, g.pattern);
        const std::string filename = std::string(g.id) + ".cfhd";
        { std::ofstream f(outdir / filename, std::ios::binary); f.write(reinterpret_cast<const char*>(e.sample.data()), std::streamsize(e.sample.size())); }
        const auto facts = InspectChunks(e.sample);
        Bytes reference_sample = e.sample;
        std::string reference_path = filename;
        if (g.w == 64) {
            const auto frozen = root / "tests" / "fixtures" / "phase3b2r" / filename;
            if (std::filesystem::exists(frozen)) {
                reference_sample = ReadFile(frozen);
                reference_path = (std::filesystem::path("tests") / "fixtures" / "phase3b2r" / filename).generic_string();
            }
        }
        const auto capture = CaptureFrames(reference_sample);
        refs.push_back(ReferenceRecord(g.id, g.interlaced ? "FIELDPLUS_interlaced" : "FIELDPLUS_progressive",
                                       reference_path, reference_sample, capture));
        std::ostringstream p;
        p << "{\"id\":" << Quote(g.id) << ",\"source_fnv1a64\":" << Quote(e.source_hash)
          << ",\"sample_fnv1a64\":" << Quote(Hex(HashAdd(kFnvOffset, e.sample.data(), e.sample.size())))
          << ",\"sample_bytes\":" << e.sample.size() << ",\"quality\":\"FilmScan3\",\"encoder_flags\":"
          << (2 | (g.interlaced ? 1 : 0)) << ",\"width\":" << g.w << ",\"height\":" << g.h
          << ",\"chunk_count\":" << facts.chunk_count << ",\"index255_count\":" << facts.index255_count
          << ",\"generation_recipe\":\"tests/test_decode_phase3b2r.cpp MakeYUY2; seed 0x1337BEEF; mode " << g.pattern << "\"}";
        practical.push_back(p.str());
    }
    WriteArray(outdir / "decode_phase3b2r_reference_frames.json", refs);
    WriteArray(outdir / "decode_phase3b2r_practical_fixtures.json", practical);
    std::cout << "wrote 10 quality probes, " << refs.size() << " two-frame reference records\n";
}

void RunReferenceSmoke(const std::filesystem::path& root) {
    struct Expected {
        const char* id;
        const char* file;
        const char* bgra0; const char* bgra1;
        const char* y0; const char* y1;
        const char* u0; const char* u1;
        const char* v0; const char* v1;
    };
    const Expected expected[] = {
        {"canonical_progressive", "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd",
         "27fd4d3948775018", "c06ad9438de588c8", "f8d081ce4f968148", "f8d081ce4f968148",
         "8071e98780828f2a", "e713bfb28771f014", "05b591c79dc59cd2", "0386a250bf0f862e"},
        {"canonical_interlaced", "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-field-f2-bands54-fmt-1.cfhd",
         "a55d736aa8b533be", "8613ce8519d1140f", "161ba387c32af215", "161ba387c32af215",
         "9bf753e04bb342a5", "5a3c5e963a99aff9", "6e34e3d830484395", "31f0444024493035"},
        {"temporal_asymmetric", "tests/fixtures/phase3b2r/temporal_asymmetric_64_progressive.cfhd",
         "33efa9b37256d867", "1b7d61cb2c78d5c2", "a7c823ef96962777", "72edd541ee2cf7eb",
         "16673fae27b8eee2", "45ea8a45ede93c6f", "8768f162f4d2d318", "f77d4ff1219565a8"},
        {"motion", "tests/fixtures/phase3b2r/motion_64_progressive.cfhd",
         "1adc1719767c4a0f", "bc81f05b2cb3c5ab", "6ffa7a7a10aea0f1", "f0ad2e3fdb7222c2",
         "e57117561ca36213", "89dc1b557f883872", "3a752fe4b3f6879e", "7e2c7d4ba9678f05"},
        {"chroma_temporal", "tests/fixtures/phase3b2r/chroma_temporal_64_progressive.cfhd",
         "87278ff765c7ffd1", "21f33d8701b527ac", "352c12b22e166ad7", "352c12b22e166ad7",
         "e245c5f1e66bcd03", "039532240a81d2e0", "78bcb7265cda2f7a", "c61c588937e1454a"},
        {"field_temporal_asymmetric", "tests/fixtures/phase3b2r/field_parity_temporal_asymmetric_64_interlaced.cfhd",
         "318af04450489f9e", "140a3f3d6432f062", "a16df92e2981bee5", "795aceeb8d86db65",
         "55d9b90fc74b53b8", "011e81c903d3cac4", "016b768aae3b4d23", "5073e5d5e6a5a594"},
    };
    for (const auto& e : expected) {
        const auto sample = ReadFile(root / std::filesystem::path(e.file));
        const auto got = CaptureFrames(sample);
        const bool pass = got.width == 64 && got.height == 64 && got.bgra[0] == e.bgra0 && got.bgra[1] == e.bgra1 &&
            got.yu64[0].y == e.y0 && got.yu64[1].y == e.y1 &&
            got.yu64[0].u == e.u0 && got.yu64[1].u == e.u1 &&
            got.yu64[0].v == e.v0 && got.yu64[1].v == e.v1;
        if (!pass) throw std::runtime_error(std::string("upstream frame oracle mismatch: ") + e.id);
        std::cout << e.id << " frame0/frame1 BGRA and Y/U/V hashes passed\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--math") { RunMathTests(); return 0; }
        if (argc == 3 && std::string(argv[1]) == "--reference-smoke") {
            RunReferenceSmoke(std::filesystem::path(argv[2])); return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--collect") {
            Collect(std::filesystem::path(argv[2]), std::filesystem::path(argv[3])); return 0;
        }
        std::cerr << "usage: test_decode_phase3b2r --math | --reference-smoke <repo-root> | --collect <repo-root> <output-dir>\n";
        return 2;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
