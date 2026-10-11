// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Offline frame-strategy research. This tool does not change decode results.
#include "decode_phase3ad_features.h"
#include "../tests/decode_phase3a_rc_support.h"
#include "phase3ad_frozen_candidate.inc"
#include "mp4reader.h"
#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>
namespace {
using namespace DecodeRC;
using namespace DecodePhase0;
constexpr uint32_t ResearchSeed = 0x3AD5EED1u;
constexpr uint32_t ResearchAltSeed = 0x3AD5EED2u;
constexpr uint32_t kHoldoutGeneratorSeed = 0xC0FFEE31u;
constexpr const char* kSplitProcess =
    "Predeclared 2026-10-11 in benchmark_decode_phase3ad.cpp before any oracle timing. "
    "Development holds the Phase 3A corpus, every supported Phase 3A-RC case, and new research-generator 1080p samples. "
    "Validation holds unobserved research resolutions, an alternate seed, validation-only families, new 8K Phase 3A patterns, and real re-encoded photographs when the manifest exists. "
    "Hold-out identities are compiled separately and are not encoded in dev-val mode.";
enum class Kind { Phase3A, Rc, External, ExternalRgb, Research, Holdout, Photo };
struct Spec {
    std::string id, family, path, license, source;
    const char* split;
    const char* origin;
    const char* generator;
    const char* sibling;
    int w = 0, h = 0, pattern = 0;
    CFHD_EncodingQuality quality = CFHD_ENCODING_QUALITY_FILMSCAN3;
    uint32_t seed = 0;
    bool previously = false;
    Kind kind = Kind::Research;
    unsigned frame = 0;
};
struct Timed { double mean = 0, minimum = 1e9, maximum = 0, cpu = 0; };
uint32_t MixA(uint32_t n) { n ^= n >> 16; n *= 0x7FEB352Du; n ^= n >> 15; n *= 0x846CA68Bu; return n ^ (n >> 16); }
uint32_t MixB(uint32_t n) { n *= 0x9E3779B1u; n ^= n >> 13; n *= 0x85EBCA77u; n ^= n >> 16; return n; }
int Clamp8(int v) { return (std::max)(0, (std::min)(255, v)); }
const char* ResearchFamily(int p) {
    static const char* names[] = {"grass", "clouds", "saturated_chroma", "shadows", "high_contrast", "repeating_pattern",
        "sparse_graphics", "defocus", "fine_textile", "sensor_noise", "skin_pores", "geometric_lines",
        "halftone", "bokeh", "window_blinds", "starfield"};
    return names[p];
}
const char* HoldoutFamilyName(int p) {
    static const char* names[] = {"brick_masonry", "ocean_horizon", "night_traffic", "paper_fiber", "cartoon_flat", "fur",
        "circuit_board", "fog_layers", "stained_glass", "handwriting", "rain_streaks", "marble"};
    return names[p];
}
const char* QualityName(CFHD_EncodingQuality q) {
    if (q == CFHD_ENCODING_QUALITY_LOW) return "low";
    if (q == CFHD_ENCODING_QUALITY_MEDIUM) return "medium";
    if (q == CFHD_ENCODING_QUALITY_HIGH) return "high";
    if (q == CFHD_ENCODING_QUALITY_FILMSCAN1) return "filmscan1";
    if (q == CFHD_ENCODING_QUALITY_FILMSCAN2) return "filmscan2";
    if (q == CFHD_ENCODING_QUALITY_FILMSCAN3) return "filmscan3";
    return "other";
}
int RunsFor(int w) { return w >= 7680 ? 30 : w >= 3840 ? 60 : 120; }
std::string Escape(std::string s) {
    std::string out; out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}
int64_t ExpectedHighpass(const std::string& id) {
    static const std::pair<const char*, int64_t> table[] = {
        {"CFHDsample422.AVI:frame0", 570820}, {"CFHDsample422.AVI:frame12", 210784}, {"CFHDsample422.AVI:frame23", 170220},
        {"1920_foliage_proxy", 3007408}, {"1920_water_proxy", 1289552}, {"1920_architecture_graphics", 837984},
        {"1920_text_ui", 1187472}, {"1920_cgi_geometry", 1327480}, {"1920_dark_noise", 2152900}, {"1920_clean_sky", 185548},
        {"1920_heavy_grain", 3519508}, {"1920_curved_gradient", 233952}, {"1920_diagonal_edges", 2252664},
        {"1920_woven_fabric", 2212948}, {"1920_skin_proxy", 1359328}, {"1920_mixed_detail", 1998776},
        {"1920_motion_blur_proxy", 2264888}, {"1920_low_quality_detail", 1154088}, {"3840_foliage_proxy", 12023456},
        {"3840_water_proxy", 5152076}, {"3840_architecture_graphics", 3369316}, {"3840_text_ui", 4799992},
        {"3840_cgi_geometry", 8382864}, {"3840_dark_noise", 8606708}, {"3840_clean_sky", 698660},
        {"3840_heavy_grain", 14072192}, {"3840_curved_gradient", 786456}, {"3840_diagonal_edges", 9005108},
        {"3840_woven_fabric", 8849292}, {"3840_skin_proxy", 5421976}, {"3840_mixed_detail", 7900548},
        {"3840_motion_blur_proxy", 9055384}, {"3840_low_quality_detail", 4613356}, {"7680_clean_sky", 2717076},
        {"7680_curved_gradient", 2895484}};
    for (const auto& row : table) if (id == row.first) return row.second;
    return -1;
}
Bytes FillPattern(int w, int h, int pattern, uint32_t seed, bool holdout) {
    Bytes out(size_t(w) * h * 2);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        auto luma = [&](int px) {
            const uint32_t n = holdout ? MixB(uint32_t(px) * 0x27BB2EE6u ^ uint32_t(y) * 0x165667B1u ^ seed ^ uint32_t(pattern) * 0x9E3779B9u)
                                       : MixA(uint32_t(px) * 0x45D9F3Bu ^ uint32_t(y) * 0x119DE1F3u ^ seed ^ uint32_t(pattern) * 0x85EBCA6Bu);
            const int noise = int(n >> 24);
            int v = 128;
            if (!holdout) {
                switch (pattern) {
                case 0: v = 46 + y * 50 / h + (px % 5 == 0 ? 78 : 0) + noise / 8; break;
                case 1: v = 168 + int(MixA(uint32_t(px / 37) ^ uint32_t(y / 29) * 19u ^ seed) >> 27) + noise / 16; break;
                case 2: v = 150 + noise / 20; break;
                case 3: v = (px * 3 < w * 2 && y * 3 > h) ? 12 + noise / 12 : 188 + noise / 18; break;
                case 4: v = (px % 96 < 2 || y % 64 < 2) ? 245 : 8; break;
                case 5: v = ((px / 6 + y / 6) & 1) ? 210 : 36; break;
                case 6: v = ((px * 8 / w) == 2 && (y * 6 / h) == 1) || ((px * 7 / w) == 5 && (y * 5 / h) > 2) ? 236 : 28; break;
                case 7: {
                    int sum = 0; for (int k = -2; k <= 2; ++k) sum += int(MixA(uint32_t(px + k * 3) ^ uint32_t(y) * 17u ^ seed) >> 24);
                    v = 96 + sum / 12; break;
                }
                case 8: v = 104 + (px % 4) * 8 + (y % 3) * 6 + noise / 18; break;
                case 9: v = 108 + (noise % 41) - 20; break;
                case 10: {
                    const int64_t dx = px - w / 2, dy = y - h / 2;
                    v = 150 - int((dx * dx / w + dy * dy / h) / 24) + noise / 28; break;
                }
                case 11: v = ((px * 9 / (y / 8 + 12)) & 3) == 0 || ((px + y) % 140) < 2 ? 230 : 42; break;
                case 12: {
                    const int dx = px % 16 - 8, dy = y % 16 - 8;
                    v = dx * dx + dy * dy < 6 + y * 12 / h ? 24 : 214; break;
                }
                case 13: {
                    v = 18;
                    const int centers[][3] = {{w / 5, h / 3, w / 14}, {w / 2, h / 2, w / 10}, {3 * w / 4, 2 * h / 3, w / 16}};
                    for (const auto& c : centers) {
                        const int64_t dx = px - c[0], dy = y - c[1];
                        if (dx * dx + dy * dy < int64_t(c[2]) * c[2]) v = 230;
                    }
                    break;
                }
                case 14: v = (y % (h / 18 + 1) < 3) ? 26 : 150 + px * 50 / w; break;
                default: v = (noise >> 4) == 0 ? 250 : 10; break;
                }
            } else {
                switch (pattern) {
                case 0: {
                    const int row = y / 28, col = (x + ((row & 1) ? 20 : 0)) / 56;
                    v = (y % 28 < 3 || (x + ((row & 1) ? 20 : 0)) % 56 < 3) ? 70 : 120 + int(MixB(uint32_t(col) * 13u ^ uint32_t(row) * 29u ^ seed) % 50);
                    break;
                }
                case 1: v = (y < h / 3 ? 200 - y * 40 / h : 70 + int(MixB(uint32_t(px / 2) ^ uint32_t(y / 6) * 9u ^ seed) % 40)); break;
                case 2: v = 14 + ((y % 37 < 2) ? 180 : 0) + ((px % 220 < 3) ? 120 : 0) + noise / 30; break;
                case 3: v = 132 + int(MixB(uint32_t(px / 3) ^ uint32_t(y) * 3u ^ seed) % 18) + px * 20 / w; break;
                case 4: v = (px * 4 / w + y * 3 / h) % 2 ? 214 : 48; break;
                case 5: v = 70 + (px % 7 == 0 ? 40 : 0) + int(MixB(uint32_t(px) ^ uint32_t(y / 2) * 11u ^ seed) % 30); break;
                case 6: v = 12 + ((y % 48 > 20 && y % 48 < 24) || (px % 64 > 30 && px % 64 < 34) ? 160 : 0); break;
                case 7: v = 150 + y * 40 / h + noise / 40; break;
                case 8: v = 40 + ((px / (w / 7 + 1) + y / (h / 5 + 1)) & 1 ? 90 : 20) + noise / 16; break;
                case 9: v = (MixB(uint32_t(y) * 97u ^ seed) % 17 == 0 && (px % 11) < 2) ? 30 : 214; break;
                case 10: v = 100 + ((px + y / 3) % 53 < 1 ? 30 : 0) + noise / 10; break;
                default: v = 90 + int(MixB(uint32_t((px + y) / 17) ^ uint32_t(y / 23) * 5u ^ seed) % 80) + noise / 20; break;
                }
            }
            return uint8_t(Clamp8(v));
        };
        int u = 128, v = 128;
        if (!holdout) {
            switch (pattern) {
            case 0: u = 96; v = 112; break;
            case 2: u = x * 4 < w ? 24 : 230; v = y * 4 < h ? 24 : 230; break;
            case 3: u = 120; v = x * 3 < w * 2 ? 150 : 110; break;
            case 6: u = 128; v = 128; break;
            case 10: u = 116; v = 146; break;
            case 13: u = 150; v = 96; break;
            default: u = 128 + (pattern - 8) * 2; v = 128 - (pattern - 8); break;
            }
        } else {
            switch (pattern) {
            case 0: u = 118; v = 138; break;
            case 1: u = 108; v = 120; break;
            case 2: u = y % 37 < 2 ? 90 : 140; v = 128; break;
            case 4: u = 80; v = 170; break;
            case 5: u = 124; v = 132; break;
            case 8: u = 100 + (x / (w / 7 + 1)) * 8; v = 150; break;
            default: u = 128; v = 128; break;
            }
        }
        const size_t i = (size_t(y) * w + x) * 2;
        out[i] = luma(x); out[i + 2] = luma(x + 1); out[i + 1] = uint8_t(Clamp8(u)); out[i + 3] = uint8_t(Clamp8(v));
    }
    return out;
}
Sample EncodeRaw(Bytes& input, int w, int h, int pattern, CFHD_EncodingQuality quality, uint32_t seed) {
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr), "phase3ad encoder open"); Encoder owner(raw);
    Check(CFHD_PrepareToEncode(raw, w, h, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422,
          CFHD_ENCODING_FLAGS_NONE, quality), "phase3ad prepare");
    const auto source_hash = Hash(input.data(), input.size());
    std::srand(int(seed & 0x7fffffffu));
    Check(CFHD_EncodeSample(raw, input.data(), w * 2), "phase3ad encode");
    void* data = nullptr; size_t size = 0; Check(CFHD_GetSampleData(raw, &data, &size), "phase3ad sample");
    Require(data && size, "phase3ad nonempty sample");
    Sample s{w, h, pattern, Bytes(size), source_hash};
    std::memcpy(s.compressed.data(), data, size); NormalizeMetadata(s.compressed); return s;
}
Bytes ReadExact(const std::filesystem::path& path, size_t bytes) {
    std::ifstream in(path, std::ios::binary); Require(bool(in), "phase3ad media open");
    Bytes out(bytes); in.read(reinterpret_cast<char*>(out.data()), std::streamsize(bytes));
    Require(size_t(in.gcount()) == bytes, "phase3ad media size"); return out;
}
Spec Make(std::string id, std::string family, const char* split, const char* origin, const char* generator,
          const char* sibling, int w, int h, int pattern, CFHD_EncodingQuality quality, uint32_t seed, bool seen, Kind kind) {
    Spec s; s.id = std::move(id); s.family = std::move(family); s.split = split; s.origin = origin; s.generator = generator;
    s.sibling = sibling; s.w = w; s.h = h; s.pattern = pattern; s.quality = quality; s.seed = seed; s.previously = seen; s.kind = kind;
    return s;
}
std::vector<Spec> LoadPhotos(const std::filesystem::path& manifest, const char* split_required) {
    std::vector<Spec> out; std::ifstream in(manifest); if (!in) return out;
    std::string line; std::getline(in, line);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> col; std::stringstream row(line); std::string cell;
        while (std::getline(row, cell, '\t')) col.push_back(cell);
        Require(col.size() >= 9, "phase3ad media manifest columns");
        if (col[1] != split_required) continue;
        auto spec = Make(col[0], col[2], split_required, "real_reencoded", "photograph", "none",
                         std::stoi(col[3]), std::stoi(col[4]), -1, CFHD_ENCODING_QUALITY_FILMSCAN3, 0x51A77u, false, Kind::Photo);
        spec.path = col[5]; spec.license = col[6]; spec.source = col[8] + " " + col[7];
        out.push_back(std::move(spec));
    }
    return out;
}
std::vector<Spec> BuildDevVal(const std::filesystem::path& root) {
    std::vector<Spec> specs;
    for (int size = 0; size < 3; ++size) {
        const int w = size == 0 ? 1920 : size == 1 ? 3840 : 7680;
        const int h = size == 0 ? 1080 : size == 1 ? 2160 : 4320;
        const int patterns = size == 2 ? 5 : 5;
        for (int p = 0; p < patterns; ++p) {
            const bool original = !(w == 7680 && p != 0);
            specs.push_back(Make("p3a_" + std::to_string(w) + "_" + PatternName(p), PatternName(p),
                original ? "development" : "validation", "phase3a_development", "phase3a_pattern",
                original ? "none" : "resolution", w, h, p, CFHD_ENCODING_QUALITY_FILMSCAN3, Seed, original, Kind::Phase3A));
        }
    }
    for (int p = 0; p < 5; ++p)
        specs.push_back(Make("p3a_1920_" + std::string(PatternName(p)) + "_medium", PatternName(p), "development",
            "phase3a_development", "phase3a_quality_variant", "quality", 1920, 1080, p, CFHD_ENCODING_QUALITY_MEDIUM, Seed, false, Kind::Phase3A));
    for (int size = 0; size < 3; ++size) {
        const int w = size == 0 ? 1920 : size == 1 ? 3840 : 7680;
        const int h = size == 0 ? 1080 : size == 1 ? 2160 : 4320;
        if (size == 0) {
            for (unsigned frame : {0u, 12u, 23u}) {
                auto spec = Make(std::string("CFHDsample422.AVI:frame") + std::to_string(frame), "bundled_422", "development",
                    "phase3a_rc_corpus", "upstream_avi", "none", 1920, 1080, -1, CFHD_ENCODING_QUALITY_FILMSCAN3, 0, true, Kind::External);
                spec.frame = frame; spec.source = "upstream/Example/videos/CFHDsample422.AVI"; specs.push_back(std::move(spec));
            }
            auto rgb = Make("CFHDsample444.AVI:frame0", "bundled_444", "development", "phase3a_rc_corpus", "upstream_avi",
                "none", 1920, 1080, -1, CFHD_ENCODING_QUALITY_FILMSCAN3, 0, true, Kind::ExternalRgb);
            rgb.source = "upstream/Example/videos/CFHDsample444.AVI"; specs.push_back(std::move(rgb));
        }
        for (int p = 0; p < 15; ++p) {
            if (size == 2 && p != 6 && p != 8) continue;
            const auto quality = p == 14 ? CFHD_ENCODING_QUALITY_LOW : CFHD_ENCODING_QUALITY_FILMSCAN3;
            specs.push_back(Make(std::to_string(w) + "_" + Family(p), Family(p), size == 0 ? "development" : "development",
                "phase3a_rc_corpus", "rc_proxy", "none", w, h, p, quality, 0x6A09E667u, true, Kind::Rc));
        }
    }
    for (int p = 0; p < 12; ++p)
        specs.push_back(Make("r1_1920_" + std::string(ResearchFamily(p)) + "_filmscan3", ResearchFamily(p), "development",
            "research_generator", "research_v1", "none", 1920, 1080, p, CFHD_ENCODING_QUALITY_FILMSCAN3, ResearchSeed, false, Kind::Research));
    for (int p : {0, 2, 4, 6, 8, 10})
        specs.push_back(Make("r1_1920_" + std::string(ResearchFamily(p)) + "_medium", ResearchFamily(p), "development",
            "research_generator", "research_v1", "quality", 1920, 1080, p, CFHD_ENCODING_QUALITY_MEDIUM, ResearchSeed, false, Kind::Research));
    for (int p = 0; p < 12; ++p)
        specs.push_back(Make("r1_3840_" + std::string(ResearchFamily(p)) + "_filmscan3", ResearchFamily(p), "validation",
            "research_generator", "research_v1", "resolution", 3840, 2160, p, CFHD_ENCODING_QUALITY_FILMSCAN3, ResearchSeed, false, Kind::Research));
    for (int p = 0; p < 12; ++p)
        specs.push_back(Make("r1b_1920_" + std::string(ResearchFamily(p)) + "_filmscan3", ResearchFamily(p), "validation",
            "research_generator", "research_v1_alt_seed", "seed", 1920, 1080, p, CFHD_ENCODING_QUALITY_FILMSCAN3, ResearchAltSeed, false, Kind::Research));
    for (int p = 12; p < 16; ++p) {
        specs.push_back(Make("r1v_1920_" + std::string(ResearchFamily(p)) + "_filmscan3", ResearchFamily(p), "validation",
            "research_generator", "research_v1_validation_family", "none", 1920, 1080, p, CFHD_ENCODING_QUALITY_FILMSCAN3, ResearchSeed, false, Kind::Research));
        specs.push_back(Make("r1v_1920_" + std::string(ResearchFamily(p)) + "_filmscan1", ResearchFamily(p), "validation",
            "research_generator", "research_v1_validation_family", "quality", 1920, 1080, p, CFHD_ENCODING_QUALITY_FILMSCAN1, ResearchSeed, false, Kind::Research));
        specs.push_back(Make("r1v_3840_" + std::string(ResearchFamily(p)) + "_filmscan3", ResearchFamily(p), "validation",
            "research_generator", "research_v1_validation_family", "resolution", 3840, 2160, p, CFHD_ENCODING_QUALITY_FILMSCAN3, ResearchSeed, false, Kind::Research));
    }
    auto photos = LoadPhotos(root / "benchmarks/phase3ad_media/validation_manifest.tsv", "validation");
    specs.insert(specs.end(), photos.begin(), photos.end());
    std::stable_sort(specs.begin(), specs.end(), [](const Spec& a, const Spec& b) {
        if (a.h != b.h) return a.h < b.h; if (a.w != b.w) return a.w < b.w; return a.id < b.id;
    });
    return specs;
}
std::vector<Spec> BuildHoldout(const std::filesystem::path& root) {
    std::vector<Spec> specs;
    for (int p = 0; p < 12; ++p) {
        specs.push_back(Make("h2_1920_" + std::string(HoldoutFamilyName(p)) + "_filmscan3", HoldoutFamilyName(p), "holdout",
            "holdout_generator", "holdout_v2", "none", 1920, 1080, p, CFHD_ENCODING_QUALITY_FILMSCAN3, kHoldoutGeneratorSeed, false, Kind::Holdout));
        specs.push_back(Make("h2_3840_" + std::string(HoldoutFamilyName(p)) + "_filmscan3", HoldoutFamilyName(p), "holdout",
            "holdout_generator", "holdout_v2", "resolution", 3840, 2160, p, CFHD_ENCODING_QUALITY_FILMSCAN3, kHoldoutGeneratorSeed, false, Kind::Holdout));
        if ((p % 2) == 0)
            specs.push_back(Make("h2_1920_" + std::string(HoldoutFamilyName(p)) + "_filmscan1", HoldoutFamilyName(p), "holdout",
                "holdout_generator", "holdout_v2", "quality", 1920, 1080, p, CFHD_ENCODING_QUALITY_FILMSCAN1, kHoldoutGeneratorSeed, false, Kind::Holdout));
    }
    for (int p : {1, 7})
        specs.push_back(Make("h2_7680_" + std::string(HoldoutFamilyName(p)) + "_filmscan3", HoldoutFamilyName(p), "holdout",
            "holdout_generator", "holdout_v2", "resolution", 7680, 4320, p, CFHD_ENCODING_QUALITY_FILMSCAN3, kHoldoutGeneratorSeed, false, Kind::Holdout));
    auto photos = LoadPhotos(root / "benchmarks/phase3ad_media/holdout_manifest.tsv", "holdout");
    specs.insert(specs.end(), photos.begin(), photos.end());
    std::stable_sort(specs.begin(), specs.end(), [](const Spec& a, const Spec& b) {
        if (a.h != b.h) return a.h < b.h; if (a.w != b.w) return a.w < b.w; return a.id < b.id;
    });
    return specs;
}
struct AviOwner {
    void* handle = nullptr; uint32_t* payload = nullptr;
    explicit AviOwner(const std::filesystem::path& path) {
        std::string name = path.string(); handle = OpenAVISource(name.data(), AVI_TRAK_TYPE, AVI_TRAK_SUBTYPE);
        Require(handle != nullptr, "phase3ad avi");
    }
    ~AviOwner() { if (payload) FreePayload(payload); if (handle) CloseSource(handle); }
};
Sample LoadExternal(const std::filesystem::path& root, const Spec& spec) {
    const bool rgb = spec.kind == Kind::ExternalRgb;
    AviOwner avi(root / (rgb ? "upstream/Example/videos/CFHDsample444.AVI" : "upstream/Example/videos/CFHDsample422.AVI"));
    Require(spec.frame < GetNumberPayloads(avi.handle), "phase3ad avi frame");
    avi.payload = GetPayload(avi.handle, avi.payload, spec.frame);
    const auto size = GetPayloadSize(avi.handle, spec.frame); Require(avi.payload && size, "phase3ad avi payload");
    Sample sample{spec.w, spec.h, -1, Bytes(size), {}}; std::memcpy(sample.compressed.data(), avi.payload, size); return sample;
}
Sample Materialize(const std::filesystem::path& root, const Spec& spec, std::map<int, std::vector<Sample>>& phase3a) {
    if (spec.kind == Kind::External || spec.kind == Kind::ExternalRgb) return LoadExternal(root, spec);
    if (spec.kind == Kind::Phase3A && spec.quality == CFHD_ENCODING_QUALITY_FILMSCAN3) {
        auto& cached = phase3a[spec.w];
        if (cached.empty()) cached = Generate(spec.w, spec.h, 5);
        return cached[size_t(spec.pattern)];
    }
    if (spec.kind == Kind::Phase3A) {
        auto input = MakeInput(spec.w, spec.h, spec.pattern);
        return EncodeRaw(input, spec.w, spec.h, spec.pattern, spec.quality, spec.seed);
    }
    if (spec.kind == Kind::Rc) {
        auto input = HoldoutInput(spec.w, spec.h, spec.pattern);
        return EncodeInput(input, spec.w, spec.h, CFHD_PIXEL_FORMAT_YUY2, spec.quality, spec.pattern);
    }
    if (spec.kind == Kind::Photo) {
        auto input = ReadExact(root / spec.path, size_t(spec.w) * spec.h * 2);
        return EncodeRaw(input, spec.w, spec.h, 0, spec.quality, spec.seed);
    }
    auto input = FillPattern(spec.w, spec.h, spec.pattern, spec.seed, spec.kind == Kind::Holdout);
    return EncodeRaw(input, spec.w, spec.h, spec.pattern, spec.quality, spec.seed);
}
Timed Measure(Decoder& d, const Sample& s, DiagnosticDecodeStrategy strategy, int count) {
    DecodeRequest request; request.diagnostic_strategy = strategy;
    auto decode = [&] { const auto t = Begin(d, s, request); Require(d.ReleaseOutput(t), "phase3ad release"); };
    for (int i = 0; i < 5; ++i) decode();
    Timed result; const double cpu = CpuMs();
    for (int i = 0; i < count; ++i) {
        const auto start = Clock::now(); decode();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        result.mean += ms; result.minimum = (std::min)(result.minimum, ms); result.maximum = (std::max)(result.maximum, ms);
    }
    result.mean /= count; result.cpu = (CpuMs() - cpu) / count; return result;
}
void WriteFeatures(std::ostream& json, const Phase3adFeatures& f) {
    json << "{\"band_count\":" << f.band_count
         << ",\"l0_share\":" << f.l0_share << ",\"l1_share\":" << f.l1_share << ",\"l2_share\":" << f.l2_share
         << ",\"l0_over_l1\":" << f.l0_over_l1 << ",\"l0_over_l2\":" << f.l0_over_l2 << ",\"l1_over_l2\":" << f.l1_over_l2
         << ",\"y_share\":" << f.y_share << ",\"u_share\":" << f.u_share << ",\"v_share\":" << f.v_share
         << ",\"chroma_share\":" << f.chroma_share << ",\"uv_asym\":" << f.uv_asym
         << ",\"top1_share\":" << f.top1_share << ",\"top3_share\":" << f.top3_share << ",\"top6_share\":" << f.top6_share
         << ",\"dense_count\":" << f.dense_count << ",\"ratio_dense_count\":" << f.ratio_dense_count
         << ",\"dense_l0\":" << f.dense_l0 << ",\"dense_l1\":" << f.dense_l1 << ",\"dense_l2\":" << f.dense_l2
         << ",\"dense_y\":" << f.dense_y << ",\"dense_chroma\":" << f.dense_chroma
         << ",\"bpco_mean\":" << f.bpco_mean << ",\"bpco_max\":" << f.bpco_max << ",\"bpco_median\":" << f.bpco_median
         << ",\"bpco_top3\":" << f.bpco_top3 << ",\"bpco_l0\":" << f.bpco_l0 << ",\"bpco_l1\":" << f.bpco_l1
         << ",\"bpco_l2\":" << f.bpco_l2 << ",\"bpco_y\":" << f.bpco_y << ",\"bpco_chroma\":" << f.bpco_chroma
         << ",\"n_bpco_ge_50\":" << f.n_bpco_ge_50 << ",\"n_bpco_ge_150\":" << f.n_bpco_ge_150
         << ",\"n_bpco_ge_400\":" << f.n_bpco_ge_400 << ",\"n_bpco_ge_800\":" << f.n_bpco_ge_800
         << ",\"n_bytes_ge_4096\":" << f.n_bytes_ge_4096 << ",\"n_bytes_ge_16384\":" << f.n_bytes_ge_16384
         << ",\"n_bytes_ge_32768\":" << f.n_bytes_ge_32768 << ",\"n_bytes_ge_65536\":" << f.n_bytes_ge_65536
         << ",\"n_bytes_ge_131072\":" << f.n_bytes_ge_131072 << ",\"n_bytes_ge_262144\":" << f.n_bytes_ge_262144
         << ",\"gini_milli\":" << f.gini_milli << ",\"hhi_permille\":" << f.hhi_permille
         << ",\"mad_permille\":" << f.mad_permille << ",\"cv_permille\":" << f.cv_permille
         << ",\"p25_share\":" << f.p25_share << ",\"p75_share\":" << f.p75_share << ",\"p90_share\":" << f.p90_share
         << ",\"l0_max_share\":" << f.l0_max_share << ",\"l1_max_share\":" << f.l1_max_share << ",\"l2_max_share\":" << f.l2_max_share
         << ",\"y_max_share\":" << f.y_max_share << ",\"chroma_max_share\":" << f.chroma_max_share
         << ",\"y_top_within\":" << f.y_top_within << ",\"chroma_top_within\":" << f.chroma_top_within
         << ",\"quant_min\":" << f.quant_min << ",\"quant_max\":" << f.quant_max << ",\"quant_mean\":" << f.quant_mean
         << ",\"cb_cs9\":" << f.cb_cs9 << ",\"cb_cs17\":" << f.cb_cs17 << ",\"cb_cs18\":" << f.cb_cs18
         << ",\"diff_count\":" << f.diff_count << ",\"active_bpco_mean\":" << f.active_bpco_mean
         << ",\"width\":" << f.width << ",\"height\":" << f.height << ",\"total_payload\":" << f.total_payload << '}';
}
void WriteTimed(std::ostream& json, const Timed& t) {
    json << "{\"mean_ms\":" << t.mean << ",\"minimum_ms\":" << t.minimum << ",\"maximum_ms\":" << t.maximum
         << ",\"process_cpu_ms_per_frame\":" << t.cpu << '}';
}
uint64_t TimeClassify(const std::vector<GpuEntropyBandDescriptor>& bands, int loops) {
    const auto start = Clock::now();
    for (int i = 0; i < loops; ++i) { DecodeFrameInfo info; ClassifyDecodeFrame(bands, info); }
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
void RunCorpus(const std::filesystem::path& root, const std::filesystem::path& output, bool holdout, const std::filesystem::path& freeze) {
    if (holdout) {
        Require(kPhase3adCandidateFrozen, "hold-out requires a frozen candidate include");
        Require(!std::filesystem::exists(output), "hold-out output already exists and must not be rerun");
        std::ifstream frozen(freeze); Require(bool(frozen), "freeze file");
        const std::string text((std::istreambuf_iterator<char>(frozen)), std::istreambuf_iterator<char>());
        Require(text.find(kPhase3adCandidateName) != std::string::npos, "freeze file does not name the compiled candidate");
        Require(text.find("\"frozen\": true") != std::string::npos || text.find("\"frozen\":true") != std::string::npos, "candidate is not frozen");
    }
    auto specs = holdout ? BuildHoldout(root) : BuildDevVal(root);
    const auto partial = output.string() + ".partial";
    std::filesystem::create_directories(output.parent_path());
    std::ofstream json(partial); Require(bool(json), "phase3ad json");
    json << std::setprecision(9) << std::boolalpha;
    cudaDeviceProp gpu{}; GpuCheck(cudaGetDeviceProperties(&gpu, 0));
    json << "{\"license\":\"MIT OR Apache-2.0\",\"phase\":\"3A-D\",\"mode\":\"" << (holdout ? "holdout" : "dev_val")
         << "\",\"gpu\":\"" << Escape(gpu.name) << "\",\"split_assignment\":\"2026-10-11\",\"split_process\":\"" << kSplitProcess
         << "\",\"candidate_name\":\"" << kPhase3adCandidateName << "\",\"candidate_frozen\":" << kPhase3adCandidateFrozen
         << ",\"timing\":\"5 warmups then 120/60/30 measured frames at 1080/4K/8K; alternating forced Lightweight and Hybrid; device-resident Queue through Ready and Release\","
         << "\"cases\":[\n";
    std::map<int, std::vector<Sample>> cache;
    std::unique_ptr<Decoder> decoder; int dw = 0, dh = 0, written = 0, supported = 0;
    uint64_t classify_ns = 0; int classify_loops = 0;
    for (size_t index = 0; index < specs.size(); ++index) {
        const auto& spec = specs[index];
        if (!decoder || dw != spec.w || dh != spec.h) {
            DecoderConfig cfg; cfg.maximum_width = spec.w; cfg.maximum_height = spec.h; cfg.slot_count = 2;
            decoder = std::make_unique<Decoder>(cfg);
            Require(decoder->Initialize() == DecodeError::None, "phase3ad decoder init");
            dw = spec.w; dh = spec.h;
        }
        auto sample = Materialize(root, spec, cache);
        std::vector<GpuEntropyBandDescriptor> bands; std::string reason; DecodeError error = DecodeError::None;
        const bool parsed = ParseCineFormSampleValidated(sample.compressed.data(), sample.compressed.size(), 24, bands, &reason, &error);
        // QueueDecode rejects samples above 32 MiB. That production limit stays frozen.
        constexpr size_t kProductionSampleLimit = 32u * 1024u * 1024u;
        const bool within_limit = sample.compressed.size() <= kProductionSampleLimit;
        const bool ok = parsed && within_limit;
        if (written++) json << ",\n";
        json << "{\"id\":\"" << spec.id << "\",\"split\":\"" << spec.split << "\",\"origin\":\"" << spec.origin
             << "\",\"family\":\"" << spec.family << "\",\"generator\":\"" << spec.generator << "\",\"sibling\":\"" << spec.sibling
             << "\",\"previously_in_phase3a_or_rc\":" << spec.previously << ",\"width\":" << spec.w << ",\"height\":" << spec.h
             << ",\"quality\":\"" << QualityName(spec.quality) << "\",\"seed\":\"" << std::hex << "0x" << spec.seed << std::dec
             << "\",\"source_identity\":\"" << Escape(spec.source) << "\",\"license\":\"" << Escape(spec.license)
             << "\",\"sample_hash\":\"" << Hash(sample.compressed.data(), sample.compressed.size())
             << "\",\"source_hash\":\"" << sample.input_hash << "\",\"sample_bytes\":" << sample.compressed.size()
             << ",\"supported\":" << ok;
        if (!parsed) {
            json << ",\"rejection\":\"" << Escape(reason) << "\"}";
            std::cout << spec.id << " unsupported " << reason << '\n' << std::flush; continue;
        }
        if (!within_limit) {
            json << ",\"rejection\":\"production decoder rejects samples above 32 MiB\"}";
            std::cout << spec.id << " unsupported sample " << sample.compressed.size() << " bytes exceeds 32 MiB\n" << std::flush; continue;
        }
        DecodeFrameInfo info; const auto baseline = ClassifyDecodeFrame(bands, info);
        const auto expected = ExpectedHighpass(spec.id);
        if (expected >= 0 && int64_t(info.highpass_bytes) != expected)
            throw std::runtime_error(spec.id + " highpass mismatch " + std::to_string(info.highpass_bytes));
        if (spec.kind == Kind::Rc) {
            const uint64_t coeffs = spec.w == 1920 ? 4082400 : spec.w == 3840 ? 16329600 : 65318400;
            if (info.coefficient_count != coeffs) throw std::runtime_error(spec.id + " coefficient mismatch");
        }
        auto features = ComputePhase3adFeatures(bands);
        Require(features.band_count == 27, "phase3ad 27 highpass bands");
        features.width = spec.w; features.height = spec.h;
        const bool compiled_baseline = kPhase3adCandidateFrozen && std::string(kPhase3adCandidateName).find("baseline") != std::string::npos;
        const auto chosen = compiled_baseline ? baseline : Phase3adFrozenCandidate(features);
        classify_ns += TimeClassify(bands, 100); classify_loops += 100; ++supported;
        json << ",\"baseline_strategy\":\"" << StrategyName(baseline) << "\",\"highpass_bytes\":" << info.highpass_bytes
             << ",\"padded_coefficients\":" << info.coefficient_count << ",\"maximum_band_bytes\":" << info.maximum_band_bytes
             << ",\"level_bytes\":[" << info.level_payload_bytes[0] << ',' << info.level_payload_bytes[1] << ',' << info.level_payload_bytes[2]
             << "],\"features\":"; WriteFeatures(json, features); json << ",\"bands\":[";
        bool first = true;
        for (const auto& band : bands) if (band.subband) {
            if (!first) json << ','; first = false;
            json << "{\"c\":" << band.channel << ",\"l\":" << band.level << ",\"b\":" << band.band
                 << ",\"bytes\":" << band.payload_bytes << ",\"coeff\":" << band.coefficient_count
                 << ",\"q\":" << band.quantization << ",\"cb\":" << band.codebook_id << ",\"diff\":" << int(band.difference_coding)
                 << ",\"w\":" << band.width << ",\"h\":" << band.height << '}';
        }
        const int runs = RunsFor(spec.w);
        Timed light, hybrid;
        try {
            if (index % 2) { light = Measure(*decoder, sample, DiagnosticDecodeStrategy::Lightweight, runs); hybrid = Measure(*decoder, sample, DiagnosticDecodeStrategy::Hybrid, runs); }
            else { hybrid = Measure(*decoder, sample, DiagnosticDecodeStrategy::Hybrid, runs); light = Measure(*decoder, sample, DiagnosticDecodeStrategy::Lightweight, runs); }
        } catch (const std::exception& ex) {
            throw std::runtime_error(spec.id + " " + ex.what());
        }
        json << "],\"measured_frames\":" << runs << ",\"lightweight\":"; WriteTimed(json, light);
        json << ",\"hybrid\":"; WriteTimed(json, hybrid);
        json << ",\"compiled_candidate_strategy\":\"" << StrategyName(chosen) << '"';
        if (holdout) {
            DecodeFrameInfo again; Require(ClassifyDecodeFrame(bands, again) == baseline, "baseline determinism");
            for (int pass = 0; pass < 100; ++pass) {
                const auto got = Phase3adFrozenCandidate(ComputePhase3adFeatures(bands));
                Require(got == chosen, "candidate determinism");
            }
            const std::string bgra = Exact(sample, *decoder);
            json << ",\"exact\":true,\"bgra_hash\":\"" << bgra << "\",\"coefficients_exact\":30,\"max_coefficient_error\":0,"
                 << "\"ll_exact\":true,\"planar_exact\":true,\"bgra_exact\":true,"
                 << "\"validated_strategies\":[\"lightweight\",\"hybrid\",\"auto\",\"host_streaming\"]";
        }
        json << '}';
        json.flush();
        std::cout << spec.split << ' ' << spec.id << " L=" << light.mean << " H=" << hybrid.mean << ' ' << StrategyName(baseline) << '\n' << std::flush;
    }
    json << "\n],\"supported_cases\":" << supported << ",\"baseline_classifier_mean_ns\":" << (classify_loops ? double(classify_ns) / classify_loops : 0)
         << ",\"status\":\"PASS\"}\n";
    json.close(); std::filesystem::rename(partial, output);
}
void Noise(const std::filesystem::path& output) {
    std::ofstream json(output); Require(bool(json), "noise json"); json << std::setprecision(9);
    cudaDeviceProp gpu{}; GpuCheck(cudaGetDeviceProperties(&gpu, 0));
    json << "{\"license\":\"MIT OR Apache-2.0\",\"gpu\":\"" << gpu.name << "\",\"cases\":[";
    bool first = true;
    for (int w : {1920, 3840}) {
        const int h = w == 1920 ? 1080 : 2160;
        auto samples = Generate(w, h, 5); DecoderConfig cfg; cfg.maximum_width = w; cfg.maximum_height = h; cfg.slot_count = 2;
        Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "noise init");
        const int runs = RunsFor(w);
        const auto hybrid = Measure(d, samples[2], DiagnosticDecodeStrategy::Hybrid, runs);
        const auto light = Measure(d, samples[2], DiagnosticDecodeStrategy::Lightweight, runs);
        if (!first) json << ','; first = false;
        json << "{\"width\":" << w << ",\"pattern\":\"noise\",\"frames\":" << runs << ",\"hybrid\":"; WriteTimed(json, hybrid);
        json << ",\"lightweight\":"; WriteTimed(json, light); json << '}';
        std::cout << "noise " << w << " H=" << hybrid.mean << " L=" << light.mean << '\n' << std::flush;
    }
    auto samples = Generate(3840, 2160, 5);
    DecoderConfig cfg; cfg.maximum_width = 3840; cfg.maximum_height = 2160; cfg.slot_count = 3;
    Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "noise stream init");
    DecodeRequest request; request.diagnostic_strategy = DiagnosticDecodeStrategy::Hybrid;
    for (int i = 0; i < 3; ++i) { auto t = Begin(d, samples[2], request); d.ReleaseOutput(t); }
    const int frames = 1000; const auto start = Clock::now(); const double cpu = CpuMs();
    std::array<DecodeTicket, 3> pending{}; int queued = 0, retired = 0;
    while (retired < frames) {
        if (queued < frames && queued - retired < 3) {
            pending[size_t(queued % 3)] = d.QueueDecode(samples[2].compressed.data(), samples[2].compressed.size(), request);
            Require(bool(pending[size_t(queued % 3)]), "noise queue"); ++queued;
        } else {
            Require(d.WaitForDecode(pending[size_t(retired % 3)]).state == DecodeState::Ready, "noise ready");
            Require(d.ReleaseOutput(pending[size_t(retired % 3)]), "noise release"); ++retired;
        }
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    json << "],\"sustained_4k_noise\":{\"frames\":1000,\"slots\":3,\"strategy\":\"hybrid\",\"fps\":" << frames / seconds
         << ",\"process_cpu_ms_per_frame\":" << (CpuMs() - cpu) / frames << "},\"historical_phase3a\":{"
         << "\"hybrid_1080_ms\":4.593265,\"hybrid_4k_ms\":8.29706333,\"sustained_4k_fps\":132.33338},"
         << "\"historical_rc_auto\":{\"noise_1080_ms\":5.060,\"noise_4k_ms\":9.702,\"sustained_4k_fps_change\":-0.173},"
         << "\"status\":\"PASS\"}\n";
    std::cout << "noise sustained fps=" << frames / seconds << '\n' << std::flush;
}
} // namespace
int main(int argc, char** argv) {
    try {
        Phase3adSelfCheck();
        Require(argc >= 2, "usage: benchmark_decode_phase3ad --self-check|--noise out|--dev-val root out|--holdout root out freeze");
        const std::string mode = argv[1];
        if (mode == "--self-check") { std::cout << "phase3ad self-check PASS\n"; return 0; }
        if (mode == "--noise") { Require(argc == 3, "noise output"); Noise(argv[2]); return 0; }
        if (mode == "--dev-val") { Require(argc == 4, "dev-val root output"); RunCorpus(argv[2], argv[3], false, {}); return 0; }
        if (mode == "--holdout") { Require(argc == 5, "holdout root output freeze"); RunCorpus(argv[2], argv[3], true, argv[4]); return 0; }
        throw std::runtime_error("unknown phase3ad mode");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
