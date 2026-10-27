# Lowpass dependency causal A/B — #120 / Draft #138

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

## Result and scope

The bounded diagnostic intervention eliminates the observed premature inverse
starts and pixel variation for the **same immutable 128/256 BGRA/FilmScan3
packets**. Each arm uses three fresh processes with five decoder opens each.
The unmodified SDK still produces different pixels from those fixed packets.
This supports the missing producer dependency as a causal contributor for these
two cases; it does not qualify a production fix or all SDK failure modes.

Native measurement source commit:
`9abcba487d57018dcea7292e4c2ceb2c65c4f9a5`.
[Measured JSON](reference_decode_dependency_ab_20261018.json) records source and
binary hashes, verified link origins, per-call API/metadata status, timing,
RGB/pixel SHA-256, PSNR, per-channel inverse order and before/after band hashes.
The [earlier temporal investigation](TEMPORAL_LOWPASS_DEPENDENCY.md) and all its
historical reports are preserved.

Production and frozen `upstream/` files are unchanged. The original SDK cases
remain **REFERENCE_DECODER_NONDETERMINISTIC**. The experimental arm remains
**REFERENCE_EVIDENCE_INCOMPLETE** with `encoder_packets_equal: null` and
release **UNRESOLVED / blocked**, even though these observed pixels are stable.
The exact original #116 failing M4 1080p sample is still **UNAVAILABLE**.

## Intervention and synchronization

Both observed arms use the same isolated `decoder_temporal.c` call observer,
unchanged original inverse filter and matching Release SDK headers. The second
manual target additionally compiles `entropy_dependency.c`, which includes the
pristine `entropy_threading.c` under a queue-local predicate substitution.
Header guards restrict it to the three predicate sites in that translation
unit. Original `image.h`, the decoder's downstream predicate and production
SDK compilation remain unchanged. Link-map symbol origins are checked before
measurement; only origins and the map checksum are published.

The experimental predicate accepts only this diagnostic's three-channel spatial
wavelets at indices 1 and 2, using the decoder/transform/channel/image identity.
It requires the needed input lowpass bit0 plus the three highpass bands. For
index1, it also acquires the **same channel's index2 producer completion**:

1. The producer executes the original inverse function exactly once and copies
   its completed output for the observer. It publishes a per-channel completion
   atomic with `memory_order_release` after these accesses.
2. The original SDK then marks output band0 valid through
   `UpdateWaveletBandValidFlags`, under `entropy_worker_new.lock`.
3. Queue selection uses that same lock and a `memory_order_acquire` completion
   load before allowing the index1 consumer. This supplies a happens-before
   relation from the producer's output writes to the consumer, in addition to
   checking the SDK band0 publication. Index2's entropy-decoded lowpass uses the
   existing SDK lock publication.

The predicate skips a not-ready job and lets the existing queue scan continue;
it never waits inside a worker for another worker. A producer finishing its
current reconstruction returns to the existing queue loop and can select the
newly ready dependent job. There are no added sleeps, spin loops, affinity or
worker-count overrides. The already-held queue lock is a recursive Windows
CRITICAL_SECTION; the predicate also locks the two subsequent out-of-lock
predicate sites. This target is specifically Windows MSVC x64 Release, not a
portable synchronization patch. Unexpected wavelet identities invalidate the
diagnostic rather than certify unsupported input.

This is **not a global replacement of BANDS_ALL_VALID with 0x0f**. Legitimate
highpass-only states elsewhere keep their original semantics. The additional
readiness requirement applies only to the identified spatial inverse queue
inputs, and includes actual producer publication rather than flags alone.

## Inputs and observations

Canonical BGRA8 seed `0x1337BEEF`, YUV422/FilmScan3, full-resolution BGRA output,
active `TAG_PROCESS_PATH=PROCESSING_ALL_ON`, original SDK worker configuration.
The historical packet/source identities are checked before execution; no
encoder is invoked in this experiment. Each call gets an aligned private copy
of the same packet; any byte mutation fails the existing classifier.

| Geometry | Immutable packet SHA-256 | Source BGRA SHA-256 |
| --- | --- | --- |
| 128x128 | `60ed81a3f164d585ae4d3dc0c4af4154a626cef12311a61b4d2dcca80be04b14` | `eead8e3032e188be2e13336cad0908ecd30d4fd61c6dcd15ff19e46c67b2035e` |
| 256x256 | `7655842a920a1d2f35f1cf568b21e99232004ce2a71e9ea5f4cf708da586f26e` | `5cebced3caab3369b8b0007c3f36d43d1c356ecff54f49424ac35563055f8868` |

| Geometry | Arm | Calls | Distinct RGB hashes | Source PSNR range, dB |
| --- | --- | --- | --- | --- |
| 128x128 | Unmodified SDK | 15 | 6 | 8.379040–35.648303 |
| 128x128 | Observed original scheduling | 15 | 8 | 8.379040–35.648303 |
| 128x128 | Dependency enforced | 15 | **1** | **35.648303** |
| 256x256 | Unmodified SDK | 15 | 6 | 6.476041–43.814284 |
| 256x256 | Observed original scheduling | 15 | 2 | 9.223716–43.814284 |
| 256x256 | Dependency enforced | 15 | **1** | **43.814284** |

All **90/90 decoder statuses** and **90/90 active metadata Open/Init/Set
triplets** are zero; no geometry, output or immutable-sample validation failed.
All observed calls record **16 transform / 4 entropy workers**, with no observer
calls in flight after DecodeSample returns. The unmodified arm uses identical
API/output/quality rules, with no internal observer; its runtime pool count is
therefore unobserved. Quality uses the unchanged Phase2C best row-orientation
BGR-only PSNR, peak 255; the threshold remains strictly greater than 35 dB.

For each observed arm and geometry, 15 opens capture 90 inverse events and 45
specific index2-producer/index1-consumer dependencies:

| Geometry / scheduling | Entry lacks bit0 | Missing producer publication | Consumer entry before producer return | Consumer return before producer entry | Consumer lowpass SHA differs | Input changes during inverse |
| --- | --- | --- | --- | --- | --- | --- |
| 128 original | 15 | 15 | 14 | 6 | 14 | 7 |
| 128 enforced | **0** | **0** | **0** | **0** | **0** | **0** |
| 256 original | 17 | 17 | 8 | 0 | 7 | 6 |
| 256 enforced | **0** | **0** | **0** | **0** | **0** | **0** |

All 180 enforced inverse events enter with valid=started=15 and their required
producer ready (index2 consumes entropy-decoded lowpass, not an inverse producer).
For each of the 90 enforced index2→index1 dependencies, producer end precedes
consumer begin and the produced band0 SHA equals the consumer's before/after
band0 SHA. Queue deferrals were exercised: **639/639** at 128px and **547/546**
at 256px for missing-lowpass/missing-producer counters respectively; these are
predicate evaluations, not additional decodes or unique jobs.

## Bounds, timing and limitations

Each child process has a **20-second wall timeout** covering all five opens,
including API setup, decode, sidecar writes and teardown. Timeout kills/reaps
that process and its SDK threads, discards partial output, records an explicit
TIMEOUT result and leaves release blocked. All 18 children completed normally;
no timeout occurred. A synthetic sleeping child exercises the actual timeout
exception path in the measurement unit suite. This is bounded parent-side
containment, not a new production SDK timeout/error implementation or proof
that all possible queue graphs are deadlock-free.

| Geometry / arm | Median decode ms | Maximum decode ms |
| --- | --- | --- |
| 128 unmodified | 1.1891 | 1.9288 |
| 128 observed original | 1.2411 | 1.7313 |
| 128 enforced | 1.1747 | 1.6617 |
| 256 unmodified | 1.1398 | 1.4315 |
| 256 observed original | 1.2234 | 1.7864 |
| 256 enforced | 1.2751 | 2.5813 |

Enforcement versus the common observed baseline has **-5.35% / +4.23% median**
and **-4.02% / +44.50% maximum** differences for 128/256 respectively. Combined
observer plus intervention versus the unmodified SDK is **-1.21% / +11.87%
median** and **-13.85% / +80.32% maximum**. Measurements cover CFHD_DecodeSample only, including
observer work inside that call; file I/O after decode and handle teardown are
excluded. Arms run sequentially in one bounded local session; these small
sample ratios are descriptive, not a performance improvement claim or benchmark
qualification. In particular, the enforced 256px arm has a 2.5813ms maximum at
process2/open5 (next largest 1.8199ms), despite a 1.2751ms median. The precise
cause of that latency outlier is unqualified; it is retained, not trimmed or
used to claim acceptable production latency. Performance acceptance remains
open for any future separately scoped production strategy.

Copies, allocations, atomics and lock acquisition perturb scheduling. Original
arm snapshots may read concurrently changing storage and are not atomic
debugger snapshots. Overlapping ordinals alone do not locate arithmetic
instructions; return-before-entry is stronger. The enforced arm additionally
has an explicit synchronization mechanism, but its success is limited to these
fixed GOP1 spatial cases. Memory safety remains an independent **#13** gate:
no ASan/UBSan, GPU sanitizer, inverse narrow-width, GOP2/slot reuse, original M4
1080p, 4K/8K, Adobe/Resolve or second-decoder qualification is implied. The
existing #21 forward-prefix limitation is also unchanged.

## Reproduction and validation

Use the retained historical inputs matching the hashes above, not newly encoded
substitutes. This command uses the relative artifact names from this lane;
configure local input paths privately if different. The work directory must
be fresh.

```powershell
cmake --build build --config Release --target reference_decode_worker reference_decode_temporal reference_decode_dependency --parallel 4
python -B tests/reference_decode/dependency_ab.py --worker build/tests/Release/reference_decode_worker.exe --temporal-worker build/tests/Release/reference_decode_temporal.exe --dependency-worker build/tests/Release/reference_decode_dependency.exe --packet128 build/bgra-metadata-ab/128/sample.cfhd --source128 build/bgra-metadata-ab/128/source.bgra --packet256 build/priority-temporal-final/control256.cfhd --source256 build/priority-temporal-final/control256.bgra --work build/dependency-ab --output build/dependency-ab.json
ctest --test-dir build -C Release --output-on-failure
```

The measurement intentionally returns **1** after a completed experiment,
including the observed bounded effect; **2** means timeout/invalid evidence.
`OBSERVED_BOUNDED_EFFECT` is a diagnostic observation, not a quality/bitstream or
release PASS. Fault-oriented unit checks reject stable bad quality, incomplete
calls, missing publication, premature order and input mutation.

Native Windows x64, CPU-only MSVC 19.44.35229 / CMake 3.31.6 / Python 3.10.9:
full Release CTest **13/13 PASS**, zero skips; 15 classifier methods, 11
measurement/analysis methods including the bounded-child fault test. Neither
full-suite success nor the manual diagnostic overrides unresolved codec gates.

Next: independently validate this intervention and packet/source provenance,
coordinate the untouched #13 safety gate, and obtain the authenticated failing
M4 packet/config through the #116 owner. Only then scope a separately reviewed
HyperCine wrapper/own-decoder strategy that waits for required dependencies or
returns a clear error. Draft #138 remains open; no merge or release.
