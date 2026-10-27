// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// #45 / #259: Metal geometry independence, proven instead of asserted.
//
// The existing 4K test shows that 1920x1080 and 3840x2160 work. That is NOT the
// same claim as "the Metal encoder is geometry-independent": a hidden 1080p or
// 4K constant would pass both sizes and still fail everything in between. This
// test walks the qualified shape range, compares COMPLETE CFHD packets and all
// 30 coefficient hashes per geometry, and requires a real GPU dispatch per
// frame. A CPU fallback is excluded twice: by construction (MetalFrontend
// throws rather than degrading) and by counters (dispatches must advance).
//
// Shape legality (width %128, height %8) and the pixel budget are separate
// questions. This test only uses shapes that pass BOTH under the current
// guards; it does not raise either limit and does not touch the capability
// split that is still open in #266.
#include "metal_audit.h"
#include "reference_frame_generator.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <sys/resource.h>
using namespace hypercine::standalone;
using metal_test::EqualPacket;
using reference_fixture::GeneratePhase3AFrame;
using reference_fixture::Phase3APattern;

namespace {

struct Geometry { int width, height; const char* note; };

// Every entry passes the current structural guard AND the current pixel
// budget: width %128 == 0, height %8 == 0, width*height <= 8294400.
const Geometry kMatrix[] = {
    {128,   24, "smallest current pipeline geometry"},
    {256,  256, "small square"},
    {1280, 720, "HD"},
    {1920, 1080, "existing reference"},
    {2048, 1080, "non-canonical width"},
    {3840, 2160, "existing 4K reference"},
};

void Require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

template <class F> void Reject(F fn, const char* what) {
    try { fn(); }
    catch (const std::invalid_argument&) { return; }
    catch (const std::length_error&) { return; }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(std::string("accepted invalid case: ") + what);
}

// Everything the sample contract carries, compared field by field.
void CompareFrame(const FilmScan3Frame& cpu, const FilmScan3Frame& metal,
                  int width, int height, int frame) {
    EqualPacket(cpu.sample, metal.sample);
    Require(cpu.coefficient_hash == FilmScan3CoefficientHash::Full &&
                metal.coefficient_hash == FilmScan3CoefficientHash::Full,
            "bit-exact gate requires full coefficient hashing on both sides");
    if (cpu.coefficient_fnv64 != metal.coefficient_fnv64) {
        for (int k = 0; k < 30; ++k)
            if (cpu.coefficient_fnv64[k] != metal.coefficient_fnv64[k]) {
                std::ostringstream s;
                s << width << 'x' << height << " frame " << frame
                  << " coefficient hash " << k << " differs: cpu=" << std::hex
                  << cpu.coefficient_fnv64[k] << " metal=" << metal.coefficient_fnv64[k];
                throw std::runtime_error(s.str());
            }
    }
    Require(cpu.tables.quant == metal.tables.quant, "quant tables differ");
    Require(cpu.tables.scale == metal.tables.scale, "scale tables differ");
    Require(cpu.state.fs_rate_limiter == metal.state.fs_rate_limiter, "rate limiter differs");
    Require(cpu.state.frame_index == metal.state.frame_index, "frame index differs");
    Require(cpu.sample.size() == metal.sample.size(), "sample size differs");
}

// A pair of independent sessions on the same geometry: CPU reference and Metal.
struct Session {
    FilmScan3Encoder cpu;
    FilmScan3Encoder metal;
    std::shared_ptr<detail::Frontend> gpu;
    explicit Session(FilmScan3Geometry g)
        : cpu(g), metal(), gpu(detail::MetalFrontend(g)) {
        detail::FrontendAccess::Use(metal, gpu);
    }
};

void Dump(const std::string& dir, int w, int h, const char* which,
          const std::vector<uint8_t>& bytes) {
    char name[160];
    std::snprintf(name, sizeof(name), "/%dx%d_%s.cfhd", w, h, which);
    std::ofstream f(dir + name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("sample dump write failed");
}

// One geometry: complete packet parity, a real GPU dispatch, padded stride and
// steady-state buffer reuse.
void RunGeometry(const Geometry& geo, const std::string& dump) {
    const FilmScan3Geometry g{geo.width, geo.height};
    const std::size_t stride = static_cast<std::size_t>(geo.width) * 4;
    const std::size_t rows = static_cast<std::size_t>(geo.height);
    AlignedVector<uint8_t> input(stride * rows);
    FilmScan3Metadata metadata;

    Session session(g);
    Require(session.gpu->Geometry().width == geo.width &&
                session.gpu->Geometry().height == geo.height,
            "Metal frontend did not carry the requested geometry");
    Require(!session.gpu->DeviceName().empty(), "Metal frontend reported no device");

    const auto before = detail::ReadMetalTiming(*session.gpu);
    GeneratePhase3AFrame(geo.width, geo.height, 0, Phase3APattern::DeterministicNoise,
                         input.data(), static_cast<int>(stride), 0x1337beef);
    const auto cpuFrame = session.cpu.Encode(input.data(), input.size(), stride, metadata);
    const auto metalFrame = session.metal.Encode(input.data(), input.size(), stride, metadata);
    CompareFrame(cpuFrame, metalFrame, geo.width, geo.height, 0);
    const auto after = detail::ReadMetalTiming(*session.gpu);

    // The device name alone does not prove execution: a fallback still reports
    // the device it declined to use. Dispatch counters do.
    Require(after.wavelet_dispatches > before.wavelet_dispatches,
            "no wavelet dispatch: geometry run did not execute on the GPU");
    Require(after.color_dispatches > before.color_dispatches,
            "no colour dispatch: geometry run did not execute on the GPU");

    std::cout << "PASS " << geo.width << 'x' << geo.height << " (" << geo.note
              << "): complete CFHD sample identical, " << cpuFrame.sample.size()
              << " bytes, 30/30 hashes, colour/wavelet dispatches +"
              << (after.color_dispatches - before.color_dispatches) << "/+"
              << (after.wavelet_dispatches - before.wavelet_dispatches) << '\n';
    (void)rows;

    if (!dump.empty()) {
        Dump(dump, geo.width, geo.height, "cpu", cpuFrame.sample);
        Dump(dump, geo.width, geo.height, "metal", metalFrame.sample);
    }
}
}  // namespace

int main(int argc, char** argv) try {
    const std::string dump = argc > 1 ? argv[1] : std::string();

    // Device identity is part of the evidence, not decoration.
    {
        auto probe = detail::MetalFrontend(FilmScan3Geometry{kMatrix[0].width, kMatrix[0].height});
        std::cout << "Required Metal device: " << probe->DeviceName() << '\n';
    }

    for (const auto& geo : kMatrix) RunGeometry(geo, dump);
    std::cout << "PASS geometry matrix: " << (sizeof(kMatrix) / sizeof(kMatrix[0]))
              << " qualified shapes, complete CFHD parity and a real GPU dispatch each\n";

    // ---- variable strides ----------------------------------------------------
    // Logical width and storage width must stay separate. A padded stride has to
    // produce the same packet as the tightly packed one, otherwise the frontend
    // is silently reading width*4 instead of the stride it was given.
    {
        const Geometry& geo = kMatrix[2];  // 1280x720
        const FilmScan3Geometry g{geo.width, geo.height};
        const std::size_t packed = static_cast<std::size_t>(geo.width) * 4;
        const std::size_t padded = packed + 64;
        AlignedVector<uint8_t> roomy(padded * static_cast<std::size_t>(geo.height), 0xa5);
        GeneratePhase3AFrame(geo.width, geo.height, 7, Phase3APattern::DeterministicNoise,
                             roomy.data(), static_cast<int>(padded), 0x1337beef);
        FilmScan3Encoder cpuPacked(g), cpuPadded(g);
        FilmScan3Encoder metalPadded;
        auto gpu = detail::MetalFrontend(g);
        detail::FrontendAccess::Use(metalPadded, gpu);
        FilmScan3Metadata metadata;

        // Same logical frame, tightly packed copy of the same rows.
        AlignedVector<uint8_t> tight(packed * static_cast<std::size_t>(geo.height));
        GeneratePhase3AFrame(geo.width, geo.height, 7, Phase3APattern::DeterministicNoise,
                             tight.data(), static_cast<int>(packed), 0x1337beef);

        const auto reference = cpuPacked.Encode(tight.data(), tight.size(), packed, metadata);
        const auto paddedCpu = cpuPadded.Encode(roomy.data(), roomy.size(), padded, metadata);
        const auto paddedMetal = metalPadded.Encode(roomy.data(), roomy.size(), padded, metadata);
        CompareFrame(reference, paddedCpu, geo.width, geo.height, 7);
        CompareFrame(reference, paddedMetal, geo.width, geo.height, 7);
        std::cout << "PASS variable stride " << geo.width << 'x' << geo.height
                  << ": padded stride " << padded << " equals packed " << packed
                  << " on CPU and Metal\n";
    }

    // ---- negative input contracts -------------------------------------------
    // These must fail. A guard that never fires is not a guard.
    {
        const Geometry& geo = kMatrix[2];
        const FilmScan3Geometry g{geo.width, geo.height};
        const std::size_t stride = static_cast<std::size_t>(geo.width) * 4;
        AlignedVector<uint8_t> input(stride * static_cast<std::size_t>(geo.height));
        GeneratePhase3AFrame(geo.width, geo.height, 0, Phase3APattern::DeterministicNoise,
                             input.data(), static_cast<int>(stride), 0x1337beef);
        FilmScan3Encoder probe;
        detail::FrontendAccess::Use(probe, detail::MetalFrontend(g));
        FilmScan3Metadata metadata;

        Reject([&] { probe.Encode(input.data(), input.size() - 1, stride, metadata); },
               "buffer one byte short of stride*height");
        Reject([&] { probe.Encode(nullptr, input.size(), stride, metadata); }, "null input");
        Reject([&] { probe.Encode(input.data(), input.size(), stride - 8, metadata); },
               "stride not a multiple of 16");
        Reject([&] { probe.Encode(input.data(), input.size(), stride - 4, metadata); },
               "stride narrower than the logical width");
        std::cout << "PASS negative input contract: short buffer, null pointer "
                     "and two illegal strides are refused\n";
    }

    // Shape legality is checked separately from the pixel budget (#259). Both
    // classes are rejected today; this test does NOT change either limit.
    {
        Reject([] { FilmScan3Encoder e(FilmScan3Backend::Metal, FilmScan3Geometry{1280, 721}); },
               "height not a multiple of 8");
        Reject([] { FilmScan3Encoder e(FilmScan3Backend::Metal, FilmScan3Geometry{1300, 720}); },
               "width not a multiple of 128");
        Reject([] { FilmScan3Encoder e(FilmScan3Backend::Metal, FilmScan3Geometry{4096, 2160}); },
               "4096x2160 exceeds the current 8294400-pixel budget");
        std::cout << "PASS shape/budget rejection: modulo and pixel-budget cases stay closed\n";
    }

    // ---- steady state: no per-frame reallocation of GPU buffers -------------
    {
        const Geometry& geo = kMatrix[3];  // 1920x1080
        const FilmScan3Geometry g{geo.width, geo.height};
        const std::size_t stride = static_cast<std::size_t>(geo.width) * 4;
        AlignedVector<uint8_t> input(stride * static_cast<std::size_t>(geo.height));
        Session session(g);
        FilmScan3Metadata metadata;
        GeneratePhase3AFrame(geo.width, geo.height, 0, Phase3APattern::DeterministicNoise,
                             input.data(), static_cast<int>(stride), 0x1337beef);
        session.metal.Encode(input.data(), input.size(), stride, metadata);  // warm
        const auto warm = detail::ReadMetalTiming(*session.gpu);
        for (int i = 1; i <= 8; ++i) {
            GeneratePhase3AFrame(geo.width, geo.height, i, Phase3APattern::DeterministicNoise,
                                 input.data(), static_cast<int>(stride), 0x1337beef);
            session.metal.Encode(input.data(), input.size(), stride, metadata);
        }
        const auto after = detail::ReadMetalTiming(*session.gpu);
        Require(after.buffer_allocations == warm.buffer_allocations,
                "steady state allocated new Metal buffers");
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << "PASS 1080p steady state: 0 buffer allocations over 8 frames, "
                     "peak_shared_buffer_bytes=" << after.peak_buffer_bytes
                  << ", peak_device_allocated_bytes=" << after.peak_device_allocated_bytes
                  << ", process_peak_rss_bytes=" << usage.ru_maxrss << '\n';
    }

    // Reset must return the session to the same starting packet, on every shape
    // that this test claims. A reset that only works at 1080p is a hidden constant.
    {
        for (const auto& geo : kMatrix) {
            const FilmScan3Geometry g{geo.width, geo.height};
            const std::size_t stride = static_cast<std::size_t>(geo.width) * 4;
            AlignedVector<uint8_t> input(stride * static_cast<std::size_t>(geo.height));
            Session session(g);
            FilmScan3Metadata metadata;
            auto encode_frame = [&](int index) {
                GeneratePhase3AFrame(geo.width, geo.height, index,
                                     Phase3APattern::DeterministicNoise, input.data(),
                                     static_cast<int>(stride), 0x1337beef);
                return session.metal.Encode(input.data(), input.size(), stride, metadata);
            };
            const auto first = encode_frame(0);
            for (int i = 1; i < 4; ++i) encode_frame(i);
            session.metal.Reset();
            session.cpu.Reset();
            const auto again = encode_frame(0);
            CompareFrame(first, again, geo.width, geo.height, 0);
            Require(again.state.frame_index == first.state.frame_index,
                    "Reset did not restore the frame index");
        }
        std::cout << "PASS Reset: every matrix geometry reproduces its first packet\n";
    }

    std::cout << "PASS Metal geometry matrix: complete encoder, real GPU, variable strides, "
                 "negatives, steady state, Reset\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
