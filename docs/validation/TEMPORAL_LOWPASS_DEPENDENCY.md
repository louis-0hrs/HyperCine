# Temporal lowpass dependency diagnostic — #120 / #138

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

## Result

**An insufficient completion predicate and out-of-order inverse dependency are
demonstrated in the isolated diagnostic.** This is a narrower, stronger finding
than the earlier post-decode retained-band differences. It does not establish
that every reported decoder failure, specifically the original M4 1080p case,
has this cause. Production codec and frozen upstream files remain unchanged;
release remains UNRESOLVED.

Source commit for the native measurements:
`3bd1214ba0c8e3dfae8d34563a1d2f2c2d14a1d4`.
[Measured observations](reference_decode_temporal_20261017.json) contain packet,
input, worker/exporter binary and source hashes; per-call API/metadata status,
PSNR/pixel hashes; ordered inverse events and per-band before/after SHA-256.
[Derived dependency analysis](reference_decode_dependency_20261017.json) records
its input-evidence hash and analyzer source hash, with each suspect dependency.

Native Windows x64, MSVC 19.44.35229, CMake 3.31.6, Python 3.10.9, CPU-only
original SDK baseline `11574d0295771edccadd17af14af74a539f924c7`. No GPU,
4K/8K, affinity override, sleeps, reseeding or new decoder implementation.

## Priority A: actual Phase2C CPU-reference sequence

The exact original #116 failing packet and authenticated SHA are **UNAVAILABLE**
locally. No private transfer has been performed. A bounded local artifact search
did not identify an authenticated original M4 sample. Packet size equality is
not an identity check. No available CDB debugger executable was found; existing
debugger support DLLs alone were not treated as a working debugger.

Rather than rerun the already completed BGRA metadata A/B, the manual
`reference_phase2c_packet` exporter follows the CPU-reference sequence in
`VerifyPhase2CIdentity`: canonical MakeFrame seed `0x1337BEEF`, BGRA/FS3/YUV422,
initial SDK encode to initialize buffers/quantizers; the exact deterministic
GUID/DATE/TIME/TIMC constants; RefreshUpstreamPyramid; the same 30 external-band
encode calls; then EncodeEntropy with one serial worker and its binding serial
byte comparison. Reused source files and canonical generator are unchanged from
the #116 source revision `5c87391e82f307fbe7db0be6a53fd0edbdaad527`.
The exporter compiles the existing CPU entropy module into a separate manual
test target; no production source or default dispatch is changed.

Recorded state: frame number **1 -> 2**, GOP1, 30 bands, internal serial byte
comparison true. The packet has **900,396 bytes**, matching the documented M4
sample size, but has no verified byte identity to that original sample:
`2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52`.

Three fresh processes/five BGRA opens each, PROCESSING_ALL_ON, produced **15/15
API successes, one RGB/BGRA hash, PSNR 44.465986 dB**. This bounded recreated
CPU-reference case did **not** reproduce the original intermittent 1080p failure.
GPU-produced packets, RunPhase2C multi-frame/slot scheduling, original M4 CPU
topology/CRT/build and the original failing packet remain unqualified. No
additional arbitrary repetition counts replace those missing conditions.

The successful internal serial comparison is not an independent CPU/GPU encoder
packet pair or a repeat-encoding study. `encoder_packets_equal` stays null and
the classifier returns REFERENCE_EVIDENCE_INCOMPLETE, with release blocked.

## Geometry control outside the 128px narrow-chroma case

Generate one independent 256x256 canonical BGRA/FS3 packet through the original
SDK, then hold its bytes fixed for all decode and temporal calls:
`7655842a920a1d2f35f1cf568b21e99232004ce2a71e9ea5f4cf708da586f26e`.

The **unmodified original decoder** gave four RGB hashes in ten opens across
two processes, both internally divergent; **PSNR 6.494–43.814 dB**, API success
10/10. All active metadata Open/Init/Set triplets succeeded. This demonstrates
that the observed BGRA variability is not confined to the known 128px case.
The #21 forward-horizontal row mapping is 128px -> width16 versus 256px ->
width32, distinguishing that specific forward-prefix finding. The observed
inverse deepest-chroma input widths are still 8 and 16 respectively; this is
**not** proof of inverse boundary safety or absence of another narrow-width bug.
It does not qualify memory safety or replace #13 sanitizers.

The existing 128px packet from the completed A/B is reused without regenerating:
`60ed81a3f164d585ae4d3dc0c4af4154a626cef12311a61b4d2dcca80be04b14`.

## What the temporal observer changes

Unlike the earlier retained-state observer, this manual MSVC x64 Release target
**recompiles an isolated copy** of pristine `decoder.c` through a wrapper include.
A preprocessor call-name redirect places an observer around
TransformInverseSpatialQuantLowpass. The observer always calls the **original
wavelet implementation exactly once with unchanged arguments**. A local linker
map verifies DecodeSample comes from decoder_temporal.obj, while the inverse
function remains in original CFHDCodecStatic:wavelet.obj; only allowlisted symbol
origins and the map hash are published, not the raw map or local paths.

No upstream file is edited, instruction replaced in the production binary,
completion flag changed, job reordered deliberately, or thread limit adjusted.
The isolated binary's call boundary and timing **are instrumented**; this must
not be described as an untouched original decoder binary. Both observer and
reference exporter remain excluded from the default build and production.
Internal SDK layout exposure is limited to matching Release headers/defines
`_ALLOCATOR=1`, `WARPSTUFF=1`; do not use with another SDK layout or Debug build.

Each call captures all four logical 16S input bands before and after the original
function, its reconstructed lowpass output after return, started/valid flags,
scale/prescale, scratch size and channel/wavelet identity. Row padding and pointer
values are omitted. An atomic ordinal brackets observer entry and original
function return. Records are appended under a mutex **after** the filter, without
serializing the original filter itself. Buffer copies/allocations still perturb
timing and can observe concurrent writes; they are not a sanitizer or atomic
snapshot of the whole codec. Raw band buffers remain private in ignored build
storage; only hashes and numeric descriptors are published.

Two processes/five opens per geometry: **20 temporal calls**, six inverse events
each (three channels, wavelet2 -> 1 and 1 -> 0). All decode/metadata statuses zero;
all **120 events** satisfy the original ALL_VALID/ALL_STARTED macros. At return
from DecodeSample, observer calls in flight are zero in every call. Actual pool
counts are **16 transform / 4 entropy workers**. These end-of-call observations
do not imply that each intermediate dependency had completed when consumed.

## The first demonstrated temporal dependency defect

Original `image.h:230` defines BANDS_ALL_VALID as **valid_flags == started_flags**,
while :231's BANDS_ALL_STARTED requires the highpass bits only. For four bands:

| Started | Valid | ALL_STARTED | ALL_VALID | Required lowpass bit0 ready |
| --- | --- | --- | --- | --- |
| 0x0E | 0x0E | true | true | **false** |
| 0x0F | 0x0F | true | true | true |

This is also exercised against the **actual frozen macro** in the new native
`reference_decoder_completion_flags` CTest, not only a Python model.

The entropy transform queue uses BANDS_ALL_VALID at
`upstream/Codec/entropy_threading.c:269` and :285, then calls
ReconstructWaveletBand at :302. The latter's guard at `decoder.c:13032` repeats
the same equality predicate; it is **not bypassed**. The predicate can return
true without requiring the lower image's lowpass dependency to have completed.
Its original inverse call is at :13045, followed by marking lowpass valid at
:13050. The producer/consumer association is the original transform's channel
and index, where :13000 selects output lowpass_index = input_index - 1.

| Geometry | Dependencies checked | Entry lacks bit0, equality macro true | Consumer return before producer entry | Consumer entry lowpass differs from later producer output |
| --- | --- | --- | --- | --- |
| 128x128 | 30 | 8 | **5** | 3 |
| 256x256 | 30 | 7 | 0 | 5 |

At 128px, for example process1/open1/channel0, the index1 consumer has ordinals
**entry1/return3** while its index2 producer has **entry4/return6**. The consumer
already returned before the producer entered the observer or ran the original
inverse function. Entry flags are **valid=started=14**; the consumed lowpass SHA
differs from the producer's eventual output. That call's source PSNR is **8.379
dB**. Two other such dependencies consume different lowpass values, while two
coincidentally contain bytes matching the later producer output (and yield a
good 35.648 dB 128px frame). The invalid ordering therefore need not visibly fail
on every call; stale storage can coincide with expected values.

At 256px, seven entries likewise lack bit0. In four function windows the input
buffer hash changes between the before/after snapshots; all changing bands are
lowpass, while highpass hashes remain stable. Five consumer-entry lowpass values
differ from the producer's final output. The temporal worker produces two RGB
hashes, PSNR **10.020–43.814 dB**, while the unmodified decoder above also varies.
There is no consumer-completed-before-producer-entered case in this small 256px
run. Overlapping ordinal windows alone must not be overstated as proof of the
arithmetic instruction order; before snapshots take time. The stronger 128px
return-before-entry relation is unambiguous at the instrumented boundary.

In both geometries the captured wavelet2 input, output and highpass hashes are
stable; the first captured differing input is the lowpass at wavelet1 entry.
This locates an invalid temporal dependency at the index1 -> 0 boundary rather
than merely a post-decode RGB difference. It does not identify a machine-code
instruction or prove every possible upstream race/OOB mechanism. It also does
not reproduce the original M4 failing 1080p packet.

## Reproduce and retain fail-closed gates

```powershell
cmake --build build --config Release --target reference_phase2c_packet reference_decode_worker reference_decode_temporal --parallel 4
python -B tests/reference_decode/priority_probe.py --worker build/tests/Release/reference_decode_worker.exe --temporal-worker build/tests/Release/reference_decode_temporal.exe --exporter build/tests/Release/reference_phase2c_packet.exe --packet128 build/bgra-ab/128/sample.cfhd --source128 build/bgra-ab/128/source.bgra --work build/priority-probe --output build/priority-probe.json
python -B tests/reference_decode/analyze_dependency.py build/priority-probe.json --output build/dependency-analysis.json
ctest --test-dir build -C Release --output-on-failure
```

Use the CPU build and existing 128px artifacts from the previous report;
configure their local paths without publishing private values. Work directory
must be fresh. Both analysis commands intentionally return 1 for unresolved
release acceptance; 2 means invalid measurement/evidence. Byte mismatches, API
errors and stable quality failure remain binding; no PSNR threshold is weakened.
The priority256 case remains REFERENCE_DECODER_NONDETERMINISTIC. No FFmpeg oracle,
new encoder study, SDK workaround, threading change or production fix is added.

Native full CPU Release CTest: **13/13 PASS**, zero skips; 15 parent classifier
methods, nine measurement/analysis methods, and the new actual-macro regression.
The green macro test records the blind spot; it is not decoder quality clearance.
All measurements use fixed synthetic input and bounded counts. No GPU or host
sanitizer, CUDA/4K/8K or performance qualification is claimed.

**Next action:** independently review the source-mapped 0x0E completion finding
and isolated temporal ordering; obtain the authenticated original #116 packet
and state for a small exact-packet 1080p confirmation if privately feasible.
Any remediation must be separately reviewed in the wrapper/own decoder layer,
without changing frozen upstream or silently converting a failing gate to PASS.
#120 and Draft #138 remain open; no main merge or release is authorized.
