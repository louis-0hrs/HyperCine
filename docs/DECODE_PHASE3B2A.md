# Decode Phase 3B-2A — Progressive FIELDPLUS/GOP-2

Dual-licensed under MIT OR Apache-2.0.

## Result and scope

This branch adds a CUDA production path for progressive FIELDPLUS / GOP-2, YUV422, coded precision 10. A request selects exactly one of the two images with `DecodeRequest::frame_index`. Interlaced FIELDPLUS remains unsupported. GOP-1 parsing and reconstruction remain on their existing path.

The implementation is exact for the frozen canonical 64×64 sample, the temporal-asymmetric and motion probes, the chroma-temporal probe, and the frozen 1920×1080 FilmScan3 sample covered by the new reconstruction test. It is not ready for production promotion: the full CUDA and CPU-only suites pass, and the mixed 5,000-output stress, FIELDPLUS lifetime, slot-mode, and multi-instance gates pass. Sanitizer, performance, 4K smoke, and 4K memory gates remain open. A partial 1920×1080 slot-memory measurement is recorded; 4K memory and stream/event deltas remain unmeasured. See the JSON result files under `benchmarks/results/` for per-gate state. Do not interpret unrun gates as passes.

## 1. Phase 3B-2R baseline

The frozen research baseline is the `decode-phase3b2r` line (base `f1d0fe4`, research commits `85c4a51`, `9acdd75`, `e493bc4`, `f433a74`). The active branch is `decode-phase3b2a`, based at `f433a74`; this work is currently uncommitted. The encoder was not changed. `upstream/` remains pristine, and `job.txt` remains a local untracked file.

## 2. Supported production profile

The parser accepts only progressive FIELDPLUS transform type 2 with two frames, three YUV422 channels, p10, six wavelets, 17 logical coefficient subbands per channel, 54 physical chunks per sample, and the supported geometry policy. It continues to reject interlaced FIELDPLUS, other transforms, unsupported precision/channel/format combinations, malformed metadata, and oversized samples.

## 3. Parser and control chunks

`FieldPlusSampleDescriptor` is separate from the GOP-1 30-band descriptor. It contains 51 coefficient-band descriptors and 54 physical-chunk descriptors, each with payload bounds, codebook, Peak/difference metadata, geometry, ownership, role, and frame association.

Each channel maps logical subbands 0–10 to physical chunks 0–10, physical 11 to the control record, and logical subbands 11–16 to physical 12–17. The three control records are exactly 8 bytes, have no coefficient payload, target temporal wavelet 2 band 1, and never enter entropy decode. Parser tests cover ordering, ownership, count, bounds, control uniqueness/position/target, truncation, and invalid profile headers.

## 4. Wavelets, channels, and geometry

The frozen ownership map is logical bands 0–3 → w5; 4–6 → w4; 7–10 → w3; 11–13 → w1; 14–16 → w0. w0 and w1 are the frame 0 and frame 1 base transforms; w2 carries temporal low/high; w3 spatially reconstructs temporal high; w4 and w5 reconstruct temporal low.

Encoded channel order is Y,V,U. The public device planar order remains Y,U,V, with the existing V/U remap retained. For 1920×1080, luma w0/w1/w2 are 960×540, w3/w4 are 480×270, and w5 is 240×135; chroma widths are half of luma, with the same heights.

The existing VLC/FSM, checkpoint, payload framing, and applicable Peak machinery are reused. No second entropy decoder was added. Difference flags and Peak metadata are retained in descriptors; no progressive FIELDPLUS Peak table was present in the tested corpus.

## 5. Reconstruction DAG and temporal arithmetic

The CUDA path decodes the bands, inverses w3 for temporal high, inverses w5 then w4 for temporal low, applies the GOP temporal inverse, then inverses only the selected frame's w0 or w1 base transform. The requested output surface is allocated per slot; the other frame's output is not materialized. Frame requests decode independently; no shared GOP cache or hidden next-frame state is used.

The GPU vector-prefix rule is `frame0 = arshift1(sat16(low - high))` and `frame1 = arshift1(sat16(low + high))`. The upstream scalar tail on the validated Windows/MSVC target also uses arithmetic right shift, but operates on the unsaturated 32-bit sum. Direct upstream vectors and CUDA vectors cover positive/negative values, saturation boundaries, odd values, extremes, and widths spanning all legal modulo-80 classes. This runtime/source audit disproved the task’s tentative `/2` tail hypothesis; the implementation follows observed upstream output. At the 1920-pixel profile, temporal widths 960 and 480 are both divisible by 40, so the tested production geometry has no scalar tail in the selected 40-wide path. Other legal widths can have a tail.

The new path uses asynchronous CUDA work and slot events; it adds no global device synchronization. Stage arrays are available through research-only inspector hooks. Per-stage timing instrumentation for FIELDPLUS is not yet available.

## 6. Frame-index API, timestamps, and tickets

`DecodeRequest::frame_index` defaults to 0. FIELDPLUS accepts 0 or 1 and rejects 2 or higher. GOP-1 accepts 0 and rejects 1; negative indices are invalid requests. A ticket owns one requested output. Frame index selects image content only; HyperCine does not invent PTS/DTS. The container or host remains responsible for timestamp association. Frame 1 can be requested directly from one independently decodable sample.

## 7. Exactness and references

The new reconstruction test captures upstream coefficient arrays and intermediate low/high/base-LL stages, compares the reconstructed GPU stages exactly, and derives signed planar references by invoking the frozen upstream spatial inverse on aligned, captured coefficient/base-LL arrays. The upstream YU64 comparison is kept separate because upstream clamps to 10-bit during some packed output paths while public signed planes preserve the raw signed inverse result. After warm-up, 400 alternating frame requests recorded zero C++ allocations and no CUDA allocation, device-byte, pinned-byte, or pageable-capacity delta.

Canonical 64×64 BGRA hashes are frame 0 `27fd4d3948775018` and frame 1 `c06ad9438de588c8`. The 1920×1080 FilmScan3 motion sample hashes are `7f633a5358642abd` and `6d174c1993f61015`. Source byte counts and FNV-1a sample hashes are checked before decoding. The tests also validate the frozen temporal-asymmetric, motion, and chroma-temporal 64×64 probes, frame ordering, repeated alternating frame selection, top-down and bottom-up output, signed Y/U/V planes, and packed upstream YU64 values.

The GPU uses the existing progressive color conversion route. Exact BGRA fixture hashes pass. BT.601/709 × CG/VS cross-product validation, an independent lowpass-offset report, and a broad quality/pattern matrix are not complete as separate Phase 3B-2A production gates.

## 8. Strategy and capability policy

FIELDPLUS is dispatched to a fixed Hybrid route. The GOP-1 Auto classifier is bypassed because its feature model assumes 30 bands. Explicit diagnostic strategy overrides for FIELDPLUS are rejected as `UnsupportedEncodingMode`; GOP-1 overrides retain their existing behavior. `DecoderCapabilities::progressive_fieldplus_gop2` is true only when the CUDA device backend is available. The CPU-only build and API tests pass; production FIELDPLUS decoding remains unavailable there, as designed.

## 9. Rejections and backward compatibility

The production API test verifies that interlaced FIELDPLUS gets a failed ticket with `UnsupportedEncodingMode`, oversized input is rejected as `SampleTooLarge`, and invalid frame indices are rejected. Parser mutation tests cover malformed FIELDPLUS. The GOP-1 regression suite keeps progressive and interlaced YUV422 p10 on the prior path; its existing exactness and 5,000-frame GOP-1 stress tests are part of CTest.

## 10. Verification status

The five focused `test_decode_phase3b2a_*` tests passed on CUDA. The first full run had 41/44 pass; three failures exposed per-call GOP-1 parser-error allocation and an obsolete test expectation that progressive FIELDPLUS remained unsupported. Both were corrected, and the three formerly failing tests passed when rerun. A later FIELDPLUS allocation gate found and fixed one temporary checkpoint-vector allocation by reusing slot scratch. The complete CUDA CTest run after the subsequent FIELDPLUS band-scratch bounds fix and expanded stress corpus passed 44/44 tests in 773.83 seconds, with zero skipped. The Issue #2 run passed 44/44 in 743.71 seconds. This continuation's first full run hit a transient `test_decode_phase3b0` heap-corruption exit; the isolated test passed, and the complete retry passed 44/44 in 749.68 seconds, with zero skipped. Phase2FA passed in 460.40 seconds on that run. The expanded mixed stress gate passed 5,000/5,000 exact output tickets across progressive/interlaced GOP-1 and both FIELDPLUS frames, multiple patterns, and Low/High/FilmScan 3 GOP-1 qualities, with alternating FIFO/reverse release and zero steady-state allocations.

The CPU-only configuration built and passed 7/7 CTest tests. FIELDPLUS parsing and the public API compile there; production decoding remains unavailable without CUDA, as designed. Canonical frame 0 and 1 each passed 200 exact repetitions; both 1080 frames passed 100 exact repetitions. Compute Sanitizer 2025.1.0.0 could not attach to either the production or minimal CUDA target; no instrumented run occurred. Memcheck/leak checking are blocked, and initcheck/racecheck/synccheck remain unrun. The 4K sample and both frames are exact; 1080p end-to-end latency distributions and 4K/1080p memory are measured. Stream/event deltas are zero by source call-site comparison. FIELDPLUS per-stage timing is unmeasured. The same-session GOP-1 comparison has one 1080p flat Automatic case at +5.14%, just above the 5% threshold. Matrix/range cross-product passed for both frames; lowpass coefficient counterfactuals discriminate the BGRA and signed-planar biases. The 5,000 mixed outputs contained 147 of each FIELDPLUS frame; the remaining GOP-1 cases varied progressive/interlaced mode, patterns, and Low/High/FilmScan 3 quality. Two- and four-instance workloads, delayed/non-FIFO release, held outputs, consumer-stream waits, failed-ticket coexistence, drain/destruction, and slot modes 1, 2, default, and 4 passed. A warmed 1920×1080 measurement recorded 85,191,172 device bytes per slot (170,532,360 total including 150,016 immutable bytes) and 36,259,276 pinned bytes per slot (72,518,552 total); pageable capacity was 85,510,728 bytes total after correcting per-slot scratch for all 36 FIELDPLUS VLC bands. The measurement exercised two slots and observed 152 CUDA allocation calls after warm-up, with zero steady-state allocation deltas across the 400 alternating requests.

## 11. Limitations and Phase 3B-2B recommendation

Production promotion is on hold until the missing validation gates are run and documented. The present implementation does not support interlaced FIELDPLUS, GPU strategies other than fixed Hybrid for FIELDPLUS, shared frame-0/frame-1 intermediate caching, or FIELDPLUS per-stage timing reports. After Phase 3B-2A gates are closed, Phase 3B-2B can add interlaced FIELDPLUS as a separate profile with its own frozen references and rejection-to-support tests; it should not weaken the progressive path or reuse unsupported GOP-1 classifier assumptions.

## 12. Issue #2 qualification follow-up (2026-10-14)

**Outcome D — qualification infrastructure blocker. Production promotion is not recommended.** The production path remains exact on the frozen 64×64 and 1920×1080 frame-0/frame-1 references, and the existing CUDA/CPU CTest, determinism, mixed stress, slot-reuse, lifetime, and multi-instance evidence remains valid. This outcome does not claim sanitizer safety or close the 4K, resource-accounting, performance, matrix/range, or lowpass-offset gates.

Compute Sanitizer 2025.1.0.0 (build 35351055) on Windows WDDM with CUDA Toolkit 12.8 and an NVIDIA GeForce RTX 4090 (driver 610.62) could not attach to either the production reconstruction test or the minimal CUDA temporal-kernel test. The direct production command was:

```text
compute-sanitizer --tool memcheck --leak-check full --error-exitcode 99 build/tests/Release/test_decode_phase3b2a_reconstruction.exe .
```

It reported `No attachable process found` and timed out before test execution. The minimal target command was:

```text
compute-sanitizer --tool memcheck --launch-timeout 15 --error-exitcode 99 build/tests/Release/test_decode_phase3b2a_temporal.exe
```

It failed with the same attach error, while the executable run natively passed its CUDA temporal-vector test. Therefore memcheck, leak checking, initcheck, racecheck, and synccheck remain unvalidated; none is reported as a pass. Exact command and environment evidence is in `benchmarks/results/decode_phase3b2a_sanitizers.json`.

The deterministic 4K smoke now passes using `build/tests/Release/test_decode_phase3b2a_reconstruction.exe --4k-smoke . benchmarks/results/decode_phase3b2a_4k.json`. Its 3840×2160 FilmScan3 progressive FIELDPLUS sample has source FNV-1a `d75570406b9b8381`, compressed sample FNV-1a `cc0bd067610e1d02`, and size 7,584,404 bytes. Upstream BGRA hashes are `8c054bd19cd88cd6` (frame 0) and `82110fddd98f60c8` (frame 1); HyperCine matches both exactly. The fixture is temporally asymmetric. Full generated-sample and memory evidence is in `benchmarks/results/decode_phase3b2a_4k.json`.

The 4K decoder memory measurement used two slots: device bytes total 456,896,520 (228,373,252 per slot, including 150,016 shared immutable bytes), pinned bytes total 72,518,552 (36,259,276 per slot), and pageable owned capacity 85,510,728. The first 4K decode grew the initialized pool by 25,026,744 device bytes and 69,816 pinned bytes across 25 CUDA allocation calls; the subsequent frame-0-to-frame-1 steady-state delta was zero. The same memory JSON records the 1080p final values.

The stream/event source audit found 13 streams and 197 events per decoder slot, with no global owned stream/event handles. The per-slot breakdown is 1 copy + 8 replay + 3 channel + 1 main stream, and 3 channel-done + 24 inverse/field + 5 frame/transfer/lowpass + 162 per-band + 3 color/ready events. For the default two-slot instance this is 26 streams and 394 events; 3B-2A adds zero streams and zero events compared with base `f433a74`. Creation loops check all handles on successful initialization, and `DecodeSlot2E::Release` destroys each owned handle. These are exact source-level create/destroy counts; the public diagnostics do not expose an independent runtime handle counter.

The frozen 1080p sample was timed with `build/tests/Release/test_decode_phase3b2a_reconstruction.exe --1080-performance . benchmarks/results/decode_phase3b2a_performance.json` for 120 requests per frame after five warmups. Frame 0 measured mean 11.1203 ms, median 10.8342 ms, p95 12.1014 ms, min/max 10.2912/12.4004 ms, standard deviation 0.640219 ms, and 89.9254 FPS-equivalent. Frame 1 measured mean 11.4790 ms, median 11.6422 ms, p95 12.3695 ms, min/max 10.3890/13.2512 ms, standard deviation 0.676141 ms, and 87.1154 FPS-equivalent. The timing includes queue/decode, device-to-host copy, hash verification, and release; it validates exact output on every iteration. It is end-to-end latency, not per-stage kernel timing. The same-session GOP-1 A/B benchmark is still open.

The matrix/range cross-product now passes: `--matrix-range` compared both FIELDPLUS frames in BGRA against upstream for BT.601/BT.709 × CG/VS, with exact FNV-1a hash equality in all eight cases. The invocation and per-case hashes are in `benchmarks/results/decode_phase3b2a_matrix_range.json`.

The canonical 64×64 fixture also discriminates both format-specific base-LL offsets. Removing the added +24 BGRA offset changes 128 coefficients; removing the +10 signed-planar offset changes 128 coefficients. The production GPU coefficients, temporal stages, and reconstructed output remain exact against upstream for both formats. Evidence is in `benchmarks/results/decode_phase3b2a_lowpass_offsets.json`; the counterfactual was computed against frozen upstream coefficients rather than a separate binary built with offsets disabled.

The same-session GOP-1 A-B-B-A comparison ran the baseline and current progressive benchmark binaries in one environment, with six 100-request measurements per case and build. Eleven of twelve case medians are within 5%. The 1920×1080 flat Automatic case is +5.14% (about 0.05 ms absolute); this single threshold miss remains unresolved, so the GOP-1 performance gate is partial. Full case data and raw runs are in `benchmarks/results/decode_phase3b2a_gop1_regression.json` and the four referenced raw JSON files.

Still open: run memcheck/leak checking, initcheck, racecheck, and synccheck on a platform where Compute Sanitizer can attach; add stage-level timing; and resolve the one GOP-1 performance case above the 5% threshold. The detailed per-gate files remain explicit about what is partial or not run.

## 14. Issue #13 Windows host instrumentation and Compute Sanitizer evidence (2026-10-16)

The Windows part of issue #13 is complete. Host AddressSanitizer (MSVC 19.44) ran `test_decode_phase3b0` 20 times at 297 cases per run with exit code 0 and zero reports, so the historical heap-corruption exit did not reproduce. The only reproducible host ASan finding is an upstream oracle read-ahead: `InvertTemporalQuant16s` (`upstream/Codec/temporal.c:9606`) reads 16 bytes past a plane allocation for the 1920×1080 and 4K FIELDPLUS captures into a YU64 target. It is a read inside the frozen reference decoder, it does not change the frozen reference hashes in a non-ASan Release build, and up to 64×64 the path stays inside its allocation.

Compute Sanitizer 2025.1.0.0 now attaches on this machine. Because this build cannot enumerate kernel launches, a diagnostics-only control binary (`tests/sanitizer_launch_control.cu`) demonstrates that device code is instrumented and that a `--kernel-name` filter selects which kernels are checked. With that control, memcheck plus leak checking, initcheck, racecheck and synccheck report zero findings on the temporal and production reconstruction targets; racecheck/synccheck on the full production target cover the first 400 kernel launches each, and the target verified its hashes and 400-request alternation inside the instrumented process. The same four tools report zero findings on the mixed `test_decode_phase3b1_acceptance --sanitizer` profile, which exercises GOP-1 interlaced, FIELDPLUS frames 0/1, a 44-frame mixed sequence, 1/2/Auto/4 slots, two and four decoder instances, lifetime handling and malformed rejection in one instrumented process.

The #13 safety gate stays HOLD: PR #52 still has not integrated the native A2 test patch #80. Details, exact commands and sanitized logs are in [DECODE_PHASE3B2A_ISSUE13_WINDOWS.md](DECODE_PHASE3B2A_ISSUE13_WINDOWS.md) and `benchmarks/results/decode_phase3b2a_issue13_windows_20261016/`.
