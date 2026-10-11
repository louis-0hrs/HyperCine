// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_phase3a_rc_peaks.h"
#include "mp4reader.h"
#include <numeric>
using namespace DecodeRC;
namespace {
struct Stats { double mean = 0, minimum = 1e9, maximum = 0, cpu = 0; };
Stats Measure(Decoder& d, const Sample& s, DiagnosticDecodeStrategy strategy, int count) {
    DecodeRequest request; request.diagnostic_strategy = strategy;
    auto decode = [&] { const auto t = Begin(d, s, request); Require(d.ReleaseOutput(t), "RC timed release"); };
    for (int i = 0; i < 5; ++i) decode();
    Stats result; const double cpu = CpuMs();
    for (int i = 0; i < count; ++i) {
        const auto start = Clock::now(); decode();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        result.mean += ms; result.minimum = (std::min)(result.minimum, ms); result.maximum = (std::max)(result.maximum, ms);
    }
    result.mean /= count; result.cpu = (CpuMs() - cpu) / count; return result;
}
void JsonStats(std::ostream& json, const Stats& s) {
    json << "{\"mean_ms\":" << s.mean << ",\"minimum_ms\":" << s.minimum << ",\"maximum_ms\":" << s.maximum
         << ",\"process_cpu_ms_per_frame\":" << s.cpu << '}';
}
struct Case { Sample sample; std::string name, origin; bool generated; int quality, precision, profile; };
std::array<int, 3> Profile(const Bytes& bytes) {
    std::array<int, 3> result{0, 0, 0};
    for (size_t p = 0; p + 4 <= bytes.size();) {
        const int raw = int16_t(BE16(bytes.data() + p)); const unsigned tag = unsigned(raw < 0 ? -raw : raw);
        const unsigned value = BE16(bytes.data() + p + 2); p += 4;
        if (tag == 70) result[0] = int(value);
        if (tag == 84) result[1] = int(value);
        if (tag == 68) result[2] = int(value);
        size_t skip = 0;
        if (tag == 2) skip = size_t(value) * 4;
        else if ((tag & 0xff00) == 0x2000) skip = size_t(((tag & 255) << 16) | value) * 4;
        else if (tag >= 0x4000 && tag < 0x6000) skip = size_t(value) * 4;
        Require(skip <= bytes.size() - p, "RC profile chunk bounds"); p += skip;
    }
    return result;
}
struct AviSource {
    void* handle; uint32_t* payload = nullptr;
    explicit AviSource(const std::filesystem::path& path) {
        std::string name = path.string(); handle = OpenAVISource(name.data(), AVI_TRAK_TYPE, AVI_TRAK_SUBTYPE);
        Require(handle != nullptr, "RC bundled AVI source");
    }
    ~AviSource() { if (payload) FreePayload(payload); if (handle) CloseSource(handle); }
    AviSource(const AviSource&) = delete;
};
Case External(const std::filesystem::path& root, bool rgb, unsigned frame) {
    const std::string file = rgb ? "CFHDsample444.AVI" : "CFHDsample422.AVI";
    AviSource source(root / "upstream/Example/videos" / file);
    Require(frame < GetNumberPayloads(source.handle), "RC AVI frame exists");
    source.payload = GetPayload(source.handle, source.payload, frame); const auto size = GetPayloadSize(source.handle, frame);
    Require(source.payload && size, "RC AVI frame payload");
    Sample sample{1920, 1080, -1, Bytes(size), {}}; std::memcpy(sample.compressed.data(), source.payload, size);
    const auto profile = Profile(sample.compressed);
    return {std::move(sample), file + ":frame" + std::to_string(frame), "upstream/Example/videos/" + file,
        false, -1, profile[0], profile[1]};
}
void Smoke(Decoder& d, std::vector<Sample>& samples, int frames) {
    std::vector<Bytes> expected; std::vector<DecodeStrategy> choices;
    std::vector<std::array<DecoderBandDecision, 27>> bands;
    for (auto& s : samples) {
        ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(s); expected.push_back(cpu.output);
        const auto t = Begin(d, s); DecodeFrameInfo info; std::array<DecoderBandDecision, 27> choice;
        Require(d.GetFrameInfo(t, &info) && DecoderInspector::BandDecisions(d, t, &choice), "RC smoke baseline");
        choices.push_back(info.strategy); bands.push_back(choice); Require(d.ReleaseOutput(t), "RC smoke warm release");
    }
    const auto before = d.MemoryStats(); uint32_t seed = HoldoutSeed;
    for (int i = 0; i < frames; i += 2) {
        std::array<DecodeTicket, 2> tickets; std::array<size_t, 2> patterns;
        for (int slot = 0; slot < 2; ++slot) {
            seed = seed * 1664525u + 1013904223u; const size_t p = seed % samples.size(); patterns[size_t(slot)] = p;
            tickets[size_t(slot)] = Begin(d, samples[p]);
        }
        // Retain both outputs and release out of order.
        for (int slot : {1, 0}) {
            const size_t p = patterns[size_t(slot)]; const auto t = tickets[size_t(slot)];
            DecodeFrameInfo info; std::array<DecoderBandDecision, 27> choice;
            Require(d.GetFrameInfo(t, &info) && info.strategy == choices[p] && DecoderInspector::BandDecisions(d, t, &choice)
                && Same(choice, bands[p]) && Image(d, t, samples[p]) == expected[p], "RC smoke bytes and choices");
            Require(d.ReleaseOutput(t), "RC smoke release");
        }
    }
    const auto after = d.MemoryStats();
    Require(before.device_bytes == after.device_bytes && before.pinned_host_bytes == after.pinned_host_bytes
        && before.pageable_capacity_bytes == after.pageable_capacity_bytes && before.cuda_allocation_calls == after.cuda_allocation_calls,
        "RC smoke stable owned memory");
}
} // namespace
int main(int argc, char** argv) {
    try {
        Require(argc == 4, "usage: test_decode_phase3a_rc --validate|--benchmark|--peaks root output-directory");
        const std::string mode = argv[1]; const bool benchmark = mode == "--benchmark";
        Require(benchmark || mode == "--validate" || mode == "--peaks", "RC valid mode");
        const std::filesystem::path root = argv[2], output = argv[3]; std::filesystem::create_directories(output);
        if (mode == "--peaks") { PeakSearch(output / "decode_phase3a_rc_peak_escape.json"); return 0; }
        std::ifstream manifest(root / "tests/decode_phase0_golden.txt"); Require(bool(manifest), "RC exclusion manifest");
        std::string frozen((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
        std::ofstream json(output / (benchmark ? "decode_phase3a_rc_holdout.json" : "decode_phase3a_rc_corpus_validation.json"));
        Require(bool(json), "RC corpus JSON open"); json << std::setprecision(12) << std::boolalpha;
        cudaDeviceProp gpu{}; GpuCheck(cudaGetDeviceProperties(&gpu, 0));
        json << "{\"license\":\"MIT OR Apache-2.0\",\"mode\":\"" << mode << "\",\"gpu\":\"" << gpu.name
             << "\",\"seed\":\"0x6A09E667\",\"hash_algorithm\":\"FNV1a64\",\"classifier_repeats_per_sample\":100,"
             << "\"timing\":\"queue through ready and release, device resident; 5 warmups; equivalent L/H/Auto counts; alternating L/H order\",\"cases\":[\n";
        int total = 0, supported_count = 0, unsupported_count = 0, smoke_frames = 0;
        for (int size = 0; size < 3; ++size) {
            const int w = size == 0 ? 1920 : size == 1 ? 3840 : 7680, h = size == 0 ? 1080 : size == 1 ? 2160 : 4320;
            DecoderConfig cfg; cfg.maximum_width = w; cfg.maximum_height = h; cfg.slot_count = 2;
            Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "RC corpus init");
            std::vector<Case> cases;
            if (size == 0) {
                for (unsigned frame : {0u, 12u, 23u}) cases.push_back(External(root, false, frame));
                cases.push_back(External(root, true, 0));
            }
            for (int p = 0; p < 15; ++p) {
                if (size == 2 && p != 6 && p != 8) continue;
                const auto quality = p == 14 ? CFHD_ENCODING_QUALITY_LOW : CFHD_ENCODING_QUALITY_FILMSCAN3;
                auto input = HoldoutInput(w, h, p); auto s = EncodeInput(input, w, h, CFHD_PIXEL_FORMAT_YUY2, quality, p);
                Require(frozen.find(s.input_hash) == std::string::npos && frozen.find(Hash(s.compressed.data(), s.compressed.size())) == std::string::npos,
                    "RC held-out excludes all frozen source/sample identities");
                const auto profile = Profile(s.compressed);
                cases.push_back({std::move(s), std::to_string(w) + "_" + Family(p), "deterministic_scene_proxy", true, int(quality), profile[0], profile[1]});
            }
            std::vector<Sample> smoke_samples;
            for (auto& c : cases) {
                auto& s = c.sample; std::vector<GpuEntropyBandDescriptor> descriptors; std::string reason; DecodeError error;
                const bool supported = ParseCineFormSampleValidated(s.compressed.data(), s.compressed.size(), 24, descriptors, &reason, &error);
                if (total++) json << ",\n";
                json << "{\"id\":\"" << c.name << "\",\"origin\":\"" << c.origin << "\",\"generated\":" << c.generated
                     << ",\"width\":" << w << ",\"height\":" << h << ",\"identified_precision\":" << c.precision
                     << ",\"encoded_format_tag84\":" << c.profile << ",\"quality_enum\":" << c.quality
                     << ",\"sample_bytes\":" << s.compressed.size() << ",\"compressed_hash\":\"" << Hash(s.compressed.data(), s.compressed.size())
                     << "\",\"supported\":" << supported;
                if (!supported) {
                    ++unsupported_count;
                    ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA); cpu.Decode(s);
                    auto t = d.QueueDecode(s.compressed.data(), s.compressed.size());
                    Require(bool(t) && d.WaitForDecode(t).state == DecodeState::Failed, "RC unsupported rejected");
                    DeviceOutput out; Require(!d.GetDeviceOutput(t, &out), "RC unsupported output unpublished"); d.ReleaseOutput(t);
                    json << ",\"frame_hash\":\"" << Hash(cpu.output.data(), cpu.output.size())
                         << "\",\"frame_hash_kind\":\"decoded_CPU_BGRA\",\"rejection_reason\":\"" << reason << "\",\"production_error\":" << int(error) << '}';
                    std::cout << c.name << " unsupported: " << reason << '\n' << std::flush; continue;
                }
                ++supported_count; DecodeFrameInfo info; const auto strategy = ClassifyDecodeFrame(descriptors, info);
                uint64_t nanos = 0;
                for (int repeat = 0; repeat < 100; ++repeat) {
                    DecodeFrameInfo current; const auto start = Clock::now(); const auto selected = ClassifyDecodeFrame(descriptors, current);
                    nanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
                    Require(selected == strategy && current.highpass_bytes == info.highpass_bytes && current.coefficient_count == info.coefficient_count
                        && current.maximum_band_bytes == info.maximum_band_bytes && current.dense_band_count == info.dense_band_count
                        && current.level_payload_bytes == info.level_payload_bytes, "RC 100 identical classifications");
                }
                const std::string bgra = Exact(s, d);
                auto t = Begin(d, s); DecodeFrameInfo production_info;
                Require(d.GetFrameInfo(t, &production_info) && production_info.strategy == strategy, "RC production classifier decision"); d.ReleaseOutput(t);
                json << ",\"frame_hash\":\"" << (c.generated ? s.input_hash : bgra)
                     << "\",\"frame_hash_kind\":\"" << (c.generated ? "source_YUY2" : "decoded_CPU_BGRA") << "\",\"bgra_hash\":\"" << bgra
                     << "\",\"coefficients_exact\":30,\"max_coefficient_error\":0,\"ll_exact\":true,\"planar_exact\":true,\"bgra_exact\":true,"
                     << "\"validated_strategies\":[\"lightweight\",\"hybrid\",\"auto\",\"host_streaming\"],\"deterministic\":true,\"auto_strategy\":\"" << StrategyName(strategy)
                     << "\",\"classifier_mean_ns\":" << nanos / 100.0 << ",\"production_classifier_ns\":" << production_info.classifier_nanoseconds
                     << ",\"highpass_payload_bytes\":" << info.highpass_bytes << ",\"padded_coefficient_count\":" << info.coefficient_count
                     << ",\"largest_band_payload_bytes\":" << info.maximum_band_bytes << ",\"dense_band_count\":" << info.dense_band_count
                     << ",\"payload_bytes_by_level\":[" << info.level_payload_bytes[0] << ',' << info.level_payload_bytes[1] << ',' << info.level_payload_bytes[2]
                     << "],\"concentrated_detail\":" << bool(info.highpass_bytes && info.level_payload_bytes[1] * 100 >= info.highpass_bytes * 75)
                     << ",\"distributed_detail\":" << bool(info.highpass_bytes >= 262144 && info.maximum_band_bytes >= 65536);
                if (benchmark) {
                    const int runs = size == 0 ? 120 : size == 1 ? 60 : 30;
                    Stats light, hybrid;
                    if (supported_count % 2) { light = Measure(d, s, DiagnosticDecodeStrategy::Lightweight, runs); hybrid = Measure(d, s, DiagnosticDecodeStrategy::Hybrid, runs); }
                    else { hybrid = Measure(d, s, DiagnosticDecodeStrategy::Hybrid, runs); light = Measure(d, s, DiagnosticDecodeStrategy::Lightweight, runs); }
                    const auto automatic = Measure(d, s, DiagnosticDecodeStrategy::Automatic, runs);
                    json << ",\"measured_frames_per_strategy\":" << runs << ",\"lightweight\":"; JsonStats(json, light);
                    json << ",\"hybrid\":"; JsonStats(json, hybrid); json << ",\"auto\":"; JsonStats(json, automatic);
                    json << ",\"auto_fps\":" << 1000 / automatic.mean << ",\"auto_megapixels_per_second\":" << double(w) * h / 1000 / automatic.mean;
                    std::cout << c.name << " L=" << light.mean << " H=" << hybrid.mean << " Auto=" << automatic.mean << ' ' << StrategyName(strategy) << '\n' << std::flush;
                } else std::cout << c.name << " 30/30 LL/planes/BGRA exact " << StrategyName(strategy) << '\n' << std::flush;
                const auto memory = d.MemoryStats();
                json << ",\"owned_device_bytes\":" << memory.device_bytes << ",\"owned_pinned_bytes\":" << memory.pinned_host_bytes
                     << ",\"known_pageable_capacity_bytes\":" << memory.pageable_capacity_bytes << ",\"peak_process_working_set_bytes\":" << WorkingSet(true) << '}';
                if (c.generated && (s.pattern == 0 || s.pattern == 5 || s.pattern == 6 || s.pattern == 10)) smoke_samples.push_back(s);
            }
            if (!benchmark) { const int frames = size == 0 ? 300 : size == 1 ? 150 : 20; Smoke(d, smoke_samples, frames); smoke_frames += frames; }
        }
        json << "],\"total_cases\":" << total << ",\"supported_cases\":" << supported_count << ",\"unsupported_cases\":" << unsupported_count
             << ",\"smoke_frames\":" << smoke_frames << ",\"status\":\"PASS\"}\n";
        if (!benchmark) PeakSearch(output / "decode_phase3a_rc_peak_escape.json");
        std::cout << "RC validation PASS " << supported_count << '/' << total << " supported; smoke " << smoke_frames << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
