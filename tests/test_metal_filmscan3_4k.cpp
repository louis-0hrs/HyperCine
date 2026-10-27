// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// #45 Schritt 2/3: a *complete* 4K CFHD encoder on Metal, not only the GPU
// wavelet stages. A GPU-stage PASS is explicitly not sufficient here, so this
// test compares whole CFHD samples, every coefficient hash, the quant/scale
// tables and the limiter state, and it dumps both samples for an independent
// FFmpeg decode.
#include "metal_audit.h"
#include "reference_frame_generator.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/resource.h>
using namespace hypercine::standalone;
using metal_test::EqualPacket;
using reference_fixture::GeneratePhase3AFrame;
using reference_fixture::Phase3APattern;

namespace {
constexpr int W = 3840, H = 2160;
constexpr std::size_t STRIDE = static_cast<std::size_t>(W) * 4;
// FilmScan3Geometry defaults to 1920x1080, so the 4K geometry must be explicit.
constexpr FilmScan3Geometry K4K{3840, 2160};

void Require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
template <class F> void Reject(F fn, const char* what) {
    try { fn(); }
    catch (const std::invalid_argument&) { return; }
    catch (const std::length_error&) { return; }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(std::string("accepted invalid case: ") + what);
}

// Everything the sample contract carries, compared field by field. A byte
// difference is reported at its exact offset by EqualPacket.
void CompareFrame(const FilmScan3Frame& cpu, const FilmScan3Frame& metal, int frame) {
    EqualPacket(cpu.sample, metal.sample);
    if (cpu.coefficient_hash != FilmScan3CoefficientHash::Full ||
        metal.coefficient_hash != FilmScan3CoefficientHash::Full)
        throw std::runtime_error("bit-exact gate requires full coefficient hashing");
    if (cpu.coefficient_fnv64 != metal.coefficient_fnv64)
        for (int k = 0; k < 30; ++k)
            if (cpu.coefficient_fnv64[k] != metal.coefficient_fnv64[k]) {
                std::ostringstream s;
                s << "frame " << frame << " coefficient hash " << k << " differs: cpu="
                  << std::hex << cpu.coefficient_fnv64[k] << " metal=" << metal.coefficient_fnv64[k];
                throw std::runtime_error(s.str());
            }
    if (cpu.tables.quant != metal.tables.quant) throw std::runtime_error("quant tables differ");
    if (cpu.tables.scale != metal.tables.scale) throw std::runtime_error("scale tables differ");
    if (cpu.state.fs_rate_limiter != metal.state.fs_rate_limiter) throw std::runtime_error("rate limiter differs");
    if (cpu.state.frame_index != metal.state.frame_index) throw std::runtime_error("frame index differs");
    if (cpu.sample.size() != metal.sample.size()) throw std::runtime_error("sample size differs");
}
}  // namespace

int main(int argc, char** argv) try {
    // Mandatory GPU: never skip and never fall back to CPU.
    const std::string dump = argc > 1 ? argv[1] : std::string();
    auto frontend = detail::MetalFrontend(K4K);
    std::cout << "Required Metal device: " << frontend->DeviceName() << '\n';
    Require(frontend->Geometry().width == W && frontend->Geometry().height == H,
            "Metal frontend did not carry the requested 4K geometry");

    FilmScan3Encoder cpu(FilmScan3Geometry{K4K});
    Require(cpu.geometry().width == W, "CPU encoder did not take the 4K geometry");

    // Schritt 1 deliverable: the public backend+geometry session. An explicitly
    // requested Metal backend must not silently become CPU.
    FilmScan3Encoder viaPublicApi(FilmScan3Backend::Metal, K4K);
    Require(viaPublicApi.geometry().width == W && viaPublicApi.geometry().height == H,
            "backend+geometry constructor did not carry 4K");
    FilmScan3Encoder metal;
    detail::FrontendAccess::Use(metal, frontend);

    AlignedVector<uint8_t> input(STRIDE * H);
    FilmScan3Metadata metadata;

    // ---- Schritt 2.1-2.4: one complete 4K frame, CPU vs Metal ----------------
    GeneratePhase3AFrame(W, H, 0, Phase3APattern::DeterministicNoise, input.data(),
                         static_cast<int>(STRIDE), 0x1337beef);
    const auto cpuFrame = cpu.Encode(input.data(), input.size(), STRIDE, metadata);
    const auto metalFrame = metal.Encode(input.data(), input.size(), STRIDE, metadata);
    CompareFrame(cpuFrame, metalFrame, 0);
    Require(cpuFrame.sample.size() == metalFrame.sample.size(), "4K sample sizes differ");
    std::cout << "PASS 4K frame 0: complete CFHD sample identical, " << cpuFrame.sample.size()
              << " bytes, 30/30 coefficient hashes, 36 quant + 36 scale entries, limiter "
              << cpuFrame.state.fs_rate_limiter << '\n';

    // The public constructor must produce the same packet as the injected path.
    const auto publicFrame = viaPublicApi.Encode(input.data(), input.size(), STRIDE, metadata);
    CompareFrame(cpuFrame, publicFrame, 0);
    std::cout << "PASS 4K frame 0: backend+geometry constructor equals the CPU reference\n";

    if (!dump.empty()) {
        for (const auto& item : {std::make_pair("cpu", cpuFrame.sample), std::make_pair("metal", metalFrame.sample)}) {
            std::ofstream f(dump + "/4k_" + item.first + ".cfhd", std::ios::binary);
            f.write(reinterpret_cast<const char*>(item.second.data()),
                    static_cast<std::streamsize>(item.second.size()));
            if (!f) throw std::runtime_error("sample dump write failed");
        }
    }

    // ---- Schritt 2.6: at least 16 distinct consecutive frames -----------------
    cpu.Reset();
    metal.Reset();
    std::vector<std::string> hashes;
    for (int i = 0; i < 16; ++i) {
        GeneratePhase3AFrame(W, H, i, Phase3APattern::DeterministicNoise, input.data(),
                             static_cast<int>(STRIDE), 0x1337beef);
        const auto c = cpu.Encode(input.data(), input.size(), STRIDE, metadata);
        const auto m = metal.Encode(input.data(), input.size(), STRIDE, metadata);
        CompareFrame(c, m, i);
        Require(c.coefficient_hash == FilmScan3CoefficientHash::Full,
                "16-frame gate requires full coefficient hashing");
        std::ostringstream h;
        for (auto v : c.coefficient_fnv64) h << std::hex << std::setw(16) << std::setfill('0') << v;
        hashes.push_back(h.str());
    }
    for (std::size_t i = 1; i < hashes.size(); ++i)
        Require(hashes[i] != hashes[i - 1], "consecutive 4K frames produced identical coefficient hashes");
    std::cout << "PASS 16 consecutive 4K frames: ordered hashes distinct and CPU==Metal on every frame\n";

    // Dump the whole ordered sequence so it can be decoded independently. These
    // are 16 PACKET comparisons above; an FFmpeg decode of all 16 is a separate,
    // stronger statement and must not be conflated with them.
    if (!dump.empty()) {
        cpu.Reset();
        metal.Reset();
        for (int i = 0; i < 16; ++i) {
            GeneratePhase3AFrame(W, H, i, Phase3APattern::DeterministicNoise, input.data(),
                                 static_cast<int>(STRIDE), 0x1337beef);
            const auto c = cpu.Encode(input.data(), input.size(), STRIDE, metadata);
            const auto m = metal.Encode(input.data(), input.size(), STRIDE, metadata);
            char name[64];
            std::snprintf(name, sizeof(name), "/seq_cpu_%04d.cfhd", i);
            std::ofstream fc(dump + name, std::ios::binary);
            fc.write(reinterpret_cast<const char*>(c.sample.data()),
                     static_cast<std::streamsize>(c.sample.size()));
            if (!fc) throw std::runtime_error("sequence dump write failed");
            std::snprintf(name, sizeof(name), "/seq_metal_%04d.cfhd", i);
            std::ofstream fm(dump + name, std::ios::binary);
            fm.write(reinterpret_cast<const char*>(m.sample.data()),
                     static_cast<std::streamsize>(m.sample.size()));
            if (!fm) throw std::runtime_error("sequence dump write failed");
        }
        std::cout << "DUMPED 16 ordered CPU and 16 ordered Metal packets for independent decode\n";
    }

    // ---- Schritt 3: stability and limits -------------------------------------
    // Padded input stride: the encoder must honour the padding, not assume width*4.
    {
        const std::size_t padded = STRIDE + 64;
        AlignedVector<uint8_t> roomy(padded * H, 0xa5);
        GeneratePhase3AFrame(W, H, 3, Phase3APattern::DeterministicNoise, roomy.data(),
                             static_cast<int>(padded), 0x1337beef);
        FilmScan3Encoder a(FilmScan3Geometry{K4K});
        FilmScan3Encoder b;
        detail::FrontendAccess::Use(b, detail::MetalFrontend(K4K));
        const auto ca = a.Encode(roomy.data(), roomy.size(), padded, metadata);
        const auto cb = b.Encode(roomy.data(), roomy.size(), padded, metadata);
        CompareFrame(ca, cb, 3);
        std::cout << "PASS 4K padded stride: CPU==Metal with stride " << padded << '\n';
    }

    // Invalid dimensions and incomplete input must be refused, on both backends.
    Reject([&] { FilmScan3Encoder e(FilmScan3Backend::Metal, FilmScan3Geometry{3840, 2161}); },
           "Metal 4K geometry with height not a multiple of 8");
    Reject([&] { FilmScan3Encoder e(FilmScan3Backend::Metal, FilmScan3Geometry{4097, 2160}); },
           "Metal geometry above kMaxDimension");
    {
        FilmScan3Encoder probe;
        detail::FrontendAccess::Use(probe, detail::MetalFrontend(K4K));
        Reject([&] { probe.Encode(input.data(), input.size() - 1, STRIDE, metadata); },
               "incomplete 4K input storage");
        Reject([&] { probe.Encode(nullptr, input.size(), STRIDE, metadata); }, "null 4K input");
        Reject([&] { probe.Encode(input.data(), input.size(), STRIDE - 8, metadata); },
               "4K stride not a multiple of 16");
    }
    std::cout << "PASS 4K invalid dimensions and incomplete input are refused\n";

    // GPU failure: the encoder state must survive a failed frame, and a Reset
    // followed by a repeated encode must reproduce the reference exactly.
    {
        FilmScan3Encoder fragile;
        auto gpu = detail::MetalFrontend(K4K);
        detail::FrontendAccess::Use(fragile, gpu);
        GeneratePhase3AFrame(W, H, 0, Phase3APattern::DeterministicNoise, input.data(),
                             static_cast<int>(STRIDE), 0x1337beef);
        const auto good = fragile.Encode(input.data(), input.size(), STRIDE, metadata);
        const auto before = fragile.state();
        detail::FailNextMetalCompletion(*gpu);
        Reject([&] { fragile.Encode(input.data(), input.size(), STRIDE, metadata); },
               "injected GPU completion failure");
        const auto after = fragile.state();
        Require(before.frame_index == after.frame_index &&
                    before.previous_sample_bytes == after.previous_sample_bytes &&
                    before.fs_rate_limiter == after.fs_rate_limiter,
                "failed frame advanced encoder state");
        fragile.Reset();
        FilmScan3Encoder ref(FilmScan3Geometry{K4K});
        CompareFrame(ref.Encode(input.data(), input.size(), STRIDE, metadata),
                     fragile.Encode(input.data(), input.size(), STRIDE, metadata), 0);
        std::cout << "PASS 4K GPU failure: state preserved, Reset+repeat reproduces the reference\n";
        (void)good;
    }

    // Resource measurement and reuse of the existing GPU buffers.
    {
        FilmScan3Encoder tuned;
        auto gpu = detail::MetalFrontend(K4K);
        detail::FrontendAccess::Use(tuned, gpu);
        GeneratePhase3AFrame(W, H, 0, Phase3APattern::DeterministicNoise, input.data(),
                             static_cast<int>(STRIDE), 0x1337beef);
        tuned.Encode(input.data(), input.size(), STRIDE, metadata);  // warm: allocates
        const auto warm = detail::ReadMetalTiming(*gpu);
        for (int i = 1; i <= 4; ++i) {
            GeneratePhase3AFrame(W, H, i, Phase3APattern::DeterministicNoise, input.data(),
                                 static_cast<int>(STRIDE), 0x1337beef);
            tuned.Encode(input.data(), input.size(), STRIDE, metadata);
        }
        const auto after = detail::ReadMetalTiming(*gpu);
        const auto allocs = after.buffer_allocations - warm.buffer_allocations;
        Require(allocs == 0, "4K steady state allocated GPU buffers");
        Require(after.peak_buffer_bytes > 0, "no peak buffer measurement");
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << "PASS 4K steady state: 0 buffer allocations over 4 frames, peak_shared_buffer_bytes="
                  << after.peak_buffer_bytes << ", peak_device_allocated_bytes="
                  << after.peak_device_allocated_bytes << ", process_peak_rss_bytes="
                  << usage.ru_maxrss << '\n';
    }

    std::cout << "PASS 4K complete Metal encoder: sample, hashes, tables, state, limits, reuse\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
