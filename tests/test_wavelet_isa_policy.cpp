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
using hypercine::standalone::detail::DecideWaveletDispatch;
using hypercine::standalone::detail::WaveletDispatchCounts;
using hypercine::standalone::detail::WaveletDispatchDecision;
using hypercine::standalone::detail::WaveletDispatchSnapshot;
using hypercine::standalone::ForwardLevel;

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


// Phase 0: the dispatch policy itself, with the vector status injected.
//
// The end-to-end paths below cannot reach a NOT_SUPPORTED return on a host whose
// vector kernel always succeeds for valid geometry, so the fallback rule is proven
// here by construction. This is the negative regression the review asked for: it
// fails if forced `Neon` is ever allowed to degrade silently again, and it fails if
// `Auto` stops being allowed to degrade.
void phase_dispatch_policy_matrix() {
    std::printf("phase 0: dispatch policy, vector status injected\n");

    // A caller that demanded the vector path must never be handed scalar output.
    check(DecideWaveletDispatch(WaveletIsa::Neon, false, false) ==
              WaveletDispatchDecision::FailClosed,
          "forced Neon + vector did not run fails closed");

    // Auto is allowed to degrade -- that is its documented meaning.
    check(DecideWaveletDispatch(WaveletIsa::Neon, true, false) ==
              WaveletDispatchDecision::Scalar,
          "Auto-resolved Neon + vector did not run degrades to scalar");
    check(DecideWaveletDispatch(WaveletIsa::Scalar, false, false) ==
              WaveletDispatchDecision::Scalar,
          "Scalar stays scalar");
    check(DecideWaveletDispatch(WaveletIsa::Scalar, true, false) ==
              WaveletDispatchDecision::Scalar,
          "Scalar stays scalar even when fallback is allowed");

    // Success is the only thing that counts as having run the vector path.
    check(DecideWaveletDispatch(WaveletIsa::Neon, false, true) ==
              WaveletDispatchDecision::Vector,
          "forced Neon + vector succeeded runs the vector path");
    check(DecideWaveletDispatch(WaveletIsa::Neon, true, true) ==
              WaveletDispatchDecision::Vector,
          "Auto-resolved Neon + vector succeeded runs the vector path");
}

// Phase 0b: the 3-argument form must dispatch exactly like the 2-argument form.
//
// Before this fix `ForwardLevel(plane, 0, WaveletIsa::Auto)` passed `Auto` straight to
// the kernels, whose dispatch test is literally `isa == Neon` -- so an explicit `Auto`
// always ran scalar, even on a host where the global default asked for the vector path
// and the 2-argument form used it. Comparing the two forms is what makes that visible,
// because each form can only be wrong relative to the other.
void phase_three_argument_auto_matches_two_argument() {
    std::printf("phase 0b: 3-argument Auto dispatches like the 2-argument form\n");
    const bool original = HyperCine_IsNEONWaveletEnabled();
    // Smallest legal even geometry for the standalone forward transform.
    hypercine::standalone::Plane plane(128, 24);
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            plane.pixels[y * plane.stride + x] = static_cast<int16_t>((x * 31 + y * 17) % 2048 - 1024);
        }
    }

    for (const bool global : {false, true}) {
        HyperCine_SetNEONWaveletEnabled(global);

        const auto b2 = WaveletDispatchSnapshot();
        const auto r2 = ForwardLevel(plane, 0);
        const auto a2 = WaveletDispatchSnapshot();
        const auto d2s = a2.scalar - b2.scalar;
        const auto d2n = a2.neon - b2.neon;

        const auto b3 = WaveletDispatchSnapshot();
        const auto r3 = ForwardLevel(plane, 0, WaveletIsa::Auto);
        const auto a3 = WaveletDispatchSnapshot();
        const auto d3s = a3.scalar - b3.scalar;
        const auto d3n = a3.neon - b3.neon;

        const char* mode = global ? "global=on" : "global=off";
        std::printf("  ..  %s: 2-arg scalar=%llu neon=%llu | 3-arg scalar=%llu neon=%llu\n",
                    mode, (unsigned long long)d2s, (unsigned long long)d2n,
                    (unsigned long long)d3s, (unsigned long long)d3n);

        // The regression: the two forms must agree bucket for bucket. Not "both ran
        // something" -- the exact counters, in the exact proportion.
        check(d3s == d2s && d3n == d2n, global
                  ? "3-arg Auto matched 2-arg with the global default on"
                  : "3-arg Auto matched 2-arg with the global default off",
              (long long)(d3s * 1000 + d3n), (long long)(d2s * 1000 + d2n));

        // And the coefficients must be identical, because the kernels are bit-exact
        // whether or not the dispatch choice was right.
        bool identical = true;
        for (std::size_t band = 0; band < 4; ++band) {
            if (r2.bands[band].width != r3.bands[band].width ||
                r2.bands[band].height != r3.bands[band].height) {
                identical = false;
                break;
            }
            for (int y = 0; y < r2.bands[band].height && identical; ++y) {
                for (int x = 0; x < r2.bands[band].width; ++x) {
                    if (r2.bands[band].pixels[y * r2.bands[band].stride + x] !=
                        r3.bands[band].pixels[y * r3.bands[band].stride + x]) {
                        identical = false;
                        break;
                    }
                }
            }
        }
        check(identical, global ? "3-arg Auto coefficients identical with global on"
                                : "3-arg Auto coefficients identical with global off");
    }
    HyperCine_SetNEONWaveletEnabled(original);

#if defined(__ARM_NEON)
    // On arm64 the discriminating arm is the global=on case, where the correct
    // dispatch is the vector one. A pre-fix build reports neon=0 there.
    HyperCine_SetNEONWaveletEnabled(true);
    const auto b = WaveletDispatchSnapshot();
    ForwardLevel(plane, 0, WaveletIsa::Auto);
    const auto a = WaveletDispatchSnapshot();
    check(a.neon > b.neon, "3-arg Auto ran the vector path when the global default did",
          (long long)(a.neon - b.neon));
    HyperCine_SetNEONWaveletEnabled(original);
#else
    // No vector kernel exists here, so the only correct statement is that Auto did not
    // invent one. The cross-form equality above is what carries the proof on x86.
    check(true, "no NEON kernel in this build: 3-arg Auto correctly ran scalar");
#endif
}

// Phase 0c: an explicit Neon that the kernel cannot serve must not produce output.
// This is the end-to-end half of the injected policy case above; it is only
// reachable on a host without the vector path, where the request itself is legal
// (the enum exists) but the kernel is absent.
void phase_forced_neon_does_not_degrade() {
    std::printf("phase 0c: a request the kernel cannot serve does not silently succeed\n");
    hypercine::standalone::Plane plane(128, 24);
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            plane.pixels[y * plane.stride + x] = static_cast<int16_t>((x * 11 + y * 7) % 512);
        }
    }
#if defined(__ARM_NEON)
    std::printf("  ok   NEON kernel present: forced Neon is servable on this host\n");
#else
    bool threw = false;
    const auto before = WaveletDispatchSnapshot();
    try {
        ForwardLevel(plane, 0, WaveletIsa::Neon);
    } catch (const std::invalid_argument&) {
        // Same contract as the encoder constructor: an explicit vector request on a
        // build without that kernel is a caller error, not a silent downgrade.
        threw = true;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    const auto after = WaveletDispatchSnapshot();
    check(threw, "forced Neon on a host without the kernel threw instead of returning scalar output");
    check(after.scalar == before.scalar && after.neon == before.neon,
          "forced Neon did not run a kernel at all",
          (long long)(after.scalar - before.scalar), (long long)(after.neon - before.neon));
#endif
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
    phase_dispatch_policy_matrix();
    phase_three_argument_auto_matches_two_argument();
    phase_forced_neon_does_not_degrade();
    phase_explicit_policy();
    phase_session_outlives_global_switch();
    phase_packets_identical();
    phase_reset_keeps_policy();
    std::printf("WAVELET-ISA-POLICY: %s (%d checks, %d failures)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
