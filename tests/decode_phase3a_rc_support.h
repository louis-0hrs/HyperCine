// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "decoder_support.h"
#include "decode_phase1_support.h"
#include "../src/core/decoder_backend.h"
#include <fstream>
#include <filesystem>

namespace DecodeRC {
using namespace ProductionDecodeTest;
using namespace HyperCine::DecodePhase1;
// Predeclared out-of-sample generator. Neither the Phase 0 generators nor their
// seeds are used here. Families and settings must not be changed after timing.
constexpr uint32_t HoldoutSeed = 0x6A09E667u;
inline const char* Family(int p) {
    static const char* names[] = {"foliage_proxy", "water_proxy", "architecture_graphics",
        "text_ui", "cgi_geometry", "dark_noise", "clean_sky", "heavy_grain",
        "curved_gradient", "diagonal_edges", "woven_fabric", "skin_proxy",
        "mixed_detail", "motion_blur_proxy", "low_quality_detail"};
    return names[p];
}
inline uint32_t Noise(int x, int y, uint32_t seed) {
    uint32_t n = uint32_t(x) * 0x9E3779B9u ^ uint32_t(y) * 0x85EBCA6Bu ^ seed;
    n ^= n >> 16; n *= 0x7FEB352Du; n ^= n >> 15; n *= 0x846CA68Bu;
    return n ^ (n >> 16);
}
inline int Triangle(int x, int period) {
    int q = x % period; if (q < 0) q += period;
    return 510 * (std::min)(q, period - q) / period;
}
inline int Luma(int x, int y, int w, int h, int p) {
    const auto seed = HoldoutSeed + uint32_t(p) * 0x1020304u;
    const int n = int(Noise(x, y, seed) >> 24);
    int v = 0;
    switch (p) {
    case 0: v = 28 + int(Noise(x / 5, y / 9, seed) % 120) + n / 4; break;
    case 1: v = 45 + Triangle(x + y * 3, 173) / 3 + Triangle(x * 3 - y, 29) / 4; break;
    case 2: v = (x % 47 < 2 || y % 31 < 2) ? 25 : 110 + int(Noise(x / 47, y / 31, seed) % 70); break;
    case 3: v = y % 96 < 3 || x % 320 < 3 ? 90 :
        (y % 96 > 20 && y % 96 < 42 && ((Noise(x / 7, y / 7, seed) >> (x % 7)) & 1)) ? 235 : 20; break;
    case 4: v = 30 + int(((int64_t(x - w / 2) * (x - w / 2) + int64_t(y - h / 2) * (y - h / 2)) / 127) % 180); break;
    case 5: v = 4 + n / 8; break;
    case 6: v = 145 + y * 65 / h + Triangle(x + y, 1280) / 64; break;
    case 7: v = 45 + Triangle(x + y, 997) / 3 + n / 2; break;
    case 8: v = 25 + int((int64_t(x) * x * 90 / (int64_t(w) * w)) +
                        (int64_t(y) * y * 90 / (int64_t(h) * h))); break;
    case 9: v = ((x * 7 + y * 11) / 97 % 3) == 0 ? 235 : 16; break;
    case 10: v = 40 + Triangle(x, 9) / 3 + Triangle(y, 13) / 3 + n / 16; break;
    case 11: v = 85 + Triangle(x, w) / 3 + Triangle(y, h) / 5 + n / 24; break;
    case 12: v = Luma(x, y, w, h, (x < w / 2 ? (y < h / 2 ? 3 : 6) : (y < h / 2 ? 0 : 7))); break;
    case 13: {
        int sum = 0; for (int k = -4; k <= 4; ++k) sum += int(Noise(x + k, y, seed) >> 24);
        v = 25 + sum * 3 / 36; break;
    }
    default: v = 16 + int(Noise(x / 3, y / 2, seed) % 220); break;
    }
    return (std::max)(0, (std::min)(255, v));
}
inline Bytes HoldoutInput(int w, int h, int p) {
    Bytes input(size_t(w) * h * 2);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        const size_t i = (size_t(y) * w + x) * 2;
        input[i] = uint8_t(Luma(x, y, w, h, p));
        input[i + 2] = uint8_t(Luma(x + 1, y, w, h, p));
        input[i + 1] = uint8_t(96 + Triangle(x + y * 2, 301) / 4);
        input[i + 3] = uint8_t(96 + Triangle(x * 2 - y, 457) / 4);
    }
    return input;
}
inline Sample EncodeInput(Bytes& input, int w, int h, CFHD_PixelFormat format,
                          CFHD_EncodingQuality quality, int pattern,
                          CFHD_EncodingFlags flags = CFHD_ENCODING_FLAGS_NONE) {
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr), "RC encoder open"); Encoder owner(raw);
    Check(CFHD_PrepareToEncode(raw, w, h, format, CFHD_ENCODED_FORMAT_YUV_422,
          flags, quality), "RC progressive prepare");
    const auto source_hash = Hash(input.data(), input.size());
    std::srand(HoldoutSeed);
    Check(CFHD_EncodeSample(raw, input.data(), w * (format == CFHD_PIXEL_FORMAT_YUY2 ? 2 : 4)), "RC encode");
    void* data = nullptr; size_t size = 0; Check(CFHD_GetSampleData(raw, &data, &size), "RC sample");
    Require(data && size, "RC nonempty sample");
    Sample s{w, h, pattern, Bytes(size), source_hash};
    std::memcpy(s.compressed.data(), data, size); NormalizeMetadata(s.compressed); return s;
}
inline void Coefficients(Decoder& d, DecodeTicket t, const std::vector<DecodeBandDescriptor>& ref) {
    for (int b = 0; b < 30; ++b) {
        std::vector<int16_t> actual; int w, h, pitch;
        Require(DecoderInspector::DownloadBand(d, t, b, actual, &w, &h, &pitch), "RC band download");
        const auto& r = ref[size_t(b)]; Require(w == r.width && h == r.height, "RC band geometry");
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            if (actual[size_t(y) * (pitch / 2) + x] != r.host_data[size_t(y) * (r.pitch / 2) + x])
                throw std::runtime_error("RC coefficient mismatch band " + std::to_string(b));
    }
}
// Returns an independently decoded BGRA frame hash. A successful return proves
// all 30 bands, both intermediate LL levels, planes and BGRA for all four paths.
inline std::string Exact(Sample& s, Decoder& d) {
    ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(s);
    auto bands = ExtractBandsFromNative(cpu.GetNative());
    auto intermediate = ComputeCpuReferenceIntermediates(cpu.GetNative());
    ReferenceDecoder yu64(s, CFHD_PIXEL_FORMAT_YU64); yu64.Decode(s);
    const auto planar_reference = ComputeCpuReferenceIntermediates(yu64.GetNative());
    for (auto strategy : {DiagnosticDecodeStrategy::Lightweight, DiagnosticDecodeStrategy::Hybrid,
                          DiagnosticDecodeStrategy::Automatic, DiagnosticDecodeStrategy::HostStreaming}) {
        DecodeRequest request; request.diagnostic_strategy = strategy;
        auto t = Begin(d, s, request); Coefficients(d, t, bands);
        for (int ch = 0; ch < 3; ++ch) for (int l = 0; l < 2; ++l) {
            std::vector<int16_t> ll;
            Require(DecoderInspector::DownloadIntermediate(d, t, ch, l, ll), "RC LL download");
            Require(ll == (l ? intermediate.ll1[ch] : intermediate.ll0[ch]), "RC LL exact");
        }
        const auto bgra = Image(d, t, s);
        if (bgra != cpu.output) {
            const size_t n = std::min(bgra.size(), cpu.output.size());
            size_t bytes = 0, first = n;
            int max_abs = 0;
            for (size_t i = 0; i < n; ++i) {
                if (bgra[i] == cpu.output[i]) continue;
                if (first == n) first = i;
                ++bytes;
                const int delta = bgra[i] > cpu.output[i] ? bgra[i] - cpu.output[i] : cpu.output[i] - bgra[i];
                if (delta > max_abs) max_abs = delta;
            }
            int channel_diffs[4] = {};
            for (size_t i = 0; i < n; ++i) if (bgra[i] != cpu.output[i]) ++channel_diffs[i & 3];
            std::string around;
            for (size_t i = first; i < n && i < first + 8; ++i)
                around += std::to_string(int(bgra[i])) + "/" + std::to_string(int(cpu.output[i])) + " ";
            const int row = int(first / (size_t(s.width) * 4));
            const int px = int((first % (size_t(s.width) * 4)) / 4);
            std::string planes;
            for (int ch = 0; ch < 3; ++ch) {
                const int cw = planar_reference.plane_width[ch];
                const int sx = ch == 0 ? px : px / 2;
                const auto sample = planar_reference.plane[ch][size_t(row) * cw + sx];
                planes += std::to_string(int(sample)) + (ch == 2 ? "" : ",");
            }
            throw std::runtime_error(
                "RC BGRA exact strategy=" + std::to_string(int(strategy)) +
                " gpu=" + std::to_string(bgra.size()) + " ref=" + std::to_string(cpu.output.size()) +
                " byte_diffs=" + std::to_string(bytes) + " first=" + std::to_string(first) +
                " max_abs=" + std::to_string(max_abs) +
                " ch=" + std::to_string(channel_diffs[0]) + "," + std::to_string(channel_diffs[1]) + "," +
                std::to_string(channel_diffs[2]) + "," + std::to_string(channel_diffs[3]) +
                " around=" + around + " plane=" + planes + " xy=" + std::to_string(px) + "," + std::to_string(row));
        }
        Require(d.ReleaseOutput(t), "RC BGRA release");
        request.format = DecodePixelFormat::PlanarYUV422Signed16; t = Begin(d, s, request);
        DeviceOutput out; Require(d.GetDeviceOutput(t, &out) && out.plane_count == 3, "RC planar output");
        std::array<HyperCineTest::AlignedVector<int16_t>, 3> planes;
        for (int ch = 0; ch < 3; ++ch) {
            const auto& plane = out.planes[size_t(ch)]; planes[size_t(ch)].resize(size_t(plane.width) * plane.height);
            GpuCheck(cudaMemcpy2D(planes[size_t(ch)].data(), size_t(plane.width) * 2, plane.device_pointer,
                plane.pitch_bytes, size_t(plane.width) * 2, plane.height, cudaMemcpyDeviceToHost));
            const int native = ch == 1 ? 2 : ch == 2 ? 1 : 0;
            Require(std::equal(planes[size_t(ch)].begin(), planes[size_t(ch)].end(), planar_reference.plane[native].begin()), "RC planes exact");
        }
        Bytes packed(size_t(s.width) * s.height * 4);
        ConvertPlanarToYU64(planes[0].data(), s.width * 2, planes[2].data(), s.width,
            planes[1].data(), s.width, s.width, s.height, 10, packed.data(), s.width * 4);
        if (packed != yu64.output) {
            const size_t n = std::min(packed.size(), yu64.output.size());
            size_t words = 0, first = n;
            int max_abs = 0;
            for (size_t i = 0; i + 1 < n; i += 2) {
                const int a = int(packed[i]) | (int(packed[i + 1]) << 8);
                const int b = int(yu64.output[i]) | (int(yu64.output[i + 1]) << 8);
                if (a == b) continue;
                if (first == n) first = i;
                ++words;
                const int delta = a > b ? a - b : b - a;
                if (delta > max_abs) max_abs = delta;
            }
            const int a0 = int(packed[first]) | (int(packed[first + 1]) << 8);
            const int b0 = int(yu64.output[first]) | (int(yu64.output[first + 1]) << 8);
            throw std::runtime_error(
                "RC YU64 exact strategy=" + std::to_string(int(strategy)) +
                " packed=" + std::to_string(packed.size()) + " ref=" + std::to_string(yu64.output.size()) +
                " pitch=" + std::to_string(yu64.pitch) + " word_diffs=" + std::to_string(words) +
                " first_byte=" + std::to_string(first) + " max_abs=" + std::to_string(max_abs) +
                " packed_word=" + std::to_string(a0) + " ref_word=" + std::to_string(b0));
        }
        Require(d.ReleaseOutput(t), "RC planar release");
    }
    return Hash(cpu.output.data(), cpu.output.size());
}
} // namespace DecodeRC
