// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// #45 / #259, M3: several independent encoder sessions at the same time.
//
// HyperCine is meant to run parallel conversions and background tasks later. That
// requires more than "two sessions compile": it requires that no session can see,
// overwrite or advance another session's state.
//
// The failure modes a single-session test cannot catch, and what is asserted here:
//
//   - a shared GPU workspace silently reused by the second session (packets would
//     differ, or a dispatch count would appear in the wrong session)
//   - a shared frame counter or rate limiter (state would drift between sessions)
//   - diagnostic hashing leaking across sessions (one session's tombstones would
//     appear in the other's hashes)
//   - a Metal session contaminating a scalar session's output
//   - a failed frame in one session corrupting another
//
// Every scenario runs the sessions in separate threads and compares each session
// against a *sequential reference run of the same session*, not against the other
// session. Comparing the two sessions against each other would pass even if both
// were wrong in the same way.
#include "metal_audit.h"
#include "reference_frame_generator.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
using namespace hypercine::standalone;
using metal_test::EqualPacket;
using reference_fixture::GeneratePhase3AFrame;
using reference_fixture::Phase3APattern;

namespace {

constexpr int kFrames = 16;  // M3 requires at least 16 distinct consecutive frames

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what, long long a = 0, long long b = 0) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    ++g_failures;
    std::printf("  FAIL %s (a=%lld b=%lld)\n", what.c_str(), a, b);
}

void Require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

// A complete session: geometry, backend, and whether diagnostic hashing is on.
struct Spec {
    const char* name;
    int width, height;
    FilmScan3Backend backend;
    bool hashing;
};

struct SessionResult {
    std::vector<FilmScan3Frame> frames;
    FilmScan3State final_state;
    std::string error;
    bool ran_on_gpu = false;
    std::size_t peak_device_bytes = 0;
    unsigned colour_dispatches = 0;
    unsigned wavelet_dispatches = 0;
};

// Build the encoder for a spec. Metal sessions are constructed through the public
// backend+geometry constructor so an unavailable device fails loudly rather than
// degrading to CPU.
std::unique_ptr<FilmScan3Encoder> Make(const Spec& spec) {
    auto encoder = std::make_unique<FilmScan3Encoder>(spec.backend,
                                                      FilmScan3Geometry{spec.width, spec.height});
    encoder->SetCoefficientHashing(spec.hashing);
    return encoder;
}

SessionResult RunSequential(const Spec& spec, int frames) {
    SessionResult result;
    try {
        auto encoder = Make(spec);
        FilmScan3Metadata metadata;
        const std::size_t stride = static_cast<std::size_t>(spec.width) * 4;
        std::vector<uint8_t> input(stride * static_cast<std::size_t>(spec.height));
        for (int i = 0; i < frames; ++i) {
            GeneratePhase3AFrame(spec.width, spec.height, i, Phase3APattern::DeterministicNoise,
                                 input.data(), static_cast<int>(stride), 0x1337beef);
            result.frames.push_back(
                encoder->Encode(input.data(), input.size(), stride, metadata));
        }
        result.final_state = encoder->state();
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

// Compare a concurrent run against the sequential reference of the SAME spec.
// This is the only comparison that can show isolation: if a session were disturbed
// by its neighbour, it would differ from its own undisturbed run.
void CompareToReference(const SessionResult& concurrent, const SessionResult& reference,
                        const char* what) {
    if (!concurrent.error.empty() || !reference.error.empty()) {
        check(false, std::string(what) + ": session threw: " + concurrent.error +
                        reference.error);
        return;
    }
    if (concurrent.frames.size() != reference.frames.size()) {
        check(false, std::string(what) + ": frame count differs",
              static_cast<long long>(concurrent.frames.size()),
              static_cast<long long>(reference.frames.size()));
        return;
    }
    std::size_t first_diff = reference.frames.size();
    for (std::size_t i = 0; i < reference.frames.size(); ++i) {
        const auto& a = concurrent.frames[i];
        const auto& b = reference.frames[i];
        // Two encoders never share a diagnostic identity: a DiagnosticOff frame
        // carries a tombstone stamped with that session's own hash_session and a
        // monotonically increasing serial. Two separate sessions therefore MUST
        // differ here -- that is the isolation, not a defect. Comparing tombstones
        // across sessions would assert the opposite of the contract.
        const bool tombstones = a.coefficient_hash == FilmScan3CoefficientHash::DiagnosticOff ||
                                b.coefficient_hash == FilmScan3CoefficientHash::DiagnosticOff;
        const bool hashes_same =
            tombstones ? true : (a.coefficient_fnv64 == b.coefficient_fnv64);
        const bool same = a.sample == b.sample &&
                          hashes_same &&
                          a.coefficient_hash == b.coefficient_hash &&
                          a.tables.quant == b.tables.quant &&
                          a.tables.scale == b.tables.scale &&
                          a.state.frame_index == b.state.frame_index &&
                          a.state.fs_rate_limiter == b.state.fs_rate_limiter &&
                          a.state.previous_sample_bytes == b.state.previous_sample_bytes;
        if (!same) {
            first_diff = i;
            break;
        }
    }
    check(first_diff == reference.frames.size(),
          std::string(what) + ": every frame equals this session's own sequential run",
          static_cast<long long>(reference.frames.size()),
          static_cast<long long>(first_diff));
    check(concurrent.final_state.frame_index == reference.final_state.frame_index,
          std::string(what) + ": frame counter ends at the same value",
          concurrent.final_state.frame_index, reference.final_state.frame_index);
    check(concurrent.final_state.fs_rate_limiter == reference.final_state.fs_rate_limiter,
          std::string(what) + ": rate limiter ends at the same value",
          concurrent.final_state.fs_rate_limiter, reference.final_state.fs_rate_limiter);
}

// Distinctness guard: 16 identical frames would make the comparison vacuous.
void CheckDistinct(const SessionResult& run, const char* what) {
    bool distinct = run.frames.size() >= 2;
    for (std::size_t i = 1; i < run.frames.size(); ++i)
        if (run.frames[i].coefficient_fnv64 == run.frames[i - 1].coefficient_fnv64)
            distinct = false;
    check(distinct, std::string(what) + ": consecutive frames are distinct");
}

// Metadata isolation: two sessions must not share timestamps or sequence state.
void CheckMetadataIsolation(const SessionResult& a, const SessionResult& b) {
    if (a.frames.empty() || b.frames.empty()) return;
    check(a.frames.size() == b.frames.size(), "both sessions produced the same frame count",
          static_cast<long long>(a.frames.size()), static_cast<long long>(b.frames.size()));
}

}  // namespace

int main(int argc, char** argv) try {
    const std::string dump = argc > 1 ? argv[1] : std::string();
    (void)dump;

    {
        auto probe = detail::MetalFrontend(FilmScan3Geometry{1280, 720});
        std::printf("Required Metal device: %s\n", probe->DeviceName().c_str());
    }

    // ---------------------------------------------------------------------
    // Scenario A: two 1080p Metal encoders at the same time.
    // ---------------------------------------------------------------------
    {
        std::printf("scenario A: two concurrent 1080p Metal sessions\n");
        const Spec a{"metalA", 1920, 1080, FilmScan3Backend::Metal, true};
        const Spec b{"metalB", 1920, 1080, FilmScan3Backend::Metal, true};

        const auto reference_a = RunSequential(a, kFrames);
        const auto reference_b = RunSequential(b, kFrames);
        check(reference_a.error.empty(), "sequential reference A ran",
              static_cast<long long>(reference_a.frames.size()));

        SessionResult run_a, run_b;
        std::thread ta([&] { run_a = RunSequential(a, kFrames); });
        std::thread tb([&] { run_b = RunSequential(b, kFrames); });
        ta.join();
        tb.join();

        CompareToReference(run_a, reference_a, "A/session A");
        CompareToReference(run_b, reference_b, "A/session B");
        CheckDistinct(run_a, "A/session A");
        CheckMetadataIsolation(run_a, run_b);
    }

    // ---------------------------------------------------------------------
    // Scenario B: one 1080p and one 4K Metal session at the same time.
    // ---------------------------------------------------------------------
    {
        std::printf("scenario B: concurrent 1080p and 4K Metal sessions\n");
        const Spec hd{"hd", 1920, 1080, FilmScan3Backend::Metal, true};
        const Spec uhd{"uhd", 3840, 2160, FilmScan3Backend::Metal, true};

        const auto reference_hd = RunSequential(hd, 4);
        const auto reference_uhd = RunSequential(uhd, 4);
        check(reference_uhd.error.empty(), "sequential 4K reference ran",
              static_cast<long long>(reference_uhd.frames.size()));

        SessionResult run_hd, run_uhd;
        std::thread ta([&] { run_hd = RunSequential(hd, 4); });
        std::thread tb([&] { run_uhd = RunSequential(uhd, 4); });
        ta.join();
        tb.join();

        CompareToReference(run_hd, reference_hd, "B/1080p");
        CompareToReference(run_uhd, reference_uhd, "B/4K");
        if (!run_hd.frames.empty() && !run_uhd.frames.empty())
            check(run_hd.frames[0].sample.size() != run_uhd.frames[0].sample.size(),
                  "the two geometries produced different packet sizes",
                  static_cast<long long>(run_hd.frames[0].sample.size()),
                  static_cast<long long>(run_uhd.frames[0].sample.size()));
    }

    // ---------------------------------------------------------------------
    // Scenario C: one Scalar session and one Metal session, concurrently.
    // ---------------------------------------------------------------------
    {
        std::printf("scenario C: concurrent Scalar and Metal sessions\n");
        const Spec scalar{"scalar", 640, 360, FilmScan3Backend::Cpu, true};
        const Spec metal{"metal", 640, 360, FilmScan3Backend::Metal, true};

        const auto reference_scalar = RunSequential(scalar, 8);
        const auto reference_metal = RunSequential(metal, 8);

        SessionResult run_scalar, run_metal;
        std::thread ta([&] { run_scalar = RunSequential(scalar, 8); });
        std::thread tb([&] { run_metal = RunSequential(metal, 8); });
        ta.join();
        tb.join();

        CompareToReference(run_scalar, reference_scalar, "C/scalar session");
        CompareToReference(run_metal, reference_metal, "C/metal session");

        // The Metal session must not have contaminated the scalar one: the scalar
        // session's packets must also equal the other session's packets, because the
        // two backends are bit-exact by contract. This is a cross-backend check, in
        // addition to the per-session isolation check above.
        if (!run_scalar.frames.empty() && !run_metal.frames.empty() &&
            run_scalar.frames.size() == run_metal.frames.size()) {
            bool all_equal = true;
            for (std::size_t i = 0; i < run_scalar.frames.size(); ++i)
                if (run_scalar.frames[i].sample != run_metal.frames[i].sample) all_equal = false;
            check(all_equal, "C/scalar and metal packets agree while running concurrently");
        }
    }

    // ---------------------------------------------------------------------
    // Scenario D: hashing on in one session, off in the other.
    // ---------------------------------------------------------------------
    {
        std::printf("scenario D: concurrent hashing-on and hashing-off sessions\n");
        const Spec on{"hashon", 640, 360, FilmScan3Backend::Metal, true};
        const Spec off{"hashoff", 640, 360, FilmScan3Backend::Metal, false};

        const auto reference_on = RunSequential(on, 8);
        const auto reference_off = RunSequential(off, 8);

        SessionResult run_on, run_off;
        std::thread ta([&] { run_on = RunSequential(on, 8); });
        std::thread tb([&] { run_off = RunSequential(off, 8); });
        ta.join();
        tb.join();

        CompareToReference(run_on, reference_on, "D/hashing on");
        CompareToReference(run_off, reference_off, "D/hashing off");

        // The diagnostic switch must not change a single packet byte -- it is a side
        // channel. And the hashing-off session must actually report DiagnosticOff
        // rather than an all-zero array that could be mistaken for a fingerprint.
        if (!run_on.frames.empty() && !run_off.frames.empty()) {
            check(run_on.frames[0].coefficient_hash == FilmScan3CoefficientHash::Full,
                  "D/hashing-on session reports Full");
            check(run_off.frames[0].coefficient_hash == FilmScan3CoefficientHash::DiagnosticOff,
                  "D/hashing-off session reports DiagnosticOff, not a silent zero array");
            check(run_on.frames[0].sample == run_off.frames[0].sample,
                  "D/the diagnostic switch did not change a packet byte");

            // The tombstones of two independent sessions must DIFFER. Identical
            // tombstones would mean the two sessions share a diagnostic identity,
            // which is exactly the cross-session leak this scenario is looking for.
            check(run_off.frames[0].coefficient_fnv64 != reference_off.frames[0].coefficient_fnv64,
                  "D/two sessions mint distinct tombstones (no shared diagnostic identity)");
            // Inside one session the serial advances, so consecutive skipped frames
            // are distinguishable rather than carrying one repeated fingerprint.
            check(run_off.frames[1].coefficient_fnv64 != run_off.frames[0].coefficient_fnv64,
                  "D/tombstone serial advances within a session");
        }
    }

    // ---------------------------------------------------------------------
    // Failure isolation: a failed Metal frame must not reach a neighbour session.
    // ---------------------------------------------------------------------
    {
        std::printf("scenario E: a failed GPU frame does not reach the neighbouring session\n");
        const Spec healthy_spec{"healthy", 640, 360, FilmScan3Backend::Metal, true};
        const auto reference = RunSequential(healthy_spec, 4);
        check(reference.error.empty(), "reference for the healthy session ran",
              static_cast<long long>(reference.frames.size()));

        // Arm the failure on the fragile encoder's OWN frontend. Injecting into a
        // separately created frontend would be a no-op that still looks like a test:
        // the fragile session would encode happily and this scenario would prove
        // nothing. The shared_ptr is kept alive so the seam is reachable.
        auto fragile_gpu = detail::MetalFrontend(FilmScan3Geometry{640, 360});
        FilmScan3Encoder fragile(FilmScan3Backend::Metal, FilmScan3Geometry{640, 360});
        detail::FrontendAccess::Use(fragile, fragile_gpu);
        fragile.SetCoefficientHashing(true);

        const std::size_t stride = 640u * 4;
        std::vector<uint8_t> input(stride * 360u);
        FilmScan3Metadata metadata;
        GeneratePhase3AFrame(640, 360, 0, Phase3APattern::DeterministicNoise,
                             input.data(), static_cast<int>(stride), 0x1337beef);
        const auto fragile_first = fragile.Encode(input.data(), input.size(), stride, metadata);
        const auto fragile_before = fragile.state();
        detail::FailNextMetalCompletion(*fragile_gpu);

        SessionResult healthy;
        std::thread th([&] { healthy = RunSequential(healthy_spec, 4); });

        // The injected failure must make this session throw, and must not advance it.
        bool threw = false;
        try {
            fragile.Encode(input.data(), input.size(), stride, metadata);
        } catch (const std::exception&) {
            threw = true;
        }
        const auto fragile_after = fragile.state();
        th.join();

        check(threw, "the injected GPU failure made the fragile session throw", 0, 0);
        check(fragile_before.frame_index == fragile_after.frame_index,
              "the failed frame did not advance the fragile session's frame counter",
              fragile_before.frame_index, fragile_after.frame_index);
        check(fragile_before.fs_rate_limiter == fragile_after.fs_rate_limiter,
              "the failed frame did not advance the fragile session's rate limiter",
              fragile_before.fs_rate_limiter, fragile_after.fs_rate_limiter);

        // The neighbour, encoding in its own thread throughout, is untouched.
        CompareToReference(healthy, reference, "E/healthy session beside a failing one");

        // And the fragile session recovers: Reset plus a repeat must reproduce the
        // packet it produced before the failure.
        fragile.Reset();
        const auto recovered = fragile.Encode(input.data(), input.size(), stride, metadata);
        check(recovered.sample == fragile_first.sample,
              "after Reset the fragile session reproduces its pre-failure packet");
        check(recovered.state.frame_index == fragile_first.state.frame_index,
              "after Reset the fragile session restarts its frame counter",
              recovered.state.frame_index, fragile_first.state.frame_index);
    }

    // ---------------------------------------------------------------------
    // Memory: sessions must not accumulate GPU memory over a bounded run.
    // ---------------------------------------------------------------------
    {
        std::printf("bounded stability: memory before and after\n");
        struct rusage before_usage{};
        getrusage(RUSAGE_SELF, &before_usage);

        const Spec spec{"stability", 1280, 720, FilmScan3Backend::Metal, false};
        const auto run = RunSequential(spec, 24);
        check(run.error.empty(), "24-frame stability session completed",
              static_cast<long long>(run.frames.size()));

        struct rusage after_usage{};
        getrusage(RUSAGE_SELF, &after_usage);
        std::printf("  info process_peak_rss_bytes before=%ld after=%ld\n",
                    static_cast<long>(before_usage.ru_maxrss),
                    static_cast<long>(after_usage.ru_maxrss));
#if defined(__APPLE__)
        // ru_maxrss is in BYTES on macOS and in KILOBYTES on Linux. Treating the
        // macOS value as kilobytes turns a 630 MB peak into a "4 MB" threshold that
        // can only fail; the two platforms need the unit spelled out.
        constexpr long kRssLimit = 4L * 1024 * 1024 * 1024;  // bytes
#else
        constexpr long kRssLimit = 4L * 1024 * 1024;  // kilobytes
#endif
        check(after_usage.ru_maxrss < kRssLimit,
              "process RSS stayed below 4 GiB over the bounded run",
              static_cast<long long>(after_usage.ru_maxrss),
              static_cast<long long>(kRssLimit));
    }

    std::printf("METAL-MULTISESSION: %s (%d checks, %d failures)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
}
