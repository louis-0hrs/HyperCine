<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Mixed-profile CPU session isolation regression (#259)

Base: `8ef96c08246dfa2fd783f5e87ef183f19c65c69c`. This is a test-adequacy correction, not a reproduced production encoder defect. The prior `IndependentProfilesConcurrently` test made sequential calls and checked profile stamps/configuration, not complete ordered state.

## Contract exercised

The existing required `encoder_profile_session_matrix` now starts two actual asynchronous workers, one Low and one FilmScan3 CPU-scalar session. Each processes 32 distinct deterministic aligned BGRA frames at the already-admitted 1280x720 geometry. Separately advanced scalar-writer sequences provide per-profile references; references are generated before either concurrent worker starts.

Each successful frame must match the complete packet, all 36 quant and 36 scale entries, Full diagnostic status and all 30 coefficient hashes, frame index, rate limiter, overbitrate token and previous sample size. Frozen session configuration, effective scalar backend/profile admission, ISA and geometry are also checked. A Low-only Reset at round 16 restarts only its own timeline; a rejected short input at round 10 must not consume its next successful frame state. The FilmScan3 lane continues uninterrupted.

Reusable rendezvous align each round and successful calls after asymmetric actions. Peer failures cancel waits, retain the original exception and collect both futures; waits have finite 30-second bounds. An asynchronous launch failure also cancels an already-launched peer. These measures do not preempt a hung codec call; the existing 300-second CTest timeout remains the outer limit.

## Nonvacuity and focused observations

The fixed stimuli alternate sustained noise and low-detail frames with profile-specific seeds/schedules. Both input sequences are checked for repeated frames. Smaller admitted 128x24 and 256x256 stimuli did not exercise Low feedback, so this test uses 1280x720 without expanding admission.

Focused Linux x86_64 Release result: **1/1 PASS, zero failures/skips, 5.76 seconds**. Observed over 32 frames per profile:

| Profile | Limiter changes | Overbitrate changes | Nonzero token frames | Token rises/falls | Quant-table changes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Low |2|30|30|16/14|0|
| FilmScan3 |30|0|0|0/0|25|

Limiter counts compare each returned frame with its pre-Encode state; Low includes initialization after construction and Reset. Quant changes exclude the reset boundary. Low's lower-profile midpoint is Low itself, so a changing token need not change its table. The test explicitly requires token rises/falls and FilmScan3 limiter/table changes; frame count alone is not hysteresis coverage.

## Effective negative controls

All fault sources/binaries remain outside Git. Production source was not modified. An exact copied-test baseline and a test-local real-writer adapter baseline pass. The adapter then injects another actual worker's mutex-protected limiter/token/previous-size snapshot before Encode, preserving the receiving profile and frame index. Low-to-FilmScan3 feedback contamination fails the new limiter comparison at round 1; quality stamp/index remain correct while packet and tables differ. This tests real writer-state fault sensitivity, not an existing production race or a mutation of the private EncoderSession implementation.

Clearing the actual Low writer token before Encode fails the overbitrate comparison even when packet/table/hash bytes remain equal. Copied actual-session schedule controls also fail for omitted Low Reset, resetting both lanes, and an extra successful Encode emulating rejected-call state consumption. The latter is a schedule-level consumption control, not a modified invalid-input production branch. A deliberate worker exception propagates without deadlocking or being masked by peer cancellation. Returned frames are not tampered with to manufacture these state-fault failures.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_ENABLE_VULKAN=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 2
ctest --test-dir build -R '^encoder_profile_session_matrix$' --output-on-failure
python3 ci/run_standalone.py --build-dir build --system Linux --machine x86_64
```

## Scope and remaining gates

Same-implementation sequential references establish isolation/determinism, not independent codec correctness, original SDK packet/hysteresis parity, historical source-to-binary/compiler/ISA provenance or decoded-pixel qualification. No codec, backend/profile admission, public API, CMake registration, CI, frozen upstream or reference corpus changes. No native Windows/Metal/CUDA, sanitizer, performance, product or SMPTE conformance qualification is claimed. Parent #259/#46/#321 gates and FilmScan3-only production GPU admission remain unchanged.

Independent read-only review: **ACCEPT** for the bounded isolation/determinism contract. The reviewer verified the exact test source SHA-256 `6de2387fc9a1a5069c8426cb237136849c104453b9cde858ca37720ac0f0b2a1`, inspected cancellation/launch-failure safety and independently replayed both passing baselines plus all six effective failing controls. Launch failure was inspected, not forced experimentally. The selected rejected-storage-input case is not general late-Encode rollback coverage.

Linux toolchain: GCC 14.2.0, CMake 3.31.6, Python 3.13.5, FFmpeg/ffprobe 7.1.5. Fresh full official Linux strict standalone gate: **126/126 PASS, zero failures/skips**, JUnit duration407seconds; build and runner exited0. Registered and completed inventories match exactly with no missing/duplicate cases. Required external decode ON; upstream and GPU backends OFF. JUnit SHA-256: `b83a79a4987b30e4999ba1a8f20f80a5f4439eafb214309872092f2dfbd92b53`. Tested source SHA-256 matches the reviewed source above; test blob `148ad38f08af5a28389718c3eab4289873f7f26c`. Optional original scalar stage replay was NOT_RUN; private Windows binaries were not required or executed.
