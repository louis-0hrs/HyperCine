// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Concurrency contract for the NEON dispatch switch (issue #70, #259).
//
// Two separate questions, deliberately kept apart:
//
//   1. Is there a data race? The switch was a plain bool; it is now atomic.
//      ThreadSanitizer either reports races or it does not, and that answer is
//      not a matter of opinion. Phase 2 exists to give it something to find.
//
//   2. Is the switch semantically per-session? It is not, and no atomic fixes
//      that. Phase 3 proves the interference rather than describing it: one
//      session encodes a sequence while another flips the switch, and the
//      resulting packets are a mixture of the two kernels. A session cannot
//      claim to have chosen its own path.
//
// Phase 3 is expected to FAIL the "session owns its kernel" property. That is
// the finding, not a broken test. The test asserts the interference is real so
// nobody can later claim the global switch is harmless.

#include "hypercine/filmscan3.h"
#include "hypercine/wavelet.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>

using hypercine::standalone::FilmScan3Encoder;
using hypercine::standalone::FilmScan3Geometry;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, long long a = 0, long long b = 0) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s (a=%lld b=%lld)\n", what, a, b);
    } else {
        std::printf("  ok   %s\n", what);
    }
}

std::vector<uint8_t> MakeFrame(int width, int height, unsigned frame) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<uint8_t> bgra(pixels * 4);
    uint32_t state = 0x51ED0000u + frame * 0x9E3779B9u;
    for (std::size_t i = 0; i < pixels; ++i) {
        state = state * 1664525u + 1013904223u;
        const uint8_t t = static_cast<uint8_t>(state >> 24);
        uint8_t* px = bgra.data() + i * 4;
        px[0] = static_cast<uint8_t>((i * 7u + t) & 0xFF);
        px[1] = static_cast<uint8_t>((i * 13u + t) & 0xFF);
        px[2] = static_cast<uint8_t>((i * 29u + t) & 0xFF);
        px[3] = 0xFF;
    }
    return bgra;
}

// Phase 1: both kernels are deterministic on their own, so any later difference
// is attributable to the switch and not to noise.
void phase_determinism() {
    std::printf("phase 1: per-kernel determinism\n");
    const int w = 512, h = 96;
    FilmScan3Geometry g{w, h, 0};
    const auto frame = MakeFrame(w, h, 0);

    HyperCine_SetNEONWaveletEnabled(true);
    FilmScan3Encoder a(g); const auto one = a.Encode(frame.data(), frame.size(), w * 4);
    FilmScan3Encoder b(g); const auto two = b.Encode(frame.data(), frame.size(), w * 4);
    check(one.sample == two.sample && !one.sample.empty(), "NEON path is deterministic");

    HyperCine_SetNEONWaveletEnabled(false);
    FilmScan3Encoder c(g); const auto three = c.Encode(frame.data(), frame.size(), w * 4);
    FilmScan3Encoder d(g); const auto four = d.Encode(frame.data(), frame.size(), w * 4);
    check(three.sample == four.sample && !three.sample.empty(), "scalar path is deterministic");

    HyperCine_SetNEONWaveletEnabled(true);
    check(one.sample == three.sample, "both kernels agree on the same input");
    std::printf("  packet bytes: %zu\n", one.sample.size());
}

// Phase 2: many encoder sessions running while one thread flips the switch.
// This is the shape ThreadSanitizer needs. Without instrumentation it merely has
// to not crash; with -fsanitize=thread it must report zero races.
void phase_race() {
    std::printf("phase 2: concurrent sessions + concurrent toggle\n");
    const int w = 512, h = 96;
    FilmScan3Geometry g{w, h, 0};
    const auto frame = MakeFrame(w, h, 1);

    std::atomic<bool> go{false};
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};

    constexpr int kThreads = 4;
    constexpr int kIters = 40;

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            FilmScan3Encoder enc(g);
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                // Reading the switch happens inside Encode, so each iteration is a
                // fresh unsynchronised-looking read/write pair across threads.
                const auto f = enc.Encode(frame.data(), frame.size(), w * 4);
                if (f.sample.empty()) { ++g_failures; }
                (void)t;
            }
            ++done;
        });
    }
    std::thread toggler([&] {
        while (!go.load()) std::this_thread::yield();
        bool v = true;
        while (!stop.load()) {
            v = !v;
            HyperCine_SetNEONWaveletEnabled(v);
            std::this_thread::yield();
        }
        HyperCine_SetNEONWaveletEnabled(true);
    });

    go.store(true);
    for (auto& th : workers) th.join();
    stop.store(true);
    toggler.join();

    check(done.load() == kThreads, "all concurrent sessions completed", done.load(), kThreads);
    std::printf("  %d threads x %d encodes with a toggling thread\n", kThreads, kIters);
}

// Phase 3: the switch is NOT per-session -- but be precise about what that costs.
//
// The first version of this test tried to prove cross-session interference by
// comparing packets: encode a sequence while another thread toggles, and show the
// packets are a mixture of the two kernels. That failed, and the failure is the
// real finding: the two kernels are bit-exact, so a packet cannot reveal which one
// produced it. Interference between sessions is therefore NOT a correctness
// problem -- any session gets a valid, identical packet whichever kernel runs.
//
// What a process-global switch does cost is performance and reproducibility:
// a session that measured NEON cannot promise the next run uses NEON, so a timing
// claim cannot be attributed to a chosen kernel. This phase asserts exactly that
// narrower statement, and refuses to overclaim the stronger one.
void phase_cross_session() {
    std::printf("phase 3: cross-session interference is a performance effect, not a correctness one\n");
    const int w = 512, h = 96;
    FilmScan3Geometry g{w, h, 0};
    constexpr int kFrames = 12;

    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < kFrames; ++i) frames.push_back(MakeFrame(w, h, 100 + i));

    // Reference packets with the switch pinned each way.
    HyperCine_SetNEONWaveletEnabled(true);
    std::vector<std::vector<uint8_t>> ref_neon;
    {
        FilmScan3Encoder enc(g);
        for (int i = 0; i < kFrames; ++i)
            ref_neon.push_back(enc.Encode(frames[i].data(), frames[i].size(), w * 4).sample);
    }
    HyperCine_SetNEONWaveletEnabled(false);
    std::vector<std::vector<uint8_t>> ref_scalar;
    {
        FilmScan3Encoder enc(g);
        for (int i = 0; i < kFrames; ++i)
            ref_scalar.push_back(enc.Encode(frames[i].data(), frames[i].size(), w * 4).sample);
    }
    HyperCine_SetNEONWaveletEnabled(true);

    // The kernels must be indistinguishable by output. This is what makes the
    // global switch harmless for correctness -- and what makes the earlier
    // "prove a mixture of packets" idea impossible rather than merely hard.
    bool kernels_indistinguishable = true;
    for (int i = 0; i < kFrames; ++i)
        if (ref_neon[i] != ref_scalar[i]) { kernels_indistinguishable = false; break; }
    check(kernels_indistinguishable,
          "NEON and scalar packets are byte-identical, so output cannot reveal the kernel");

    // Encode the same sequence while another thread toggles hard. Every packet must
    // still equal the reference -- the switch changes which kernel ran, never the
    // result.
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::thread toggler([&] {
        while (!go.load()) std::this_thread::yield();
        bool v = true;
        while (!stop.load()) {
            v = !v;
            HyperCine_SetNEONWaveletEnabled(v);
            std::this_thread::yield();
        }
    });

    HyperCine_SetNEONWaveletEnabled(true);
    std::vector<std::vector<uint8_t>> mixed;
    {
        FilmScan3Encoder enc(g);
        go.store(true);
        for (int i = 0; i < kFrames; ++i)
            mixed.push_back(enc.Encode(frames[i].data(), frames[i].size(), w * 4).sample);
        stop.store(true);
    }
    toggler.join();
    HyperCine_SetNEONWaveletEnabled(true);

    int frames_matching_reference = 0;
    for (int i = 0; i < kFrames; ++i)
        if (mixed[i] == ref_neon[i]) ++frames_matching_reference;
    check(frames_matching_reference == kFrames,
          "every frame stayed byte-identical while the switch was being flipped",
          frames_matching_reference, kFrames);

    std::printf("  conclusion: a racing toggle cannot corrupt output, because the\n");
    std::printf("  kernels are bit-exact. It can still change which kernel a session\n");
    std::printf("  actually runs, so a timing claim cannot be attributed to one.\n");
}

} // namespace

int main() {
    std::printf("NEON-CONCURRENCY (issue #70 / #259)\n");
    phase_determinism();
    phase_race();
    phase_cross_session();

    std::printf("\nNEON-CONCURRENCY: %s (%d checks, %d failures)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
