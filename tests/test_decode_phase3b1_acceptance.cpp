// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "decode_phase3a_rc_support.h"
#include "hypercine/research/decode_phase2e.h"
#include "hypercine/decode_color_cuda.h"
extern "C" {
#include "temporal.h"
#include "RGB2YUV.h"
#include "bayer.h"
}
#include <atomic>
#include <thread>
#include <new>

static std::atomic<uint64_t> heap_requests{0};
void* operator new(size_t n) { ++heap_requests; if (auto p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {
using namespace DecodeRC;
using HyperCineTest::AlignedVector;
using Planes = std::array<AlignedVector<int16_t>, 3>; // wavelet order Y,V,U
constexpr DiagnosticDecodeStrategy strategies[] = {DiagnosticDecodeStrategy::Lightweight,
    DiagnosticDecodeStrategy::Hybrid, DiagnosticDecodeStrategy::Automatic, DiagnosticDecodeStrategy::HostStreaming};
constexpr const char* strategy_names[] = {"Lightweight", "Hybrid", "Auto", "HostStreaming"};
constexpr const char* patterns[] = {"flat", "horizontal_gradient", "vertical_gradient", "diagonal_gradient",
    "horizontal_edges", "vertical_edges", "checkerboard", "thin_lines", "sparse_marks", "alternating_fields",
    "field_luma", "field_chroma", "saturated_yuv", "near_black", "near_white", "noise", "texture"};
constexpr CFHD_EncodingQuality qualities[] = {CFHD_ENCODING_QUALITY_LOW, CFHD_ENCODING_QUALITY_MEDIUM,
    CFHD_ENCODING_QUALITY_HIGH, CFHD_ENCODING_QUALITY_FILMSCAN1, CFHD_ENCODING_QUALITY_FILMSCAN2, CFHD_ENCODING_QUALITY_FILMSCAN3};
constexpr const char* quality_names[] = {"Low", "Medium", "High", "FilmScan1", "FilmScan2", "FilmScan3"};

struct Evidence {
    std::filesystem::path output;
    std::vector<std::string> cases;
    explicit Evidence(std::filesystem::path p) : output(std::move(p)) { std::filesystem::create_directories(output); }
    void Save(const char* name, const std::vector<std::string>& rows) const {
        std::ofstream f(output / name);
        f << "{\"license\":\"MIT OR Apache-2.0\",\"seed\":\"0x1337BEEF\",\"cases\":[\n";
        for (size_t i = 0; i < rows.size(); ++i) { if (i) f << ",\n"; f << rows[i]; }
        f << "\n]}\n"; Require(bool(f), "write evidence");
    }
};
Bytes Load(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary); Require(bool(f), "fixture open");
    f.seekg(0, std::ios::end); const auto n = f.tellg(); f.seekg(0);
    Bytes b(static_cast<size_t>(n)); f.read(reinterpret_cast<char*>(b.data()), n); Require(bool(f), "fixture read"); return b;
}
void SaveSample(const std::filesystem::path& p, const Sample& s) {
    std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(s.compressed.data()), s.compressed.size());
    Require(bool(f), "preserve sample");
}
Bytes Input(int w, int h, int p) {
    Bytes b(size_t(w) * h * 2);
    uint32_t rng = Seed;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        auto luma = [&](int xx) {
            rng = rng * 1664525u + 1013904223u;
            switch (p) {
            case 0: return 126;
            case 1: return 16 + xx * 219 / (w - 1);
            case 2: return 16 + y * 219 / (h - 1);
            case 3: return 16 + (xx + y) * 219 / (w + h - 2);
            case 4: return y < h / 2 ? 16 : 235;
            case 5: return xx < w / 2 ? 16 : 235;
            case 6: return ((xx / 2 + y / 2) & 1) ? 235 : 16;
            case 7: return xx % 13 == 0 || y % 11 == 0 ? 235 : 16;
            case 8: return (xx * 17 + y * 31) % 199 == 0 ? 235 : 80;
            case 9: return y & 1 ? 220 : 24;
            case 10: return y & 1 ? 180 + xx % 37 : 20 + xx % 29;
            case 11: return 80 + xx % 64;
            case 12: return xx < w / 2 ? 0 : 255;
            case 13: return xx % 5;
            case 14: return 251 + xx % 5;
            case 15: return int(rng >> 24);
            default: return 16 + ((xx * 11 + y * 7 + (xx ^ y)) % 220);
            }
        };
        const size_t i = (size_t(y) * w + x) * 2;
        b[i] = uint8_t(luma(x)); b[i + 2] = uint8_t(luma(x + 1));
        b[i + 1] = uint8_t(p == 11 ? ((y & 1) ? 224 - x % 17 : 20 + x % 13) : p == 12 ? 0 : 96 + y * 64 / h);
        b[i + 3] = uint8_t(p == 11 ? ((y & 1) ? 24 + x % 19 : 220 - x % 11) : p == 12 ? 255 : 96 + x * 64 / w);
    }
    return b;
}
Sample Encode(int w, int h, int p, int q, bool interlaced = true) {
    auto b = Input(w, h, p);
    return EncodeInput(b, w, h, CFHD_PIXEL_FORMAT_YUY2, qualities[q], p,
        interlaced ? CFHD_ENCODING_FLAGS_YUV_INTERLACED : CFHD_ENCODING_FLAGS_NONE);
}
Sample Frozen1080() {
    auto b = HyperCineTest::GenerateTestVectorYUY2(HyperCineTest::PATTERN_PSEUDO_RANDOM_NOISE, 1920, 1080);
    Sample s = EncodeInput(b, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, qualities[5], 15, CFHD_ENCODING_FLAGS_YUV_INTERLACED);
    uint64_t folded = (14695981039346656037ULL ^ HyperCineTest::ComputeFNV1a64(b.data(), b.size())) * 1099511628211ULL;
    std::ostringstream hash; hash << std::hex << std::setfill('0') << std::setw(16) << folded; s.input_hash = hash.str();
    Require(s.input_hash == "2d12a276f3594b09" && Hash(s.compressed.data(), s.compressed.size()) == "356ad5c204104b7d", "frozen 1080 source/sample"); return s;
}
struct CpuStages { Planes ll1, ll0, low, high, plane; };
CpuStages Reconstruct(DECODER* d) {
    CpuStages s;
    for (int ch = 0; ch < 3; ++ch) {
        auto* w2 = d->transform[ch]->wavelet[2]; auto* w1 = d->transform[ch]->wavelet[1]; auto* w0 = d->transform[ch]->wavelet[0];
        s.ll1[ch].resize(size_t(w1->width) * w1->height); s.ll0[ch].resize(size_t(w0->width) * w0->height);
        AlignedVector<PIXEL> scratch(size_t(w1->width) * 32);
        ROI roi2{w2->width, w2->height}, roi1{w1->width, w1->height};
        InvertSpatialQuant16s(w2->band[0], w2->pitch, w2->band[1], w2->pitch, w2->band[2], w2->pitch,
            w2->band[3], w2->pitch, s.ll1[ch].data(), w1->width * 2, roi2, scratch.data(), scratch.size() * 2, w2->quantization);
        InvertSpatialQuantDescale16s(s.ll1[ch].data(), w1->width * 2, w1->band[1], w1->pitch, w1->band[2], w1->pitch,
            w1->band[3], w1->pitch, s.ll0[ch].data(), w0->width * 2, roi1, scratch.data(), scratch.size() * 2, 2, w1->quantization);
        const int w = w0->width * 2, rows = w0->height;
        s.low[ch].resize(size_t(w) * rows); s.high[ch].resize(size_t(w) * rows); s.plane[ch].resize(size_t(w) * rows * 2);
        AlignedVector<uint16_t> pair(size_t(w) * 2);
        for (int r = 0; r < rows; ++r) {
            auto* low = s.low[ch].data() + size_t(r) * w; auto* high = s.high[ch].data() + size_t(r) * w;
            InvertHorizontalRow16s(s.ll0[ch].data() + size_t(r) * w0->width, w0->band[1] + size_t(r) * (w0->pitch / 2), low, w0->width);
            InvertHorizontalRow16s(w0->band[2] + size_t(r) * (w0->pitch / 2), w0->band[3] + size_t(r) * (w0->pitch / 2), high, w0->width);
            InvertInterlacedRow16sToRow16u(low, high, pair.data(), w * 2, w, w, 0, 10);
            for (int x = 0; x < w * 2; ++x) s.plane[ch][size_t(r) * w * 2 + x] = int16_t(pair[x] >> 6);
        }
    }
    return s;
}
Planes ReadPlanes(Decoder& d, DecodeTicket t) {
    DecoderReconstruction r; Require(DecoderInspector::Reconstruction(d, t, &r), "plane inspection"); Planes p;
    for (int ch = 0; ch < 3; ++ch) {
        const auto& v = r.planes[ch == 0 ? 0 : 3 - ch]; p[ch].resize(size_t(v.width) * v.height);
        GpuCheck(cudaMemcpy2D(p[ch].data(), size_t(v.width) * 2, v.device_pointer, v.pitch_bytes,
            size_t(v.width) * 2, v.height, cudaMemcpyDeviceToHost));
    }
    return p;
}
int Colorspace(DecodeMatrix m, DecodeRange r) {
    return m == DecodeMatrix::BT709 ? (r == DecodeRange::ComputerGraphics ? COLOR_SPACE_CG_709 : COLOR_SPACE_VS_709)
                                  : (r == DecodeRange::ComputerGraphics ? COLOR_SPACE_CG_601 : COLOR_SPACE_VS_601);
}
Bytes UpstreamColor(DECODER* d, const Planes& p, int w, int h, DecodeMatrix m, DecodeRange range, DecodeOrientation orientation) {
    Bytes out(size_t(w) * h * 4); AlignedVector<uint16_t> packed(size_t(w) * 2), scratch(size_t(w) * 16);
    const int cs = Colorspace(m, range);
    for (int y = 0; y < h; ++y) {
        auto* yy = packed.data(); auto* vv = yy + w; auto* uu = vv + w / 2;
        for (int x = 0; x < w; ++x) yy[x] = uint16_t(uint16_t(p[0][size_t(y) * w + x]) << 6);
        for (int x = 0; x < w / 2; ++x) { vv[x] = uint16_t(uint16_t(p[1][size_t(y) * (w / 2) + x]) << 6); uu[x] = uint16_t(uint16_t(p[2][size_t(y) * (w / 2) + x]) << 6); }
        unsigned short* channels[] = {yy, vv, uu};
        auto* rgb = scratch.data() + size_t(w) * 6;
        ChannelYUYV16toPlanarYUV16(channels, scratch.data(), w, cs);
        PlanarYUV16toPlanarRGB16(scratch.data(), rgb, w, cs | COLOR_SPACE_8_PIXEL_PLANAR);
        const int out_y = orientation == DecodeOrientation::BottomUp ? h - 1 - y : y;
        ConvertLinesToOutput(d, w, 1, y, rgb, out.data() + size_t(out_y) * w * 4, w * 4,
            d->frame.format, 16, COLOR_FORMAT_RGB_8PIXEL_PLANAR);
    }
    return out;
}
template<class A, class B> size_t Differences(const A& a, const B& b, size_t* first = nullptr) {
    Require(a.size() == b.size(), "comparison size"); size_t n = 0;
    if (first) *first = a.size();
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { if (first && !n) *first = i; ++n; }
    return n;
}
std::string Identity(const Sample& s, const std::string& name) {
    std::ostringstream j; j << "\"id\":\"" << name << "\",\"width\":" << s.width << ",\"height\":" << s.height
        << ",\"source_hash\":\"" << s.input_hash << "\",\"sample_hash\":\"" << Hash(s.compressed.data(), s.compressed.size()) << "\""; return j.str();
}
bool Exact(Sample& s, Decoder& d, const std::string& name, Evidence& evidence, bool colors) {
    ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(s);
    const auto stages = Reconstruct(cpu.GetNative()); auto bands = ExtractBandsFromNative(cpu.GetNative());
    std::vector<GpuEntropyBandDescriptor> parsed; FrameReconstruction mode;
    Require(ParseCineFormSampleValidated(s.compressed.data(), s.compressed.size(), 24, parsed, nullptr, nullptr, &mode)
        && mode == FrameReconstruction::InterlacedGop1, "interlaced profile");
    int peaks = 0, differences = 0;
    for (const auto& b : parsed) { peaks += b.has_peaks && b.peak_count > 0; differences += b.difference_coding; }
    if (name == "frozen1080") {
        for (int ch = 0; ch < 3; ++ch) Require(parsed[ch * 10 + 8].difference_coding && parsed[ch * 10 + 8].codebook_id == 2,
            "frozen subband 8 difference coding and cs18 codebook");
        Require(peaks > 0, "genuine frozen interlaced Peak table");
    }
    bool pass = true;
    for (int k = 0; k < 4; ++k) {
        DecodeRequest req; req.diagnostic_strategy = strategies[k];
        auto t = Begin(d, s, req); size_t coefficients = 0, coeff_diff = 0, plane_diff = 0, ll_diff = 0; int max_error = 0;
        for (int b = 0; b < 30; ++b) {
            std::vector<int16_t> actual; int w, h, pitch; Require(DecoderInspector::DownloadBand(d, t, b, actual, &w, &h, &pitch), "band download");
            const auto& r = bands[b]; Require(w == r.width && h == r.height, "band geometry");
            Require(parsed[b].width == w && parsed[b].height == h && parsed[b].channel == r.channel && parsed[b].level == r.level && parsed[b].band == r.band, "parsed descriptor exact");
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const int delta = std::abs(int(actual[size_t(y) * (pitch / 2) + x]) - int(r.host_data[size_t(y) * (r.pitch / 2) + x]));
                ++coefficients; coeff_diff += delta != 0; max_error = (std::max)(max_error, delta);
            }
        }
        for (int ch = 0; ch < 3; ++ch) for (int l = 0; l < 2; ++l) {
            std::vector<int16_t> ll; Require(DecoderInspector::DownloadIntermediate(d, t, ch, l, ll), "LL inspection");
            ll_diff += Differences(ll, l ? stages.ll1[ch] : stages.ll0[ch]);
        }
        const auto planes = ReadPlanes(d, t); for (int ch = 0; ch < 3; ++ch) plane_diff += Differences(planes[ch], stages.plane[ch]);
        const auto actual = Image(d, t, s); size_t first; const size_t bgra_diff = Differences(actual, cpu.output, &first);
        DecodeFrameInfo info; Require(d.GetFrameInfo(t, &info), "strategy info");
        const bool ok = !coeff_diff && !ll_diff && !plane_diff && !bgra_diff; pass &= ok;
        std::ostringstream j; j << '{' << Identity(s, name) << ",\"requested_strategy\":\"" << strategy_names[k]
            << "\",\"actual_strategy\":\"" << StrategyName(info.strategy) << "\",\"coefficients\":" << coefficients
            << ",\"coefficient_mismatches\":" << coeff_diff << ",\"max_coefficient_error\":" << max_error << ",\"ll_mismatches\":" << ll_diff
            << ",\"planar_mismatches\":" << plane_diff << ",\"bgra_byte_mismatches\":" << bgra_diff
            << ",\"bgra_hash\":\"" << Hash(actual.data(), actual.size()) << "\",\"upstream_bgra_hash\":\"" << Hash(cpu.output.data(), cpu.output.size())
            << "\",\"peak_bands\":" << peaks << ",\"difference_bands\":" << differences << ",\"status\":\"" << (ok ? "PASS" : "FAIL") << "\"";
        if (bgra_diff) j << ",\"first_byte\":" << first << ",\"expected\":" << int(cpu.output[first]) << ",\"actual\":" << int(actual[first]);
        j << '}'; evidence.cases.push_back(j.str()); d.ReleaseOutput(t);
        if (colors) for (auto matrix : {DecodeMatrix::BT601, DecodeMatrix::BT709}) for (auto range : {DecodeRange::ComputerGraphics, DecodeRange::VideoSystems})
            for (auto orientation : {DecodeOrientation::BottomUp, DecodeOrientation::TopDown}) {
                req.matrix = matrix; req.range = range; req.orientation = orientation;
                auto ct = Begin(d, s, req); const auto got = Image(d, ct, s);
                const auto want = UpstreamColor(cpu.GetNative(), stages.plane, s.width, s.height, matrix, range, orientation);
                const auto mismatch = Differences(got, want); pass &= mismatch == 0;
                std::ostringstream row; row << '{' << Identity(s, name) << ",\"requested_strategy\":\"" << strategy_names[k]
                    << "\",\"matrix\":\"" << (matrix == DecodeMatrix::BT709 ? "BT709" : "BT601") << "\",\"range\":\"" << (range == DecodeRange::ComputerGraphics ? "CG" : "VS")
                    << "\",\"orientation\":\"" << (orientation == DecodeOrientation::TopDown ? "top_down" : "bottom_up") << "\",\"bgra_byte_mismatches\":" << mismatch << ",\"status\":\"" << (mismatch ? "FAIL" : "PASS") << "\"}";
                evidence.cases.push_back(row.str()); d.ReleaseOutput(ct);
            }
    }
    // Planar output deliberately retains its existing +4 offset. Its CPU oracle
    // must therefore be decoded independently as YU64, rather than reusing BGRA.
    ReferenceDecoder yu64(s, CFHD_PIXEL_FORMAT_YU64); yu64.Decode(s); const auto planar = Reconstruct(yu64.GetNative());
    for (int k = 0; k < 4; ++k) {
        DecodeRequest req; req.format = DecodePixelFormat::PlanarYUV422Signed16; req.diagnostic_strategy = strategies[k];
        auto t = Begin(d, s, req); const auto got = ReadPlanes(d, t); size_t diffs[3]{};
        for (int ch = 0; ch < 3; ++ch) diffs[ch] = Differences(got[ch], planar.plane[ch]);
        pass &= !(diffs[0] + diffs[1] + diffs[2]);
        std::ostringstream row; row << '{' << Identity(s, name) << ",\"surface\":\"planar_plus4\",\"requested_strategy\":\"" << strategy_names[k]
            << "\",\"Y_mismatches\":" << diffs[0] << ",\"V_mismatches\":" << diffs[1] << ",\"U_mismatches\":" << diffs[2] << ",\"status\":\"" << (diffs[0] + diffs[1] + diffs[2] ? "FAIL" : "PASS") << "\"}";
        evidence.cases.push_back(row.str()); d.ReleaseOutput(t);
    }
    if (!pass) {
        SaveSample(evidence.output / (name + "-failure.cfhd"), s);
        evidence.Save((name + "-failure.json").c_str(), evidence.cases);
    }
    std::cout << name << ' ' << (pass ? "PASS" : "FAIL") << " peaks=" << peaks << " difference=" << differences << '\n' << std::flush;
    return pass;
}

int16_t Saturate(int v) { return int16_t((std::max)(-32768, (std::min)(32767, v))); }
int16_t CpuField(int16_t lo, int16_t hi, bool odd) {
    const auto v = Saturate(odd ? int(lo) + hi : int(lo) - hi);
    const uint16_t biased = uint16_t(Saturate(int(v) + 30720));
    return int16_t(int16_t(biased < 30720 ? 0 : biased - 30720) >> 1);
}
void FieldUnit(Evidence& e) {
    constexpr int w = 64, rows = 5, stride = 80;
    constexpr int16_t values[] = {-32768, -30721, -30720, -2048, -3, -1, 0, 1, 2, 3, 2046, 2047, 2048, 32766, 32767};
    AlignedVector<int16_t> low(stride * rows), high(stride * rows), got(stride * rows * 2, -999);
    for (int r = 0; r < rows; ++r) for (int x = 0; x < w; ++x) {
        const int index = r * w + x;
        low[r * stride + x] = values[index % 15]; high[r * stride + x] = values[(index / 15) % 15];
    }
    DeviceBuffer<int16_t> dl(low.size()), dh(high.size()), out(got.size());
    GpuCheck(cudaMemcpy(dl.p, low.data(), low.size() * 2, cudaMemcpyHostToDevice)); GpuCheck(cudaMemcpy(dh.p, high.data(), high.size() * 2, cudaMemcpyHostToDevice));
    GpuCheck(cudaMemcpy(out.p, got.data(), got.size() * 2, cudaMemcpyHostToDevice));
    Stream stream; Require(LaunchInterlacedFieldCombine(dl.p, stride * 2, dh.p, stride * 2, out.p, stride * 2, w, rows, stream.handle), "direct field kernel");
    GpuCheck(cudaStreamSynchronize(stream.handle)); GpuCheck(cudaMemcpy(got.data(), out.p, got.size() * 2, cudaMemcpyDeviceToHost));
    size_t count = 0; int extreme_upstream_wraps = 0;
    for (int r = 0; r < rows; ++r) for (int x = 0; x < stride; ++x) for (int parity = 0; parity < 2; ++parity) {
        const auto expected = x < w ? CpuField(low[r * stride + x], high[r * stride + x], parity != 0) : -999;
        Require(got[(2 * r + parity) * stride + x] == expected, "independent CPU field formula including pitch guards"); ++count;
        extreme_upstream_wraps += x < w && (expected < 0 || expected > 1023);
    }
    Require(CpuField(0, 3, false) == 0 && CpuField(-3, 0, true) == 0, "negative clamp before shift regression");
    AlignedVector<uint16_t> upstream_pair(size_t(w) * 2);
    for (int r = 0; r < rows; ++r) {
        InvertInterlacedRow16sToRow16u(low.data() + r * stride, high.data() + r * stride,
            upstream_pair.data(), w * 2, w, w, 0, 10);
        for (int x = 0; x < w; ++x) for (int parity = 0; parity < 2; ++parity)
            Require(uint16_t(uint16_t(got[(2 * r + parity) * stride + x]) << 6) == upstream_pair[parity * w + x], "direct pristine upstream SSE field arithmetic");
    }
    // Deliberately distinguish routing through the old progressive converter.
    auto sample = Encode(64, 64, 10, 5); ReferenceDecoder cpu(sample, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(sample);
    auto stages = Reconstruct(cpu.GetNative()); DeviceBuffer<int16_t> dy(4096), dv(2048), du(2048); DeviceBuffer<uint8_t> color(16384);
    ReferenceDecoder offset4(sample, CFHD_PIXEL_FORMAT_YU64); offset4.Decode(sample);
    const auto alternate = Reconstruct(offset4.GetNative());
    Require(alternate.plane != stages.plane && UpstreamColor(cpu.GetNative(), alternate.plane, 64, 64, DecodeMatrix::BT709,
        DecodeRange::ComputerGraphics, DecodeOrientation::BottomUp) != cpu.output, "BGRA fixture detects substitution of lowpass +4 for +24");
    GpuCheck(cudaMemcpy(dy.p, stages.plane[0].data(), 8192, cudaMemcpyHostToDevice));
    GpuCheck(cudaMemcpy(dv.p, stages.plane[1].data(), 4096, cudaMemcpyHostToDevice)); GpuCheck(cudaMemcpy(du.p, stages.plane[2].data(), 4096, cudaMemcpyHostToDevice));
    Bytes image(16384); std::string hash;
    for (bool interlaced : {true, false}) {
        Require(LaunchYuv422ToBgra8(dy.p, 128, dv.p, 64, du.p, 64, color.p, 256, 64, 64, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, stream.handle, nullptr, interlaced), "direct color kernel");
        GpuCheck(cudaStreamSynchronize(stream.handle)); GpuCheck(cudaMemcpy(image.data(), color.p, image.size(), cudaMemcpyDeviceToHost));
        if (interlaced) { Require(image == cpu.output, "interlaced 16-bit upstream color"); hash = Hash(image.data(), image.size()); }
        else Require(image != cpu.output, "fixture must detect accidental progressive color routing");
    }
    e.Save("field_unit.json", {"{\"status\":\"PASS\",\"samples_and_pitch_guards\":" + std::to_string(count) + ",\"negative_clamp_before_shift\":true,\"extreme_sse_unsigned_wraps\":" + std::to_string(extreme_upstream_wraps) + ",\"interlaced_color_separate\":true,\"bgra_hash\":\"" + hash + "\"}"});
}

void InspectFields(Sample& sample, Evidence& e, const char* name) {
    ReferenceDecoder cpu(sample, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(sample); const auto reference = Reconstruct(cpu.GetNative());
    CudaDecoderPipelinePhase2E pipe; Require(pipe.Initialize(sample.width, sample.height, 1, 10, 4, 2), "field stage init");
    std::vector<GpuEntropyBandDescriptor> parsed;
    Require(ParseCineFormSampleValidated(sample.compressed.data(), sample.compressed.size(), 24, parsed, nullptr), "field stage parse");
    auto t = pipe.QueueDecodeParsed(sample.compressed.data(), sample.compressed.size(), parsed, DeviceSurfaceType::BGRA8,
        ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, true, 0, true);
    Require(t && pipe.WaitForDecode(t), "field stage ready");
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<int16_t> lo, hi, plane; int width, rows, pitch, output_pitch;
        Require(pipe.DownloadInterlacedFieldStage(t, ch, lo, hi, plane, &width, &rows, &pitch, &output_pitch), "field stage download");
        Require(Differences(lo, reference.low[ch]) == 0 && Differences(hi, reference.high[ch]) == 0 && Differences(plane, reference.plane[ch]) == 0,
            "horizontal temporal low/high and field combine independently exact");
    }
    Phase2eSlotStageTimings timing; Require(pipe.GetStageTimings(t, &timing), "field timing");
    std::ostringstream row; row << '{' << Identity(sample, name) << ",\"temporal_low_mismatches\":0,\"temporal_high_mismatches\":0,\"field_mismatches\":0,\"field_combine_ms\":"
        << timing.field_combine_ms << ",\"color_convert_ms\":" << timing.color_convert_ms << ",\"status\":\"PASS\"}";
    e.Save((std::string(name) + "_field_stages.json").c_str(), {row.str()}); pipe.ReleaseOutput(t);
}

size_t TagOffset(const Bytes& b, unsigned wanted) {
    for (size_t i = 0; i + 4 <= b.size();) {
        const int raw = int16_t(BE16(b.data() + i)); const unsigned tag = raw < 0 ? -raw : raw; const size_t n = BE16(b.data() + i + 2);
        if (tag == wanted || (wanted == 0x2000 && (tag & 0xff00) == wanted)) return i;
        i += 4;
        if (tag == 2) i += n * 4;
        else if ((tag & 0xff00) == 0x2000) i += (((tag & 255) << 16) | n) * 4;
        else if (tag >= 0x4000 && tag < 0x6000) i += n * 4;
    }
    throw std::runtime_error("missing mutation tag " + std::to_string(wanted));
}
void SetTag(Bytes& b, unsigned tag, uint16_t value) { auto i = TagOffset(b, tag); b[i + 2] = uint8_t(value >> 8); b[i + 3] = uint8_t(value); }
void Rejections(const std::filesystem::path& root, Evidence& e) {
    DecoderConfig cfg; cfg.maximum_width = cfg.maximum_height = 128; Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "rejections init");
    std::vector<std::string> rows;
    auto reject = [&](const std::string& name, const Bytes& bytes, DecodeError want) {
        DecodeError error; auto t = d.QueueDecode(bytes.data(), bytes.size(), {}, &error); Require(bool(t), "failed ticket exists");
        const auto status = d.WaitForDecode(t); DeviceOutput out;
        const bool published = d.GetDeviceOutput(t, &out);
        const bool ok = status.state == DecodeState::Failed && status.error == want && !published;
        std::ostringstream row; row << "{\"profile\":\"" << name << "\",\"expected\":" << int(want) << ",\"actual\":" << int(status.error) << ",\"output_published\":" << (published ? "true" : "false") << ",\"sample_hash\":\"" << Hash(bytes.data(), bytes.size()) << "\",\"status\":\"" << (ok ? "PASS" : "FAIL") << "\"}";
        rows.push_back(row.str()); e.Save("rejections.json", rows); d.ReleaseOutput(t); Require(ok, name.c_str());
    };
    for (const auto& entry : std::filesystem::directory_iterator(root / "tests/fixtures/phase3b0")) {
        if (entry.path().extension() != ".cfhd") continue;
        const auto name = entry.path().stem().string();
        if (name.find("fieldplus-prog-f2") != std::string::npos) {
            Sample fieldplus{64, 64, 0, Load(entry.path()), name};
            const char* expected[] = {"27fd4d3948775018", "c06ad9438de588c8"};
            for (int frame = 0; frame < 2; ++frame) {
                DecodeRequest request; request.frame_index = frame;
                DecodeError error; auto ticket = d.QueueDecode(fieldplus.compressed.data(), fieldplus.compressed.size(), request, &error);
                Require(bool(ticket) && error == DecodeError::None, "progressive FIELDPLUS supported ticket");
                Require(d.WaitForDecode(ticket).state == DecodeState::Ready, "progressive FIELDPLUS output ready");
                const auto image = Image(d, ticket, fieldplus);
                Require(Hash(image.data(), image.size()) == expected[frame],
                    "progressive FIELDPLUS canonical frame hash");
                Require(d.ReleaseOutput(ticket), "progressive FIELDPLUS ticket released");
            }
        }
        else if (name.find("fieldplus") != std::string::npos) reject(name, Load(entry.path()), DecodeError::UnsupportedEncodingMode);
        else if (name.find("YUV422") == std::string::npos) reject(name, Load(entry.path()), DecodeError::InvalidSample);
    }
    auto s = Encode(128, 64, 11, 5);
    for (auto mutation : {std::pair<unsigned, uint16_t>{70, 8}, {70, 12}, {20, 63}, {21, 65}, {12, 4}, {62, 4}, {0x2000, 65535}}) {
        auto b = s.compressed; SetTag(b, mutation.first, mutation.second); reject("bad_tag_" + std::to_string(mutation.first) + "_" + std::to_string(mutation.second), b, DecodeError::InvalidSample);
    }
    // The entropy chunks survive removal of the final frame trailer, but the
    // upstream decoder rejects that interrupted frame (error 2048). Publication
    // by the production decoder was a concrete parser defect, not a valid tail.
    for (size_t n : {size_t(1), size_t(7), s.compressed.size() / 2, s.compressed.size() - 8, s.compressed.size() - 4}) {
        auto b = s.compressed; b.resize(n);
        if (n == s.compressed.size() - 4) {
            auto interrupted = s; interrupted.compressed = b;
            ReferenceDecoder ref(interrupted, CFHD_PIXEL_FORMAT_BGRA);
            const auto upstream_error = CFHD_DecodeSample(ref.GetHandle(), b.data(), b.size(), ref.output.data(), ref.pitch);
            Require(upstream_error != CFHD_ERROR_OKAY, "upstream rejects missing frame trailer");
            SaveSample(e.output / "missing_frame_trailer.cfhd", interrupted);
            rows.push_back("{\"profile\":\"missing_frame_trailer_upstream\",\"sample_hash\":\"" + Hash(b.data(), b.size()) + "\",\"upstream_error\":" + std::to_string(upstream_error) + ",\"status\":\"PASS\"}");
        }
        reject("truncated_" + std::to_string(n), b, DecodeError::InvalidSample);
    }
    auto missing_band = s.compressed; const size_t chunk_start = TagOffset(missing_band, 0x2000);
    const size_t chunk_bytes = (((size_t(BE16(missing_band.data() + chunk_start)) & 255) << 16) | BE16(missing_band.data() + chunk_start + 2)) * 4;
    missing_band.erase(missing_band.begin() + chunk_start, missing_band.begin() + chunk_start + 4 + chunk_bytes);
    reject("29_bands", missing_band, DecodeError::InvalidSample);
    auto inconsistent = s.compressed; inconsistent.insert(inconsistent.end(), {0, 22, 0, 3});
    reject("inconsistent_frame_format", inconsistent, DecodeError::InvalidSample);
    auto encoded_format = s.compressed; SetTag(encoded_format, 84, 3);
    reject("inconsistent_encoded_format_RGB", encoded_format, DecodeError::InvalidSample);
    Bytes oversized(32 * 1024 * 1024 + 1); reject("oversized", oversized, DecodeError::SampleTooLarge);
    e.Save("rejections.json", rows);
}

void Matrix(const std::filesystem::path& root, Evidence& e, bool sanitizer) {
    FieldUnit(e);
    DecoderConfig cfg; cfg.maximum_width = 1920; cfg.maximum_height = 1080; cfg.slot_count = 2;
    Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "matrix init");
    Sample frozen_small{64, 64, 0, Load(root / "tests/fixtures/phase3b0/YUV422-p10-ch3-spatial-field-f1-bands30-fmt-1.cfhd"), "committed_phase3b0"};
    bool pass = Exact(frozen_small, d, "frozen64", e, true);
    auto large = Frozen1080(); pass &= Exact(large, d, "frozen1080", e, !sanitizer);
    InspectFields(frozen_small, e, "frozen64"); InspectFields(large, e, "frozen1080");
    if (!sanitizer) {
        for (auto geometry : {std::pair<int,int>{64,64}, {128,64}, {128,128}}) for (int q = 0; q < 6; ++q) for (int p = 0; p < 17; ++p) {
            auto sample = Encode(geometry.first, geometry.second, p, q);
            const std::string name = std::string(patterns[p]) + "_" + quality_names[q] + "_" + std::to_string(geometry.first) + "x" + std::to_string(geometry.second);
            pass &= Exact(sample, d, name, e, p == 10 || p == 11 || p == 12);
        }
        for (int p : {0, 10, 11, 12, 16}) { auto sample = Encode(1920, 1080, p, 5); pass &= Exact(sample, d, std::string(patterns[p]) + "_FilmScan3_1920x1080", e, true); }
    } else for (int p : {10, 11, 15}) { auto sample = Encode(128, 64, p, 5); pass &= Exact(sample, d, std::string(patterns[p]) + "_sanitizer", e, true); }
    e.Save("correctness.json", e.cases);
    Rejections(root, e); Require(pass, "matrix exactness gate; preserved failure samples and JSON");
}

struct Cached {
    Sample sample;
    Bytes bgra;
    Planes planes;
    int frame_index = 0;
    bool fieldplus = false;
};
bool ExactCachedImage(const Cached& expected, const uint8_t* actual) {
    const size_t bytes = size_t(expected.sample.width) * size_t(expected.sample.height) * 4;
    if (expected.fieldplus) {
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < bytes; ++i) {
            hash ^= actual[i];
            hash *= 1099511628211ULL;
        }
        return hash == (expected.frame_index == 0 ? 0x27fd4d3948775018ULL : 0xc06ad9438de588c8ULL);
    }
    return expected.bgra.size() == bytes && std::memcmp(actual, expected.bgra.data(), bytes) == 0;
}
std::vector<Cached> Corpus(const std::filesystem::path& root) {
    std::vector<Cached> c;
    for (bool interlaced : {false, true}) for (int q : {0, 2, 5}) for (int p : {0, 10, 11, 15, 16}) {
        auto s = Encode(128, 64, p, q, interlaced); ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(s);
        c.push_back({std::move(s), cpu.output, {}});
    }
    auto s = Frozen1080(); ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(s); c.push_back({std::move(s), cpu.output, {}});
    Sample fixture{64, 64, 0, Load(root / "tests/fixtures/phase3b0/YUV422-p10-ch3-spatial-field-f1-bands30-fmt-1.cfhd"), "committed_phase3b0"};
    ReferenceDecoder small_cpu(fixture, CFHD_PIXEL_FORMAT_BGRA); small_cpu.Decode(fixture); c.push_back({std::move(fixture), small_cpu.output, {}});
    Sample fieldplus{64, 64, 0, Load(root / "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd"), "progressive_fieldplus_gop2"};
    for (int frame = 0; frame < 2; ++frame) {
        c.push_back({fieldplus, {}, {}, frame, true});
    }
    return c;
}
void Stress(const std::filesystem::path& root, Evidence& e, bool sanitizer) {
    auto corpus = Corpus(root);
    std::array<int, 2> fieldplus_index{-1, -1};
    for (size_t i = 0; i < corpus.size(); ++i) {
        if (corpus[i].fieldplus) fieldplus_index[size_t(corpus[i].frame_index)] = int(i);
    }
    Require(fieldplus_index[0] >= 0 && fieldplus_index[1] >= 0, "FIELDPLUS stress corpus frames");
    DecoderConfig cfg; cfg.slot_count = 3; Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "stress init");
    // Warm all slots, strategies, and content before observing allocation counts.
    for (size_t corpus_index = 0; corpus_index < corpus.size(); ++corpus_index) {
      auto& c = corpus[corpus_index];
      if (c.fieldplus) continue;
      for (auto strategy : strategies) for (int slot = 0; slot < 3; ++slot) {
        DecodeRequest req; req.diagnostic_strategy = strategy; req.frame_index = c.frame_index;
        auto t = Begin(d, c.sample, req); const auto image = Image(d, t, c.sample);
        Require(ExactCachedImage(c, image.data()), "warm exact");
        c.planes = ReadPlanes(d, t);
        d.ReleaseOutput(t);
      }
    }
    // Queue three FIELDPLUS requests before releasing any ticket so each slot
    // allocates its persistent 51-band scratch before the steady-state gate.
    for (int frame = 0; frame < 2; ++frame) {
        const auto& c = corpus[size_t(fieldplus_index[size_t(frame)])];
        std::array<DecodeTicket, 3> tickets{};
        for (auto& ticket : tickets) {
            DecodeRequest req; req.diagnostic_strategy = DiagnosticDecodeStrategy::Automatic;
            req.frame_index = frame;
            ticket = d.QueueDecode(c.sample.compressed.data(), c.sample.compressed.size(), req);
            Require(bool(ticket), "warm FIELDPLUS slot queue");
        }
        for (const auto ticket : tickets) {
            const auto status = d.WaitForDecode(ticket);
            if (status.state != DecodeState::Ready) {
                std::cerr << "FIELDPLUS slot warm decode failed frame=" << frame
                    << " state=" << int(status.state) << " error=" << int(status.error) << '\n';
            }
            Require(status.state == DecodeState::Ready, "warm FIELDPLUS slot ready");
            const auto image = Image(d, ticket, c.sample);
            Require(ExactCachedImage(c, image.data()), "warm FIELDPLUS slot exact");
            d.ReleaseOutput(ticket);
        }
    }
    Bytes scratch(size_t(1920) * 1080 * 4); const auto before = d.MemoryStats(); const auto alloc_before = heap_requests.load();
    const int frames = sanitizer ? 44 : 5000;
    std::array<int, 2> fieldplus_outputs{};
    int done = 0;
    while (done < frames) {
        DecodeTicket tickets[3]; int content[3]; const int n = (std::min)(3, frames - done);
        for (int k = 0; k < n; ++k) {
            content[k] = (done + k) % int(corpus.size()); auto& s = corpus[content[k]].sample;
            auto& item = corpus[content[k]];
            if (item.fieldplus) ++fieldplus_outputs[size_t(item.frame_index)];
            DecodeRequest req; req.diagnostic_strategy = item.fieldplus ? DiagnosticDecodeStrategy::Automatic : strategies[(done + k) % 4];
            req.frame_index = item.frame_index;
            tickets[k] = d.QueueDecode(s.compressed.data(), s.compressed.size(), req);
            Require(bool(tickets[k]), "mixed queue");
        }
        // Alternate FIFO and reverse release orders deterministically. Reverse
        // batches hold the oldest ticket while newer frames complete/release.
        for (int release_index = 0; release_index < n; ++release_index) {
            const int k = (done / 3) & 1 ? release_index : n - 1 - release_index;
            auto& c = corpus[content[k]];
            Require(d.WaitForDecode(tickets[k]).state == DecodeState::Ready, "mixed ready");
            Download(d, tickets[k], scratch.data(), size_t(c.sample.width) * 4);
            Require(ExactCachedImage(c, scratch.data()), "mixed every BGRA byte/hash exact"); d.ReleaseOutput(tickets[k]);
        }
        done += n;
        if (done % 999 == 0) std::cout << "mixed " << done << '/' << frames << '\n' << std::flush;
    }
    const auto steady_alloc = heap_requests.load() - alloc_before; const auto after = d.MemoryStats();
    std::cout << "mixed steady-state deltas cpp=" << steady_alloc
        << " cuda_calls=" << int64_t(after.cuda_allocation_calls) - int64_t(before.cuda_allocation_calls)
        << " device=" << int64_t(after.device_bytes) - int64_t(before.device_bytes)
        << " pinned=" << int64_t(after.pinned_host_bytes) - int64_t(before.pinned_host_bytes)
        << " pageable=" << int64_t(after.pageable_capacity_bytes) - int64_t(before.pageable_capacity_bytes) << '\n';
    Require(steady_alloc == 0 && before.cuda_allocation_calls == after.cuda_allocation_calls && before.device_bytes == after.device_bytes
        && before.pinned_host_bytes == after.pinned_host_bytes && before.pageable_capacity_bytes == after.pageable_capacity_bytes, "zero steady-state allocation gate");
    std::vector<std::string> rows;
    for (int instances : {2, 4}) {
        std::vector<std::unique_ptr<Decoder>> decoders;
        for (int i = 0; i < instances; ++i) { decoders.push_back(std::make_unique<Decoder>(cfg)); Require(decoders.back()->Initialize() == DecodeError::None, "multi init"); }
        std::atomic<bool> failed{false}; std::vector<std::thread> workers;
        for (int i = 0; i < instances; ++i) workers.emplace_back([&, i] {
            try { for (int r = 0; r < (sanitizer ? 4 : 60); ++r) {
                auto& c = corpus[(r + i) % corpus.size()]; DecodeRequest req;
                req.diagnostic_strategy = c.fieldplus ? DiagnosticDecodeStrategy::Automatic : strategies[r % 4];
                req.frame_index = c.frame_index;
                auto t = Begin(*decoders[i], c.sample, req); const auto image = Image(*decoders[i], t, c.sample);
                Require(ExactCachedImage(c, image.data()), "concurrent exact"); decoders[i]->ReleaseOutput(t);
            } } catch (...) { failed = true; }
        });
        for (auto& t : workers) t.join(); Require(!failed, "concurrent instance gate");
        rows.push_back("{\"instances\":" + std::to_string(instances) + ",\"frames_per_instance\":" + std::to_string(sanitizer ? 4 : 60) + ",\"status\":\"PASS\"}");
    }
    for (int slots : {1, 2, 0, 4}) {
        cfg.slot_count = slots; cfg.scheduling = slots == 4 ? DecodeSchedulingPolicy::HighThroughput : DecodeSchedulingPolicy::Auto;
        Decoder instance(cfg); Require(instance.Initialize() == DecodeError::None, "slot mode init");
        for (auto& c : corpus) {
            DecodeRequest req; req.frame_index = c.frame_index;
            auto t = Begin(instance, c.sample, req); const auto image = Image(instance, t, c.sample);
            Require(ExactCachedImage(c, image.data()), "slot mode exact"); instance.ReleaseOutput(t);
        }
    }
    // Consumer registration, delayed release, mixed failed/successful tickets, drain.
    auto& c = corpus[size_t(fieldplus_index[0])]; Stream consumer;
    DecodeRequest held_request; held_request.frame_index = c.frame_index;
    auto held = d.QueueDecode(c.sample.compressed.data(), c.sample.compressed.size(), held_request); Require(bool(held), "pending FIELDPLUS consumer ticket");
    Require(d.StreamWait(held, CudaDecodeConsumer(consumer.handle)), "consumer wait before host wait");
    Require(d.WaitForDecode(held).state == DecodeState::Ready, "consumer producer ready");
    DeviceOutput surface; Require(d.GetDeviceOutput(held, &surface), "held surface");
    const size_t held_bytes = size_t(c.sample.width) * size_t(c.sample.height) * 4;
    DeviceBuffer<uint8_t> copy(held_bytes);
    GpuCheck(cudaMemcpy2DAsync(copy.p, c.sample.width * 4, surface.planes[0].device_pointer, surface.planes[0].pitch_bytes,
        c.sample.width * 4, c.sample.height, cudaMemcpyDeviceToDevice, consumer.handle));
    auto& good_fieldplus = corpus[size_t(fieldplus_index[1])]; DecodeRequest good_request; good_request.frame_index = good_fieldplus.frame_index;
    auto good = Begin(d, good_fieldplus.sample, good_request); Bytes bad(7); auto failed = d.QueueDecode(bad.data(), bad.size());
    const auto good_image = Image(d, good, good_fieldplus.sample);
    const auto held_image = Image(d, held, c.sample);
    Require(d.WaitForDecode(failed).state == DecodeState::Failed &&
        ExactCachedImage(good_fieldplus, good_image.data()) && ExactCachedImage(c, held_image.data()),
        "held FIELDPLUS frame and failed ticket independent");
    d.ReleaseOutput(good); d.ReleaseOutput(held); d.ReleaseOutput(failed);
    Bytes copied(held_bytes); GpuCheck(cudaMemcpy(copied.data(), copy.p, copied.size(), cudaMemcpyDeviceToHost));
    Require(ExactCachedImage(c, copied.data()), "release drains FIELDPLUS consumer");
    { Decoder drain(cfg); Require(drain.Initialize() == DecodeError::None, "drain init");
      auto t = drain.QueueDecode(c.sample.compressed.data(), c.sample.compressed.size(), held_request); Require(bool(t), "destroy with FIELDPLUS in flight"); }
    // Repeated coefficients, planes and image, with independent strategy overrides.
    for (int index : {11, 10}) {
        auto& item = corpus[index]; ReferenceDecoder cpu(item.sample, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(item.sample);
        const auto ref = ExtractBandsFromNative(cpu.GetNative()); const int count = sanitizer ? 4 : index == 10 ? 100 : 200;
        for (int r = 0; r < count; ++r) {
            DecodeRequest req; req.diagnostic_strategy = strategies[r % 4]; auto t = Begin(d, item.sample, req);
            Coefficients(d, t, ref); Require(ReadPlanes(d, t) == item.planes && Image(d, t, item.sample) == item.bgra, "deterministic coefficients planes image"); d.ReleaseOutput(t);
        }
        rows.push_back("{\"determinism_width\":" + std::to_string(item.sample.width) + ",\"decodes\":" + std::to_string(count) + ",\"coefficients_planes_bgra\":\"exact\",\"status\":\"PASS\"}");
    }
    rows.push_back("{\"mixed_frames\":" + std::to_string(frames) + ",\"fieldplus_frame0_outputs\":" + std::to_string(fieldplus_outputs[0])
        + ",\"fieldplus_frame1_outputs\":" + std::to_string(fieldplus_outputs[1]) + ",\"steady_state_cpp_allocations\":" + std::to_string(steady_alloc)
        + ",\"cuda_allocation_delta\":0,\"release_order\":\"alternating_FIFO_reverse\",\"slot_modes\":[1,2,0,4],\"lifetime\":\"PASS\",\"status\":\"PASS\"}"); e.Save("stress.json", rows);
    std::ostringstream memory; memory << "{\"slots\":" << before.slot_count << ",\"device_bytes\":" << before.device_bytes << ",\"pinned_bytes\":" << before.pinned_host_bytes
        << ",\"pageable_bytes\":" << before.pageable_capacity_bytes << ",\"cuda_allocation_calls\":" << before.cuda_allocation_calls << ",\"steady_state_cpp_allocations\":" << steady_alloc << ",\"status\":\"PASS\"}";
    e.Save("memory.json", {memory.str()});
}
void Performance(Evidence& e) {
    std::vector<std::string> rows;
    for (int resolution = 0; resolution < 3; ++resolution) for (int p : {0, 15}) {
        const bool interlaced = resolution == 0; const int w = resolution == 2 ? 3840 : 1920, h = resolution == 2 ? 2160 : 1080;
        auto s = interlaced ? Frozen1080() : Encode(w, h, p, 5, false);
        if (interlaced && p == 0) continue;
        DecoderConfig cfg; cfg.maximum_width = w; cfg.maximum_height = h; cfg.slot_count = 2;
        Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "performance init");
        for (int k = 0; k < 3; ++k) {
            DecodeRequest req; req.diagnostic_strategy = strategies[k];
            for (int r = 0; r < 12; ++r) { auto t = Begin(d, s, req); d.ReleaseOutput(t); }
            double total = 0, minimum = 1e9, maximum = 0; constexpr int count = 100;
            for (int r = 0; r < count; ++r) {
                const auto start = Clock::now(); auto t = Begin(d, s, req);
                const double ms = std::chrono::duration<double,std::milli>(Clock::now() - start).count(); d.ReleaseOutput(t);
                total += ms; minimum = (std::min)(minimum, ms); maximum = (std::max)(maximum, ms);
            }
            std::ostringstream row; row << '{' << Identity(s, interlaced ? "frozen1080_interlaced" : std::string("progressive_") + patterns[p])
                << ",\"strategy\":\"" << strategy_names[k] << "\",\"frames\":" << count << ",\"latency_avg_ms\":" << total / count << ",\"latency_min_ms\":" << minimum
                << ",\"latency_max_ms\":" << maximum << ",\"fps\":" << count * 1000.0 / total << ",\"megapixels_per_second\":" << double(w) * h * count / (total * 1000)
                << ",\"peak_working_set_bytes\":" << WorkingSet(true) << ",\"device_bytes\":" << d.MemoryStats().device_bytes << '}'; rows.push_back(row.str());
            std::cout << (interlaced ? "interlaced" : "progressive") << ' ' << w << ' ' << strategy_names[k] << ' ' << total / count << " ms\n" << std::flush;
        }
    }
    e.Save("performance.json", rows);
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 4, "usage: acceptance ROOT OUTPUT --matrix|--stress|--sanitizer|--performance");
        Evidence e(argv[2]); const std::string mode = argv[3];
        if (mode == "--matrix" || mode == "--sanitizer") Matrix(argv[1], e, mode == "--sanitizer");
        if (mode == "--stress" || mode == "--sanitizer") Stress(argv[1], e, mode == "--sanitizer");
        if (mode == "--performance") Performance(e);
        if (mode == "--rejections") Rejections(argv[1], e);
        std::cout << "Phase 3B-1 acceptance " << mode << " PASS\n"; return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
