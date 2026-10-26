// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #70 / #259: a CPU encoder session must own its wavelet ISA policy.
//
// The claim being tested is narrow and it is the one the maintainer asked for:
// a session that asked for Scalar keeps running scalar while another thread
// flips the process-global default, and a session that asked for Neon keeps
// running neon while the global default is off.
//
// How this can be tested at all: the two kernels are bit-exact, so a packet
// cannot reveal which one ran, and a timestamp cannot either once the global
// default is being flipped underneath. The dispatch decision itself is
// therefore counted, thread-locally, and the test asserts that its own session
// moved only the counter it should have moved. Thread-local is deliberate: a
// concurrent session must not be able to blur the numbers this test reads.
//
// The test is written so that reverting the session binding makes it fail. If
// Forward() goes back to resolving Auto against the global switch per call, the
// "global flipped mid-life" cases below turn red.

#include "hypercine/filmscan3.h"
#include "hypercine/standalone.h"
#include "hypercine/wavelet.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using hypercine::standalone::FilmScan3Encoder;
using hypercine::standalone::FilmScan3Geometry;
using hypercine::standalone::WaveletIsa;
using hypercine::standalone::detail::WaveletDispatchCounts;
using hypercine::standalone::detail::WaveletDispatchSnapshot;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, long long a = 0, long long b = 0) {
    ++g_checks;
    if (ok) {
        std::printf("  ok   %s\n", what);
        return;
    }
    ++g_failures;
    std::printf("  FAIL %s (a=%lld b=%lld)\n", what, a, b);
}

// 128x24 is the smallest geometry the FilmScan3 contract accepts, which keeps the
// test fast while still exercising all three pyramid levels.
constexpr int kWidth = 128;
constexpr int kHeight = 24;

std::vector<uint8_t> MakeFrame(unsigned seed) {
    const std::size_t pixels = static_cast<std::size_t>(kWidth) * kHeight;
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

struct Delta {
    std::uint64_t scalar = 0;
    std::uint64_t neon = 0;
};

Delta EncodeOnce(FilmScan3Encoder& encoder, const std::vector<uint8_t>& frame) {
    const auto before = WaveletDispatchSnapshot();
    const auto result = encoder.Encode(frame.data(), frame.size(), kWidth * 4);
    const auto after = WaveletDispatchSnapshot();
    Delta d;
    d.scalar = after.scalar - before.scalar;
    d.neon = after.neon - before.neon;
    (void)result;
    return d;
}

// Phase 1: an explicit policy is honoured, and it is the only bucket that moves.
// This half is architecture-independent apart from which backend exists.
void phase_explicit_policy() {
    std::printf("phase 1: explicit policy selects exactly one kernel\n");
    const auto frame = MakeFrame(1);

    FilmScan3Encoder scalar(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Scalar);
    const auto ds = EncodeOnce(scalar, frame);
    check(ds.scalar > 0, "scalar session ran scalar kernels", (long long)ds.scalar);
    check(ds.neon == 0, "scalar session ran no neon kernels", (long long)ds.neon);
    check(scalar.wavelet_isa() == WaveletIsa::Scalar, "scalar session reports Scalar");

#if defined(__ARM_NEON)
    FilmScan3Encoder neon(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Neon);
    const auto dn = EncodeOnce(neon, frame);
    check(dn.neon > 0, "neon session ran neon kernels", (long long)dn.neon);
    check(dn.scalar == 0, "neon session ran no scalar kernels", (long long)dn.scalar);
    check(neon.wavelet_isa() == WaveletIsa::Neon, "neon session reports Neon");
#else
    // An explicit NEON request on a target without the vector path must fail loudly
    // instead of quietly encoding on scalar and calling it NEON.
    bool threw = false;
    try {
        FilmScan3Encoder absent(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Neon);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "explicit Neon on a non-NEON build throws");
#endif
}

// Phase 2: the actual regression. Change the process-global default *after* the
// session was built, in both directions, and prove the session did not follow it.
void phase_session_outlives_global_switch() {
    std::printf("phase 2: session keeps its kernel while the global default flips\n");
    const auto frame = MakeFrame(2);
    const bool original = HyperCine_IsNEONWaveletEnabled();

    FilmScan3Encoder scalar(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Scalar);
    FilmScan3Encoder auto_session(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Auto);
    EncodeOnce(scalar, frame);        // warm up, take the baseline reading
    EncodeOnce(auto_session, frame);

    // Both directions, deliberately. A mutation that makes the session follow the
    // global switch can hide in whichever direction happens to agree with the
    // session's own choice: a first version of this test only flipped towards
    // false, which is exactly where "Scalar session ran scalar" is true either way.
    // Setting it to true is the direction that actually discriminates, because a
    // leaking session would then run NEON while claiming Scalar.
    for (const bool global : {true, false}) {
        // Changed from another thread, so the session is not merely reading a value
        // this thread happened to leave behind.
        std::thread flipper([&] { HyperCine_SetNEONWaveletEnabled(global); });
        flipper.join();

        const auto ds = EncodeOnce(scalar, frame);
        check(ds.neon == 0, global ? "scalar session ignored global=on"
                                   : "scalar session ignored global=off",
              (long long)ds.neon);
        check(ds.scalar > 0, "scalar session still ran scalar kernels", (long long)ds.scalar);
    }

    // Auto is documented to follow the global default per call, which is the
    // historical behaviour. Checking that it does proves the global flips above
    // were real and not no-ops that would make the scalar checks vacuous.
    //
    // On a build without a NEON kernel the flip is a no-op by construction -- there is
    // no vector path to switch to -- so the only correct expectation there is that the
    // global switch does not conjure a kernel that was never compiled in. Asserting
    // "Auto moved to neon" on x86 would be asserting a bug.
    for (const bool global : {true, false}) {
        HyperCine_SetNEONWaveletEnabled(global);
        const auto da = EncodeOnce(auto_session, frame);
#if defined(__ARM_NEON)
        if (global) {
            check(da.neon > 0, "auto session followed the global default to neon", (long long)da.neon);
        } else {
            check(da.scalar > 0, "auto session followed the global default to scalar", (long long)da.scalar);
        }
#else
        check(da.neon == 0, "auto session stayed scalar: no NEON kernel exists to switch to",
              (long long)da.neon);
        check(da.scalar > 0, "auto session kept encoding on scalar", (long long)da.scalar);
#endif
    }

    HyperCine_SetNEONWaveletEnabled(original);
}

// Phase 3: the session-owned policy must not change a single output byte. Both
// kernels are bit-exact, so a full CFHD sample from a Scalar session and one from
// a Neon session have to be identical -- otherwise "the choice only changes which
// code ran" would be false.
void phase_packets_identical() {
    std::printf("phase 3: policy changes the kernel, not the packet\n");
#if defined(__ARM_NEON)
    const auto frame = MakeFrame(3);
    FilmScan3Encoder scalar(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Scalar);
    FilmScan3Encoder neon(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Neon);
    const auto a = scalar.Encode(frame.data(), frame.size(), kWidth * 4);
    const auto b = neon.Encode(frame.data(), frame.size(), kWidth * 4);
    check(a.sample == b.sample, "scalar and neon sessions produced identical samples",
          (long long)a.sample.size(), (long long)b.sample.size());
    check(a.coefficient_fnv64 == b.coefficient_fnv64, "coefficient hashes identical");
    check(!a.sample.empty(), "sample is not empty", (long long)a.sample.size());
#else
    std::printf("  ok   no NEON build, packet comparison not applicable\n");
#endif
}

// Phase 4: resetting a session must not smuggle in a different kernel.
void phase_reset_keeps_policy() {
    std::printf("phase 4: Reset keeps the session policy\n");
    const auto frame = MakeFrame(4);
    FilmScan3Encoder scalar(FilmScan3Geometry{kWidth, kHeight}, WaveletIsa::Scalar);
    EncodeOnce(scalar, frame);
    scalar.Reset();
    const auto d = EncodeOnce(scalar, frame);
    check(d.neon == 0, "post-reset scalar session ran no neon kernels", (long long)d.neon);
    check(scalar.wavelet_isa() == WaveletIsa::Scalar, "policy survives Reset");
}

} // namespace

int main() {
    std::printf("WAVELET-ISA-POLICY (issue #70 / #259)\n");
    phase_explicit_policy();
    phase_session_outlives_global_switch();
    phase_packets_identical();
    phase_reset_keeps_policy();
    std::printf("WAVELET-ISA-POLICY: %s (%d checks, %d failures)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
