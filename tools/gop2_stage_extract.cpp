// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Reuse the exact Phase-A SDK setup, aligned buffers, RAII and typed accessor.
// Its encoder/export entry point is not called. Input is ONLY an archived packet.
#define main Gop2PhaseAUnusedMain
#include "gop2_reference_export.cpp"
#undef main

namespace {
struct Capture {
    int width, height;
    bool failed = false;
    int alternate_path_checks = 0;
    std::array<Stage, 51> entropy{};
    std::array<int, 51> band_calls{};
    std::array<std::array<Bytes, 3>, 2> signed_planes;
    std::array<std::array<std::vector<int>, 3>, 2> row_calls;
    Capture(int w, int h) : width(w), height(h) {
        for (int f = 0; f < 2; ++f) for (int ch = 0; ch < 3; ++ch) {
            signed_planes[f][ch].resize(size_t(ch == 0 ? w : w/2) * h * 2);
            row_calls[f][ch].resize(h);
        }
    }
    void Complete() const {
        Require(!failed, "observer callback failed");
        Require(alternate_path_checks == 6, "alternate original path neutrality checks missing");
        for (size_t i = 0; i < band_calls.size(); ++i) if (band_calls[i] != 3) {
            std::cerr << "entropy_callback channel=" << i/17 << " logical=" << i%17 << " calls=" << band_calls[i] << '\n';
            Require(false, "missing/repeated unexpected entropy callback");
        }
        for (const auto& frame : row_calls) for (const auto& channel : frame)
            for (int count : channel) Require(count == 3, "signed-plane row coverage incomplete");
    }
};
// Only this standalone, one-CPU, one-decoder-at-a-time observer process uses it.
Capture* active_capture = nullptr;
struct CaptureScope {
    explicit CaptureScope(Capture& capture) { Require(!active_capture, "nested observer"); active_capture = &capture; }
    ~CaptureScope() { active_capture = nullptr; }
};
Bytes ReadStage(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    Require(bool(in), "reference read failed");
    const auto size = in.tellg(); Require(size > 0 && size < 256 * 1024 * 1024, "reference size invalid");
    Bytes out(static_cast<size_t>(size)); in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    Require(bool(in), "reference read incomplete"); return out;
}
void RequireEqual(const Bytes& actual, const Bytes& expected) { Require(actual == expected, "original reference neutrality mismatch"); }
int StageRun(const fs::path& reference, const fs::path& output, int w, int h) {
    Require(!fs::exists(output), "new stage output already exists");
    DWORD_PTR mask = 0, system = 0;
    Require(GetProcessAffinityMask(GetCurrentProcess(), &mask, &system) && mask, "CPU mask unavailable");
    Require(SetProcessAffinityMask(GetCurrentProcess(), mask & (~mask + 1)), "CPU restriction failed");
    fs::create_directories(output);
    const Bytes packet = ReadStage(reference / "original_sdk.cfhd");
    Capture baseline(w, h);
    int frame_checks = 0, resident_checks = 0;
    for (int fmt = 0; fmt < 2; ++fmt) for (int fresh = 0; fresh < 3; ++fresh) {
        Capture captured(w, h);
        Observation observed;
        if (fmt == 0) {
            CaptureScope scope(captured);
            observed = Decode(packet, w, h, CFHD_PIXEL_FORMAT_YU64, true);
        } else observed = Decode(packet, w, h, CFHD_PIXEL_FORMAT_BGRA, false);
        const std::string format = fmt == 0 ? "YU64" : "BGRA";
        for (int f = 0; f < 2; ++f) {
            RequireEqual(observed.frames[f], ReadStage(reference / (format + "_call0_frame" + std::to_string(f) + ".raw")));
            ++frame_checks;
        }
        if (fmt == 0) {
            captured.Complete();
            for (const auto& s : observed.stages) {
                RequireEqual(s.bytes, ReadStage(reference / "stages" / (s.id + ".i16le")));
                ++resident_checks;
            }
            if (fresh == 0) baseline = std::move(captured);
            else {
                for (int band = 0; band < 51; ++band) RequireEqual(captured.entropy[band].bytes, baseline.entropy[band].bytes);
                for (int f = 0; f < 2; ++f) for (int ch = 0; ch < 3; ++ch)
                    RequireEqual(captured.signed_planes[f][ch], baseline.signed_planes[f][ch]);
            }
        }
        // Detect any SDK modification of input bytes, separately from output.
        RequireEqual(packet, ReadStage(reference / "original_sdk.cfhd"));
    }
    std::ofstream report(output / "stage_observations.json");
    report << "{\"schema\":\"hypercine.gop2.original_stages/1\",\"observer_version\":2,"
        << "\"width\":" << w << ",\"height\":" << h << ",\"source_quality\":\"FilmScan3\","
        << "\"sdk_revision\":\"11574d0295771edccadd17af14af74a539f924c7\","
        << "\"producer\":\"original_sdk_private_observer_overlay\",\"sample_reencoded\":false,"
        << "\"neutrality_frame_checks\":" << frame_checks << ",\"neutrality_resident_checks\":" << resident_checks
        << ",\"fresh_decoders_per_format\":3,\"same_instance_calls\":3,\"stage_observations_per_band_or_row\":9,"
        << "\"alternate_original_path_checks\":18,\"signed_path\":\"alternate_original_SDK_spatial_signed_path_with_default_output_parity\","
        << "\"cpu_affinity_count\":1,\"allowed_cpus\":1,\"entropy_pool\":0,\"worker_pool\":1,"
        << "\"stage_output_format\":\"YU64\",\"matrix\":\"BT709\",\"range\":\"video\","
        << "\"signed_plane_channel_order\":[\"Y\",\"V\",\"U\"],\"signed_plane_orientation\":\"top_down\","
        << "\"signed_plane_storage\":\"signed_int16_little_endian_before_p10_clipping_and_YU64_packing\","
        << "\"native_signed16_saturation_retained\":true,\"pre_inverse_bands\":[\n";
    for (int b = 0; b < 51; ++b) {
        const auto& s = baseline.entropy[b];
        Save(output, s.id + ".i16le", s.bytes);
        if (b) report << ",\n";
        const int logical = b % 17;
        report << "{\"id\":\"" << s.id << ".i16le\",\"channel\":" << s.channel
            << ",\"logical_band\":" << logical << ",\"wavelet\":" << s.wavelet << ",\"band\":" << s.band
            << ",\"width\":" << s.width << ",\"height\":" << s.height << ",\"pitch\":" << s.width*2
            << ",\"original_pitch\":" << s.pitch << ",\"quantization\":" << s.quantization
            << ",\"prescale\":" << s.prescale << ",\"level\":" << s.level
            << ",\"frame_association\":\"" << (logical >= 11 && logical <= 13 ? "frame1" : logical >= 14 ? "frame0" : "both")
            << "\",\"capture_point\":\"DecodeSampleSubband_after_entropy_before_inverse_submission\"}";
    }
    report << "\n],\"signed_planes\":[\n";
    for (int f = 0; f < 2; ++f) for (int ch = 0; ch < 3; ++ch) {
        const auto id = "signed_frame" + std::to_string(f) + "_ch" + std::to_string(ch) + ".i16le";
        Save(output, id, baseline.signed_planes[f][ch]);
        if (f || ch) report << ",\n";
        report << "{\"id\":\"" << id << "\",\"frame_index\":" << f << ",\"channel\":" << ch
            << ",\"width\":" << (ch == 0 ? w : w/2) << ",\"height\":" << h << ",\"pitch\":" << (ch == 0 ? w*2 : w)
            << ",\"capture_point\":\"TransformInverseSpatialToV210_before_ConvertYUVStripPlanarToV210\"}";
    }
    report << "\n],\"stability\":\"PASS\",\"instrumentation_neutrality\":\"PASS\",\"candidate_comparison\":\"PENDING\","
        << "\"release_gate\":\"BLOCKED_PENDING_STAGE_COMPARISON_AND_OWNER_REVIEW\"}\n";
    report.close(); Require(bool(report), "stage report write failed");
    std::cout << "original SDK stages " << w << 'x' << h << ": bands=51 signed_planes=6 neutrality="
        << frame_checks << "+" << resident_checks << " PASS\n";
    return 0;
}
}

extern "C" void HyperCineGop2ObserveBand(void* decoder_ptr, void* image_ptr, int ch, int logical, int wavelet, int band) {
    if (!active_capture) return;
    try {
        auto& capture = *active_capture;
        auto* decoder = static_cast<DECODER*>(decoder_ptr);
        const auto* image = static_cast<IMAGE*>(image_ptr);
        Require(decoder->entropy_worker_new.pool.thread_count == 0 && ch >= 0 && ch < 3 && logical >= 0 && logical < 17,
            "unexpected entropy callback contract");
        auto bytes = Plane(image, band);
        const int i = ch * 17 + logical;
        if (capture.band_calls[i] == 0) {
            capture.entropy[i] = {"pre_ch" + std::to_string(ch) + "_band" + std::to_string(logical),
                ch,wavelet,band,image->width,image->height,image->pitch,image->level,image->wavelet_type,
                decoder->transform[ch]->prescale[wavelet],image->quantization[band],std::move(bytes)};
        } else RequireEqual(bytes, capture.entropy[i].bytes);
        ++capture.band_calls[i];
    } catch (...) { active_capture->failed = true; }
}

extern "C" void HyperCineGop2ObserveSignedSpatial(void* decoder_ptr, int frame, void* original_output, int pitch) {
    if (!active_capture) return;
    try {
        auto& capture = *active_capture;
        auto* decoder = static_cast<DECODER*>(decoder_ptr);
        Require(pitch == capture.width * 4, "alternate output contract mismatch");
        Bytes alternative(size_t(pitch) * capture.height, 0xcd);
        FRAME_INFO info = decoder->frame;
        // This is a frozen original SDK routine, not a rewritten inverse or a
        // reconstruction from our CUDA output. It materializes signed strips.
        TransformInverseSpatialToV210(decoder->transform, frame, 3, alternative.data(), pitch,
            &info, &decoder->scratch, decoder->codec.chroma_offset, decoder->codec.precision);
        Require(std::memcmp(alternative.data(), original_output, alternative.size()) == 0,
            "alternate original signed path differs from default SDK");
        ++capture.alternate_path_checks;
    } catch (...) { active_capture->failed = true; }
}

extern "C" void HyperCineGop2ObserveRows(void* planes_ptr, int* pitches, int frame, int first_row, int width, int rows) {
    if (!active_capture) return;
    try {
        auto& capture = *active_capture;
        auto** planes = static_cast<PIXEL**>(planes_ptr);
        Require(frame >= 0 && frame < 2 && first_row >= 0 && first_row + rows <= capture.height &&
            width == capture.width && rows == 2, "unexpected signed strip contract");
        for (int ch = 0; ch < 3; ++ch) for (int y = 0; y < rows; ++y) {
            const int row = first_row + y, w = ch == 0 ? width : width/2;
            Require(pitches[ch] >= w * 2, "signed strip pitch invalid");
            auto* destination = capture.signed_planes[frame][ch].data() + size_t(row) * w * 2;
            const auto* source = reinterpret_cast<const uint8_t*>(planes[ch]) + size_t(y) * pitches[ch];
            if (capture.row_calls[frame][ch][row]) Require(std::memcmp(source, destination, size_t(w)*2) == 0, "signed strip unstable");
            else std::memcpy(destination, source, size_t(w)*2);
            ++capture.row_calls[frame][ch][row];
        }
    } catch (...) { active_capture->failed = true; }
}

int main(int argc, char** argv) {
    try {
        Require(argc == 5, "usage: gop2_stage_extract EXISTING_REFERENCE NEW_DIRECTORY WIDTH HEIGHT");
        const int w = std::stoi(argv[3]), h = std::stoi(argv[4]);
        Require((w == 1920 && h == 1080) || (w == 3840 && h == 2160), "unsupported stage geometry");
        return StageRun(argv[1], argv[2], w, h);
    } catch (const std::exception& e) {
        // Only fixed harness messages are printed, never SDK/OS path text.
        const std::string error = e.what();
        if (error == "original reference neutrality mismatch" || error == "missing/repeated unexpected entropy callback" ||
            error == "signed-plane row coverage incomplete" || error == "observer callback failed") std::cerr << error << '\n';
        std::cerr << "original stage extraction failed; preserve partial data; gate BLOCKED\n";
        return 1;
    }
}
