// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Test-only observer of the untouched, statically linked original SDK.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_vectors.h"
#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"
#include "decoder.h"
#include "ISampleDecoder.h"
#include "SampleDecoder.h"

extern "C" void ReconstructSampleFrameToBuffer(DECODER*, int, uint8_t*, int);
namespace fs = std::filesystem;
using Bytes = HyperCineTest::AlignedVector<uint8_t>;
namespace {
constexpr unsigned seed = 0x1337BEEF;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Check(CFHD_Error status) { Require(status == CFHD_ERROR_OKAY, "SDK call failed"); }
struct EncoderDelete { void operator()(void* p) const { if (p) CFHD_CloseEncoder(p); } };
struct DecoderDelete { void operator()(void* p) const { if (p) CFHD_CloseDecoder(p); } };
struct MetadataDelete { void operator()(void* p) const { if (p) CFHD_CloseMetadata(p); } };
// Typed research accessor reused from decode_phase0_support.h; no layout guess,
// modified upstream header, instrumented library, or HyperCine decoder involved.
struct DecoderMemberTag {
    using type = DECODER* CSampleDecoder::*;
    friend type DecoderMember(DecoderMemberTag);
};
template<class Tag, typename Tag::type Member> struct ResearchMemberAccess {
    friend typename Tag::type DecoderMember(Tag) { return Member; }
};
template struct ResearchMemberAccess<DecoderMemberTag, &CSampleDecoder::m_decoder>;
DECODER* Native(void* handle) {
    return static_cast<CSampleDecoder*>(handle)->*DecoderMember(DecoderMemberTag{});
}
void Save(const fs::path& root, const std::string& name, const Bytes& bytes) {
    const auto path = root / name;
    Require(!fs::exists(path), "refusing to overwrite artifact");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.close(); Require(bool(out), "artifact write failed");
}
Bytes Source(int w, int h, int frame) {
    Bytes out(size_t(w) * h * 2);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        const size_t i = (size_t(y) * w + x) * 2;
        const auto luma = [&](int px) {
            return uint8_t(32 + 70 * px / w + 30 * y / h + frame * 45 +
                (((px / 128 + y / 128 + frame) & 1) ? 12 : 0));
        };
        out[i] = luma(x); out[i + 2] = luma(x + 1);
        out[i + 1] = uint8_t(90 + 35 * y / h + frame * 12);
        out[i + 3] = uint8_t(140 + 25 * x / w - frame * 17);
    }
    return out;
}
Bytes Encode(int w, int h, const std::array<Bytes, 2>& sources) {
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr));
    std::unique_ptr<void, EncoderDelete> encoder(raw);
    Check(CFHD_PrepareToEncode(raw, w, h, CFHD_PIXEL_FORMAT_YUY2,
        CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_YUV_2FRAME_GOP,
        CFHD_ENCODING_QUALITY_FILMSCAN3));
    for (const auto& source : sources) {
        std::srand(seed);
        Check(CFHD_EncodeSample(raw, const_cast<uint8_t*>(source.data()), w * 2));
    }
    void* data = nullptr; size_t length = 0;
    Check(CFHD_GetSampleData(raw, &data, &length));
    Require(data && length && length < 32u * 1024u * 1024u, "invalid SDK sample size");
    const auto* first = static_cast<uint8_t*>(data);
    return Bytes(first, first + length); // Untouched raw packet, metadata included.
}
Bytes Plane(const IMAGE* image, int band) {
    Require(image && band < image->num_bands && image->band[band], "stage unavailable");
    Require(image->pixel_type[band] == PIXEL_TYPE_16S, "stage is not signed int16");
    Require(image->width > 0 && image->height > 0 && image->pitch >= image->width * 2,
        "invalid stage geometry");
    Bytes out(size_t(image->width) * image->height * 2);
    for (int y = 0; y < image->height; ++y)
        std::memcpy(out.data() + size_t(y) * image->width * 2,
            reinterpret_cast<const uint8_t*>(image->band[band]) + size_t(y) * image->pitch,
            size_t(image->width) * 2);
    return out;
}
struct Stage { std::string id; int channel, wavelet, band, width, height, pitch, level, type, prescale, quantization; Bytes bytes; };
struct Observation {
    std::array<Bytes, 2> frames;
    std::vector<Stage> stages;
    int pitch = 0, entropy_pool = 0, worker_pool = 0, allowed_cpus = 0;
    int transform_type = 0, precision = 0, progressive = 0, num_frames = 0;
};
Observation Decode(const Bytes& packet, int w, int h, CFHD_PixelFormat requested, bool stages) {
    void* raw = nullptr; Check(CFHD_OpenDecoder(&raw, nullptr));
    std::unique_ptr<void, DecoderDelete> decoder(raw);
    int actual_w = 0, actual_h = 0;
    CFHD_PixelFormat actual_format = CFHD_PIXEL_FORMAT_UNKNOWN;
    Check(CFHD_PrepareToDecode(raw, 0, 0, requested, CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(packet.data()), packet.size(),
        &actual_w, &actual_h, &actual_format));
    Require(actual_w == w && actual_h == h && actual_format == requested, "output contract mismatch");
    void* metadata_raw = nullptr; Check(CFHD_OpenMetadata(&metadata_raw));
    std::unique_ptr<void, MetadataDelete> metadata(metadata_raw);
    Check(CFHD_InitSampleMetadata(metadata_raw, METADATATYPE_ORIGINAL,
        const_cast<uint8_t*>(packet.data()), uint32_t(packet.size())));
    uint32_t cpu_max = 1, matrix = 2, range = 1;
    Check(CFHD_SetActiveMetadata(raw, metadata_raw, TAG_CPU_MAX, METADATATYPE_HIDDEN, &cpu_max, sizeof(cpu_max)));
    Check(CFHD_SetActiveMetadata(raw, metadata_raw, TAG_COLORSPACE_YUV, METADATATYPE_UINT32, &matrix, sizeof(matrix)));
    Check(CFHD_SetActiveMetadata(raw, metadata_raw, TAG_COLORSPACE_RGB, METADATATYPE_UINT32, &range, sizeof(range)));
    auto* native = Native(raw);
    // Explicit bounded control, as in the existing SDK research harness. This
    // does not qualify the SDK's default or oversubscribed threading behaviour.
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    Observation result;
    int32_t pitch = 0; Check(CFHD_GetImagePitch(w, requested, &pitch));
    Require(pitch == w * 4 && pitch % 16 == 0, "invalid aligned output pitch");
    result.pitch = pitch;
    result.frames[0].assign(size_t(pitch) * h, 0xcd);
    std::srand(seed);
    Check(CFHD_DecodeSample(raw, const_cast<uint8_t*>(packet.data()), packet.size(), result.frames[0].data(), pitch));
    result.entropy_pool = native->entropy_worker_new.pool.thread_count;
    result.worker_pool = native->worker_thread.pool.thread_count;
    result.allowed_cpus = native->thread_cntrl.capabilities >> 16;
    result.transform_type = native->codec.transform_type;
    result.precision = native->codec.precision;
    result.progressive = native->codec.progressive;
    result.num_frames = native->codec.num_frames;
    Require(result.entropy_pool <= 1 && result.worker_pool <= 1 && result.allowed_cpus == 1,
        "bounded CPU control not effective");
    Require(result.precision == 10 && result.num_frames == 2 && result.progressive == 1 &&
        result.transform_type == TRANSFORM_TYPE_FIELDPLUS,
        "sample is not progressive p10 GOP2");
    if (stages) {
        // Observe only resident SDK-produced buffers after the original inverse.
        // These are NOT pre-inverse entropy coefficients or final signed planes.
        constexpr int owners[17] = {5,5,5,5,4,4,4,3,3,3,3,1,1,1,0,0,0};
        constexpr int bands[17] = {0,1,2,3,1,2,3,0,1,2,3,1,2,3,1,2,3};
        for (int ch = 0; ch < 3; ++ch) {
            const auto* transform = native->transform[ch];
            Require(transform && transform->num_wavelets == 6, "unexpected transform topology");
            auto add = [&](int wi, int band, const std::string& label) {
                const auto* image = transform->wavelet[wi];
                auto bytes = Plane(image, band);
                result.stages.push_back({"ch" + std::to_string(ch) + "_" + label,
                    ch, wi, band, image->width, image->height, image->pitch, image->level,
                    image->wavelet_type, transform->prescale[wi], image->quantization[band], std::move(bytes)});
            };
            for (int b = 0; b < 17; ++b) add(owners[b], bands[b], "resident_band" + std::to_string(b));
            add(4, 0, "wavelet4_ll");
            add(2, 0, "temporal_low"); add(2, 1, "temporal_high");
            add(0, 0, "frame0_base_ll"); add(1, 0, "frame1_base_ll");
        }
    }
    result.frames[1].assign(size_t(pitch) * h, 0xcd);
    std::srand(seed);
    ReconstructSampleFrameToBuffer(native, 1, result.frames[1].data(), pitch);
    Require(result.frames[0] != result.frames[1], "temporal asymmetry lost");
    for (int repeat = 0; repeat < 2; ++repeat) {
        Bytes repeated(size_t(pitch) * h, 0xcd);
        std::srand(seed);
        Check(CFHD_DecodeSample(raw, const_cast<uint8_t*>(packet.data()), packet.size(), repeated.data(), pitch));
        Require(repeated == result.frames[0], "UNSTABLE_REFERENCE same-instance frame0");
        std::srand(seed);
        ReconstructSampleFrameToBuffer(native, 1, repeated.data(), pitch);
        Require(repeated == result.frames[1], "UNSTABLE_REFERENCE same-instance frame1");
    }
    return result;
}
double Psnr(const Bytes& source, const Bytes& output) {
    // YUY2 source (Y0,U,Y1,V) -> YU64 (Y0,V,Y1,U), top-down,
    // native unsigned 16-bit little-endian values (10-bit code << 6).
    long double error = 0;
    const auto* words = reinterpret_cast<const uint16_t*>(output.data());
    constexpr int order[4] = {0,3,2,1};
    for (size_t i = 0; i < source.size(); ++i) {
        const double delta = double(words[(i & ~size_t(3)) + order[i % 4]]) / 256.0 - source[i];
        error += delta * delta;
    }
    return error == 0 ? 999.0 : 10 * std::log10(255.0 * 255.0 * source.size() / double(error));
}
int Run(const fs::path& root, int w, int h) {
    Require(!fs::exists(root), "refusing to overwrite export directory");
    DWORD_PTR process_mask = 0, system_mask = 0;
    Require(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask,
        "cannot read CPU affinity");
    const DWORD_PTR selected = process_mask & (~process_mask + 1);
    Require(SetProcessAffinityMask(GetCurrentProcess(), selected), "cannot bound CPU affinity");
    fs::create_directories(root);
    const std::array<Bytes, 2> sources{Source(w,h,0), Source(w,h,1)};
    Save(root, "source_frame0.yuy2", sources[0]); Save(root, "source_frame1.yuy2", sources[1]);
    const auto packet = Encode(w,h,sources); Save(root, "original_sdk.cfhd", packet);
    std::ofstream log(root / "observations.json");
    log << "{\n\"schema\":\"hypercine.gop2.observations/1\",\n\"exporter_version\":1,\n"
        << "\"source_commit\":\"" << HYPERCINE_SOURCE_COMMIT << "\",\n"
        << "\"sdk_revision\":\"11574d0295771edccadd17af14af74a539f924c7\",\n"
        << "\"compiler\":\"MSVC " << _MSC_FULL_VER << "\",\n"
        << "\"width\":" << w << ",\"height\":" << h << ",\"source_pitch\":" << w*2
        << ",\"output_pitch\":" << w*4 << ",\n\"source_format\":\"YUY2\",\"source_bits\":8,"
        << "\"encoded_bits\":10,\"quality\":\"FilmScan3\",\"encoder_flags\":2,\n"
        << "\"matrix\":\"BT709\",\"source_range\":\"video\",\"bgra_range\":\"computer_graphics\","
        << "\"orientation\":\"top_down\",\"seed\":322420463,\n"
        << "\"control\":\"TAG_CPU_MAX=1 + native limit/capabilities=1 + affinity=1\",\n"
        << "\"original_default_threading_qualified\":false,\n\"observations\":[\n";
    bool stable = true, quality = true;
    std::array<Observation, 2> first;
    for (int format_index = 0; format_index < 2; ++format_index) {
        for (int repeat = 0; repeat < 3; ++repeat) {
            const char* format = format_index == 0 ? "YU64" : "BGRA";
            auto observation = Decode(packet, w, h,
                format_index == 0 ? CFHD_PIXEL_FORMAT_YU64 : CFHD_PIXEL_FORMAT_BGRA, format_index == 0);
            if (format_index || repeat) log << ",\n";
            log << "{\"format\":\"" << format << "\",\"fresh_decoder_ordinal\":" << repeat
                << ",\"api_status\":0,\"same_instance_repeats\":3,\"entropy_pool\":" << observation.entropy_pool
                << ",\"worker_pool\":" << observation.worker_pool << ",\"allowed_cpus\":" << observation.allowed_cpus
                << ",\"precision\":" << observation.precision << ",\"transform_type\":" << observation.transform_type
                << ",\"progressive\":" << observation.progressive << ",\"num_frames\":" << observation.num_frames;
            if (format_index == 0) {
                const auto p0 = Psnr(sources[0], observation.frames[0]);
                const auto p1 = Psnr(sources[1], observation.frames[1]);
                quality = quality && p0 > 35 && p1 > 35;
                log << ",\"source_psnr_db\":[" << p0 << ',' << p1 << ']';
            }
            for (int f = 0; f < 2; ++f) Save(root,
                std::string(format) + "_call" + std::to_string(repeat) + "_frame" + std::to_string(f) + ".raw",
                observation.frames[f]);
            if (repeat == 0) {
                fs::create_directory(root / "stages");
                for (const auto& stage : observation.stages) Save(root / "stages", stage.id + ".i16le", stage.bytes);
                first[format_index] = std::move(observation);
            } else {
                for (int f = 0; f < 2; ++f) stable = stable && observation.frames[f] == first[format_index].frames[f];
                Require(observation.stages.size() == first[format_index].stages.size(), "stage count changed");
                for (size_t i = 0; i < observation.stages.size(); ++i)
                    stable = stable && observation.stages[i].bytes == first[format_index].stages[i].bytes;
            }
            log << '}';
        }
    }
    log << "\n],\n\"stable\":" << (stable ? "true" : "false")
        << ",\"source_quality_pass\":" << (quality ? "true" : "false") << ",\n\"stages\":[\n";
    for (size_t i = 0; i < first[0].stages.size(); ++i) {
        const auto& s = first[0].stages[i];
        if (i) log << ",\n";
        log << "{\"id\":\"" << s.id << "\",\"channel\":" << s.channel << ",\"wavelet\":" << s.wavelet
            << ",\"band\":" << s.band << ",\"width\":" << s.width << ",\"height\":" << s.height
            << ",\"pitch\":" << s.width*2 << ",\"original_pitch\":";
        // Original row padding is deliberately removed, never hashed as coefficients.
        log << s.pitch << ",\"level\":" << s.level << ",\"wavelet_type\":" << s.type
            << ",\"prescale\":" << s.prescale << ",\"quantization\":" << s.quantization
            << ",\"producer\":\"original_sdk_post_inverse_resident_buffer\"}";
    }
    log << "\n],\n\"release_gate\":\"BLOCKED\",\"candidate_comparison\":\"MISSING\","
        << "\"pre_inverse_entropy_oracle\":\"MISSING\",\"final_signed_planar_oracle\":\"MISSING\"\n}\n";
    log.close(); Require(bool(log), "observation write failed");
    std::cout << "exported " << w << 'x' << h << " stable=" << stable << " quality=" << quality << '\n';
    return stable && quality ? 0 : 2;
}
} // namespace
int main(int argc, char** argv) {
    try {
        Require(argc == 4, "usage: gop2_reference_export NEW_DIRECTORY WIDTH HEIGHT");
        const int w = std::stoi(argv[2]), h = std::stoi(argv[3]);
        Require((w == 1920 && h == 1080) || (w == 3840 && h == 2160), "unsupported geometry");
        return Run(argv[1], w, h);
    } catch (const std::exception&) {
        // Never print private artifact paths or arbitrary SDK/OS exception text.
        std::cerr << "reference export failed; preserve partial artifacts, gate BLOCKED\n";
        return 1;
    }
}
