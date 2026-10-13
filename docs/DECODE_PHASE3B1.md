<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under the MIT License or Apache License, Version 2.0. -->
<!-- See LICENSE-MIT and LICENSE-APACHE for details. -->

# Decode Phase 3B-1 — GOP-1 interlaced YUV422 completion gate

This report records the implementation actually validated in Phase 3B-1C.
The numerical algorithm was preserved. Broader validation found two parser
boundary defects, described below; no entropy, transform mathematics, color
coefficients, Auto policy, geometry policy, or slot architecture was redesigned.

## 1. Lineage, preservation, and scope

- Branch: `decode-phase3b1`.
- Starting HEAD: `7296f6f3b60ae4d9b902415f1a531f20f4ba70ad`.
- Starting working tree: fourteen modified tracked implementation/test files,
  untracked `tests/test_decode_phase3b1.cpp`, and the pre-existing `job.txt`.
  `job.txt` is unrelated local input and is excluded from commits.
- The original implementation and untracked smoke test were copied into
  `build/phase3b1-start/`, with HEAD and a binary-capable patch, before fixes.
  No checkpoint commit was made before the mandatory full test gate.
- The frozen 64x64 and 1080p references were rerun first; both were exact.
- `upstream/` is unchanged. Its reference baseline remains
  `11574d0295771edccadd17af14af74a539f924c7`.
- No encoder implementation is changed. `main` remains
  `51435890ba5853e87e8cf1ddc7cc747954bd9e80`.
- No merge, push, tag, or release is performed.

The newly accepted production profile is exclusively YUV 4:2:2, coded precision
10, three channels, thirty coefficient bands, spatial transform type 0, one
frame/GOP-1, and absent or clear progressive flag. It requires consistent YUV
descriptors, the existing valid geometry, and a complete frame/group trailer.
Explicit non-YUV encoded-format metadata is rejected on this path.

FIELDPLUS/GOP-2, RGB444, RGBA4444, Bayer, alpha, YUV coded precision 8 or 12,
and production YU64 remain outside the supported profile. Existing geometry
limits remain 64..7680 by 64..4320, width divisible by 16 and height divisible
by 8, additionally bounded by `DecoderConfig`. The 32 MiB sample cap is unchanged.

## 2. Parser and internal mode

`ParseCineFormSampleValidated` returns the private `FrameReconstruction` mode:
`Progressive` or `InterlacedGop1`. The mode is chosen only after validating the
thirty-band YUV422/precision-10 descriptor contract. Transform types other
than 0 and frame counts other than 1 retain `UnsupportedEncodingMode`.
The research parser's existing behavior is preserved.

`QueueDecode` forwards the private mode to the backend. The slot stores and
resets its `interlaced_gop1` flag for every frame, including alternating
progressive/interlaced input and changing output format. There is no temporal
reference-frame state and no GOP-2 scheduling path.

Two concrete validation-driven corrections were made:

1. Removing the final four-byte **frame trailer** leaves all thirty sized
   entropy chunks intact. The pristine CPU decoder rejects the interrupted
   frame with error `2048`; the initial production parser accepted it. The
   new interlaced path now requires a frame or group trailer. Progressive
   parsing policy is unchanged. An initial check incorrectly required only
   a group trailer; frozen-reference tests immediately detected that mistake,
   and the check was corrected to accept either actual trailer kind.
2. A valid YUV422 interlaced payload with tag 84 changed to internal RGB format
   3 was initially accepted and published output. The new interlaced parser
   now rejects explicit encoded-format values other than internal YUV422 value
   1. This does not widen any unsupported format's acceptance.

The initial failing rejection evidence and interrupted validation logs are
retained. No pixel oracle or frozen expected hash was changed to obtain a pass.

## 3. Reconstruction sequence and dependency graph

For each wavelet channel, internally ordered Y, V, U:

1. Level 2 spatial inverse reconstructs LL1 using the existing quantized
   signed-16 vertical/horizontal inverse.
2. Level 1 spatial inverse reconstructs LL0 with the existing descale value 2.
3. At level 0, horizontal inverse of `(LL0, band 1)` constructs temporal low;
   horizontal inverse of `(band 2, band 3)` constructs temporal high.
4. The field kernel combines those two horizontal outputs into alternating
   even and odd full-frame rows.
5. Existing channel completion events join before output conversion/publication.

Levels 2 and 1, entropy replay, dequantization, difference coding, and Peak
restoration are unchanged. Progressive level 0 still runs its existing
spatial vertical/horizontal inverse and never launches the field kernel.

The existing `d_vert_low`/`d_vert_high` scratch and `d_final_plane` allocations
are reused. No second permanent full-frame YUV buffer is introduced. The field
kernel has one thread per column and field row, separate byte pitches for low,
high and output, bounds checks, and the existing frame-error guard.

Luma geometry is `width x height`; each chroma plane is `(width/2) x height`.
Each low/high temporal intermediate has half the full-frame row count. Field
row `r` writes frame row `2*r` from low-minus-high and row `2*r+1` from
low-plus-high. Pitches remain padded to sixteen-byte boundaries. Public plane
order is Y, U, V. Public planar output is always top-down. Bottom-up BGRA
reverses complete output rows after field parity is established.

## 4. Exact SSE2 field arithmetic

Let `sat16` saturate to signed sixteen bits, `bits16` preserve a sixteen-bit
bit pattern, and `usub16` perform unsigned saturated subtraction:

```text
even_sum = sat16(low - high)
odd_sum  = sat16(low + high)
biased   = sat16(sum + 30720)
limited  = usub16(bits16(biased), 30720)
code     = arithmetic_right_shift(int16(limited), 1)
```

The upstream row routine subsequently writes the sixteen-bit bit pattern of
`code << 6`. HyperCine retains `code` in its signed plane; interlaced color
conversion applies that container scaling itself.

For ordinary negative ten-bit reconstruction values, saturation clamps the
sum/difference to zero **before** the right shift. Signed add/subtract must
occur before any unsigned reinterpretation. Shift-first signed reconstruction
without this limiter produces a different negative result.

The literal SSE2 sequence has an extreme-input quirk: at very negative synthetic
signed-16 sums, the biased value can remain negative. Its unsigned subtraction
and subsequent signed shift then wrap outside the nominal ten-bit code range.
A generic mathematical `[0,2047]` clamp is therefore not a replacement for the
instruction sequence over the full signed-16 domain. This is preserved and
documented in [UPSTREAM.md](UPSTREAM.md), with no upstream edit.

The direct permanent test covers all 225 combinations of fifteen signed
boundary values, repeated across five rows at width 64 with an eighty-sample
pitch. It verifies 800 output/padding positions, including first/last rows,
against an independent CPU instruction model and the pristine upstream packed
routine. Negative sums/differences, zero, odd rounding, clamp boundaries,
positive overflow and extreme negative inputs are included.

## 5. Lowpass offsets and color conversion

BGRA retains lowpass channel offset **+24**. Public signed planar output
retains the existing **+4** convention, independently compared with coefficient
and inverse stages from a CPU YU64 decode. The two native references must not
be interchanged. The permanent field-luma fixture proves that substituting +4
into the BGRA reconstruction changes both planes and pixels.

Inactive-metadata interlaced BGRA follows the pristine upstream sequence:

```text
ChannelYUYV16toPlanarYUV16
  -> PlanarYUV16toPlanarRGB16 (including TWEAK_YUV2RGB)
  -> ConvertLinesToOutput
```

The CUDA row16 converter reproduces the upstream signed multiply-high,
saturated additions/subtractions, sixteen-bit shifts, and output limiter.
It uses the upstream BT.601/BT.709, CG/VS coefficient adjustments. A direct
test compares it with actual upstream BGRA and proves the same fixture differs
when routed through the progressive converter.

Progressive BGRA keeps the existing signed eight-bit reduction, SIMD semantics,
and scalar right-border path. It is selected with `interlaced_row16=false`;
interlaced BGRA selects the separate specialization. No YU64 helper or
production YU64 behavior changes.

## 6. Frozen references

| Reference | Encoded coefficients checked | Coefficient mismatches | BGRA byte mismatches |
| --- | ---: | ---: | ---: |
| Committed 64x64 FilmScan1 interlaced fixture | 8,192 | 0 | 0 |
| Frozen 1920x1080 FilmScan3 interlaced recipe | 4,147,200 | 0 | 0 |

The 1080p identities remain:

| Identity | Hash |
| --- | --- |
| Original source identity | `2d12a276f3594b09` |
| Normalized 4,655,384-byte sample | `356ad5c204104b7d` |
| Original native coefficient identity | `3455cbb3a354825f` |
| Upstream and HyperCine BGRA | `41a4e58b09555e3e` |

The original source identity is the existing folded FNV recipe; the native
coefficient identity folds all allocated native wavelet bands, including
reconstructed LL bands. The 4,147,200 comparison count instead covers the
thirty encoded coefficient bands, with padding excluded. Both definitions
remain unchanged. Max coefficient error is zero.

Representative field-stage inspection independently verifies temporal low,
temporal high, and field-combine output against pristine CPU horizontal and
field routines at both frozen geometries. Final signed planes are exact for
BGRA's +24 reconstruction and, separately, the public +4 planar request.

## 7. Pattern, quality, field, chroma, matrix, and orientation matrix

The small matrix covers all seventeen families at all six qualities on all
three small geometries: **306 samples**. Adding both frozen references and
five representative 1080p FilmScan3 samples gives **313 samples**.

Families: flat; horizontal, vertical and diagonal gradients; hard horizontal
and vertical edges; checkerboard; thin lines; sparse marks; alternating fields;
field-asymmetric luma; field-asymmetric chroma; saturated YUV; near black;
near white; pseudo-random noise; structured texture.

Qualities: Low, Medium, High, FilmScan1, FilmScan2, FilmScan3.
Geometries: 64x64, 128x64, 128x128, and 1920x1080. Progressive performance
context additionally covers 3840x2160. No geometry rule was changed and no new
canonical 4K interlaced fixture was invented.

Pattern generation uses seed `0x1337BEEF` and explicit integer arithmetic.
The reused `EncodeInput` helper seeds the encoder CRT with `0x6A09E667`.
Generated source and normalized sample hashes are recorded per case.

Every sample is compared independently under Lightweight, Hybrid, Auto and
HostStreaming. Each has all thirty coefficient bands, parsed geometry/channel/
level/band descriptors, both LL intermediates, signed planes and final BGRA
checked. There are **1,252 BGRA/stage strategy rows** and **1,252 independently
referenced public-planar rows**.

Dark even/bright odd luma and strongly opposed U/V field content are permanent
fixtures. Tight Y, V and U comparisons detect swapped fields, parity, starting
row, field pitch, U/V interchange and half-width chroma-pitch errors. These
fixtures vary numerically between fields and across columns; a symmetric
flat image is not the field-order oracle.

Sixty-one representative samples additionally cover all four matrix/range
combinations and both BGRA orientations under all four strategies: **1,952
color/orientation rows**, all exact. For those requested conversions the oracle
calls the three pristine upstream color/output routines on independently
reconstructed CPU planes. Default BT.709/CG bottom-up output is also compared
directly with `CFHD_DecodeSample` on every sample. Matrix/range settings do not
modify the inverse transform.

## 8. Difference coding, Peak-Escape, and strategies

The frozen 1080p FilmScan3 sample retains difference coding on subband 8 in all
three channels and codebook id 2 (`cs18`). Its Y subband 8 contains a genuine
Peak table. Tests require those facts rather than accepting a peak-free
replacement. Additional FilmScan3 field and noise patterns naturally contain
Peak tables too.

Peak restoration, all coefficients, LL inverses, horizontal temporal bands,
field combine and final BGRA are exact. The shared entropy pipeline is used
without an alternate interlaced replay path.

Lightweight uses the existing frame-barrier production strategy; Hybrid uses
the existing adaptive Phase2FC mode; HostStreaming uses existing full overlap.
Auto's original classifier is unchanged and its actual choice is recorded for
every sample. Pixel equality is checked independently for every strategy,
including final planar output. No early exit hides a later strategy failure.

## 9. Progressive and unsupported-profile isolation

The frozen progressive corpus remains **11/11**, with original source/sample/
output identities unchanged. `tests/decode_phase0_golden.txt` has no diff and
expected output hashes were not regenerated. The production test compares all
four strategies and stage references. Phase 3A-E's signedness unit test and
real brick/grass/handwriting stage audit also pass, including all supported
matrix/range/orientation combinations.

| Profile | Required result | Actual |
| --- | --- | --- |
| YUV422 progressive p10 GOP-1 | PASS | PASS |
| YUV422 interlaced p10 GOP-1 | PASS | PASS |
| YUV422 FIELDPLUS progressive GOP-2 | UnsupportedEncodingMode | Rejected, no output |
| YUV422 FIELDPLUS interlaced GOP-2 | UnsupportedEncodingMode | Rejected, no output |
| RGB444 p12 | Reject | Rejected, no output |
| RGBA4444 p12 | Reject | Rejected, no output |
| Bayer p12 | Reject | Rejected, no output |
| YUV coded precision 8/12 | Reject | Rejected, no output |
| Sample above 32 MiB | SampleTooLarge | Rejected, no output |

Malformed coverage includes short headers, payload truncation, missing final
trailer, twenty-nine bands, invalid subband size, invalid channel selection or
channel count, unsupported precision, invalid width/height, contradictory frame
format, contradictory encoded-format metadata, and oversized input. Failed
tickets return clean errors, publish no output and can be released. Phase 3B-0
fixtures are consumed unchanged; its regression's generated evidence/fixtures
are now written to the build directory rather than overwriting historical
committed artifacts.

## 10. Stress, determinism, instances, and lifetime

The mixed corpus contains progressive flat/noise/texture and additional field
patterns; interlaced flat/luma-asymmetric/chroma-asymmetric/noise/texture;
the frozen 1080p difference-plus-Peak sample; and the committed 64x64 sample.
Every BGRA byte is checked in **5,000 frames** using three slots, held tickets
and alternating FIFO/reverse release orders. No stale field/progressive data
or output corruption occurs.

The small frozen fixture is decoded **200 times**, and frozen 1080p **100
times**, cycling independent strategies. Every coefficient, signed plane and
BGRA byte remains identical. Slot counts one, two, default, and four with the
high-throughput policy produce identical output.

Two and four decoder instances each run sixty frames per instance concurrently
on mixed progressive/interlaced content. Outputs remain exact and slot state
is independent. Lifetime coverage includes immediate, delayed and non-FIFO
release; a held output while other frames run; consumer-stream registration
before a host wait; device-copy completion drained by release; simultaneous
failed/successful tickets; and destruction with work queued. Compute Sanitizer
also covers a focused forty-four-frame mixed/reuse/lifetime workload.

## 11. Memory and allocation accounting

Before/after memory is measured from two-slot instances of pre-3B-1 HEAD and
the current implementation at 1080p and 4K in the same session. Owned device
bytes increase by **17,694,720 bytes at 1080p** and **66,355,200 bytes at 4K**
for two slots, or 8,847,360 / 33,177,600 bytes per slot respectively. Pinned
bytes and CUDA buffer allocation counts have zero delta. Owned pageable
capacity increases by **112 bytes per two-slot instance**, or 56 bytes per slot.

The frozen implementation enlarges the existing two vertical scratch surfaces
per channel to full channel width while retaining full-frame allocation height.
Interlaced horizontal inverse needs full-width, half-height rows; progressive
vertical inverse still needs half-width, full-height rows. This common pitched
allocation preserves both layouts, at the measured additional permanent VRAM
cost. No new buffer object or separate second YUV-frame allocation is created,
but the extra aggregate scratch capacity is substantial and is not described
as a zero-byte change. A future capacity/pitch optimization could investigate
whether original byte capacities can host both layouts with mode-specific
strides. It is outside this algorithm/memory freeze and is not required to
repair a correctness or safety failure.

Source auditing counts nine streams per slot before and after, and 191 versus
197 events per slot: **six additional initialization-time timing events**, no
additional streams. They measure the three channel field kernels. No event or
stream is created per frame. Driver-owned event backing memory and thread
stacks are outside `DecoderMemoryStats`; the report does not substitute owned
buffer accounting for total driver memory.

The three-slot stress measures 246,177,016 owned device bytes, 108,673,104
pinned bytes, 127,907,128 pageable capacity bytes, and 190 initialization-time
CUDA buffer allocation calls. After warm-up the measured C++ heap requests
per frame are **zero**, and owned capacities/allocation counts are stable.
No separate permanent full-frame buffer was added. Memcheck's full leak check finds
zero owned leaks.

## 12. Build, sanitizer, performance, and promotion results

The mandatory commands are retained in raw logs:

```powershell
ctest --test-dir build -C Release --output-on-failure
cmake -B build-cpu -S . -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
```

All four Compute Sanitizer tools run the acceptance target's `--sanitizer`
mode, with `--error-exitcode 99`; memcheck additionally uses `--leak-check full`.
Unattached or intentionally interrupted initial attempts are retained and
are not counted as passes. The final instrumented summaries are authoritative.

The host is Windows x64, MSVC 19.44, CUDA 12.8, NVIDIA RTX 4090 Laptop GPU
(16,376 MiB reported), driver 610.62, Release architecture 89. A fresh isolated
HEAD archive/build supplies the pre-3B-1 baseline. Its compiler identification
needed access to the MSVC PDB service outside the sandbox; no source workaround
or upstream edit was used.

The user reported a brief battery interval. Correctness results remain valid;
that interval's timing observations are excluded from the performance decision.
The final A/B order is baseline/current/baseline/current, with three
hundred-frame repeats per pattern/strategy in each block, after warm-up.
Windows AC line status is required before and after each timed block. Stage
profiling checks AC on every frame. Only these same-session powered runs
support the progressive >5% regression decision. `benchmark_codec` is also
run against its saved historical reference for encoder/CPU context; historical
differences are not the sole decoder performance evidence.

<!-- MEASURED_RESULTS_BEGIN -->

### Final gate result

**Outcome A — PRODUCTION PASS**. Promote GOP-1 interlaced ten-bit YUV422 to the production decoder. All critical correctness/safety gates pass; there are no remaining correctness or lifetime blockers.

| Gate | Passed / total | Failed | Elapsed |
| --- | ---: | ---: | ---: |
| CUDA CTest, Release | 37 / 37 | 0 | 850.47 s |
| CPU-only CTest, Release | 6 / 6 | 0 | 4.56 s |

| Sanitizer | Result | Elapsed |
| --- | --- | ---: |
| memcheck | ERROR SUMMARY: 0 errors; PASS | 74.97 s |
| initcheck | ERROR SUMMARY: 0 errors; PASS | 81.39 s |
| racecheck | RACECHECK SUMMARY: 0 hazards displayed (0 errors, 0 warnings); PASS | 135.44 s |
| synccheck | ERROR SUMMARY: 0 errors; PASS | 9.09 s |

Memcheck additionally reports **0 bytes leaked in 0 allocations**.

### Powered frozen-1080p measurements

| Strategy | Average latency | Min / max latency | FPS | MP/s | Peak process working set |
| --- | ---: | ---: | ---: | ---: | ---: |
| Lightweight | 60.2528 ms | 50.7358 / 138.8220 ms | 16.60 | 34.41 | 205,283,328 bytes |
| Hybrid | 39.1784 ms | 38.3836 / 41.3659 ms | 25.52 | 52.93 | 205,684,736 bytes |
| Auto | 39.2500 ms | 38.6757 / 40.4095 ms | 25.48 | 52.83 | 205,684,736 bytes |

Latency is queue-to-host-observed-ready for a single frame, after twelve warm-ups; 100 timed frames per strategy. The Lightweight maximum is retained rather than trimmed. No comparable pre-phase production interlaced output exists; these values are observations, not a speedup claim. The dense difference/Peak fixture remains an expensive entropy workload; this phase does not tune that existing path.

| Strategy | Field event window, summed over 3 channels | Color event window | Pipeline critical path |
| --- | ---: | ---: | ---: |
| Lightweight | 0.919003 ms | 0.031179 ms | 52.0917 ms |
| Hybrid | 0.194624 ms | 0.072631 ms | 38.8306 ms |
| Auto | 0.160909 ms | 0.121907 ms | 45.8644 ms |

Stage observations use a separate warmed research-pipeline run. Event intervals can include competing-channel scheduling; their summed durations are not exclusive device-kernel execution time and do not sum to wall latency. Field/color work does not dominate total latency. The source introduces no synchronization wait on unrelated decoder instances.

### Same-session progressive comparison

| Resolution | Content | Strategy | Baseline median | Current median | Latency change |
| --- | --- | --- | ---: | ---: | ---: |
| 1920x1080 | flat | Auto | 1.0547 ms | 1.0530 ms | -0.16% |
| 1920x1080 | flat | Lightweight | 1.1036 ms | 1.0684 ms | -3.19% |
| 1920x1080 | flat | Hybrid | 2.4115 ms | 2.3717 ms | -1.65% |
| 1920x1080 | noise | Auto | 4.6986 ms | 4.7564 ms | +1.23% |
| 1920x1080 | noise | Lightweight | 17.1444 ms | 16.9901 ms | -0.90% |
| 1920x1080 | noise | Hybrid | 4.7345 ms | 4.7913 ms | +1.20% |
| 3840x2160 | flat | Auto | 1.6281 ms | 1.6698 ms | +2.56% |
| 3840x2160 | flat | Lightweight | 1.6015 ms | 1.6316 ms | +1.88% |
| 3840x2160 | flat | Hybrid | 3.4460 ms | 3.4289 ms | -0.50% |
| 3840x2160 | noise | Auto | 8.5697 ms | 8.4382 ms | -1.54% |
| 3840x2160 | noise | Lightweight | 67.3076 ms | 67.1017 ms | -0.31% |
| 3840x2160 | noise | Hybrid | 8.5844 ms | 8.4186 ms | -1.93% |

The worst measured median latency change is +2.56%. There are 0 combinations above the 5% gate. All compared sample hashes match and all 144 timed repetition records report AC line status 1 before/after. No Auto tuning or codec optimization is performed.

### Exact owned-memory deltas

| Maximum width, 2 slots | Device before / after | Device delta per slot | Pinned delta | Pageable delta per slot | CUDA allocation-count delta |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920 | 146,473,296 / 164,168,016 bytes | +8,847,360 bytes | +0 bytes | +56 bytes | +0 |
| 3840 | 365,514,576 / 431,869,776 bytes | +33,177,600 bytes | +0 bytes | +56 bytes | +0 |

There are six additional timing events and zero additional streams per slot. Steady-state C++ requests, CUDA buffer allocation, stream creation and event creation are zero per frame. The increased scratch VRAM is the principal remaining resource limitation.

### Reference codec context

| Resolution | Same-session encoder baseline / current | Same-session CPU decoder baseline / current |
| --- | ---: | ---: |
| 1080p | 82.59 / 83.51 FPS | 495.10 / 490.37 FPS |
| 4K_UHD | 23.04 / 23.50 FPS | 130.18 / 129.98 FPS |
| 8K_UHD | 5.70 / 6.32 FPS | 29.41 / 34.27 FPS |

The first 1080p encoder observation was slower than the historical record. An additional same-session run of unchanged pre-phase and current codec sources shows no material negative encoder/CPU throughput delta. The historical comparison is context and is not used to attribute an optimization or decoder regression.

The final completion JSON lists the implementation/test commits. The documentation/evidence commit is the commit containing this report; the final response supplies its hash. Merge recommendation: accept the narrow profile after review; no automatic merge.

<!-- MEASURED_RESULTS_END -->

## 13. Public capability and next phase

`DecoderCapabilities.interlaced_output` is true when the CUDA device backend
is available. Its public header explicitly defines the supported profile as
GOP-1 interlaced YUV422 coded precision 10, and explicitly excludes FIELDPLUS,
GOP-2, RGB and other temporal profiles. CPU-only builds leave it false and
retain a valid public API with `BackendUnavailable` for device decoding.

The updated Phase 3B-0 row is parse/entropy/coefficient/spatial/field/planar/
BGRA PASS for this narrow profile, contingent on the final promotion result
above. All other profile rows remain unchanged. No Phase 3B-0 report is rewritten.

Recommended Phase 3B-2 target: define and research FIELDPLUS/GOP-2 temporal
reconstruction and its dependency/lifetime contract in isolation. It requires
new references and its own acceptance phase; this completion gate adds none
of that production support. Merge remains a separate user decision.

## 14. Changed files and evidence index

Production/research interface changes: `include/hypercine/decoder.h`,
`include/hypercine/decode_color_cuda.h`,
`include/hypercine/research/decode_phase2e.h`, `src/core/decode_parser.cpp`,
`src/core/decoder.cpp`, `src/core/decoder_backend.h`,
`src/cuda/decode_color_cuda.cu`, `src/cuda/decode_inverse_kernels.cuh`,
`src/cuda/decode_inverse_stub.cpp`, `src/cuda/decode_phase2e_pipeline.cu`,
and `src/cuda/decode_phase2e_stub.cpp`.

Test changes: `tests/CMakeLists.txt`, `tests/test_decoder.cpp`,
`tests/test_decode_phase3b0.cpp`, the preserved frozen smoke in
`tests/test_decode_phase3b1.cpp`, and the new permanent
`tests/test_decode_phase3b1_acceptance.cpp`.

Measurement/evidence tooling: `benchmarks/CMakeLists.txt`,
`benchmarks/benchmark_decode_phase3b1_progressive.cpp`,
`benchmarks/benchmark_decode_phase3b1_stages.cpp`, and
`benchmarks/summarize_decode_phase3b1.py`. Documentation: this report and
`docs/UPSTREAM.md`.

Machine-readable artifacts under `benchmarks/results/`:

- `decode_phase3b1_correctness.json`: frozen references, coefficients, LL,
  horizontal/field stages, signed planes, BGRA and direct kernel regressions.
- `decode_phase3b1_patterns.json`: every source/sample identity, pattern,
  quality, geometry, difference and genuine Peak coverage.
- `decode_phase3b1_matrix_range.json`: all 1,952 matrix/range/orientation rows.
- `decode_phase3b1_strategy_matrix.json`: independent requested/actual strategies.
- `decode_phase3b1_rejections.json`: full supported/unsupported profile table
  and malformed input/error/publication evidence.
- `decode_phase3b1_progressive_regression.json`: eleven original progressive
  cases and Phase 3A-E real signedness stage results.
- `decode_phase3b1_stress.json`: repeated coefficients/planes/BGRA, 5,000
  mixed frames, slots, release orders, lifetime and two/four instances.
- `decode_phase3b1_memory.json`: exact measured before/after owned-memory deltas
  and audited stream/event counts.
- `decode_phase3b1_performance.json` and `decode_phase3b1_stage_performance.json`:
  powered same-session latencies/FPS, stage times, memory and baseline comparison.
- `decode_phase3b1_sanitizers.json`: final four instrumented summaries and logs.
- `decode_phase3b1_completion.json`: gate outcome, scope, commits, limitations
  and promotion/next-phase recommendation.

Raw final CUDA/CPU CTest, focused variable-release CTest, initial interrupted
CTest, all sanitizer stdout/tool logs, real signedness audit, frozen initial
reference log, and initial parser failure evidence are retained. The original
implementation snapshot and bulky intermediate generated samples remain in
ignored build storage. All new source and evidence are MIT OR Apache-2.0.
