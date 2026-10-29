// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #259, implementation step 2: the existing internal CUDA FilmScan3
// frontend bound to the same capability-gated detail::EncoderSession that already
// carries CPU scalar and Metal.
//
// This is a bounded experimental internal session backend. It is not a public
// backend enum, a product selector, a CUDA product qualification, an original-SDK
// reference comparison or a conformance claim. The sole FilmScan3 writer and the
// pinned 30/1 timecode behaviour are untouched.
//
// The check is a real GPU run: the session must select the CUDA identity, dispatch
// real spatial kernels, and produce the complete packet, quantization/scale tables,
// rate state and 30 coefficient hashes that an independent explicit-scalar session
// produces from the same input. A CUDA frontend that quietly became CPU would be
// caught both by the dispatch counter and by the identity assertions.
#include "../src/core/encoder_backend_selection.h"
#include "../src/cuda/filmscan3_cuda_stats.h"
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <string>

using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

EncoderSessionConfig Config(int w, int h, bool hashes = true) {
    EncoderSessionConfig c;
    c.width = w; c.height = h; c.backend = SessionBackend::Cuda;
    // Padded storage is deliberate: the session must accept a caller stride above
    // the row and still validate the real pointer/extent on every frame.
    c.input_stride_bytes = static_cast<std::size_t>(w) * 4 + 64;
    c.input_bytes = c.input_stride_bytes * h;
    c.max_sample_bytes = 8u * static_cast<std::size_t>(w) * h + 16384u;
    c.diagnostic = hashes ? DiagnosticMode::CoefficientFnv64 : DiagnosticMode::None;
    return c;
}

void Fill(AlignedVector<std::uint8_t>& input, const EncoderSessionConfig& c, unsigned f) {
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x) {
            auto* p = input.data() + y * static_cast<std::size_t>(c.input_stride_bytes) + 4 * x;
            p[0] = static_cast<std::uint8_t>(x + f * 17);
            p[1] = static_cast<std::uint8_t>(y + f * 31);
            p[2] = static_cast<std::uint8_t>((x ^ y) + f * 7);
            p[3] = 255;
        }
}

// Compares everything the sole writer produces. The packet comparison alone would
// hide a table or rate-state difference that happens not to change bytes here.
void Equal(const FilmScan3Frame& a, const FilmScan3Frame& b, bool hashes) {
    Require(!a.sample.empty() && a.sample == b.sample, "unified CUDA session packet differs from scalar");
    Require(a.tables.quant == b.tables.quant && a.tables.scale == b.tables.scale,
            "unified CUDA session tables differ");
    Require(a.state.frame_index == b.state.frame_index &&
            a.state.fs_rate_limiter == b.state.fs_rate_limiter &&
            a.state.previous_sample_bytes == b.state.previous_sample_bytes,
            "unified CUDA session rate/frame state differs");
    if (hashes) Require(FilmScan3FullCoefficientHashesEqual(a, b), "30 coefficient hashes differ");
    else Require(a.coefficient_hash == FilmScan3CoefficientHash::DiagnosticOff &&
                 b.coefficient_hash == FilmScan3CoefficientHash::Full,
                 "opposite diagnostic session policy lost");
}

void Dispatch(std::uint64_t before, std::uint64_t after, const FilmScan3Frame& frame) {
    // Three channels, two spatial levels each; the prescale-2 middle level stays on
    // the scalar CPU path by design and must not be counted as a CUDA launch.
    Require(after > before, "unified session did not execute real CUDA work");
    Require(after - before == 6u, "unexpected CUDA spatial launch count for one frame");
    Require(!frame.sample.empty(), "empty packet after real CUDA dispatch");
}

void Dump(const std::string& root, int w, int h, const char* backend,
          const std::vector<std::uint8_t>& packet) {
    if (root.empty()) return;
    const auto name = root + "/" + std::to_string(w) + "x" + std::to_string(h) + "_" + backend + ".cfhd";
    std::ofstream out(name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
    Require(static_cast<bool>(out), "packet dump failed");
}

void Run(int w, int h, unsigned frames, bool hashes, EncoderDevice device,
         std::shared_future<void> start, const std::string& dump,
         std::promise<void>* prepared = nullptr) {
    auto c = Config(w, h, hashes);
    EncoderSession session(c, device);
    const auto& selection = session.selection();
    const auto frontend = selection.frontend; // Value snapshot, independent of later selection state.
    Require(selection.effective.effective_backend == SessionBackend::Cuda &&
            selection.fallback == ScalarFallbackReason::None &&
            selection.frontend->Backend() == FrontendBackend::Cuda,
            "CUDA-required unified session silently fell back");
    Require(session.geometry().sample_capacity_bytes == c.max_sample_bytes,
            "CUDA session lost sample capacity");
    FilmScan3Encoder scalar({w, h, static_cast<std::size_t>(c.max_sample_bytes)}, WaveletIsa::Scalar);
    AlignedVector<std::uint8_t> input(static_cast<std::size_t>(c.input_bytes), 0xa5);
    // Both caller-owned objects may change after construction; session policy may not.
    device.cuda_available = false;
    c.backend = SessionBackend::CpuScalar;
    if (prepared) prepared->set_value();
    if (start.valid()) start.wait();
    std::set<std::vector<std::uint8_t>> packets;
    std::set<std::uint64_t> inputs;
    for (unsigned f = 0; f < frames; ++f) {
        Fill(input, c, f);
        std::uint64_t input_hash = 14695981039346656037ull;
        for (auto byte : input) { input_hash ^= byte; input_hash *= 1099511628211ull; }
        inputs.insert(input_hash);
        auto before = CudaSpatialDispatchCount(*selection.frontend);
        bool refused = false;
        try { (void)session.Encode(input.data(), input.size(), c.input_stride_bytes - 16); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "bound CUDA storage bypassed");
        Require(CudaSpatialDispatchCount(*selection.frontend) == before, "invalid input reached the GPU");
        if (f == frames / 2) {
            // Failure after real GPU completion: the frame must fail, the backend
            // must not change, and the following frame must still be correct.
            ArmCudaMidframeFailureAfter(*selection.frontend, before + 1);
            bool failed = false;
            try { (void)session.Encode(input.data(), input.size(), c.input_stride_bytes); }
            catch (const std::runtime_error&) { failed = true; }
            // Disarm before anything else: a still-armed session would fail the
            // very next frame and make the recovery check meaningless.
            ArmCudaMidframeFailureAfter(*selection.frontend, 0);
            Require(failed, "injected CUDA mid-frame failure ignored");
            Require(CudaSpatialDispatchCount(*selection.frontend) > before, "injected failure did not reach the GPU");
            Require(selection.frontend->Backend() == FrontendBackend::Cuda &&
                    selection.fallback == ScalarFallbackReason::None, "failed frame switched backend");
            before = CudaSpatialDispatchCount(*selection.frontend);
        }
        const auto actual = session.Encode(input.data(), input.size(), c.input_stride_bytes);
        const auto expected = scalar.Encode(input.data(), input.size(), c.input_stride_bytes);
        Equal(actual, expected, hashes);
        Dispatch(before, CudaSpatialDispatchCount(*selection.frontend), actual);
        packets.insert(actual.sample);
        if (f == 0) { Dump(dump, w, h, "cuda", actual.sample); Dump(dump, w, h, "cpu", expected.sample); }
    }
    Require(packets.size() == frames && inputs.size() == frames,
            "sequence did not contain distinct inputs/packets");
    session.Reset(); scalar.Reset();
    Equal(session.Encode(input.data(), input.size(), c.input_stride_bytes),
          scalar.Encode(input.data(), input.size(), c.input_stride_bytes), hashes);
    Require(session.selection().frontend == frontend &&
            session.config().backend == SessionBackend::Cuda &&
            session.config().diagnostic == (hashes ? DiagnosticMode::CoefficientFnv64 : DiagnosticMode::None),
            "Reset or caller mutation changed session policy");
    std::cout << "PASS unified CUDA " << w << 'x' << h << " frames=" << frames
              << " hashes=" << hashes
              << " full-packet/state/tables/dispatch/rollback/Reset\n";
}
}  // namespace

int main(int argc, char** argv) try {
    if (argc != 1 && argc != 2) throw std::invalid_argument("optional packet dump directory only");
    const std::string dump = argc == 2 ? argv[1] : "";
    const auto device = ProbeCudaEncoderDevice();
    Require(device.cuda_available && device.cuda_hardware_observed &&
            device.cuda_total_global_memory_bytes != 0,
            "real CUDA device facts unavailable");
    std::cout << "Required CUDA device memory: " << device.cuda_total_global_memory_bytes
              << " bytes; frontend: " << CudaSessionFrontend({128, 24})->DeviceName() << '\n';
    for (auto size : {std::pair<int, int>{128, 24}, {256, 256}, {1280, 720}, {1920, 1080},
                      {2048, 1080}, {3840, 2160}})
        Run(size.first, size.second, 1, true, device, {}, dump);
    std::promise<void> start;
    const auto ready = start.get_future().share();
    std::promise<void> on_prepared, off_prepared;
    auto on_ready = on_prepared.get_future(), off_ready = off_prepared.get_future();
    const auto prepared_run = [&](int w, int h, bool hashes, std::promise<void>& prepared) {
        try { Run(w, h, 16, hashes, device, ready, "", &prepared); }
        catch (...) {
            try { prepared.set_exception(std::current_exception()); }
            catch (const std::future_error&) {}  // Already prepared: failure occurred during Encode.
            throw;
        }
    };
    auto on = std::async(std::launch::async, [&] { prepared_run(1280, 720, true, on_prepared); });
    auto off = std::async(std::launch::async, [&] { prepared_run(256, 256, false, off_prepared); });
    // Both sessions finish construction before either enters the encode loop.
    // Unblock the peer before propagating a construction failure.
    try { on_ready.get(); off_ready.get(); }
    catch (...) { start.set_value(); throw; }
    start.set_value(); on.get(); off.get();
    std::cout << "PASS six qualified geometries and two concurrent opposite-policy ordered CUDA sessions\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
}
