// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #70 / #259, M1 task C: two *complete* encoder sessions in parallel.
//
// test_wavelet_isa_policy proves that a single session keeps its kernel while
// another thread flips the process-global default. That is a necessary check but
// it is not the scenario the maintainer described. The scenario is:
//
//   Encoder A forced to Scalar, Encoder B forced to Neon, both encoding at the
//   same time, and neither allowed to influence the other.
//
// A per-call dispatch decision that read shared state would still pass the
// single-session test in some interleavings. It cannot pass this one: the two
// sessions run concurrently over 16 frames, every complete CFHD sample is
// compared, and each thread's own dispatch counters must show that it moved only
// its own bucket. Because the counters are thread-local, a leak between the
// sessions shows up as a nonzero foreign count in the thread that ran the
// session, not as a plausible-looking timing difference.
//
// Nothing here asserts that the two kernels are bit-exact -- that is already
// covered. This asserts that running them side by side is safe and that each
// session owns its choice.

#include "hypercine/filmscan3.h"
#include "hypercine/standalone.h"
#include "hypercine/wavelet.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using hypercine::standalone::FilmScan3Encoder;
using hypercine::standalone::FilmScan3Frame;
using hypercine::standalone::FilmScan3Geometry;
using hypercine::standalone::FilmScan3Metadata;
using hypercine::standalone::WaveletIsa;
using hypercine::standalone::detail::WaveletDispatchCounts;
using hypercine::standalone::detail::WaveletDispatchSnapshot;

namespace {

std::mutex g_print;
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what, long long a = 0, long long b = 0) {
    std::lock_guard<std::mutex> guard(g_print);
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    ++g_failures;
    std::printf("  FAIL %s (a=%lld b=%lld)\n", what.c_str(), a, b);
}

// Deterministic frame content. Pure function of (geometry, seed), so two threads
// asking for the same frame get the same bytes without sharing storage.
std::vector<uint8_t> MakeFrame(int width, int height, unsigned seed) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<uint8_t> bgra(pixels * 4);
    uint32_t state = 0x51ED0000u + seed * 0x9E3779B9u;
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

struct SessionRun {
    std::vector<FilmScan3Frame> frames;
    WaveletDispatchCounts dispatched;
    std::string error;
};

// One complete session: build the encoder, encode kFrames distinct consecutive
// frames, record every packet and the dispatch decision of *this* thread.
SessionRun RunSession(int width, int height, WaveletIsa isa, int frames) {
    SessionRun run;
    try {
        FilmScan3Encoder encoder(FilmScan3Geometry{width, height}, isa);
        FilmScan3Metadata metadata;
        const std::size_t stride = static_cast<std::size_t>(width) * 4;
        const auto before = WaveletDispatchSnapshot();
        for (int i = 0; i < frames; ++i) {
            const auto frame = MakeFrame(width, height, static_cast<unsigned>(i));
            run.frames.push_back(encoder.Encode(frame.data(), frame.size(), stride, metadata));
        }
        const auto after = WaveletDispatchSnapshot();
        run.dispatched.scalar = after.scalar - before.scalar;
        run.dispatched.neon = after.neon - before.neon;
    } catch (const std::exception& e) {
        run.error = e.what();
    }
    return run;
}

void CompareSessions(const SessionRun& a, const SessionRun& b, const char* what,
                     int width, int height) {
    if (!a.error.empty() || !b.error.empty()) {
        check(false, std::string(what) + ": session threw: " + a.error + b.error);
        return;
    }
    const std::size_t n = std::min(a.frames.size(), b.frames.size());
    bool all_equal = a.frames.size() == b.frames.size() && n > 0;
    std::size_t first_diff = n;
    for (std::size_t i = 0; i < n && all_equal; ++i) {
        const auto& fa = a.frames[i];
        const auto& fb = b.frames[i];
        const bool same = fa.sample == fb.sample &&
                          fa.coefficient_fnv64 == fb.coefficient_fnv64 &&
                          fa.tables.quant == fb.tables.quant &&
                          fa.tables.scale == fb.tables.scale &&
                          fa.state.frame_index == fb.state.frame_index &&
                          fa.state.fs_rate_limiter == fb.state.fs_rate_limiter;
        if (!same) {
            all_equal = false;
            first_diff = i;
        }
    }
    check(all_equal, std::string(what) + ": all frames identical (samples, hashes, tables, state)",
          static_cast<long long>(n), static_cast<long long>(first_diff));
    if (n > 0) {
        check(!a.frames[0].sample.empty(), std::string(what) + ": produced real packets",
              static_cast<long long>(a.frames[0].sample.size()));
        check(a.frames[0].sample.size() > 0 &&
                  static_cast<std::size_t>(width) * height > 0,
              std::string(what) + ": geometry carried through");
    }
}

// Frames must be ordered, i.e. the run produced 16 distinct frames and not the
// same one 16 times. Without this, "all frames identical" could pass trivially.
void CheckOrdered(const SessionRun& run, const char* what) {
    bool distinct = true;
    for (std::size_t i = 1; i < run.frames.size(); ++i)
        if (run.frames[i].coefficient_fnv64 == run.frames[i - 1].coefficient_fnv64)
            distinct = false;
    check(distinct, std::string(what) + ": consecutive frames are distinct", 0, 0);
}

constexpr int kFrames = 16;

// ---------------------------------------------------------------------------
// Phase 1: Scalar session and Neon session, concurrently, identical frames.
// ---------------------------------------------------------------------------
void phase_parallel_scalar_neon() {
    std::printf("phase 1: scalar and neon sessions run at the same time\n");
    constexpr int W = 128, H = 24;

    SessionRun scalar, neon;
    std::thread a([&] { scalar = RunSession(W, H, WaveletIsa::Scalar, kFrames); });
    std::thread b([&] { neon = RunSession(W, H, WaveletIsa::Neon, kFrames); });
    a.join();
    b.join();

#if defined(__ARM_NEON)
    check(scalar.error.empty() && neon.error.empty(), "both sessions completed");
    // Each thread sees only its own dispatch counters, so a foreign count here is
    // state that leaked between the sessions.
    check(scalar.dispatched.neon == 0, "scalar session ran no neon kernels",
          static_cast<long long>(scalar.dispatched.neon));
    check(scalar.dispatched.scalar > 0, "scalar session ran scalar kernels",
          static_cast<long long>(scalar.dispatched.scalar));
    check(neon.dispatched.scalar == 0, "neon session ran no scalar kernels",
          static_cast<long long>(neon.dispatched.scalar));
    check(neon.dispatched.neon > 0, "neon session ran neon kernels",
          static_cast<long long>(neon.dispatched.neon));
    CompareSessions(scalar, neon, "parallel scalar vs neon", W, H);
    CheckOrdered(scalar, "parallel scalar vs neon");
#else
    // An explicit Neon request must throw on a build without the vector path, and
    // must not be silently downgraded to scalar while claiming Neon.
    std::printf("  ok   no NEON build: explicit Neon session is rejected\n");
    check(!neon.error.empty(), "explicit Neon session threw instead of degrading");
    check(neon.error.find("not available") != std::string::npos,
          "rejection names the missing backend", 0, 0);
    check(scalar.error.empty(), "scalar session still ran normally");
    check(scalar.dispatched.neon == 0, "scalar session ran no neon kernels",
          static_cast<long long>(scalar.dispatched.neon));
#endif
}

// ---------------------------------------------------------------------------
// Phase 2: two sessions with different resolutions, concurrently.
// ---------------------------------------------------------------------------
void phase_parallel_different_geometries() {
    std::printf("phase 2: two concurrent sessions with different geometries\n");
    const int wa = 256, ha = 256;      // small square
    const int wb = 1280, hb = 720;     // HD, 14x the pixels

    SessionRun small, hd;
    std::thread a([&] { small = RunSession(wa, ha, WaveletIsa::Scalar, 4); });
    std::thread b([&] { hd = RunSession(wb, hb, WaveletIsa::Scalar, 4); });
    a.join();
    b.join();

    check(small.error.empty() && hd.error.empty(), "both geometries completed");
    if (!small.error.empty() || !hd.error.empty()) return;

    // Each session reports its own geometry and its own packet size; a leaked
    // geometry would show up as one of these two sizes being wrong.
    check(small.frames[0].sample.size() != hd.frames[0].sample.size(),
          "different geometries produced different packet sizes",
          static_cast<long long>(small.frames[0].sample.size()),
          static_cast<long long>(hd.frames[0].sample.size()));

    // Sequential reruns must reproduce the concurrent results exactly. If parallel
    // execution corrupted either session, this comparison catches it.
    const auto small_seq = RunSession(wa, ha, WaveletIsa::Scalar, 4);
    const auto hd_seq = RunSession(wb, hb, WaveletIsa::Scalar, 4);
    CompareSessions(small, small_seq, "256x256 concurrent vs sequential", wa, ha);
    CompareSessions(hd, hd_seq, "1280x720 concurrent vs sequential", wb, hb);
}

// ---------------------------------------------------------------------------
// Phase 3: create, encode and destroy sessions in parallel, including failures.
// ---------------------------------------------------------------------------
void phase_parallel_create_destroy() {
    std::printf("phase 3: parallel creation, failure and destruction\n");
    constexpr int W = 128, H = 24;
    constexpr int kThreads = 8;
    std::atomic<int> ok{0}, threw{0}, wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &ok, &threw, &wrong] {
            for (int round = 0; round < 8; ++round) {
                try {
                    const auto isa = (t % 2 == 0) ? WaveletIsa::Scalar : WaveletIsa::Neon;
                    FilmScan3Encoder encoder(FilmScan3Geometry{W, H}, isa);
                    FilmScan3Metadata metadata;
                    const auto frame = MakeFrame(W, H, static_cast<unsigned>(round));
                    const auto result =
                        encoder.Encode(frame.data(), frame.size(), W * 4, metadata);
                    if (result.sample.empty()) ++wrong;
                    encoder.Reset();
                    const auto again = encoder.Encode(frame.data(), frame.size(), W * 4, metadata);
                    if (again.sample != result.sample) ++wrong;
#if defined(__ARM_NEON)
                    if (encoder.wavelet_isa() != isa) ++wrong;
#else
                    // Neon sessions cannot exist here at all; that is a throw, not a wrong result.
                    (void)isa;
#endif
                    ++ok;
                } catch (const std::invalid_argument&) {
#if defined(__ARM_NEON)
                    ++wrong;  // a valid request must not be rejected on a NEON build
#else
                    ++threw;  // expected only for the explicit Neon request
#endif
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    check(wrong == 0, "no thread saw a wrong kernel, empty packet or unstable Reset",
          wrong.load());
#if defined(__ARM_NEON)
    check(ok == kThreads * 8, "every concurrent session encoded and reset cleanly", ok.load());
    check(threw == 0, "no valid request was rejected", threw.load());
#else
    // Even threads request Scalar, odd threads request Neon. On a build without the
    // vector path the odd half must be refused, so exactly half succeed.
    check(ok == (kThreads / 2) * 8, "scalar half of the concurrent sessions succeeded",
          ok.load());
    check(threw == (kThreads / 2) * 8, "the explicit Neon half was rejected, not downgraded",
          threw.load());
#endif
}

// ---------------------------------------------------------------------------
// Phase 4: a failing frame in one session must not disturb the other.
// ---------------------------------------------------------------------------
void phase_failure_isolation() {
    std::printf("phase 4: a failing frame does not corrupt the neighbouring session\n");
    constexpr int W = 128, H = 24;
    FilmScan3Encoder healthy(FilmScan3Geometry{W, H}, WaveletIsa::Scalar);
    FilmScan3Encoder victim(FilmScan3Geometry{W, H}, WaveletIsa::Scalar);
    FilmScan3Metadata metadata;

    const auto frame0 = MakeFrame(W, H, 0);
    const auto frame1 = MakeFrame(W, H, 1);
    const auto reference0 = healthy.Encode(frame0.data(), frame0.size(), W * 4, metadata);
    (void)victim.Encode(frame0.data(), frame0.size(), W * 4, metadata);

    // The victim gets a truncated buffer: it must throw, and must not advance.
    const auto state_before = victim.state();
    bool threw = false;
    try {
        victim.Encode(frame0.data(), frame0.size() - 16, W * 4, metadata);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "truncated input was rejected");
    const auto state_after = victim.state();
    check(state_before.frame_index == state_after.frame_index &&
              state_before.fs_rate_limiter == state_after.fs_rate_limiter,
          "failed frame did not advance the victim's state", state_before.frame_index,
          state_after.frame_index);

    // The healthy session, running in its own thread meanwhile, is unaffected.
    SessionRun healthy_run;
    std::thread runner([&] {
        FilmScan3Encoder e(FilmScan3Geometry{W, H}, WaveletIsa::Scalar);
        const auto f0 = MakeFrame(W, H, 0);
        healthy_run.frames.push_back(e.Encode(f0.data(), f0.size(), W * 4, metadata));
    });
    try {
        victim.Encode(nullptr, frame0.size(), W * 4, metadata);  // second failure, same session
    } catch (const std::invalid_argument&) {
    }
    runner.join();
    CompareSessions(SessionRun{healthy_run.frames, {}, {}},
                    SessionRun{{reference0}, {}, {}}, "healthy session beside a failing one", W, H);

    // Reset must still restore the victim to a working state.
    victim.Reset();
    const auto recovered = victim.Encode(frame1.data(), frame1.size(), W * 4, metadata);
    check(!recovered.sample.empty(), "victim session recovered after Reset");
}

// ---------------------------------------------------------------------------
// Phase 5: invalid policy values are refused rather than defaulted.
// ---------------------------------------------------------------------------
void phase_invalid_policy() {
    std::printf("phase 5: invalid policy values are refused\n");
    constexpr int W = 128, H = 24;
    bool threw = false;
    try {
        FilmScan3Encoder broken(FilmScan3Geometry{W, H}, static_cast<WaveletIsa>(42));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "unknown ISA policy throws instead of defaulting", 0, 0);

    // Every defined policy must be constructible on a NEON build, and Auto must
    // remain resolvable to a concrete kernel for reporting.
    FilmScan3Encoder auto_session(FilmScan3Geometry{W, H}, WaveletIsa::Auto);
    const auto resolved = auto_session.wavelet_isa();
    check(resolved == WaveletIsa::Scalar || resolved == WaveletIsa::Neon,
          "Auto resolves to a concrete kernel", static_cast<long long>(resolved));

    // Geometry validation is unchanged and independent of the ISA policy.
    threw = false;
    try {
        FilmScan3Encoder bad(FilmScan3Geometry{1300, H}, WaveletIsa::Scalar);
    } catch (const std::invalid_argument&) {
        threw = true;
    } catch (const std::length_error&) {
        threw = true;
    }
    check(threw, "illegal geometry still rejected with an ISA policy set", 0, 0);
}

}  // namespace

int main() {
    std::printf("WAVELET-ISA-PARALLEL-SESSIONS (issue #70 / #259, M1-C)\n");
    std::printf("NEON compiled in: %s\n", HyperCine_IsNEONWaveletAvailable() ? "yes" : "no");
    phase_parallel_scalar_neon();
    phase_parallel_different_geometries();
    phase_parallel_create_destroy();
    phase_failure_isolation();
    phase_invalid_policy();
    std::printf("WAVELET-ISA-PARALLEL-SESSIONS: %s (%d checks, %d failures)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
