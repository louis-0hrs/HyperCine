// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_phase3a_rc_support.h"
#include "hypercine/decode_color_cuda.h"
#include "decode_phase3ae_color_reference.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

using namespace DecodeRC;
namespace {
struct Photo {
    const char* id;
    const char* source_hash;
    const char* sample_hash;
    size_t bytes;
    size_t old_mismatches;
};
constexpr Photo photos[] = {
    {"photo_holdout_brick", "1252b69a6e689d6a", "1fad1210cf321a16", 3180804, 447},
    {"photo_holdout_grass", "df9a67919f22f7b1", "262eddc0bd80b320", 4255116, 93},
    {"photo_holdout_handwriting", "4500cf86ac819d0c", "80955c5750f8ef94", 1569836, 33}
};
Sample PhotoSample(const std::filesystem::path& root, const Photo& photo,
                   const std::filesystem::path& output) {
    Bytes input(1920 * 1080 * 2);
    std::ifstream file(root / "benchmarks/phase3ad_media" / (std::string(photo.id) + ".yuv"), std::ios::binary);
    file.read(reinterpret_cast<char*>(input.data()), std::streamsize(input.size()));
    Require(file.gcount() == std::streamsize(input.size()), "photo source unavailable");
    Require(Hash(input.data(), input.size()) == photo.source_hash, "original photo source hash");
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr), "photo encoder"); Encoder owner(raw);
    Check(CFHD_PrepareToEncode(raw, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422,
          CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3), "photo prepare");
    std::srand(0x51A77);
    Check(CFHD_EncodeSample(raw, input.data(), 1920 * 2), "photo encode");
    void* data = nullptr; size_t size = 0;
    Check(CFHD_GetSampleData(raw, &data, &size), "photo sample");
    Sample s{1920, 1080, 0, Bytes(size), photo.source_hash};
    std::memcpy(s.compressed.data(), data, size); NormalizeMetadata(s.compressed);
    Require(size == photo.bytes && Hash(s.compressed.data(), size) == photo.sample_hash, "original encoded photo identity");
    const auto saved = output / (std::string(photo.id) + ".cfhd");
    if (!std::filesystem::exists(saved)) {
        std::ofstream f(saved, std::ios::binary);
        f.write(reinterpret_cast<const char*>(s.compressed.data()), std::streamsize(size));
        Require(bool(f), "save normalized photo sample");
    }
    return s;
}
size_t PixelDiffs(const Bytes& a, const Bytes& b) {
    Require(a.size() == b.size(), "BGRA size");
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        if (std::memcmp(a.data() + i, b.data() + i, 4)) ++n;
    return n;
}
void Tuple(std::ostream& out, const Bytes& image, size_t index) {
    out << '[';
    for (int c = 0; c < 4; ++c) { if (c) out << ','; out << int(image[index + c]); }
    out << ']';
}
void Reproduce(const std::filesystem::path& root, const std::filesystem::path& output) {
    std::ofstream json(output / "reproduction.json");
    json << "{\"license\":\"MIT OR Apache-2.0\",\"production_modified\":false,\"cases\":[";
    bool comma = false;
    for (const auto& photo : photos) {
        auto s = PhotoSample(root, photo, output);
        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA); ref.Decode(s);
        Decoder d; Require(d.Initialize() == DecodeError::None, "repro init");
        DecodeRequest request; request.diagnostic_strategy = DiagnosticDecodeStrategy::Lightweight;
        Bytes first; bool identical = true; size_t diffs = 0;
        DeviceOutput surface; DecodeTicket first_ticket;
        for (int i = 0; i < 100; ++i) {
            const auto t = Begin(d, s, request);
            auto image = Image(d, t, s);
            if (i == 0) { first = image; first_ticket = t; d.GetDeviceOutput(t, &surface); diffs = PixelDiffs(first, ref.output); }
            else identical &= image == first;
            Require(d.ReleaseOutput(t), "repro release");
        }
        Require(diffs == photo.old_mismatches, "original mismatch count must reproduce before repair");
        Require(identical, "repeatability gate: moving mismatch requires different investigation");
        if (comma) json << ','; comma = true;
        json << "{\"frame\":\"" << photo.id << "\",\"source_fnv1a64\":\"" << photo.source_hash
             << "\",\"sample_fnv1a64\":\"" << photo.sample_hash << "\",\"sample_bytes\":" << photo.bytes
             << ",\"repeats\":100,\"identical_counts_coordinates_values\":true,\"bgra_mismatching_pixels\":" << diffs
             << ",\"gpu_bgra_fnv1a64\":\"" << Hash(first.data(), first.size())
             << "\",\"cpu_bgra_fnv1a64\":\"" << Hash(ref.output.data(), ref.output.size())
             << "\",\"matrix\":\"BT709\",\"range\":\"CG\",\"orientation\":\"bottom_up\",\"width\":1920,\"height\":1080"
             << ",\"bgra_pitch\":" << surface.planes[0].pitch_bytes << ",\"strategy\":\"lightweight\",\"ticket\":" << first_ticket.value
             << ",\"first32\":[";
        int recorded = 0;
        for (size_t i = 0; i < first.size() && recorded < 32; i += 4) {
            if (!std::memcmp(first.data() + i, ref.output.data() + i, 4)) continue;
            if (recorded++) json << ',';
            json << "{\"x\":" << (i / 4) % 1920 << ",\"output_row\":" << i / (1920 * 4)
                 << ",\"gpu_bgra\":"; Tuple(json, first, i); json << ",\"cpu_bgra\":"; Tuple(json, ref.output, i); json << '}';
        }
        json << "]}";
        std::cout << photo.id << " reproduced " << diffs << " pixels, 100/100 identical\n" << std::flush;
    }
    json << "]}\n";
}
using Planes = std::array<HyperCineTest::AlignedVector<int16_t>, 3>;
Planes ReadPlanes(const DecoderReconstruction& r) {
    Planes out;
    for (int ch = 0; ch < 3; ++ch) {
        const auto& p = r.planes[ch]; out[ch].resize(size_t(p.width) * p.height);
        GpuCheck(cudaMemcpy2D(out[ch].data(), size_t(p.width) * 2, p.device_pointer, p.pitch_bytes,
                             size_t(p.width) * 2, p.height, cudaMemcpyDeviceToHost));
    }
    return out;
}
void ReferenceColor(ReferenceDecoder& ref, Sample& s, const DecodeRequest& request) {
    CFHD_MetadataRef raw = nullptr; Check(CFHD_OpenMetadata(&raw), "color metadata"); Metadata owner(raw);
    Check(CFHD_InitSampleMetadata(raw, METADATATYPE_ORIGINAL, s.compressed.data(), uint32_t(s.compressed.size())), "color metadata init");
    uint32_t matrix = request.matrix == DecodeMatrix::BT709 ? 2 : 1;
    uint32_t range = request.range == DecodeRange::ComputerGraphics ? 1 : 2;
    Check(CFHD_SetActiveMetadata(ref.GetHandle(), raw, TAG_COLORSPACE_YUV, METADATATYPE_UINT32, &matrix, 4), "reference matrix");
    Check(CFHD_SetActiveMetadata(ref.GetHandle(), raw, TAG_COLORSPACE_RGB, METADATATYPE_UINT32, &range, 4), "reference range");
    ref.Decode(s);
    if (request.orientation == DecodeOrientation::TopDown) {
        for (int y = 0; y < s.height / 2; ++y)
            for (int x = 0; x < ref.pitch; ++x)
                std::swap(ref.output[size_t(y) * ref.pitch + x], ref.output[size_t(s.height - 1 - y) * ref.pitch + x]);
    }
}
void YuvTuple(std::ostream& out, const Planes& p, int w, int x, int row) {
    out << '[' << p[0][size_t(row) * w + x] << ',' << p[1][size_t(row) * (w / 2) + x / 2]
        << ',' << p[2][size_t(row) * (w / 2) + x / 2] << ']';
}
Planes CpuPlanes(const CpuIntermediateReference& ref) {
    Planes p;
    for (int ch = 0; ch < 3; ++ch) {
        const auto& v = ref.plane[ch == 1 ? 2 : ch == 2 ? 1 : 0]; p[ch].assign(v.begin(), v.end());
    }
    return p;
}
void PixelEvidence(std::ostream& out, const Sample& s, const DecodeRequest& request,
                   const DecoderReconstruction& r, const DeviceOutput& surface,
                   const Planes& cpu, const Planes& gpu, const Bytes& expected, const Bytes& actual,
                   int x, int output_row, bool neighborhood) {
    const int row = request.orientation == DecodeOrientation::BottomUp ? s.height - 1 - output_row : output_row;
    const size_t index = (size_t(output_row) * s.width + x) * 4;
    out << "{\"x\":" << x << ",\"output_row\":" << output_row << ",\"plane_row\":" << row
        << ",\"chroma_x\":" << x / 2 << ",\"cpu_yuv\":"; YuvTuple(out, cpu, s.width, x, row);
    out << ",\"gpu_yuv\":"; YuvTuple(out, gpu, s.width, x, row);
    out << ",\"cpu_bgra\":"; Tuple(out, expected, index);
    out << ",\"gpu_bgra\":"; Tuple(out, actual, index);
    out << ",\"addresses\":[";
    for (int ch = 0; ch < 4; ++ch) {
        if (ch) out << ',';
        const auto& p = ch == 3 ? surface.planes[0] : r.planes[ch];
        const int address_row = ch == 3 ? output_row : row;
        const int col = ch == 1 || ch == 2 ? x / 2 : x;
        const size_t element = ch == 3 ? 4 : 2;
        const size_t offset = size_t(address_row) * p.pitch_bytes + size_t(col) * element;
        const size_t allocation = ch == 3 ? p.pitch_bytes * s.height : r.allocation_bytes[ch];
        Require(offset + element <= allocation, "pixel offset in allocation");
        out << "{\"base\":\"" << p.device_pointer << "\",\"pitch\":" << p.pitch_bytes << ",\"row\":" << address_row
            << ",\"column\":" << col << ",\"byte_offset\":" << offset << ",\"allocation_bytes\":" << allocation << '}';
    }
    out << ']';
    if (neighborhood) {
        out << ",\"neighborhood3x3\":["; bool comma = false;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = output_row + dy;
            if (nx < 0 || nx >= s.width || ny < 0 || ny >= s.height) continue;
            if (comma) out << ','; comma = true;
            PixelEvidence(out, s, request, r, surface, cpu, gpu, expected, actual, nx, ny, false);
        }
        out << ']';
    }
    out << '}';
}
bool AuditFrame(Decoder& d, Sample& s, const std::string& id, const DecodeRequest& request,
                std::ostream& stages, std::ostream* pixels, bool& comma) {
    ReferenceDecoder bgra(s, CFHD_PIXEL_FORMAT_BGRA); ReferenceColor(bgra, s, request);
    ReferenceDecoder yu64(s, CFHD_PIXEL_FORMAT_YU64); yu64.Decode(s);
    const auto bands = ExtractBandsFromNative(bgra.GetNative());
    // The upstream LL channel offset is output-format dependent (BGRA=24,
    // YU64/planar=4). Compare each request with its corresponding CPU decode.
    const auto reference = ComputeCpuReferenceIntermediates(bgra.GetNative());
    const auto planar_reference = ComputeCpuReferenceIntermediates(yu64.GetNative());
    const auto cpu = CpuPlanes(reference);
    std::vector<GpuEntropyBandDescriptor> parsed; std::string error;
    Require(ParseCineFormSampleValidated(s.compressed.data(), s.compressed.size(), 24, parsed, &error), "audit parse");
    size_t descriptor_diffs = 0;
    for (size_t b = 0; b < parsed.size(); ++b)
        descriptor_diffs += parsed[b].channel != bands[b].channel || parsed[b].level != bands[b].level ||
                            parsed[b].band != bands[b].band || parsed[b].width != bands[b].width || parsed[b].height != bands[b].height;
    const auto t = Begin(d, s, request);
    DeviceOutput surface; Require(d.GetDeviceOutput(t, &surface), "audit output");
    DecoderReconstruction reconstruction; Require(DecoderInspector::Reconstruction(d, t, &reconstruction), "same ticket reconstruction");
    const auto actual = Image(d, t, s); const auto gpu = ReadPlanes(reconstruction);
    size_t coeff_diffs[30] = {}, ll_diffs[6] = {}, planar_diffs[3] = {};
    for (int b = 0; b < 30; ++b) {
        std::vector<int16_t> v; int w, h, pitch;
        Require(DecoderInspector::DownloadBand(d, t, b, v, &w, &h, &pitch), "audit coefficients");
        Require(w == bands[b].width && h == bands[b].height, "audit coefficient geometry");
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            coeff_diffs[b] += v[size_t(y) * (pitch / 2) + x] != bands[b].host_data[size_t(y) * (bands[b].pitch / 2) + x];
    }
    for (int ch = 0; ch < 3; ++ch) {
        for (int l = 0; l < 2; ++l) {
            std::vector<int16_t> ll; Require(DecoderInspector::DownloadIntermediate(d, t, ch, l, ll), "audit LL");
            const auto& expected = l ? reference.ll1[ch] : reference.ll0[ch];
            Require(ll.size() == expected.size(), "audit LL geometry");
            for (size_t i = 0; i < ll.size(); ++i) ll_diffs[2 * ch + l] += ll[i] != expected[i];
        }
        for (size_t i = 0; i < cpu[ch].size(); ++i) planar_diffs[ch] += cpu[ch][i] != gpu[ch][i];
    }
    const size_t bgra_diffs = PixelDiffs(actual, bgra.output);
    // Re-run color after inverse has completed on a single non-default stream.
    // Two different sentinel fills must both be completely overwritten.
    Stream color_stream; DeviceBuffer<uint8_t> color(size_t(s.width) * s.height * 4);
    bool isolated_identical = true;
    for (int sentinel : {0xcd, 0xa5}) {
        GpuCheck(cudaMemsetAsync(color.p, sentinel, size_t(s.width) * s.height * 4, color_stream.handle));
        Require(LaunchYuv422ToBgra8(static_cast<int16_t*>(reconstruction.planes[0].device_pointer), reconstruction.planes[0].pitch_bytes,
            static_cast<int16_t*>(reconstruction.planes[2].device_pointer), reconstruction.planes[2].pitch_bytes,
            static_cast<int16_t*>(reconstruction.planes[1].device_pointer), reconstruction.planes[1].pitch_bytes,
            color.p, size_t(s.width) * 4, s.width, s.height,
            request.matrix == DecodeMatrix::BT709 ? ColorMatrix::BT709 : ColorMatrix::BT601,
            request.range == DecodeRange::ComputerGraphics ? ColorRange::CG : ColorRange::VS,
            request.orientation == DecodeOrientation::BottomUp ? RowOrientation::BottomUp : RowOrientation::TopDown,
            color_stream.handle), "isolated color launch");
        GpuCheck(cudaStreamSynchronize(color_stream.handle));
        Bytes isolated(actual.size()); GpuCheck(cudaMemcpy(isolated.data(), color.p, isolated.size(), cudaMemcpyDeviceToHost));
        isolated_identical &= isolated == actual;
    }
    Require(isolated_identical, "serialized color must reproduce production bytes");
    size_t clone_old_matches = 0, clone_signed_matches = 0, negative_y_mismatches = 0;
    const auto coefficients = GetColorCoefficients(request.matrix == DecodeMatrix::BT709 ? ColorMatrix::BT709 : ColorMatrix::BT601,
        request.range == DecodeRange::ComputerGraphics ? ColorRange::CG : ColorRange::VS);
    for (size_t i = 0; i < actual.size(); i += 4) {
        if (!std::memcmp(actual.data() + i, bgra.output.data() + i, 4)) continue;
        const int x = int((i / 4) % s.width), outrow = int(i / (s.width * 4));
        const int row = request.orientation == DecodeOrientation::BottomUp ? s.height - 1 - outrow : outrow;
        const auto y = gpu[0][size_t(row) * s.width + x], u = gpu[1][size_t(row) * (s.width / 2) + x / 2],
                   v = gpu[2][size_t(row) * (s.width / 2) + x / 2];
        const auto old = DecodeAE::Color(y, u, v, x, s.width, coefficients, false);
        const auto corrected = DecodeAE::Color(y, u, v, x, s.width, coefficients, true);
        clone_old_matches += !std::memcmp(old.data(), actual.data() + i, 4);
        clone_signed_matches += !std::memcmp(corrected.data(), bgra.output.data() + i, 4);
        negative_y_mismatches += y < 0;
    }
    DecodeFrameInfo info; Require(d.GetFrameInfo(t, &info), "audit strategy");
    // Preserve the BGRA ticket during the separate planar request.
    DecodeRequest planar_request = request; planar_request.format = DecodePixelFormat::PlanarYUV422Signed16;
    const auto pt = Begin(d, s, planar_request);
    DecoderReconstruction pr; Require(DecoderInspector::Reconstruction(d, pt, &pr), "planar request reconstruction");
    const auto pp = ReadPlanes(pr); const auto pcpu = CpuPlanes(planar_reference);
    size_t public_planar_diffs = 0;
    for (int ch = 0; ch < 3; ++ch) for (size_t i = 0; i < pp[ch].size(); ++i) public_planar_diffs += pp[ch][i] != pcpu[ch][i];
    Bytes packed(size_t(s.width) * s.height * 4);
    ConvertPlanarToYU64(pp[0].data(), s.width * 2, pp[2].data(), s.width, pp[1].data(), s.width,
                        s.width, s.height, 10, packed.data(), s.width * 4);
    size_t yu64_diffs = 0;
    for (size_t i = 0; i < packed.size(); i += 2) yu64_diffs += std::memcmp(packed.data() + i, yu64.output.data() + i, 2) != 0;
    Require(d.ReleaseOutput(pt), "planar audit release");
    if (comma) stages << ','; comma = true;
    stages << "{\"frame\":\"" << id << "\",\"requested_strategy\":" << int(request.diagnostic_strategy)
           << ",\"selected_strategy\":\"" << StrategyName(info.strategy) << "\",\"matrix\":" << int(request.matrix)
           << ",\"range\":" << int(request.range) << ",\"orientation\":" << int(request.orientation)
           << ",\"slot\":" << reconstruction.slot_index << ",\"ticket\":" << t.value
           << ",\"descriptor_differences\":" << descriptor_diffs << ",\"coefficient_differences\":[";
    for (int b = 0; b < 30; ++b) { if (b) stages << ','; stages << coeff_diffs[b]; }
    stages << "],\"ll_differences\":[";
    for (int i = 0; i < 6; ++i) { if (i) stages << ','; stages << ll_diffs[i]; }
    stages << "],\"planar_differences\":[";
    for (int i = 0; i < 3; ++i) { if (i) stages << ','; stages << planar_diffs[i]; }
    stages << "],\"public_planar_request_differences\":" << public_planar_diffs
           << ",\"yu64_word_differences\":" << yu64_diffs << ",\"bgra_pixel_differences\":" << bgra_diffs
           << ",\"serialized_color_identical\":true,\"sentinel_fills\":[205,165],\"old_cpu_clone_matches_gpu\":" << clone_old_matches
           << ",\"signed_cpu_clone_matches_reference\":" << clone_signed_matches << ",\"negative_y_mismatches\":" << negative_y_mismatches << '}';
    if (pixels) {
        *pixels << "{\"frame\":\"" << id << "\",\"strategy\":" << int(request.diagnostic_strategy) << ",\"pixels\":[";
        int n = 0;
        for (size_t i = 0; i < actual.size(); i += 4) {
            if (!std::memcmp(actual.data() + i, bgra.output.data() + i, 4)) continue;
            if (n) *pixels << ',';
            PixelEvidence(*pixels, s, request, reconstruction, surface, cpu, gpu, bgra.output, actual,
                          int((i / 4) % s.width), int(i / (s.width * 4)), n++ < 16);
        }
        *pixels << "]}\n";
    }
    Require(d.ReleaseOutput(t), "audit release");
    const bool exact = !descriptor_diffs && !bgra_diffs && !yu64_diffs && !public_planar_diffs &&
        std::all_of(std::begin(coeff_diffs), std::end(coeff_diffs), [](size_t n) { return n == 0; }) &&
        std::all_of(std::begin(ll_diffs), std::end(ll_diffs), [](size_t n) { return n == 0; }) &&
        std::all_of(std::begin(planar_diffs), std::end(planar_diffs), [](size_t n) { return n == 0; });
    std::cout << id << " strategy=" << int(request.diagnostic_strategy) << " planar=" << planar_diffs[0] << ','
              << planar_diffs[1] << ',' << planar_diffs[2] << " BGRA=" << bgra_diffs << '\n' << std::flush;
    return exact;
}
void Audit(const std::filesystem::path& root, const std::filesystem::path& output, bool strict, bool focused = false) {
    std::ofstream stages(output / "stage_compare.json"), pixels(output / "pixels.jsonl");
    stages << "{\"license\":\"MIT OR Apache-2.0\",\"cases\":[";
    bool comma = false, exact = true;
    for (const auto& photo : photos) {
        auto sample = PhotoSample(root, photo, output);
        Decoder decoder; Require(decoder.Initialize() == DecodeError::None, "audit init");
        for (auto strategy : {DiagnosticDecodeStrategy::Lightweight, DiagnosticDecodeStrategy::Hybrid,
                              DiagnosticDecodeStrategy::HostStreaming, DiagnosticDecodeStrategy::Automatic})
            for (int matrix = 0; matrix < 2; ++matrix) for (int range = 0; range < 2; ++range) for (int orientation = 0; orientation < 2; ++orientation) {
                if (focused && (matrix || range || orientation)) continue;
                DecodeRequest request; request.diagnostic_strategy = strategy;
                request.matrix = DecodeMatrix(matrix); request.range = DecodeRange(range); request.orientation = DecodeOrientation(orientation);
                exact &= AuditFrame(decoder, sample, photo.id, request, stages,
                                    matrix == 0 && range == 0 && orientation == 0 ? &pixels : nullptr, comma);
            }
    }
    stages << "]}\n"; stages.close();
    Require(!strict || exact, "audit requires zero divergence");
}
Sample Synthetic() {
    Bytes input(size_t(64) * 64 * 2);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        input[(size_t(y) * 64 + x) * 2] = ((x >= 24 && x < 32 && y >= 20 && y < 44) || (x > 59 && y > 59)) ? 255 : 0;
        input[(size_t(y) * 64 + x) * 2 + 1] = 128;
    }
    return EncodeInput(input, 64, 64, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODING_QUALITY_FILMSCAN3, 0);
}
void Unit(const std::filesystem::path& output, bool strict) {
    // Literal real-photo trigger, followed by signed overshoots, clamp edges,
    // chroma extremes, both fast and scalar columns, and both orientations.
    constexpr int values[] = {-32768, -1024, -64, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 63, 64, 65, 511, 512, 513, 1023, 1024, 1087, 32767};
    constexpr int w = 64, h = 64;
    Planes planes; planes[0].resize(w * h); planes[1].resize(w / 2 * h); planes[2].resize(w / 2 * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) planes[0][y * w + x] = int16_t(values[(x + 3 * y) % std::size(values)]);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w / 2; ++x) {
        planes[1][y * w / 2 + x] = int16_t(values[(x * 7 + y) % std::size(values)]);
        planes[2][y * w / 2 + x] = int16_t(values[(x * 3 + y * 5) % std::size(values)]);
    }
    planes[0][3] = -1; planes[1][1] = 495; planes[2][1] = 524;
    DeviceBuffer<int16_t> dy(w * h), du(w / 2 * h), dv(w / 2 * h);
    DeviceBuffer<uint8_t> packed(w * h * 4); Stream stream;
    GpuCheck(cudaMemcpy(dy.p, planes[0].data(), w * h * 2, cudaMemcpyHostToDevice));
    GpuCheck(cudaMemcpy(du.p, planes[1].data(), w * h, cudaMemcpyHostToDevice));
    GpuCheck(cudaMemcpy(dv.p, planes[2].data(), w * h, cudaMemcpyHostToDevice));
    size_t diffs = 0;
    for (auto matrix : {ColorMatrix::BT709, ColorMatrix::BT601}) for (auto range : {ColorRange::CG, ColorRange::VS})
        for (auto orientation : {RowOrientation::TopDown, RowOrientation::BottomUp}) {
            GpuCheck(cudaMemsetAsync(packed.p, 0xcd, w * h * 4, stream.handle));
            Require(LaunchYuv422ToBgra8(dy.p, w * 2, dv.p, w, du.p, w, packed.p, w * 4, w, h, matrix, range, orientation, stream.handle), "unit color launch");
            GpuCheck(cudaStreamSynchronize(stream.handle));
            Bytes actual(w * h * 4); GpuCheck(cudaMemcpy(actual.data(), packed.p, actual.size(), cudaMemcpyDeviceToHost));
            if (matrix == ColorMatrix::BT709 && range == ColorRange::CG) {
                const int row = orientation == RowOrientation::TopDown ? 0 : h - 1;
                const uint8_t literal[] = {0, 0, 5, 255};
                diffs += std::memcmp(actual.data() + (row * w + 3) * 4, literal, 4) != 0;
            }
            const auto c = GetColorCoefficients(matrix, range);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const int outrow = orientation == RowOrientation::TopDown ? y : h - 1 - y;
                const auto expected = DecodeAE::Color(planes[0][y * w + x], planes[1][y * (w / 2) + x / 2],
                    planes[2][y * (w / 2) + x / 2], x, w, c, true);
                diffs += std::memcmp(expected.data(), actual.data() + (outrow * w + x) * 4, 4) != 0;
            }
        }
    std::ofstream stages(output / "unit.json"); stages << "{\"direct_color_differences\":" << diffs << ",\"encoded_cases\":[";
    auto s = Synthetic(); DecoderConfig cfg; cfg.maximum_width = cfg.maximum_height = 64; Decoder d(cfg);
    Require(d.Initialize() == DecodeError::None, "synthetic init"); bool comma = false, exact = diffs == 0;
    for (auto strategy : {DiagnosticDecodeStrategy::Lightweight, DiagnosticDecodeStrategy::Hybrid,
                          DiagnosticDecodeStrategy::HostStreaming, DiagnosticDecodeStrategy::Automatic}) {
        DecodeRequest r; r.diagnostic_strategy = strategy;
        exact &= AuditFrame(d, s, "synthetic_64x64_signed_undershoot", r, stages, nullptr, comma);
    }
    stages << "]}\n"; stages.close();
    Require(!strict || exact, "self-contained signed-plane regression");
}
void Performance(const std::filesystem::path& root, const std::filesystem::path& output) {
    std::ofstream json(output / "performance.json"); json << "{\"cases\":["; bool comma = false;
    auto measure = [&](Sample& s, const std::string& id) {
        DecoderConfig config; config.maximum_width = s.width; config.maximum_height = s.height;
        Decoder d(config); Require(d.Initialize() == DecodeError::None, "performance init");
        for (auto strategy : {DiagnosticDecodeStrategy::Lightweight, DiagnosticDecodeStrategy::Hybrid}) {
            DecodeRequest r; r.diagnostic_strategy = strategy;
            for (int i = 0; i < 15; ++i) { auto t = Begin(d, s, r); d.ReleaseOutput(t); }
            double total = 0, low = 1e9, high = 0;
            for (int i = 0; i < 120; ++i) {
                const auto start = Clock::now(); auto t = Begin(d, s, r); Require(d.ReleaseOutput(t), "timed release");
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                total += ms; low = (std::min)(low, ms); high = (std::max)(high, ms);
            }
            const auto memory = d.MemoryStats();
            if (comma) json << ','; comma = true;
            json << "{\"frame\":\"" << id << "\",\"strategy\":" << int(strategy) << ",\"frames\":120,\"mean_ms\":" << total / 120
                 << ",\"min_ms\":" << low << ",\"max_ms\":" << high << ",\"fps\":" << 120000 / total
                 << ",\"megapixels_per_second\":" << double(s.width) * s.height * 0.12 / total
                 << ",\"device_bytes\":" << memory.device_bytes << ",\"pinned_bytes\":" << memory.pinned_host_bytes
                 << ",\"cuda_allocation_calls\":" << memory.cuda_allocation_calls << ",\"peak_working_set\":" << WorkingSet(true) << '}';
        }
    };
    for (int w : {1920, 3840}) {
        auto samples = Generate(w, w == 1920 ? 1080 : 2160, 3);
        measure(samples[0], std::to_string(w) + "_flat"); measure(samples[2], std::to_string(w) + "_noise");
    }
    for (const auto& photo : photos) { auto s = PhotoSample(root, photo, output); measure(s, photo.id); }
    json << "]}\n";
}
void Reuse(const std::filesystem::path& root, const std::filesystem::path& output, bool strict, int repetitions) {
    auto input = MakeInput(1920, 1080, 0);
    for (size_t i = 0; i < input.size(); i += 2) input[i] = 0;
    auto black = EncodeInput(input, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODING_QUALITY_FILMSCAN3, 0);
    for (size_t i = 0; i < input.size(); i += 2) input[i] = 255;
    auto white = EncodeInput(input, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODING_QUALITY_FILMSCAN3, 0);
    std::ofstream json(output / "reuse.json"); json << "{\"cases\":["; bool comma = false;
    for (const auto& photo : photos) {
        auto s = PhotoSample(root, photo, output); ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA); ref.Decode(s);
        for (int slots : {1, 2}) {
            DecoderConfig cfg; cfg.slot_count = slots; Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "reuse init");
            DecodeRequest request; request.diagnostic_strategy = DiagnosticDecodeStrategy::Lightweight;
            const auto fresh = Begin(d, s, request); const auto expected = Image(d, fresh, s);
            const size_t errors = PixelDiffs(expected, ref.output); Require(!strict || errors == 0, "fresh exact");
            d.ReleaseOutput(fresh); const auto before = d.MemoryStats();
            Stream consumer; DeviceBuffer<uint8_t> consumed(expected.size());
            for (int i = 0; i < repetitions; ++i) {
                auto& prior = i % 2 ? black : white;
                const auto preceding = Begin(d, prior, request);
                DeviceOutput preceding_output; Require(d.GetDeviceOutput(preceding, &preceding_output), "prior output");
                DecoderReconstruction prior_planes; Require(DecoderInspector::Reconstruction(d, preceding, &prior_planes), "prior planes");
                // Poison completed borrowed output and inverse surfaces before
                // release. This tests stale reuse without production debug fills.
                GpuCheck(cudaMemset(preceding_output.planes[0].device_pointer, i % 2 ? 0xcd : 0xa5,
                                    preceding_output.planes[0].pitch_bytes * s.height));
                for (int ch = 0; ch < 3; ++ch) GpuCheck(cudaMemset(prior_planes.planes[ch].device_pointer,
                    i % 2 ? 0xa5 : 0xcd, prior_planes.allocation_bytes[ch]));
                if (slots == 1 || i % 3) Require(d.ReleaseOutput(preceding), "preceding release");
                const auto t = Begin(d, s, request);
                Require(Image(d, t, s) == expected, "previous-frame or sentinel contamination");
                Require(d.StreamWait(t, CudaDecodeConsumer(consumer.handle)), "consumer ready wait");
                DeviceOutput out; Require(d.GetDeviceOutput(t, &out), "consumer output");
                GpuCheck(cudaMemcpy2DAsync(consumed.p, size_t(s.width) * 4, out.planes[0].device_pointer,
                    out.planes[0].pitch_bytes, size_t(s.width) * 4, s.height, cudaMemcpyDeviceToDevice, consumer.handle));
                if (slots == 2 && !(i % 3)) {
                    Require(d.ReleaseOutput(t), "non-FIFO release");
                    Require(d.ReleaseOutput(preceding), "held release");
                } else Require(d.ReleaseOutput(t), "reuse release");
                Bytes copy(expected.size()); GpuCheck(cudaMemcpy(copy.data(), consumed.p, copy.size(), cudaMemcpyDeviceToHost));
                Require(copy == expected, "release drains registered consumer");
            }
            const auto after = d.MemoryStats();
            Require(before.device_bytes == after.device_bytes && before.pinned_host_bytes == after.pinned_host_bytes &&
                    before.cuda_allocation_calls == after.cuda_allocation_calls, "owned capacities stable");
            if (comma) json << ','; comma = true;
            json << "{\"frame\":\"" << photo.id << "\",\"slots\":" << slots << ",\"repeats\":" << repetitions
                 << ",\"identical_to_fresh\":true,\"mismatches_vs_cpu\":" << errors
                 << ",\"previous_frame_contamination\":false,\"sentinel_retention\":false,\"consumer_and_non_fifo\":true,\"owned_memory_stable\":true}";
        }
    }
    json << "]}\n";
}
void Mixed(const std::filesystem::path& root, const std::filesystem::path& output) {
    auto samples = Generate(1920, 1080, 5);
    for (const auto& photo : photos) samples.push_back(PhotoSample(root, photo, output));
    auto text = HoldoutInput(1920, 1080, 3);
    samples.push_back(EncodeInput(text, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODING_QUALITY_FILMSCAN3, 3));
    std::vector<Bytes> expected;
    for (auto& s : samples) { ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA); ref.Decode(s); expected.push_back(ref.output); }
    Decoder d; Require(d.Initialize() == DecodeError::None, "mixed init");
    const auto before = d.MemoryStats(); size_t free_before, total;
    GpuCheck(cudaMemGetInfo(&free_before, &total)); const auto ws_before = WorkingSet();
    for (int i = 0; i < 2000; i += 2) {
        DecodeTicket tickets[2]; int indices[2];
        for (int j = 0; j < 2; ++j) {
            indices[j] = (i + j) % int(samples.size());
            DecodeRequest r;
            r.diagnostic_strategy = (i + j) % 3 == 0 ? DiagnosticDecodeStrategy::Lightweight :
                                    (i + j) % 3 == 1 ? DiagnosticDecodeStrategy::Hybrid : DiagnosticDecodeStrategy::Automatic;
            DecodeError error;
            auto& s = samples[indices[j]];
            tickets[j] = d.QueueDecode(s.compressed.data(), s.compressed.size(), r, &error);
            Require(bool(tickets[j]) && error == DecodeError::None, "mixed queue");
        }
        for (int j : {1, 0}) {
            Require(d.WaitForDecode(tickets[j]).state == DecodeState::Ready, "mixed ready");
            Require(Image(d, tickets[j], samples[indices[j]]) == expected[indices[j]], "mixed exact output");
        }
        // Vary release order while both completed outputs remain held.
        const int first = (i / 2) % 2;
        Require(d.ReleaseOutput(tickets[first]) && d.ReleaseOutput(tickets[1 - first]), "mixed release");
        if (i % 500 == 0) std::cout << "mixed " << i + 2 << "/2000\n" << std::flush;
    }
    const auto after = d.MemoryStats(); size_t free_after;
    GpuCheck(cudaMemGetInfo(&free_after, &total));
    Require(before.device_bytes == after.device_bytes && before.pinned_host_bytes == after.pinned_host_bytes &&
            before.cuda_allocation_calls == after.cuda_allocation_calls, "mixed owned allocations stable");
    std::ofstream json(output / "mixed.json");
    json << "{\"frames\":2000,\"cases\":9,\"slots\":2,\"exact_every_frame\":true,\"non_fifo_release\":true,\"owned_capacities_stable\":true"
         << ",\"free_vram_before\":" << free_before << ",\"free_vram_after\":" << free_after
         << ",\"working_set_before\":" << ws_before << ",\"working_set_after\":" << WorkingSet() << "}\n";
}
void Instances(const std::filesystem::path& root, const std::filesystem::path& output) {
    std::vector<Sample> samples; std::vector<Bytes> expected;
    for (const auto& photo : photos) {
        samples.push_back(PhotoSample(root, photo, output));
        ReferenceDecoder ref(samples.back(), CFHD_PIXEL_FORMAT_BGRA); ref.Decode(samples.back()); expected.push_back(ref.output);
    }
    std::ofstream json(output / "instances.json"); json << "{\"cases\":[";
    for (int count : {2, 4}) {
        std::vector<std::thread> threads; std::array<std::exception_ptr, 4> errors{};
        for (int instance = 0; instance < count; ++instance) threads.emplace_back([&, instance] {
            try {
                Decoder d; Require(d.Initialize() == DecodeError::None, "instance init");
                DecodeRequest r; r.diagnostic_strategy = DiagnosticDecodeStrategy::Lightweight;
                for (int i = 0; i < 100; ++i) {
                    const size_t index = size_t(i + instance) % samples.size();
                    const auto t = Begin(d, samples[index], r);
                    Require(Image(d, t, samples[index]) == expected[index], "cross-instance output");
                    Require(d.ReleaseOutput(t), "instance release");
                }
            } catch (...) { errors[instance] = std::current_exception(); }
        });
        for (auto& t : threads) t.join();
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        if (count == 4) json << ',';
        json << "{\"instances\":" << count << ",\"frames_per_instance\":100,\"real_photo_output_exact\":true}";
    }
    json << "]}\n";
}
void Golden(const std::filesystem::path& root, const std::filesystem::path& output) {
    std::ifstream manifest(root / "tests/decode_phase0_golden.txt"); Require(bool(manifest), "frozen golden manifest");
    std::ofstream json(output / "golden.json"); json << "{\"expected_hashes_regenerated\":false,\"cases\":[";
    std::string line; int previous_width = 0, count = 0; bool comma = false, exact = true;
    std::vector<Sample> samples; std::unique_ptr<Decoder> decoder;
    while (std::getline(manifest, line)) {
        if (line.empty() || line[0] == '#') continue;
        int w, h, p; size_t bytes; std::string ih, ch, bh, yh; std::istringstream row(line);
        Require(bool(row >> w >> h >> p >> ih >> ch >> bytes >> bh >> yh), "golden row");
        if (w != previous_width) {
            previous_width = w; samples = Generate(w, h, w == 7680 ? 1 : 5);
            DecoderConfig cfg; cfg.maximum_width = w; cfg.maximum_height = h;
            decoder = std::make_unique<Decoder>(cfg); Require(decoder->Initialize() == DecodeError::None, "golden init");
        }
        auto& s = samples[p];
        Require(s.input_hash == ih && s.compressed.size() == bytes && Hash(s.compressed.data(), bytes) == ch, "frozen golden sample");
        ReferenceDecoder bgra(s, CFHD_PIXEL_FORMAT_BGRA), yu64(s, CFHD_PIXEL_FORMAT_YU64); bgra.Decode(s); yu64.Decode(s);
        Require(Hash(bgra.output.data(), bgra.output.size()) == bh && Hash(yu64.output.data(), yu64.output.size()) == yh, "unchanged golden output hashes");
        for (auto strategy : {DiagnosticDecodeStrategy::Lightweight, DiagnosticDecodeStrategy::Hybrid,
                              DiagnosticDecodeStrategy::HostStreaming, DiagnosticDecodeStrategy::Automatic}) {
            DecodeRequest request; request.diagnostic_strategy = strategy;
            exact &= AuditFrame(*decoder, s, std::to_string(w) + "_" + PatternName(p), request, json, nullptr, comma);
        }
        ++count;
    }
    json << "],\"samples\":" << count << "}\n"; json.close();
    Require(exact && count == 11, "11/11 golden all four strategies and stages");
}
} // namespace
int main(int argc, char** argv) {
    try {
        Require(argc >= 4, "usage: test_decode_phase3ae --reproduce ROOT OUTPUT");
        const std::filesystem::path root = argv[2], output = argv[3];
        std::filesystem::create_directories(output);
        if (std::string(argv[1]) == "--reproduce") Reproduce(root, output);
        else if (std::string(argv[1]) == "--audit") Audit(root, output, false);
        else if (std::string(argv[1]) == "--validate") Audit(root, output, true);
        else if (std::string(argv[1]) == "--performance") Performance(root, output);
        else if (std::string(argv[1]) == "--reuse-pre") Reuse(root, output, false, 25);
        else if (std::string(argv[1]) == "--reuse") Reuse(root, output, true, 250);
        else if (std::string(argv[1]) == "--unit") Unit(output, true);
        else if (std::string(argv[1]) == "--unit-pre") Unit(output, false);
        else if (std::string(argv[1]) == "--focused-pre") { Unit(output, false); Audit(root, output, false, true); Reuse(root, output, false, 2); }
        else if (std::string(argv[1]) == "--sanitizer") { Unit(output, true); Audit(root, output, true, true); Reuse(root, output, true, 2); }
        else if (std::string(argv[1]) == "--mixed") Mixed(root, output);
        else if (std::string(argv[1]) == "--instances") Instances(root, output);
        else if (std::string(argv[1]) == "--golden") Golden(root, output);
        else throw std::runtime_error("unknown mode");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
