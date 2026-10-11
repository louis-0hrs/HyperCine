<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE. -->
# Decode Phase 3A-E — sparse output correctness audit

## Scope and provenance

Phase 3A-D discovered sparse full-scale BGRA errors in three real photographs
and ended with a dispatcher promotion HOLD. This phase investigates that
correctness failure independently. The original D report, measurements, source
fingerprints and failed records are preserved verbatim.

Branch: `decode-phase3ae`, created from the clean final D commit
`9f63cfa191671175a2a34991a3084631332f90bd`. Before implementation changes,
`src/` and `include/` matched production baseline
`e6e9cb905aee848808682942d13f07f2ff505a2d` exactly. `main` was
`51435890ba5853e87e8cf1ddc7cc747954bd9e80`. The upstream subtree matched
GoPro baseline `11574d0295771edccadd17af14af74a539f924c7` (tree
`837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`).

Audit host: Windows, MSVC Release, RTX 4090 Laptop GPU, CUDA 12.8, driver
610.62. GPU checks run sequentially. This phase adds no classifier features,
thresholds, formats or performance optimization.

## Original failures and repeatability

The original normalized encoded files were not retained in D. They were
materialized from the retained YUY2 sources using D's exact encoder recipe and
checked against the original encoded FNV-1a64 **and byte count** before decode.
The new photo manifest adds SHA256 fingerprints. Encoding uses the unchanged
upstream encoder, FilmScan 3, flags NONE, seed `0x51A77`, pitch 3840, followed
by the existing metadata normalization. No golden output was regenerated.

| Photo | Source FNV-1a64 | Encoded FNV-1a64 | Encoded bytes | Original wrong pixels |
| --- | --- | --- | ---: | ---: |
| brick | 1252b69a6e689d6a | 1fad1210cf321a16 | 3180804 | 447 |
| grass | df9a67919f22f7b1 | 262eddc0bd80b320 | 4255116 | 93 |
| handwriting | 4500cf86ac819d0c | 80955c5750f8ef94 | 1569836 | 33 |

Each photo reproduced its original count in 100 Lightweight decodes before the
repair. Every complete BGRA image was identical, proving identical coordinates
and channel values as well as counts. Default output was 1920×1080, BT.709/CG,
bottom-up, BGRA pitch 7680. `decode_phase3ae_reproduction.json` records the
first 32 coordinates, reference/GPU hashes, ticket and output details.

## Independent stage and strategy comparison

The new audit checks each representation even when BGRA differs. It compares
validated descriptor geometry with native CPU bands, every active coefficient
in all 30 bands, both LL checkpoints for all three channels, actual final signed
planes from the **same BGRA ticket**, a separate public planar request, packed
YU64, and every BGRA pixel. All four strategies are requested independently.

The pre-fix default matrix is:

| Frame | Requested strategy | Coefficients | LL | Signed planes | YU64 words | BGRA pixels |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| brick | Lightweight | 0 | 0 | 0 | 0 | 447 |
| brick | Hybrid | 0 | 0 | 0 | 0 | 447 |
| brick | HostStreaming | 0 | 0 | 0 | 0 | 447 |
| brick | Auto (Hybrid selected) | 0 | 0 | 0 | 0 | 447 |
| grass | Lightweight | 0 | 0 | 0 | 0 | 93 |
| grass | Hybrid | 0 | 0 | 0 | 0 | 93 |
| grass | HostStreaming | 0 | 0 | 0 | 0 | 93 |
| grass | Auto (Hybrid selected) | 0 | 0 | 0 | 0 | 93 |
| handwriting | Lightweight | 0 | 0 | 0 | 0 | 33 |
| handwriting | Hybrid | 0 | 0 | 0 | 0 | 33 |
| handwriting | HostStreaming | 0 | 0 | 0 | 0 | 33 |
| handwriting | Auto (Hybrid selected) | 0 | 0 | 0 | 0 | 33 |

Numbers are differences, not PSNR or tolerances. The full 96 combinations
(3 photos × 4 strategies × BT.601/709 × CG/VS × both orientations) have exact
coefficients, LL, signed planes and YU64 before the fix. The first divergent
representation is BGRA; the first wrong arithmetic operation is signed-sample
precision reduction in the shared color kernel.

Reference discipline matters: upstream's LL channel offset depends on the
requested output (BGRA: 24; YU64/planar: 4). BGRA reconstruction must be compared
with the CPU BGRA reconstruction; a separate planar request uses CPU YU64
intermediates. Mixing those references creates false inverse differences.
Also, bottom-up BGRA row `r` corresponds to top-down plane row `height-1-r`.
The earlier D output-row/plane-row observation is not used as evidence of a
plane discrepancy. The new audit reads the actual mapped buffers directly.

## Actual pixels, addresses and numerical proof

All values below were downloaded from completed tickets, not inferred from
compressed coefficients. Triplets are Y/U/V; quads are B/G/R/A.

| Photo | Output x,row | Plane row | Chroma x | CPU = GPU YUV | CPU BGRA | Old GPU BGRA |
| --- | --- | ---: | ---: | --- | --- | --- |
| brick | 711,2 | 1077 | 355 | -1,495,524 | 0,0,5,255 | 255,255,255,255 |
| grass | 834,0 | 1079 | 417 | -6,483,511 | 0,2,0,255 | 255,255,255,255 |
| handwriting | 1833,0 | 1079 | 916 | -3,522,507 | 4,1,0,255 | 255,255,255,255 |

Brick has Y pitch 4096, U/V pitch 2048 and BGRA pitch 7680. Its byte offsets
are `1077*4096+711*2 = 4412814` in Y,
`1077*2048+355*2 = 2206406` in each chroma plane, and
`2*7680+711*4 = 18204` in BGRA. Allocations are respectively 4423680,
2211840, 2211840 and 8294400 bytes. Each access includes its element size
in the bounds assertion. `decode_phase3ae_pixels.json` retains allocation bases,
all failing coordinates for all four strategies, and clipped 3×3 neighborhoods
for the first 16 failures per photo/strategy.

The old kernel evaluated `uint16_t(raw_y) >> 2`. For brick, `-1` became
`65535`, then `16383`, instead of `-1`. The subsequent SIMD-compatible
luma-offset/limiter produced 255. The multiply-high/shift luma contribution
became 18996; unchanged chroma terms yielded pre-pack B/G/R of 286/296/302.
All three final unsigned packs therefore saturated to 255.
The repaired reduction preserves `-1`; its luma limiter yields zero and its
unchanged chroma terms produce `[0,0,5,255]`, exactly the upstream result.

Every one of the 573 default-photo mismatches has negative reconstructed Y.
Across all 96 combinations, the test-only CPU clone of the old integer
arithmetic reproduces **every** wrong GPU pixel, and changing only the clone's
input signedness reproduces **every** upstream reference pixel. Histograms in
the root-cause JSON retain signed values, x modulo 2/4/8/16/32/64, tail counts
and row distances. Even/odd chroma mapping is `x/2`, native Y/V/U is explicitly
mapped to Y/U/V, and top-down/bottom-up physical rows agree. This is a sample
value trigger rather than a tile, pitch, orientation or chroma-index defect.
Y ranges from -3 to -1 in brick and handwriting, and -6 to -1 in grass.
Both x parities occur (brick: 231 even/216 odd; grass: 53/40; handwriting:
7/26). Scalar-tail failures also occur (30/1/1 respectively). The correction
therefore covers the fast-loop and border branches without changing their
different rounding/limiter rules.

Pristine `upstream/Codec/spatial.c`,
`InvertHorizontalStripYUV16sToPackedRGB32`, prevents negative fast-loop
reconstruction from becoming unsigned highlights before color conversion;
its scalar border uses signed reduction. HyperCine's separate signed-plane
reconstruction can legitimately undershoot zero and must preserve that sign
on entry to its existing converter. The upstream file is unchanged.

## Synchronization, initialization and lifetime investigation

The Lightweight dependency chain waits for compressed-copy completion,
produces index/replay completion events, waits for the three high-pass bands
and LL at each inverse level, records each channel's completion after final
inverse, then makes the main stream wait for all three channel events. Color
conversion and applicable frame-error copy are queued before `ev_ready` on
that same main stream. Hybrid and HostStreaming also join channel completion
before color/Ready. The facade waits/polls the Ready event before exposing a
completed output. Registered consumer streams wait on Ready and are drained
before recycling a released slot.

The audit explicitly waits for inverse and production Ready, then re-runs
color on one independent non-default stream with an explicit stream
synchronization. Both `0xCD` and `0xA5` output fills are fully overwritten.
The isolated color result is identical to production, including every wrong
pre-fix pixel. This serializes the work at the first divergent stage without
changing production scheduling or adding `cudaDeviceSynchronize()`.

Pre-fix tests alternate black/white preceding frames in both orders, poison
completed output and final inverse allocations before release, exercise one
and two slots, wraparound, held outputs, non-FIFO release and a registered
consumer copy. Each configuration repeats 25 times per photo. Fresh and reused
outputs have the same original wrong image; no sentinel or previous-frame
contamination occurs. Owned capacities and CUDA allocation counts remain
constant. These results plus the arithmetic clone distinguish a deterministic
logic defect from a race.

The color launcher checks `cudaGetLastError()`, and the focused audit checks
its return and stream completion. Existing backend call sites do not
consistently consume every individual launcher return; this phase does not
claim that source audit alone eliminates all possible launch-failure paths.
The relevant completed production tickets, exact buffers and pre/post native
sanitizer runs provide runtime evidence for these samples. No broad production
synchronization or unrelated launch-error refactor is added.

## Narrow repair and permanent regressions

The behavioral repair removes three unsigned casts in
`src/cuda/decode_color_cuda.cu`: reduce raw signed Y, V and U with `raw >> 2`.
The existing matrix coefficients, limiter, saturation, rounding, SIMD/scalar
boundary, chroma bias, packing and orientation remain intact. Nonnegative
sample reduction is identical. Negative values now follow the upstream clamp
semantics instead of reinterpreting a signed overshoot as a highlight.
No entropy, inverse, dispatch or bitstream behavior changes.

The other library changes add a research inspector for the final planes of a
completed BGRA ticket. It locks existing state and returns borrowed pointers,
actual pitches, allocation sizes and slot index; it introduces no buffers or
per-frame allocations. The CPU-only stub returns false. It is outside the
unconditional public decoder API and does not affect its layout.

`test_decode_phase3ae` is registered in CUDA CTest and needs no photo files or
network access. Its deterministic 64×64 YUY2 pattern has a black background,
a small white rectangle and a white corner. Encoding it with the existing
FilmScan 3 recipe yields 32 wrong pixels per strategy before the repair and
zero after it. All coefficient, LL, plane and YU64 checks pass in both versions.
Direct signed/extreme vectors exercise both matrix/range combinations, both
orientations, fast-loop and scalar-tail pixels, clamp thresholds, negative
chroma and saturation. The literal brick triplet asserts `[0,0,5,255]`.
The old kernel fails 23502 direct comparisons; the repaired kernel fails zero.

Earlier frozen generators did not trigger these negative reconstruction
samples. The original direct color test did include negative values, but its
CPU oracle copied the same unsigned-cast defect and therefore asserted the
wrong semantics. The first E full-suite run exposed that obsolete expectation;
its three precision-reduction expressions are corrected, with its input
patterns, tolerance and timeout unchanged. The new literal and upstream-encoded
regressions provide independent evidence. Exact coefficient and LL checks could not prove correct conversion
of the final signed planes. The D checker also threw at its first BGRA
difference, skipping later plane/YU64 comparisons and subsequent strategies.
The new photo audit separates those comparisons and continues across them.
`tests/decode_phase3ae_photos.json` preserves real-photo provenance and
deterministic reproduction instructions without committing large media.

## Validation and completion

The final validation, timing tables, integrity checks and all 67 requested
completion fields are generated from retained run artifacts by
`benchmarks/summarize_decode_phase3ae.py`. See the appended results below and
the corresponding JSON files under `benchmarks/results/`.

<!-- GENERATED E RESULTS -->

### Completed correctness gates

Outcome: **A — FIXED**. Brick/grass/handwriting now have 0/0/0 wrong pixels. All 96 photo configurations and all four requested strategies match upstream at every audited stage.

Each photo passes 500 Lightweight repetitions (250 with one slot, 250 with two), including alternating black/white predecessors, poisoned completed surfaces, held/non-FIFO releases and registered consumer copies.

Frozen golden: 11/11 samples, 44 strategy comparisons, original sample and CPU output hashes unchanged. Frozen RC/D corpus: 156/156 supported cases in 158 total cases pass all four strategies and stages; 2 unsupported controls remain unsupported, with zero failures. These controls are RGB444 and 8K noise above the existing 32 MiB sample limit. Coverage includes photographs, text/UI, architecture, foliage, water, grain/noise, gradients and high contrast; no dispatcher fitting is performed.

CUDA CTest: 33/33 pass in 700.21 s. CPU-only CTest: 6/6 pass in 4.85 s. The first CUDA run had 32 passes and one obsolete unsigned CPU-oracle failure (706.18 s); its log is retained as `decode_phase3ae_cuda_ctest_first.txt`. A fresh full run follows the oracle correction; no timeout or tolerance was relaxed.

All four native Compute Sanitizer tools pass both before and after repair: memcheck/initcheck/synccheck report zero errors; racecheck reports zero hazards, errors and warnings. Coverage includes the self-contained regression, all three real photos through all four strategies, stage downloads, isolated synchronized color and poisoned slot/consumer reuse. A first sandbox memcheck attempt could not attach and timed out; its failure log is preserved, its hung audit processes were terminated, and the native rerun passed. That attempt is not counted as a clean sanitizer run. Post-fix memcheck with full leak checking also reports 0 bytes leaked in 0 allocations.

The 2000-frame mixed smoke verifies every full BGRA image while switching Lightweight, Hybrid and Auto over nine photo/synthetic/text cases with two held tickets and varying release order. Concurrent runs with two and four independent decoders each complete 100 real-photo frames per instance exactly. Existing facade tests also pass destruction/drain, failed work, consumer waits, malformed rejection and ticket identity checks.

Owned device/pinned capacities and CUDA allocation counts remain unchanged. Existing facade warm-state accounting reports 0 C++ allocations. Mixed-smoke working set changes from 295849984 to 297213952 bytes; global free VRAM changes from 15634268160 to 15634268160 bytes. Global free VRAM is observational and includes driver activity; owned-byte equality is the allocator invariant. The repair adds no buffers, streams or events.

### Performance regression measurement

Same-session ABBA uses the saved old-kernel binary and repaired binary, 15 warmups followed by 120 measured frames per case/strategy/window. It measures Queue through Ready and Release without download. Means below average the two windows per version. All individual FPS, megapixels/s, min/max latency and peak working-set values are retained in the performance JSON.

| Case | Strategy | Before ms | After ms | Latency change | Both paired comparisons >5% |
| --- | --- | ---: | ---: | ---: | --- |
| 1920_flat | Lightweight | 1.0826 | 1.0057 | -7.10% | False |
| 1920_flat | Hybrid | 2.2586 | 2.2078 | -2.25% | False |
| 1920_noise | Lightweight | 14.7849 | 14.9643 | +1.21% | False |
| 1920_noise | Hybrid | 4.3920 | 4.4370 | +1.03% | False |
| 3840_flat | Lightweight | 1.5551 | 1.6033 | +3.10% | False |
| 3840_flat | Hybrid | 3.2401 | 3.1870 | -1.64% | False |
| 3840_noise | Lightweight | 64.0880 | 64.5705 | +0.75% | False |
| 3840_noise | Hybrid | 8.0791 | 7.9968 | -1.02% | False |
| photo_holdout_brick | Lightweight | 10.0216 | 10.0558 | +0.34% | False |
| photo_holdout_brick | Hybrid | 6.7758 | 6.6342 | -2.09% | False |
| photo_holdout_grass | Lightweight | 11.0464 | 10.9689 | -0.70% | False |
| photo_holdout_grass | Hybrid | 6.9436 | 6.8650 | -1.13% | False |
| photo_holdout_handwriting | Lightweight | 6.6114 | 6.7089 | +1.48% | False |
| photo_holdout_handwriting | Hybrid | 5.0942 | 5.1353 | +0.81% | False |

Positive change means slower. Cross-window variation is recorded explicitly; this phase claims no optimization. The >5% flag requires both paired comparisons to regress. Owned device/pinned capacities and allocation calls match across all four windows.

`benchmark_codec` was also run at 1080p, 4K and 8K and compared with the frozen Windows reference. That is the unchanged CPU SDK benchmark and a cross-session comparison, not evidence of GPU repair cost.

| CPU reference benchmark | Historical decode ms | Current decode ms | Change | Historical/current peak bytes |
| --- | ---: | ---: | ---: | --- |
| 1080p | 2.5368 | 1.9580 | -22.82% | 96985088/96206848 |
| 4K_UHD | 7.9796 | 7.7774 | -2.53% | 240435200/239599616 |
| 8K_UHD | 35.7600 | 32.1569 | -10.08% | 644014080/642977792 |

### Repository integrity and recommendation

Upstream is pristine; encoder files, main, frozen golden manifest and historical reports/results are unchanged. Only the shared CUDA color reduction changes decoding behavior. Research inspection and audit/test tools supply the evidence. No merge, tag or push was performed. The source baseline and report provenance hashes are retained in completion JSON.

Review the minimal signedness repair and regression coverage as a correctness patch. The supported progressive YUV422 output blocker is cleared by the clean acceptance gates above. Phase 3A-D's Auto-dispatch HOLD remains; this phase does not authorize Phase 3B promotion or certify additional formats/backends.

### Reproduction commands

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake -B build-cpu -S . -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
.\build\tests\Release\test_decode_phase3ae.exe --validate . build/phase3ae/post
.\build\tests\Release\test_decode_phase3ae.exe --reuse . build/phase3ae/post
.\build\tests\Release\test_decode_phase3ae.exe --golden . build/phase3ae/post
.\build\tests\Release\test_decode_phase3ae.exe --mixed . build/phase3ae/post
.\build\tests\Release\test_decode_phase3ae.exe --instances . build/phase3ae/post
# Native host, repeat --tool for memcheck/initcheck/racecheck/synccheck:
compute-sanitizer --tool memcheck --error-exitcode 99 .\build\tests\Release\test_decode_phase3ae.exe --sanitizer . build/phase3ae/post-native-memcheck
python benchmarks/summarize_decode_phase3ae.py .
```

The corpus fingerprint TSV can be reconstructed from the `id`/`sample_hash` pairs in the unchanged D measurements and hold-out JSON files, then passed to `audit_decode_phase3ae_corpus ROOT EXPECTED_TSV OUTPUT_JSON`. Pre-fix reproductions require the saved old binary or the parent implementation, not the repaired binary.

### Requested completion fields

All 67 fields are below; full structured values and file/commit lists are in `decode_phase3ae_completion.json`. Commit entries capture implementation commits available when evidence is generated; the commit containing this report is identified by `git log 9f63cfa..HEAD`.

| # | Field | Result |
| ---: | --- | --- |
| 1 | branch | decode-phase3ae |
| 2 | base commit | 9f63cfa191671175a2a34991a3084631332f90bd |
| 3 | commits | ["5f839527b67a94aa5c48e5a7c7d50d0e3c1ee2e7 fix(decoder): preserve signed inverse samples in BGRA conversion"] |
| 4 | files changed | Recorded in completion JSON field 4 |
| 5 | production files changed | Recorded in completion JSON field 5 |
| 6 | upstream status | pristine |
| 7 | encoder status | unchanged |
| 8 | main status | 51435890ba5853e87e8cf1ddc7cc747954bd9e80 |
| 9 | original brick mismatch count | 447 |
| 10 | original grass mismatch count | 93 |
| 11 | original handwriting mismatch count | 33 |
| 12 | repeatability before fix | 100 per photo; complete images identical |
| 13 | coefficients status | 30/30 bands exact |
| 14 | LL status | 6/6 checkpoints exact |
| 15 | planar status | BGRA ticket and public planar request exact |
| 16 | YU64 status | bit-exact words |
| 17 | BGRA status | original 447/93/33; now zero |
| 18 | first divergent stage | DeviceBGRA8 precision reduction |
| 19 | first divergent frame | photo_holdout_brick |
| 20 | first divergent coordinate | {"x": 711, "bottom_up_output_row": 2, "plane_row": 1077} |
| 21 | CPU YUV there | [-1, 495, 524] |
| 22 | GPU YUV there | [-1, 495, 524] |
| 23 | CPU BGRA there | [0, 0, 5, 255] |
| 24 | GPU BGRA there | [255, 255, 255, 255] |
| 25 | pitch/address calculation | Recorded in completion JSON field 25 |
| 26 | root cause class | signedness |
| 27 | exact root cause | uint16_t reinterprets negative reconstructed samples before precision reduction |
| 28 | deterministic vs race | deterministic logic |
| 29 | previous-frame contamination | False |
| 30 | slot reuse involvement | False |
| 31 | stream/event involvement | False |
| 32 | color math involvement | input signedness; matrix formula unchanged |
| 33 | bounds involvement | False |
| 34 | signedness/overflow involvement | unsigned reinterpretation before clamp; no inverse overflow |
| 35 | minimal reproducer | 64x64 encoded undershoot plus literal signed-plane vectors |
| 36 | repair summary | remove uint16 casts from Y/V/U precision reduction |
| 37 | why repair is correct | preserves negative overshoots for upstream-compatible limiter; positive values unchanged |
| 38 | why previous corpus missed it | frozen generators missed negative values; direct test CPU oracle copied the unsigned defect; D checker stopped early |
| 39 | post-fix brick mismatches | 0 |
| 40 | post-fix grass mismatches | 0 |
| 41 | post-fix handwriting mismatches | 0 |
| 42 | 500 repeat result | 500 per photo, slots 1/2, all full images exact |
| 43 | Lightweight vs Hybrid equality | True |
| 44 | HostStreaming equality | True |
| 45 | 11/11 Golden result | 11/11, four strategies, unchanged frozen hashes |
| 46 | broader correctness corpus result | {"passed": 156, "unsupported_controls": 2, "failed": 0} |
| 47 | CUDA CTest | Recorded in completion JSON field 47 |
| 48 | CPU-only CTest | Recorded in completion JSON field 48 |
| 49 | initcheck | zero errors |
| 50 | memcheck | zero errors |
| 51 | racecheck | zero hazards/errors/warnings |
| 52 | synccheck | zero errors |
| 53 | mixed 2000-frame smoke | Recorded in completion JSON field 53 |
| 54 | two-instance result | {"instances": 2, "frames_per_instance": 100, "real_photo_output_exact": true} |
| 55 | four-instance result | {"instances": 4, "frames_per_instance": 100, "real_photo_output_exact": true} |
| 56 | output lifetime result | held/non-FIFO/consumer reuse; existing completed/failed output checks and destruction/drain pass |
| 57 | allocation change | zero steady-state C++ allocations; CUDA calls unchanged |
| 58 | VRAM change | owned bytes unchanged |
| 59 | pinned-memory change | owned bytes unchanged |
| 60 | 1080p performance impact | Recorded in completion JSON field 60 |
| 61 | 4K performance impact | Recorded in completion JSON field 61 |
| 62 | failing-photo performance impact | Recorded in completion JSON field 62 |
| 63 | final outcome | A — FIXED |
| 64 | remaining correctness blockers | [] |
| 65 | dispatcher blocker status | Phase 3A-D HOLD remains |
| 66 | merge recommendation | review minimal fix separately; no automatic merge |
| 67 | recommended next step | human review; keep Phase 3B dispatcher promotion blocked |
