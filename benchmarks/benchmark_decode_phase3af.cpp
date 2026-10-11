// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Phase 3A-F policy measurement. This tool does not modify ClassifyDecodeFrame,
// thresholds, features, or either decode algorithm.
#include "../tests/decode_phase3a_rc_support.h"
#include "mp4reader.h"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>
namespace {
using namespace DecodeRC;
using namespace DecodePhase0;
constexpr uint32_t kSyntheticSeed = 0x3AF0BEEFu;
constexpr size_t kProductionSampleLimit = 32u * 1024u * 1024u;
constexpr const char* kDesign =
    "Frozen before any Phase 3A-F timing. Synthetic pixels use seed 0x3AF0BEEF and 22 classes "
    "that are not the Phase 3A, RC, or 3A-D generators. Each class is encoded at 1920x1080 in low, "
    "medium and filmscan3, and at 3840x2160 in medium and filmscan3. Even class indexes also get "
    "1920 high and filmscan1. Odd class indexes also get 1920 filmscan2. Six smooth or sparse classes "
    "are also attempted at 7680x4320 in low and medium. Photographs come from photos.tsv: two 1920 "
    "qualities cycled across low, medium, high, filmscan1, filmscan2 and filmscan3, plus a 3840 "
    "filmscan3 or medium encode for up to the first eight sources whose original is at least 3200x1800. "
    "Native frames are CFHDsample422.AVI frames 0, 3, 6, 12, 18 and 23. Frames 0, 12 and 23 are "
    "marked previously_timed and are not part of the untouched gate. No case is dropped because of "
    "its Auto label. Samples above 32 MiB stay in the manifest and are unsupported.";

struct ClassDef { const char* name; const char* tag; };
constexpr ClassDef kClasses[] = {
    {"noise_field", "high_entropy"}, {"heavy_grain", "high_entropy"}, {"foliage_canopy", "high_entropy"},
    {"grass_blades", "high_entropy"}, {"water_ripples", "high_entropy"}, {"weave_textile", "high_entropy"},
    {"pcb_traces", "high_entropy"}, {"stone_facade", "neutral"}, {"motion_streak", "neutral"},
    {"defocus_blob", "neutral"}, {"brick_course", "sparse_structured"}, {"ink_script", "sparse_structured"},
    {"ui_glyphs", "sparse_structured"}, {"flat_graphics", "sparse_structured"}, {"night_points", "sparse_structured"},
    {"open_sky", "sparse_structured"}, {"radial_ramp", "sparse_structured"}, {"skin_smooth", "sparse_structured"},
    {"hard_contrast", "sparse_structured"}, {"cgi_solids", "sparse_structured"}, {"lattice_repeat", "sparse_structured"},
    {"sparse_marks", "sparse_structured"},
};
static_assert(sizeof(kClasses) / sizeof(kClasses[0]) == 22, "Phase 3A-F class count");
constexpr const char* kEightK[] = {"open_sky", "radial_ramp", "sparse_marks", "flat_graphics", "cgi_solids", "skin_smooth"};

struct Spec {
    enum class Kind { Synthetic, Photo, Native };
    std::string id, family, path, license, source, content, provenance, tag, quality_name;
    int w = 0, h = 0, pattern = -1;
    unsigned frame = 0;
    CFHD_EncodingQuality quality = CFHD_ENCODING_QUALITY_FILMSCAN3;
    uint32_t seed = 0;
    bool seed_applies = true;
    bool previously = false;
    Kind kind = Kind::Synthetic;
};
struct Timed {
    double mean = 0, median = 0, minimum = 1e300, maximum = 0, stddev = 0, cpu = 0;
    int count = 0;
};
struct Frozen {
    std::string sample_hash, source_hash, strategy;
    size_t sample_bytes = 0;
    bool supported = false;
    bool seen = false;
};

uint32_t Mix(uint32_t n) {
    n ^= n >> 16; n *= 0xA3B19535u; n ^= n >> 13; n *= 0x7C3A1D2Fu; return n ^ (n >> 16);
}
int Clamp8(int v) { return (std::max)(0, (std::min)(255, v)); }
int Noise8(int x, int y, int pattern) {
    return int(Mix(uint32_t(x) * 0x27BB2EE6u ^ uint32_t(y) * 0x165667B1u ^ kSyntheticSeed ^ uint32_t(pattern) * 0x9E3779B9u) >> 24);
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
CFHD_EncodingQuality QualityFrom(const std::string& name) {
    if (name == "low") return CFHD_ENCODING_QUALITY_LOW;
    if (name == "medium") return CFHD_ENCODING_QUALITY_MEDIUM;
    if (name == "high") return CFHD_ENCODING_QUALITY_HIGH;
    if (name == "filmscan1") return CFHD_ENCODING_QUALITY_FILMSCAN1;
    if (name == "filmscan2") return CFHD_ENCODING_QUALITY_FILMSCAN2;
    if (name == "filmscan3") return CFHD_ENCODING_QUALITY_FILMSCAN3;
    throw std::runtime_error("unknown quality " + name);
}
int RunsFor(int w) { return w >= 7680 ? 16 : w >= 3840 ? 32 : 64; }
std::string Escape(std::string s) {
    std::string out; out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}
std::string Field(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const auto pos = line.find(needle);
    if (pos == std::string::npos) return {};
    const auto begin = pos + needle.size();
    std::string out;
    for (size_t i = begin; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) { out.push_back(line[++i]); continue; }
        if (line[i] == '"') break;
        out.push_back(line[i]);
    }
    return out;
}
bool Flag(const std::string& line, const char* key) {
    const auto pos = line.find(std::string("\"") + key + "\":");
    if (pos == std::string::npos) throw std::runtime_error(std::string("missing flag ") + key);
    return line.compare(pos + std::strlen(key) + 3, 4, "true") == 0;
}
size_t NumberField(const std::string& line, const char* key) {
    const auto pos = line.find(std::string("\"") + key + "\":");
    if (pos == std::string::npos) throw std::runtime_error(std::string("missing number ") + key);
    return size_t(std::stoull(line.substr(pos + std::strlen(key) + 3)));
}
int Luma(int x, int y, int w, int h, int pattern) {
    const int n = Noise8(x, y, pattern);
    int v = 128;
    switch (pattern) {
    case 0: v = 16 + n; break;
    case 1: v = 48 + y * 80 / h + (n % 90); break;
    case 2: v = 36 + int(Mix(uint32_t(x / 17) * 13u ^ uint32_t(y / 19) * 29u ^ kSyntheticSeed) % 140) + (n / 10); break;
    case 3: v = 28 + ((x % 4) == 0 ? 110 : 0) + (n / 8) + y * 24 / h; break;
    case 4: v = 70 + (std::abs((x + y / 3) % 48 - 24) * 4) + n / 16; break;
    case 5: v = 90 + ((x / 3 + y / 2) & 1 ? 48 : 0) + (n / 20); break;
    case 6: v = 18 + ((y % 22 < 2) || (x % 30 < 2) ? 150 : 0) + n / 30; break;
    case 7: v = ((x / 96 + y / 72) & 1 ? 150 : 96) + n / 24; break;
    case 8: {
        int sum = 0; for (int k = -4; k <= 4; ++k) sum += Noise8(x + k * 2, y, pattern);
        v = sum / 9; break;
    }
    case 9: v = 80 + int(Mix(uint32_t(x / 12) ^ uint32_t(y / 12) * 17u ^ kSyntheticSeed) % 70); break;
    case 10: {
        const int row = y / 18;
        const int col = (x + ((row & 1) ? 16 : 0)) / 36;
        v = (y % 18 < 2 || (x + ((row & 1) ? 16 : 0)) % 36 < 2) ? 64 : 130 + int(Mix(uint32_t(col) * 7u ^ uint32_t(row) ^ kSyntheticSeed) % 40);
        break;
    }
    case 11: v = (Mix(uint32_t(y / 3) * 97u ^ kSyntheticSeed ^ uint32_t(pattern)) % 11 == 0 && (x % 9) < 2) ? 28 : 226; break;
    case 12: v = (y % 28 < 14 && x % 18 < 10 && ((x / 18 + y / 28) % 5) != 0) ? 24 : 210; break;
    case 13: v = (x * 3 < w ? 32 : x * 3 < w * 2 ? 180 : 96); break;
    case 14: v = (n > 248 ? 230 : 8); break;
    case 15: v = 170 + y * 50 / h; break;
    case 16: {
        const int64_t dx = x - w / 5, dy = y - h / 4;
        v = 40 + int((dx * dx + dy * dy) / (int64_t(w) * 8)); break;
    }
    case 17: v = 150 + (n % 9) - 4 + x * 12 / w; break;
    case 18: v = (x % 64 < 8 || y % 40 < 6) ? 245 : 12; break;
    case 19: {
        v = 48;
        const int centers[][3] = {{w / 6, h / 3, w / 8}, {w / 2, h / 2, w / 7}, {4 * w / 5, 2 * h / 3, w / 9}};
        for (const auto& c : centers) {
            const int64_t dx = x - c[0], dy = y - c[1];
            if (dx * dx + dy * dy < int64_t(c[2]) * c[2]) v = 200;
        }
        break;
    }
    case 20: v = ((x % 14 < 2) || (y % 14 < 2)) ? 220 : 50; break;
    default: v = 118 + ((x > w / 3 && x < w / 3 + 24 && y > h / 4 && y < h / 2) || (x > 2 * w / 3 && y > 3 * h / 5 && y < 3 * h / 5 + 16) ? 80 : 0);
        break;
    }
    return Clamp8(v);
}
void Chroma(int pattern, int x, int y, int w, int h, int& u, int& v) {
    u = 128; v = 128;
    switch (pattern) {
    case 2: u = 96; v = 112; break;
    case 3: u = 108; v = 118; break;
    case 4: u = 112; v = 124; break;
    case 5: u = 116 + (x / 6 % 3) * 6; v = 140; break;
    case 6: u = 128; v = 110; break;
    case 10: u = 118; v = 136; break;
    case 13: u = x * 2 < w ? 90 : 170; v = y * 2 < h ? 90 : 160; break;
    case 17: u = 122; v = 142; break;
    case 19: u = 100; v = 150; break;
    default: break;
    }
    (void)h;
}
Bytes FillSynthetic(int w, int h, int pattern) {
    Bytes out(size_t(w) * h * 2);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        int u = 128, v = 128; Chroma(pattern, x, y, w, h, u, v);
        const size_t i = (size_t(y) * w + x) * 2;
        out[i] = uint8_t(Luma(x, y, w, h, pattern));
        out[i + 2] = uint8_t(Luma(x + 1, y, w, h, pattern));
        out[i + 1] = uint8_t(Clamp8(u));
        out[i + 3] = uint8_t(Clamp8(v));
    }
    return out;
}
Sample EncodeRaw(Bytes& input, int w, int h, int pattern, CFHD_EncodingQuality quality, uint32_t seed) {
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr), "phase3af encoder open"); Encoder owner(raw);
    Check(CFHD_PrepareToEncode(raw, w, h, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422,
          CFHD_ENCODING_FLAGS_NONE, quality), "phase3af prepare");
    const auto source_hash = Hash(input.data(), input.size());
    std::srand(int(seed & 0x7fffffffu));
    Check(CFHD_EncodeSample(raw, input.data(), w * 2), "phase3af encode");
    void* data = nullptr; size_t size = 0; Check(CFHD_GetSampleData(raw, &data, &size), "phase3af sample");
    Require(data && size, "phase3af nonempty sample");
    Sample s{w, h, pattern, Bytes(size), source_hash};
    std::memcpy(s.compressed.data(), data, size); NormalizeMetadata(s.compressed); return s;
}
Bytes ReadExact(const std::filesystem::path& path, size_t bytes) {
    std::ifstream in(path, std::ios::binary); Require(bool(in), "phase3af media open");
    Bytes out(bytes); in.read(reinterpret_cast<char*>(out.data()), std::streamsize(bytes));
    Require(size_t(in.gcount()) == bytes, "phase3af media size"); return out;
}
uint32_t EncodeSeed(int pattern, CFHD_EncodingQuality quality, int w) {
    const uint32_t mixed = kSyntheticSeed ^ (uint32_t(pattern) * 0x9E3779B9u) ^ (uint32_t(quality) * 0x85EBCA6Bu) ^ (uint32_t(w) * 0xC2B2AE35u);
    return mixed & 0x7fffffffu;
}
void AddSynthetic(std::vector<Spec>& specs, int pattern, int w, int h, CFHD_EncodingQuality quality) {
    Spec s;
    s.kind = Spec::Kind::Synthetic; s.pattern = pattern; s.w = w; s.h = h; s.quality = quality;
    s.quality_name = QualityName(quality); s.content = kClasses[pattern].name; s.family = s.content;
    s.tag = kClasses[pattern].tag; s.provenance = "synthetic"; s.seed = EncodeSeed(pattern, quality, w);
    s.id = "syn_" + std::to_string(w) + "_" + s.content + "_" + s.quality_name;
    s.license = "generated"; s.source = "phase3af synthetic generator";
    specs.push_back(std::move(s));
}
int ClassIndex(const char* name) {
    for (int i = 0; i < 22; ++i) if (std::string(kClasses[i].name) == name) return i;
    throw std::runtime_error("missing 8K class");
}
std::vector<std::string> SplitTab(const std::string& line) {
    std::vector<std::string> col; std::stringstream row(line); std::string cell;
    while (std::getline(row, cell, '\t')) col.push_back(cell);
    return col;
}
std::vector<Spec> BuildSpecs(const std::filesystem::path& root) {
    std::vector<Spec> specs;
    for (int p = 0; p < 22; ++p) {
        AddSynthetic(specs, p, 1920, 1080, CFHD_ENCODING_QUALITY_LOW);
        AddSynthetic(specs, p, 1920, 1080, CFHD_ENCODING_QUALITY_MEDIUM);
        AddSynthetic(specs, p, 1920, 1080, CFHD_ENCODING_QUALITY_FILMSCAN3);
        AddSynthetic(specs, p, 3840, 2160, CFHD_ENCODING_QUALITY_MEDIUM);
        AddSynthetic(specs, p, 3840, 2160, CFHD_ENCODING_QUALITY_FILMSCAN3);
        if ((p % 2) == 0) {
            AddSynthetic(specs, p, 1920, 1080, CFHD_ENCODING_QUALITY_HIGH);
            AddSynthetic(specs, p, 1920, 1080, CFHD_ENCODING_QUALITY_FILMSCAN1);
        } else {
            AddSynthetic(specs, p, 1920, 1080, CFHD_ENCODING_QUALITY_FILMSCAN2);
        }
    }
    for (const char* name : kEightK) {
        const int p = ClassIndex(name);
        AddSynthetic(specs, p, 7680, 4320, CFHD_ENCODING_QUALITY_LOW);
        AddSynthetic(specs, p, 7680, 4320, CFHD_ENCODING_QUALITY_MEDIUM);
    }
    for (unsigned frame : {0u, 3u, 6u, 12u, 18u, 23u}) {
        Spec s; s.kind = Spec::Kind::Native; s.id = "native_cfhd422_frame" + std::to_string(frame);
        s.family = "bundled_422"; s.content = "camera_sample"; s.provenance = "native_cineform";
        s.tag = "neutral"; s.w = 1920; s.h = 1080; s.frame = frame; s.quality_name = "native";
        s.seed_applies = false; s.previously = frame == 0u || frame == 12u || frame == 23u;
        s.license = "upstream CFHDsample422.AVI"; s.source = "upstream/Example/videos/CFHDsample422.AVI";
        s.path = s.source; specs.push_back(std::move(s));
    }
    const auto manifest = root / "benchmarks/phase3af_media/photos.tsv";
    std::ifstream photos(manifest); Require(bool(photos), "phase3af photos.tsv");
    std::string line; std::getline(photos, line);
    while (std::getline(photos, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto col = SplitTab(line); Require(col.size() >= 13, "phase3af photo columns");
        Spec s; s.kind = Spec::Kind::Photo; s.id = col[0]; s.family = col[1]; s.content = col[1];
        s.w = std::stoi(col[2]); s.h = std::stoi(col[3]); s.path = col[4]; s.license = col[5];
        s.source = col[7] + " " + col[6]; s.quality_name = col[8]; s.quality = QualityFrom(col[8]);
        s.seed = uint32_t(std::stoul(col[9], nullptr, 16)); s.provenance = col[10]; s.tag = col[11];
        s.previously = col[12] == "true"; specs.push_back(std::move(s));
    }
    std::set<std::string> ids;
    for (const auto& s : specs) if (!ids.insert(s.id).second) throw std::runtime_error("duplicate case " + s.id);
    std::stable_sort(specs.begin(), specs.end(), [](const Spec& a, const Spec& b) {
        if (a.h != b.h) return a.h < b.h; if (a.w != b.w) return a.w < b.w; return a.id < b.id;
    });
    return specs;
}
struct AviOwner {
    void* handle = nullptr; uint32_t* payload = nullptr;
    explicit AviOwner(const std::filesystem::path& path) {
        std::string name = path.string(); handle = OpenAVISource(name.data(), AVI_TRAK_TYPE, AVI_TRAK_SUBTYPE);
        Require(handle != nullptr, "phase3af avi");
    }
    ~AviOwner() { if (payload) FreePayload(payload); if (handle) CloseSource(handle); }
};
Sample Materialize(const std::filesystem::path& root, const Spec& spec) {
    if (spec.kind == Spec::Kind::Native) {
        AviOwner avi(root / spec.path);
        Require(spec.frame < GetNumberPayloads(avi.handle), "phase3af avi frame");
        avi.payload = GetPayload(avi.handle, avi.payload, spec.frame);
        const auto size = GetPayloadSize(avi.handle, spec.frame); Require(avi.payload && size, "phase3af avi payload");
        Sample sample{spec.w, spec.h, -1, Bytes(size), {}};
        std::memcpy(sample.compressed.data(), avi.payload, size);
        sample.input_hash = Hash(sample.compressed.data(), sample.compressed.size());
        return sample;
    }
    Bytes input = spec.kind == Spec::Kind::Photo
        ? ReadExact(root / spec.path, size_t(spec.w) * spec.h * 2)
        : FillSynthetic(spec.w, spec.h, spec.pattern);
    return EncodeRaw(input, spec.w, spec.h, spec.pattern, spec.quality, spec.seed);
}
void WriteCache(const std::filesystem::path& path, const Sample& sample) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); Require(bool(out), "phase3af cache");
    out.write(reinterpret_cast<const char*>(sample.compressed.data()), std::streamsize(sample.compressed.size()));
}
Sample ReadCache(const std::filesystem::path& path, const Spec& spec, const std::string& source_hash) {
    std::ifstream in(path, std::ios::binary | std::ios::ate); Require(bool(in), "phase3af cache open");
    const auto size = size_t(in.tellg()); in.seekg(0);
    Sample sample{spec.w, spec.h, spec.pattern, Bytes(size), source_hash};
    in.read(reinterpret_cast<char*>(sample.compressed.data()), std::streamsize(size));
    Require(size_t(in.gcount()) == size, "phase3af cache read"); return sample;
}
Timed Measure(Decoder& d, const Sample& s, DiagnosticDecodeStrategy strategy, int count) {
    DecodeRequest request; request.diagnostic_strategy = strategy;
    std::vector<double> samples; samples.reserve(size_t(count));
    auto decode = [&] { const auto t = Begin(d, s, request); Require(d.ReleaseOutput(t), "phase3af release"); };
    Timed result; const double cpu = CpuMs(); result.count = count;
    for (int i = 0; i < count; ++i) {
        const auto start = Clock::now(); decode();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        samples.push_back(ms); result.mean += ms;
        result.minimum = (std::min)(result.minimum, ms); result.maximum = (std::max)(result.maximum, ms);
    }
    result.mean /= count; result.cpu = (CpuMs() - cpu) / count;
    std::sort(samples.begin(), samples.end());
    result.median = (count % 2) ? samples[size_t(count / 2)] : 0.5 * (samples[size_t(count / 2 - 1)] + samples[size_t(count / 2)]);
    double acc = 0; for (double ms : samples) { const double d = ms - result.mean; acc += d * d; }
    result.stddev = count > 1 ? std::sqrt(acc / (count - 1)) : 0; return result;
}
void Warm(Decoder& d, const Sample& s, DiagnosticDecodeStrategy strategy) {
    DecodeRequest request; request.diagnostic_strategy = strategy;
    for (int i = 0; i < 5; ++i) { const auto t = Begin(d, s, request); Require(d.ReleaseOutput(t), "phase3af warmup release"); }
}
void WriteTimed(std::ostream& json, const Timed& t) {
    json << "{\"mean_ms\":" << t.mean << ",\"median_ms\":" << t.median << ",\"minimum_ms\":" << t.minimum
         << ",\"maximum_ms\":" << t.maximum << ",\"stddev_ms\":" << t.stddev
         << ",\"process_cpu_ms_per_frame\":" << t.cpu << ",\"frames\":" << t.count << '}';
}
const char* GateRole(const Spec& spec, bool supported) {
    if (!supported) return "unsupported";
    return spec.previously ? "previously_timed" : "primary";
}
std::string SeedText(const Spec& spec) {
    if (!spec.seed_applies) return {};
    std::ostringstream hex; hex << "0x" << std::hex << spec.seed; return hex.str();
}
void WriteIdentity(std::ostream& json, const Spec& spec, const Sample& sample, bool supported, const std::string& strategy) {
    const auto encoded = Hash(sample.compressed.data(), sample.compressed.size());
    json << "{\"id\":\"" << spec.id << "\",\"content_class\":\"" << spec.content << "\",\"family\":\"" << spec.family
         << "\",\"provenance\":\"" << spec.provenance << "\",\"workload_tag\":\"" << spec.tag
         << "\",\"previously_timed\":" << spec.previously << ",\"gate_role\":\"" << GateRole(spec, supported)
         << "\",\"width\":" << spec.w << ",\"height\":" << spec.h << ",\"quality\":\"" << spec.quality_name
         << "\",\"seed\":\"" << SeedText(spec) << "\",\"seed_applies\":" << spec.seed_applies
         << ",\"source_identity\":\"" << Escape(spec.source) << "\",\"license\":\"" << Escape(spec.license)
         << "\",\"source_hash\":\"" << sample.input_hash
         << "\",\"encoded_hash\":\"" << encoded
         << "\",\"sample_hash\":\"" << encoded
         << "\",\"sample_bytes\":" << sample.compressed.size()
         << ",\"frozen_auto_strategy\":\"" << strategy << "\",\"supported\":" << supported;
}
std::map<std::string, Frozen> LoadFrozen(const std::filesystem::path& path) {
    std::ifstream in(path); Require(bool(in), "phase3af corpus open");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Require(text.find("\"timing_performed\":false") != std::string::npos, "corpus is not a pre-timing manifest");
    std::map<std::string, Frozen> out; std::stringstream rows(text); std::string line;
    while (std::getline(rows, line)) {
        if (line.find("\"sample_hash\"") == std::string::npos) continue;
        Frozen row; const auto id = Field(line, "id"); Require(!id.empty(), "manifest id");
        row.sample_hash = Field(line, "sample_hash"); row.source_hash = Field(line, "source_hash");
        row.strategy = Field(line, "frozen_auto_strategy");
        row.sample_bytes = NumberField(line, "sample_bytes"); row.supported = Flag(line, "supported");
        out.emplace(id, row);
    }
    Require(!out.empty(), "manifest has no cases"); return out;
}
void EnsureDecoder(std::unique_ptr<Decoder>& decoder, int& dw, int& dh, const Spec& spec) {
    if (decoder && dw == spec.w && dh == spec.h) return;
    DecoderConfig cfg; cfg.maximum_width = spec.w; cfg.maximum_height = spec.h; cfg.slot_count = 2;
    decoder = std::make_unique<Decoder>(cfg);
    Require(decoder->Initialize() == DecodeError::None, "phase3af decoder init");
    dw = spec.w; dh = spec.h;
}
void RunManifest(const std::filesystem::path& root, const std::filesystem::path& output) {
    auto specs = BuildSpecs(root);
    std::filesystem::create_directories(output.parent_path());
    const auto partial = output.string() + ".partial";
    std::ofstream json(partial); Require(bool(json), "phase3af manifest");
    json << std::setprecision(9) << std::boolalpha;
    json << "{\"license\":\"MIT OR Apache-2.0\",\"phase\":\"3A-F\",\"timing_performed\":false,\"design\":\""
         << Escape(kDesign) << "\",\"classifier\":\"ClassifyDecodeFrame Phase 3A integer rule, not modified\","
         << "\"production_sample_limit_bytes\":" << kProductionSampleLimit << ",\"cases\":[\n";
    int written = 0, supported = 0, primary = 0, real_sources = 0;
    std::set<std::string> photos, qualities;
    for (const auto& spec : specs) {
        auto sample = Materialize(root, spec);
        std::vector<GpuEntropyBandDescriptor> bands; std::string reason; DecodeError error = DecodeError::None;
        const bool parsed = ParseCineFormSampleValidated(sample.compressed.data(), sample.compressed.size(), 24, bands, &reason, &error);
        const bool within = sample.compressed.size() <= kProductionSampleLimit;
        const bool ok = parsed && within;
        std::string strategy = "unclassified";
        if (parsed) {
            DecodeFrameInfo info; strategy = StrategyName(ClassifyDecodeFrame(bands, info));
            DecodeFrameInfo again; Require(ClassifyDecodeFrame(bands, again) == info.strategy, "manifest classify determinism");
        }
        if (written++) json << ",\n";
        WriteIdentity(json, spec, sample, ok, strategy);
        if (!parsed) json << ",\"rejection\":\"" << Escape(reason) << "\"}";
        else if (!within) json << ",\"rejection\":\"production decoder rejects samples above 32 MiB\"}";
        else { json << '}'; ++supported; if (!spec.previously) ++primary; }
        if (spec.provenance == "reencoded_real") photos.insert(spec.family);
        qualities.insert(spec.quality_name);
        std::cout << spec.id << " bytes=" << sample.compressed.size() << " supported=" << ok << ' ' << strategy << '\n' << std::flush;
    }
    json << "\n],\"case_count\":" << specs.size() << ",\"supported_cases\":" << supported
         << ",\"primary_supported_cases\":" << primary << ",\"real_image_sources\":" << photos.size()
         << ",\"quality_labels\":" << qualities.size() << ",\"status\":\"FROZEN_BEFORE_TIMING\"}\n";
    json.close(); std::filesystem::rename(partial, output);
    std::cout << "manifest cases=" << specs.size() << " supported=" << supported << " primary=" << primary
              << " photo_sources=" << photos.size() << '\n' << std::flush;
}
void RunGate(const std::filesystem::path& root, const std::filesystem::path& corpus,
             const std::filesystem::path& measurements, const std::filesystem::path& correctness) {
    Require(!std::filesystem::exists(measurements), "measurements already exist");
    Require(!std::filesystem::exists(correctness), "correctness already exists");
    auto frozen = LoadFrozen(corpus);
    auto specs = BuildSpecs(root);
    Require(specs.size() == frozen.size(), "spec count does not match frozen manifest");
    const auto cache = root / "build" / "phase3af_cache";
    std::filesystem::create_directories(measurements.parent_path());
    const auto partial = measurements.string() + ".partial";
    std::ofstream json(partial); Require(bool(json), "phase3af measurements");
    json << std::setprecision(9) << std::boolalpha;
    cudaDeviceProp gpu{}; GpuCheck(cudaGetDeviceProperties(&gpu, 0));
    json << "{\"license\":\"MIT OR Apache-2.0\",\"phase\":\"3A-F\",\"gpu\":\"" << Escape(gpu.name)
         << "\",\"corpus\":\"" << Escape(corpus.string()) << "\",\"timing\":\"5 Lightweight warmups and 5 Hybrid warmups, then forced Lightweight and Hybrid in alternating case order, then 5 Auto warmups and a separate Auto measurement. Repetitions are 64 at 1080p, 32 at 4K and 16 at 8K. Device-resident Queue through Ready and Release. Sample standard deviation uses n-1.\","
         << "\"oracle\":\"minimum of the forced Lightweight and Hybrid means. Auto timing is not the oracle.\","
         << "\"cases\":[\n";
    std::unique_ptr<Decoder> decoder; int dw = 0, dh = 0, written = 0, supported = 0;
    uint64_t classify_ns = 0; int classify_frames = 0;
    struct Kept { Spec spec; std::string source_hash, strategy; bool supported; };
    std::vector<Kept> kept; kept.reserve(specs.size());
    for (size_t index = 0; index < specs.size(); ++index) {
        const auto& spec = specs[index];
        const auto found = frozen.find(spec.id);
        if (found == frozen.end()) throw std::runtime_error("manifest missing " + spec.id);
        auto sample = Materialize(root, spec);
        const auto hash = Hash(sample.compressed.data(), sample.compressed.size());
        if (hash != found->second.sample_hash || sample.compressed.size() != found->second.sample_bytes || sample.input_hash != found->second.source_hash)
            throw std::runtime_error(spec.id + " does not match the frozen manifest");
        found->second.seen = true;
        std::vector<GpuEntropyBandDescriptor> bands; std::string reason; DecodeError error = DecodeError::None;
        const bool parsed = ParseCineFormSampleValidated(sample.compressed.data(), sample.compressed.size(), 24, bands, &reason, &error);
        const bool ok = parsed && sample.compressed.size() <= kProductionSampleLimit;
        if (ok != found->second.supported) throw std::runtime_error(spec.id + " support flag changed");
        WriteCache(cache / (spec.id + ".cfhd"), sample);
        std::string strategy = "unclassified";
        if (written++) json << ",\n";
        json << "{\"id\":\"" << spec.id << "\",\"width\":" << spec.w << ",\"height\":" << spec.h
             << ",\"quality\":\"" << spec.quality_name << "\",\"provenance\":\"" << spec.provenance
             << "\",\"content_class\":\"" << spec.content << "\",\"workload_tag\":\"" << spec.tag
             << "\",\"previously_timed\":" << spec.previously << ",\"gate_role\":\"" << GateRole(spec, ok)
             << "\",\"sample_hash\":\"" << hash << "\",\"sample_bytes\":" << sample.compressed.size()
             << ",\"supported\":" << ok;
        if (!ok) {
            json << ",\"rejection\":\"" << Escape(parsed ? "production decoder rejects samples above 32 MiB" : reason) << "\"}";
            kept.push_back({spec, sample.input_hash, strategy, false});
            std::cout << spec.id << " unsupported\n" << std::flush; continue;
        }
        EnsureDecoder(decoder, dw, dh, spec);
        DecodeFrameInfo info; strategy = StrategyName(ClassifyDecodeFrame(bands, info));
        if (strategy != found->second.strategy) throw std::runtime_error(spec.id + " frozen Auto strategy changed");
        for (int pass = 0; pass < 100; ++pass) {
            DecodeFrameInfo again; Require(ClassifyDecodeFrame(bands, again) == info.strategy, "classify determinism");
        }
        const auto started = Clock::now();
        for (int pass = 0; pass < 100; ++pass) { DecodeFrameInfo ignored; ClassifyDecodeFrame(bands, ignored); }
        const auto nanos = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
        classify_ns += nanos; ++classify_frames; ++supported;
        const int runs = RunsFor(spec.w);
        Warm(*decoder, sample, DiagnosticDecodeStrategy::Lightweight);
        Warm(*decoder, sample, DiagnosticDecodeStrategy::Hybrid);
        Timed light, hybrid;
        if (index % 2) {
            light = Measure(*decoder, sample, DiagnosticDecodeStrategy::Lightweight, runs);
            hybrid = Measure(*decoder, sample, DiagnosticDecodeStrategy::Hybrid, runs);
        } else {
            hybrid = Measure(*decoder, sample, DiagnosticDecodeStrategy::Hybrid, runs);
            light = Measure(*decoder, sample, DiagnosticDecodeStrategy::Lightweight, runs);
        }
        Warm(*decoder, sample, DiagnosticDecodeStrategy::Automatic);
        const auto automatic = Measure(*decoder, sample, DiagnosticDecodeStrategy::Automatic, runs);
        json << ",\"auto_strategy\":\"" << strategy << "\",\"highpass_bytes\":" << info.highpass_bytes
             << ",\"measured_frames\":" << runs << ",\"classifier_batch_ns\":" << nanos
             << ",\"lightweight\":"; WriteTimed(json, light);
        json << ",\"hybrid\":"; WriteTimed(json, hybrid);
        json << ",\"auto\":"; WriteTimed(json, automatic); json << '}';
        json.flush();
        kept.push_back({spec, sample.input_hash, strategy, true});
        std::cout << spec.id << " L=" << light.mean << " H=" << hybrid.mean << " A=" << automatic.mean
                  << ' ' << strategy << '\n' << std::flush;
    }
    for (const auto& item : frozen) if (!item.second.seen) throw std::runtime_error("unmatched manifest id " + item.first);
    json << "\n],\"supported_cases\":" << supported << ",\"classifier_mean_ns\":"
         << (classify_frames ? double(classify_ns) / (classify_frames * 100.0) : 0)
         << ",\"classifier_deterministic\":true,\"status\":\"PASS\"}\n";
    json.close(); std::filesystem::rename(partial, measurements);

    const auto partial_correct = correctness.string() + ".partial";
    std::ofstream exact(partial_correct); Require(bool(exact), "phase3af correctness");
    exact << std::setprecision(9) << std::boolalpha;
    exact << "{\"license\":\"MIT OR Apache-2.0\",\"phase\":\"3A-F\",\"reference\":\"CPU BGRA, 30 coefficients, both LL levels, signed planar and YU64\","
          << "\"cases\":[\n";
    int exact_written = 0, exact_pass = 0, exact_fail = 0;
    dw = 0; dh = 0; decoder.reset();
    for (const auto& item : kept) {
        if (exact_written++) exact << ",\n";
        exact << "{\"id\":\"" << item.spec.id << "\",\"supported\":" << item.supported;
        if (!item.supported) { exact << ",\"checked\":false}"; continue; }
        try {
            EnsureDecoder(decoder, dw, dh, item.spec);
            auto sample = ReadCache(cache / (item.spec.id + ".cfhd"), item.spec, item.source_hash);
            Require(Hash(sample.compressed.data(), sample.compressed.size()) == frozen.at(item.spec.id).sample_hash, "cache hash");
            const auto bgra = Exact(sample, *decoder);
            exact << ",\"checked\":true,\"exact\":true,\"lightweight_exact\":true,\"hybrid_exact\":true,"
                  << "\"auto_exact\":true,\"host_streaming_exact\":true,\"coefficients_exact\":30,\"max_coefficient_error\":0,"
                  << "\"ll_exact\":true,\"planar_exact\":true,\"bgra_exact\":true,\"bgra_hash\":\"" << bgra
                  << "\",\"validated_strategies\":[\"lightweight\",\"hybrid\",\"auto\",\"host_streaming\"]}";
            ++exact_pass;
            std::cout << item.spec.id << " exact\n" << std::flush;
        } catch (const std::exception& ex) {
            exact << ",\"checked\":true,\"exact\":false,\"exact_error\":\"" << Escape(ex.what()) << "\"}";
            ++exact_fail;
            std::cout << item.spec.id << " exact fail " << ex.what() << '\n' << std::flush;
            decoder.reset(); dw = 0; dh = 0;
        }
        exact.flush();
    }
    exact << "\n],\"checked_passes\":" << exact_pass << ",\"checked_failures\":" << exact_fail
          << ",\"status\":\"" << (exact_fail ? "FAIL" : "PASS") << "\"}\n";
    exact.close(); std::filesystem::rename(partial_correct, correctness);
}
// Continues every stage of Lightweight, Hybrid, Auto, and HostStreaming.
// Exact() stops at the first throw, so a Lightweight YU64 difference never
// reaches Hybrid. This mode does not time frames and does not change the corpus.
const char* AuditName(DiagnosticDecodeStrategy strategy) {
    if (strategy == DiagnosticDecodeStrategy::Lightweight) return "lightweight";
    if (strategy == DiagnosticDecodeStrategy::Hybrid) return "hybrid";
    if (strategy == DiagnosticDecodeStrategy::Automatic) return "auto";
    return "host_streaming";
}
int PlaneSampleForWord(const std::array<std::vector<int16_t>, 3>& planes, const std::array<int, 3>& widths,
                       int frame_width, size_t byte_index) {
    if ((byte_index & 1u) || frame_width <= 0) return -32768;
    const size_t word = byte_index / 2;
    const int words_per_row = frame_width * 2;
    const int row = int(word / size_t(words_per_row));
    const int col = int(word % size_t(words_per_row));
    const int slot = col % 4;
    const int pair = col / 4;
    int channel = 0;
    int x = pair;
    if (slot == 0) { channel = 0; x = pair * 2; }
    else if (slot == 2) { channel = 0; x = pair * 2 + 1; }
    else if (slot == 1) channel = 2;
    else channel = 1;
    if (x < 0 || x >= widths[size_t(channel)]) return -32768;
    const size_t index = size_t(row) * size_t(widths[size_t(channel)]) + size_t(x);
    if (index >= planes[size_t(channel)].size()) return -32768;
    return planes[size_t(channel)][index];
}
void RunStageAudit(const std::filesystem::path& root, const std::filesystem::path& corpus,
                   const std::filesystem::path& correctness, const std::filesystem::path& output) {
    Require(!std::filesystem::exists(output), "stage audit already exists");
    auto frozen = LoadFrozen(corpus);
    std::map<std::string, Spec> by_id;
    for (const auto& spec : BuildSpecs(root)) by_id.emplace(spec.id, spec);
    std::ifstream in(correctness); Require(bool(in), "phase3af correctness open");
    std::vector<std::string> failed;
    size_t declared_failures = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"checked_failures\"") != std::string::npos)
            declared_failures = NumberField(line, "checked_failures");
        if (line.find("\"exact\":false") == std::string::npos) continue;
        const auto id = Field(line, "id");
        Require(!id.empty(), "correctness failure id");
        failed.push_back(id);
    }
    Require(failed.size() == declared_failures, "correctness failure count mismatch");
    const auto cache = root / "build" / "phase3af_cache";
    std::filesystem::create_directories(output.parent_path());
    const auto partial = output.string() + ".partial";
    std::ofstream json(partial); Require(bool(json), "phase3af stage audit");
    json << std::boolalpha;
    json << "{\"license\":\"MIT OR Apache-2.0\",\"phase\":\"3A-F\","
         << "\"purpose\":\"Follow-up to the gate checker. Exact() throws on the first failing stage, and Lightweight runs first, so Hybrid was not stage-checked on these frames. This audit runs Lightweight, Hybrid, Auto, and HostStreaming independently through coefficients, both LL levels, BGRA, signed planar planes, and packed YU64.\","
         << "\"yu64_convention\":\"ConvertPlanarToYU64 saturates to 1023 at 10-bit and then shifts, producing 65472. The CPU YU64 buffer can store 65535 for the same full-scale sample. A difference is the known pack convention only when every differing word is packed 65472 versus reference 65535 and the GPU plane sample is above 1023.\","
         << "\"timing_performed\":false,\"cases\":[\n";
    const DiagnosticDecodeStrategy strategies[] = {
        DiagnosticDecodeStrategy::Lightweight, DiagnosticDecodeStrategy::Hybrid,
        DiagnosticDecodeStrategy::Automatic, DiagnosticDecodeStrategy::HostStreaming};
    std::unique_ptr<Decoder> decoder;
    int dw = 0, dh = 0, written = 0;
    int image_exact = 0, convention_only = 0, path_dependent = 0;
    for (const auto& id : failed) {
        const auto spec_found = by_id.find(id);
        if (spec_found == by_id.end()) throw std::runtime_error("stage audit missing spec " + id);
        const auto& spec = spec_found->second;
        const auto frozen_found = frozen.find(id);
        if (frozen_found == frozen.end()) throw std::runtime_error("stage audit missing manifest " + id);
        if (written++) json << ",\n";
        json << "{\"id\":\"" << id << "\"";
        bool case_image_exact = false;
        bool case_convention = false;
        bool case_path_dependent = true;
        try {
            auto sample = ReadCache(cache / (id + ".cfhd"), spec, frozen_found->second.source_hash);
            Require(Hash(sample.compressed.data(), sample.compressed.size()) == frozen_found->second.sample_hash, "stage audit cache hash");
            ReferenceDecoder cpu(sample, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(sample);
            auto bands = ExtractBandsFromNative(cpu.GetNative());
            auto intermediate = ComputeCpuReferenceIntermediates(cpu.GetNative());
            ReferenceDecoder yu64(sample, CFHD_PIXEL_FORMAT_YU64); yu64.Decode(sample);
            const auto planar_reference = ComputeCpuReferenceIntermediates(yu64.GetNative());
            struct Row {
                bool ran = false, coefficients = false, ll = false, bgra = false, planar = false;
                int coefficient_mismatches = 0, max_coefficient_abs = 0, ll_mismatches = 0, bgra_max_abs = 0, yu64_max_abs = 0;
                int first_packed = 0, first_ref = 0, first_plane_sample = 0;
                size_t bgra_byte_diffs = 0, planar_diffs = 0, yu64_word_diffs = 0, yu64_fullscale = 0, yu64_other = 0;
                size_t yu64_plane_above_1023 = 0, yu64_plane_not_above_1023 = 0;
                std::string bgra_hash, packed_hash, error;
            };
            std::map<std::string, Row> rows;
            for (auto strategy : strategies) {
                Row row;
                try {
                    EnsureDecoder(decoder, dw, dh, spec);
                    DecodeRequest request; request.diagnostic_strategy = strategy;
                    auto ticket = Begin(*decoder, sample, request);
                    bool geometry = true;
                    for (int b = 0; b < 30; ++b) {
                        std::vector<int16_t> actual; int w = 0, h = 0, pitch = 0;
                        Require(DecoderInspector::DownloadBand(*decoder, ticket, b, actual, &w, &h, &pitch), "audit band download");
                        const auto& ref = bands[size_t(b)];
                        if (w != ref.width || h != ref.height) { geometry = false; continue; }
                        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                            const int a = actual[size_t(y) * (pitch / 2) + x];
                            const int r = ref.host_data[size_t(y) * (ref.pitch / 2) + x];
                            if (a == r) continue;
                            ++row.coefficient_mismatches;
                            row.max_coefficient_abs = (std::max)(row.max_coefficient_abs, std::abs(a - r));
                        }
                    }
                    row.coefficients = geometry && row.coefficient_mismatches == 0;
                    for (int ch = 0; ch < 3; ++ch) for (int level = 0; level < 2; ++level) {
                        std::vector<int16_t> ll;
                        Require(DecoderInspector::DownloadIntermediate(*decoder, ticket, ch, level, ll), "audit LL download");
                        if (ll != (level ? intermediate.ll1[ch] : intermediate.ll0[ch])) ++row.ll_mismatches;
                    }
                    row.ll = row.ll_mismatches == 0;
                    const auto bgra = Image(*decoder, ticket, sample);
                    row.bgra_hash = Hash(bgra.data(), bgra.size());
                    if (bgra.size() == cpu.output.size()) {
                        for (size_t i = 0; i < bgra.size(); ++i) if (bgra[i] != cpu.output[i]) {
                            ++row.bgra_byte_diffs;
                            row.bgra_max_abs = (std::max)(row.bgra_max_abs, std::abs(int(bgra[i]) - int(cpu.output[i])));
                        }
                        row.bgra = row.bgra_byte_diffs == 0;
                    }
                    Require(decoder->ReleaseOutput(ticket), "audit BGRA release");
                    request.format = DecodePixelFormat::PlanarYUV422Signed16;
                    ticket = Begin(*decoder, sample, request);
                    DeviceOutput out; Require(decoder->GetDeviceOutput(ticket, &out) && out.plane_count == 3, "audit planar output");
                    std::array<std::vector<int16_t>, 3> planes;
                    std::array<int, 3> widths{};
                    bool planar_sizes = true;
                    for (int ch = 0; ch < 3; ++ch) {
                        const auto& plane = out.planes[size_t(ch)];
                        widths[size_t(ch)] = plane.width;
                        planes[size_t(ch)].resize(size_t(plane.width) * plane.height);
                        GpuCheck(cudaMemcpy2D(planes[size_t(ch)].data(), size_t(plane.width) * 2, plane.device_pointer,
                            plane.pitch_bytes, size_t(plane.width) * 2, plane.height, cudaMemcpyDeviceToHost));
                        const int native = ch == 1 ? 2 : ch == 2 ? 1 : 0;
                        const auto& ref = planar_reference.plane[native];
                        if (planes[size_t(ch)].size() != ref.size()) { planar_sizes = false; row.planar_diffs += planes[size_t(ch)].size() + ref.size(); continue; }
                        for (size_t i = 0; i < ref.size(); ++i) if (planes[size_t(ch)][i] != ref[i]) ++row.planar_diffs;
                    }
                    row.planar = planar_sizes && row.planar_diffs == 0;
                    Bytes packed(size_t(sample.width) * sample.height * 4);
                    ConvertPlanarToYU64(planes[0].data(), sample.width * 2, planes[2].data(), sample.width,
                        planes[1].data(), sample.width, sample.width, sample.height, 10, packed.data(), sample.width * 4);
                    row.packed_hash = Hash(packed.data(), packed.size());
                    if (packed.size() == yu64.output.size()) {
                        bool first = true;
                        for (size_t i = 0; i + 1 < packed.size(); i += 2) {
                            const int a = int(packed[i]) | (int(packed[i + 1]) << 8);
                            const int b = int(yu64.output[i]) | (int(yu64.output[i + 1]) << 8);
                            if (a == b) continue;
                            if (first) {
                                first = false; row.first_packed = a; row.first_ref = b;
                                row.first_plane_sample = PlaneSampleForWord(planes, widths, sample.width, i);
                            }
                            ++row.yu64_word_diffs;
                            row.yu64_max_abs = (std::max)(row.yu64_max_abs, std::abs(a - b));
                            const int sample_value = PlaneSampleForWord(planes, widths, sample.width, i);
                            if (sample_value > 1023) ++row.yu64_plane_above_1023;
                            else ++row.yu64_plane_not_above_1023;
                            if (a == 65472 && b == 65535) ++row.yu64_fullscale;
                            else ++row.yu64_other;
                        }
                    } else row.error = "packed size does not match the CPU YU64 buffer";
                    Require(decoder->ReleaseOutput(ticket), "audit planar release");
                    row.ran = row.error.empty();
                } catch (const std::exception& ex) {
                    row.error = ex.what();
                    decoder.reset(); dw = 0; dh = 0;
                }
                rows.emplace(AuditName(strategy), std::move(row));
            }
            json << ",\"strategies\":{";
            bool first_strategy = true;
            bool hashes_agree = true;
            bool pixels_agree = true;
            bool convention = true;
            std::string bgra_hash, packed_hash;
            for (auto strategy : strategies) {
                const auto& row = rows.at(AuditName(strategy));
                if (!first_strategy) json << ',';
                first_strategy = false;
                json << '"' << AuditName(strategy) << "\":{\"ran\":" << row.ran
                     << ",\"coefficients_exact\":" << row.coefficients
                     << ",\"coefficient_mismatches\":" << row.coefficient_mismatches
                     << ",\"max_coefficient_abs\":" << row.max_coefficient_abs
                     << ",\"ll_exact\":" << row.ll << ",\"ll_mismatches\":" << row.ll_mismatches
                     << ",\"bgra_exact\":" << row.bgra << ",\"bgra_byte_diffs\":" << row.bgra_byte_diffs
                     << ",\"bgra_max_abs\":" << row.bgra_max_abs << ",\"bgra_hash\":\"" << row.bgra_hash
                     << "\",\"planar_exact\":" << row.planar << ",\"planar_sample_diffs\":" << row.planar_diffs
                     << ",\"yu64_word_diffs\":" << row.yu64_word_diffs
                     << ",\"yu64_fullscale_65472_vs_65535\":" << row.yu64_fullscale
                     << ",\"yu64_other_diffs\":" << row.yu64_other
                     << ",\"yu64_max_abs\":" << row.yu64_max_abs
                     << ",\"yu64_plane_samples_above_1023\":" << row.yu64_plane_above_1023
                     << ",\"yu64_plane_samples_not_above_1023\":" << row.yu64_plane_not_above_1023
                     << ",\"first_packed_word\":" << row.first_packed << ",\"first_ref_word\":" << row.first_ref
                     << ",\"first_plane_sample\":" << row.first_plane_sample
                     << ",\"packed_hash\":\"" << row.packed_hash << "\"";
                if (!row.error.empty()) json << ",\"error\":\"" << Escape(row.error) << '"';
                json << '}';
                const bool row_pixels = row.ran && row.coefficients && row.ll && row.bgra && row.planar && row.error.empty();
                if (!row_pixels) pixels_agree = false;
                const bool row_convention = row_pixels && row.yu64_other == 0 && row.yu64_plane_not_above_1023 == 0
                    && row.yu64_word_diffs == row.yu64_fullscale;
                if (!row_convention) convention = false;
                if (bgra_hash.empty()) bgra_hash = row.bgra_hash;
                else if (bgra_hash != row.bgra_hash || row.bgra_hash.empty()) hashes_agree = false;
                if (packed_hash.empty()) packed_hash = row.packed_hash;
                else if (packed_hash != row.packed_hash || row.packed_hash.empty()) hashes_agree = false;
            }
            json << "},\"bgra_and_packed_hashes_agree\":" << hashes_agree
                 << ",\"coefficients_ll_bgra_planar_exact\":" << pixels_agree
                 << ",\"yu64_only_fullscale_pack_convention\":" << convention
                 << ",\"path_dependent_pixel_difference\":" << (!pixels_agree || !hashes_agree) << '}';
            case_image_exact = pixels_agree && hashes_agree;
            case_convention = convention && hashes_agree;
            case_path_dependent = !case_image_exact;
        } catch (const std::exception& ex) {
            json << ",\"error\":\"" << Escape(ex.what()) << "\",\"path_dependent_pixel_difference\":true}";
            decoder.reset(); dw = 0; dh = 0;
        }
        if (case_path_dependent) ++path_dependent;
        else ++image_exact;
        if (case_convention && !case_path_dependent) ++convention_only;
        std::cout << id << " image_exact=" << case_image_exact << " convention=" << case_convention
                  << " path_dependent=" << case_path_dependent << '\n' << std::flush;
        json.flush();
    }
    const char* status = "FAIL_PATH_DEPENDENT";
    if (failed.empty()) status = "FAIL_NO_CASES";
    else if (path_dependent == 0 && convention_only == int(failed.size()))
        status = "PASS_IMAGE_EXACT_WITH_FULLSCALE_YU64_PACK_CONVENTION";
    else if (path_dependent == 0) status = "PASS_IMAGE_EXACT";
    json << "\n],\"audited_cases\":" << failed.size() << ",\"image_exact_cases\":" << image_exact
         << ",\"fullscale_pack_convention_cases\":" << convention_only
         << ",\"path_dependent_cases\":" << path_dependent << ",\"status\":\"" << status << "\"}\n";
    json.close(); std::filesystem::rename(partial, output);
    std::cout << "stage-audit cases=" << failed.size() << " image_exact=" << image_exact
              << " convention=" << convention_only << " path_dependent=" << path_dependent
              << " status=" << status << '\n' << std::flush;
}
} // namespace
int main(int argc, char** argv) {
    try {
        Require(argc >= 2, "usage: benchmark_decode_phase3af --count root | --manifest root out | --gate root corpus measurements correctness | --stage-audit root corpus correctness output");
        const std::string mode = argv[1];
        if (mode == "--count") {
            Require(argc == 3, "count root");
            const auto specs = BuildSpecs(argv[2]);
            int real = 0, native = 0, synthetic = 0, seen = 0;
            std::set<std::string> photos, qualities;
            for (const auto& s : specs) {
                if (s.provenance == "reencoded_real") { ++real; photos.insert(s.family); }
                else if (s.provenance == "native_cineform") ++native;
                else ++synthetic;
                if (s.previously) ++seen;
                qualities.insert(s.quality_name);
            }
            std::cout << "cases=" << specs.size() << " synthetic=" << synthetic << " real_encodes=" << real
                      << " real_sources=" << photos.size() << " native=" << native << " previously_timed=" << seen
                      << " qualities=" << qualities.size() << '\n';
            return 0;
        }
        if (mode == "--manifest") { Require(argc == 4, "manifest root output"); RunManifest(argv[2], argv[3]); return 0; }
        if (mode == "--gate") {
            Require(argc == 6, "gate root corpus measurements correctness");
            RunGate(argv[2], argv[3], argv[4], argv[5]); return 0;
        }
        if (mode == "--stage-audit") {
            Require(argc == 6, "stage-audit root corpus correctness output");
            RunStageAudit(argv[2], argv[3], argv[4], argv[5]); return 0;
        }
        throw std::runtime_error("unknown phase3af mode");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
