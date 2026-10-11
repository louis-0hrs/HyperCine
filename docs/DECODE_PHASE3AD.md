# Decode Phase 3A-D dispatch research

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

**Promotion gate: HOLD.** Do not merge. The frozen candidate is the existing Phase 3A classifier. It captured 29.5 percent of the Oracle improvement on the validation split and 14.4 percent on the untouched hold-out. Neither clears the 90 percent minimum. Production decoder sources were not changed.

Branch `decode-phase3ad`. Research base is Phase 3A-RC `0516b90143de694906058bbfa99f7c743540759c`. Decoder sources still match Phase 3A `e6e9cb905aee848808682942d13f07f2ff505a2d`. `main` was not moved. Nothing was merged, tagged, or pushed.

## 1. Why Phase 3A-RC failed promotion

Phase 3A-RC held the decoder. The release candidate failed only because Auto dispatch did not generalize: 23 of 35 Oracle matches, 26.79 percent of the Oracle improvement captured, mean regret 0.871 ms, and a worst regret of 5.378 ms on `1920_text_ui`. Every material miss chose Hybrid when Lightweight was faster. Decoder correctness on that corpus was exact. This phase asked whether cheap, already-parsed frame metadata can predict the faster of Lightweight and Hybrid on progressive YUV 4:2:2 frames the rule has not been fit to.

## 2. Split methodology

Assignment date 2026-10-11. The process string is `kSplitProcess` in `benchmarks/benchmark_decode_phase3ad.cpp`, committed in `04f7ea9` before any timing file existed.

| Split | Role |
| --- | --- |
| Development | Fit rules, tree depth, and linear hyperparameters. Includes every supported Phase 3A and Phase 3A-RC case. |
| Validation | Compared model classes once. It could influence which class was frozen. It was not the final gate by itself. |
| Hold-out | Created and measured only after the candidate, features, and constants were frozen and committed. |

Leave-one-family-out on development groups by the family string, so a 1080p frame and its 4K sibling stay together. The Phase 3A-RC cases are seen development data. They are not called hold-out and they are not called generalization.

One validation limitation was left as assigned: `photo_grass` uses the family name `grass`, which is also a development generator family, so the novel-family guard does not treat that photograph as novel. Hold-out photographs use a `holdout_` family prefix.

## 3. Corpus provenance

Synthetic frames are labeled with their generator. Photographs are real images re-encoded to CineForm. They are not native camera CineForm.

Development and validation, 116 rows, 114 timed:

- 69 development, 45 validation.
- 6 re-encoded CC0 validation photographs. Clouds and textile searches returned HTTP 429 and were not in the validation manifest.
- 108 other timed frames are synthetic or the bundled progressive YUV 4:2:2 samples.
- `CFHDsample444.AVI:frame0` is an unsupported RGB control.
- `p3a_7680_noise` parses, but the sample is 75,620,560 bytes. `QueueDecode` rejects samples above 32 MiB. The first measurement attempt aborted there. The rerun records the case as unsupported. The decoder limit was not raised. That file is the one the analyzer fit.

Hold-out, measured once in `benchmarks/results/decode_phase3ad_holdout.json`: 42 timed frames, 32 from generator `holdout_v2` and 10 re-encoded CC0 photographs. No hold-out frame exceeded 32 MiB. Seven photograph originals were rate-limited; those files were fetched as 1920-pixel thumbnails of the same Commons titles. The manifest records that. Earlier aborted hold-out processes did not write this file and were not given to the analyzer.

Seeds: Phase 3A `0x1337BEEF`, RC proxy `0x6A09E667`, research `0x3AD5EED1`, alternate `0x3AD5EED2`, hold-out generator `0xC0FFEE31`, photograph encode `0x51A77`.

## 4. Oracle

On each frame, one session measures forced Lightweight and forced Hybrid. Five warmups, then 120, 60, or 30 frames at 1080p, 4K, or 8K. Two slots, one outstanding ticket, device-resident queue through release. The Lightweight/Hybrid order alternates by case. Oracle is Lightweight when its mean is strictly lower, otherwise Hybrid. `delta_ms` is Hybrid mean minus Lightweight mean. Regret is the chosen mean minus the faster mean. Relative regret divides by the faster mean.

A miss is catastrophic when regret exceeds 2 ms or relative regret exceeds 20 percent. Near-ties are reported and do not change the objective. The objective is the unweighted sum of the chosen means. Capture is `(min(sum L, sum H) - sum selected) / (min(sum L, sum H) - sum Oracle)`.

## 5. Feature research

`ComputePhase3adFeatures` reads the parsed highpass band descriptors only. There is no pixel read, no entropy pre-scan, and no runtime model. Eligible features are resolution-normalized shares, bytes per coefficient, level and channel ratios, dense-band counts, inequality (Gini, HHI, MAD, CV), and existing quantizer and codebook fields. Absolute payload thresholds, width, height, and total payload are shadow fields. A shadow tree may use them and is marked ineligible for production.

## 6. Feature statistics

The eligible feature table for every timed development and validation frame is `benchmarks/results/decode_phase3ad_features.json`. Band bytes, coefficient counts, quantization, and codebook ids are in the measurement file. The analyzer checks `top1_share`, `total_payload`, Gini, and the baseline decision against the raw bands before fitting.

## 7. Baseline classifier

`ClassifyDecodeFrame` is unchanged. Hybrid is chosen when the frame is not concentrated and it is either dense or distributed.

- Dense band: payload at least 32,768 bytes and `payload * 100 >= coefficients * 105`.
- Concentrated: level-1 bytes times 100 is at least 75 percent of the highpass total.
- Distributed: highpass total at least 262,144 and the largest band at least 65,536.

The arithmetic is integer. The function writes a caller-provided `DecodeFrameInfo` and allocates nothing else. It does not read clocks, previous frames, or pixels.

## 8. Candidate classifiers

Compared on development, then scored once on validation:

| Class | Eligible | Validation capture | Validation regret | Notes |
| --- | --- | --- | --- | --- |
| Baseline | yes | 29.54% | 25.938 ms | Complexity 0. Selected. |
| Hand rule r3 `(100, 250, 200)` on active bytes/coeff, HHI, chroma share | no | 18.8% | 29.891 ms | Failed the 4K guard. |
| Tree depth 2, minimum leaf 12, features `bpco_y` and `l0_over_l1` | yes | 15.55% | 31.084 ms | Development capture 79.97%. Worst validation regret 16.002 ms. |
| Weighted logistic, L2 0.001 | no | 4.28% | 35.233 ms | Novel-family guard failed. Fixed-point disagreed on 1 frame. |
| 27-cell bins on active bytes/coeff, HHI, chroma share | no | 4.06% | 35.313 ms | Development capture 85.53%. Failed the 4K guard. |
| Shadow tree using absolute band sizes | no | 40.27% | 21.986 ms | Best validation number. Ineligible by the predeclared rule. |

Rule grids were the committed 320, 100, and 80 settings. Tree depth 2 through 6 and minimum leaf 4, 8, and 12 were chosen by development leave-one-family-out regret, then refit. Linear weights were standardized on development only. The simplicity rule keeps a model within `max(2 ms, 2 percent)` of the best eligible regret when it is simpler. Baseline had the lowest eligible regret, so nothing else was eligible to replace it.

## 9. Cross-validation

Development leave-one-family-out total regret was 21.962 ms for the baseline and 20.837 ms for the chosen tree. That small development gain did not survive validation.

The predeclared 1080p-only refit, scored on validation frames at least 3840 wide (16 frames): baseline capture 31.6 percent, regret 7.968 ms. The tree's 1080p refit reached 80.8 percent capture on that slice and still lost on full validation regret. Novel validation families (17 frames): baseline mean regret 1.173 ms. Training without 8K was reported and was not a promotion gate. Details are in `decode_phase3ad_cross_validation.json`.

## 10. Regret-weighted evaluation

Accuracy is secondary. On validation the baseline matches 36 of 45 frames and still leaves 25.938 ms of regret because the misses are the large ones. The tree matches 40 of 45 and has more regret, including a 16 ms miss. Bins look strong on development (7.296 ms regret) and fail on validation (35.313 ms).

## 11. Selected model

`phase3ad_baseline`. Frozen in `59631d4` before any hold-out image was downloaded and before `--holdout` was run. `satisfactory_for_production_change` is false: capture is below 90 percent, and the selected class is the baseline, so the 20 percent regret-reduction bar is not met. `ClassifyDecodeFrame` was not edited.

## 12. Selected features

The production rule still uses highpass total, level-1 share, largest band size, and the dense-band count. No new feature was promoted. The tree that fit development used only `bpco_y` and `l0_over_l1`. It was not promoted.

## 13. Frozen constants

The frozen include names `phase3ad_baseline`. Hold-out dispatch calls `ClassifyDecodeFrame` rather than the include's reimplementation, because the name contains `baseline`. The include's integer form matched that call on all 42 hold-out frames for 100 passes. Thresholds remain 32768, 105/100, 75/100, 262144, and 65536.

## 14. Production implementation

None. Entropy, inverse wavelet, Hybrid band policy, Lightweight, DeviceOutput, tickets, slots, streams, the malformed parser, Peak-Escape, interlaced policy, and output formats are unchanged. Forced Lightweight, Hybrid, and HostStreaming overrides still apply after classification.

## 15. Old RC corpus, not hold-out

The 35 supported RC frames were remeasured in the development session with the same unchanged classifier. This is not the RC promotion window and it is not hold-out.

Capture 36.23 percent. Total regret 17.831 ms. Mean 0.509 ms. Worst 5.336 ms on `1920_architecture_graphics` (relative 124 percent). Matches 28 of 35. Seven misses, all Hybrid while Lightweight was faster:

| Frame | Regret | Relative |
| --- | --- | --- |
| `1920_architecture_graphics` | 5.336 ms | 124% |
| `1920_text_ui` | 4.393 ms | 76% |
| `1920_mixed_detail` | 3.002 ms | 31% |
| `3840_clean_sky` | 2.023 ms | 49% |
| `3840_curved_gradient` | 1.847 ms | 43% |
| `3840_architecture_graphics` | 1.008 ms | 8.2% |
| `3840_cgi_geometry` | 0.222 ms | 0.8% |

The original RC report remains the promotion record for that corpus. These are same-decoder, different-session numbers.

## 16. Validation result

45 frames. Capture 29.54 percent. Total regret 25.938 ms. Mean 0.576 ms. Median 0. p95 3.219 ms. Worst 5.587 ms. Matches 36. Misses 9. Catastrophic 7.

Every validation miss chose Hybrid when Lightweight was faster:

| Frame | Regret |
| --- | --- |
| `r1v_1920_halftone_filmscan1` | 5.587 ms |
| `r1v_1920_halftone_filmscan3` | 5.494 ms |
| `r1v_1920_window_blinds_filmscan3` | 3.044 ms |
| `r1_3840_high_contrast_filmscan3` | 3.219 ms |
| `r1b_1920_high_contrast_filmscan3` | 2.777 ms |
| `r1v_3840_window_blinds_filmscan3` | 2.514 ms |
| `r1v_3840_halftone_filmscan3` | 2.236 ms |
| `photo_skin` | 0.699 ms |
| `photo_city` | 0.369 ms |

Development fit of the same rule: 69 frames, capture 56.44 percent, regret 21.962 ms, mean 0.318 ms, median 0, p95 2.977 ms, worst 5.336 ms, 59 matches, 6 catastrophic.

## 17. Final hold-out result

One completed file, candidate `phase3ad_baseline`, status PASS, 42 of 42 supported. Capture **14.39 percent**. Total regret 32.091 ms. Mean 0.764 ms. Median 0. p95 3.657 ms. Worst **4.854 ms** on `h2_1920_night_traffic_filmscan1` (relative 104 percent). Matches 28. Misses 14. Catastrophic 10.

All 14 misses chose Hybrid when Lightweight was faster. The catastrophic frames are:

| Frame | Regret | Relative |
| --- | --- | --- |
| `h2_1920_night_traffic_filmscan1` | 4.854 ms | 104% |
| `h2_1920_circuit_board_filmscan1` | 3.812 ms | 101% |
| `h2_1920_handwriting_filmscan3` | 3.657 ms | 107% |
| `h2_1920_night_traffic_filmscan3` | 3.451 ms | 44% |
| `h2_1920_circuit_board_filmscan3` | 2.639 ms | 73% |
| `h2_1920_brick_masonry_filmscan1` | 2.633 ms | 75% |
| `h2_3840_handwriting_filmscan3` | 2.626 ms | 34% |
| `h2_3840_circuit_board_filmscan3` | 1.895 ms | 22% |
| `h2_1920_brick_masonry_filmscan3` | 1.719 ms | 39% |
| `photo_holdout_clouds` | 1.929 ms | 52% |

The other misses are `h2_3840_brick_masonry_filmscan3` (1.172 ms, 11 percent), `h2_3840_night_traffic_filmscan3` (1.349 ms, 6.5 percent), `photo_holdout_foliage` (0.258 ms, 4.8 percent), and `h2_1920_stained_glass_filmscan1` (0.097 ms, 2.4 percent).

Near-ties did not dominate the sum. Two frames differ by less than 0.25 ms. Three differ by less than 5 percent. The regret sum is carried by the Hybrid-on-sparse-structure misses, the same pattern as Phase 3A-RC.

The hold-out file was not read by the analyzer. No threshold was edited after these numbers existed.

## 18. Classifier runtime

The harness times 100 back-to-back `ClassifyDecodeFrame` calls per frame and records the batch mean. It does not store a per-call minimum, median, p95, or maximum. Development/validation batch mean: **66.4 ns**. Hold-out batch mean: **53.1 ns**. Both are under the 1 µs preference and far under 10 µs. The call allocates nothing beyond the stack `DecodeFrameInfo` and the band vector the parser already owns.

## 19. Determinism

Every hold-out frame repeated classification 100 times. The frozen include matched `ClassifyDecodeFrame` on all 42 frames. The same inputs produce the same strategy. There is no timing input and no frame history.

## 20. Correctness

Production bytes did not change, so the Phase 3A-RC golden result still applies: 11 of 11 frozen golden samples exact, and strategy does not change those pixels.

On the new hold-out, 39 of 42 frames matched the CPU reference in all 30 coefficients, both LL levels, signed planar samples, YU64, and Device BGRA, for Lightweight, Hybrid, Auto, and HostStreaming.

Three re-encoded photographs failed the Lightweight BGRA check after coefficients and both LL levels had already matched. Hybrid, Auto, and HostStreaming were not reached for those three frames, because the checker stopped at the first mismatch. Alpha matched. The RGB differences are sparse full-scale misses, not a one-code rounding error:

| Frame | Pixels | First GPU / reference BGRA | CPU plane Y,U,V |
| --- | --- | --- | --- |
| `photo_holdout_brick` | 447 | 255,255,255,255 / 0,0,5,255 | 240, 565, 455 |
| `photo_holdout_grass` | 93 | 255,255,255,255 / 0,2,0,255 | 566, 532, 382 |
| `photo_holdout_handwriting` | 33 | 255,255,255,255 / 4,1,0,255 | 812, 533, 243 |

The CPU reconstruction at the first brick pixel is dark, and the reference BGRA is dark, while the GPU BGRA pixel is white. That is a decoder output difference, not a dispatch difference. This phase did not modify the color converter or the inverse.

A test-helper fix was required before the hold-out could get past synthetic frames. `ConvertPlanarToYU64` used to shift a 10-bit sample and then clamp to 16 bits, so code 1024 became 65535. Upstream saturates to 1023 and then shifts, which is 65472. The helper now saturates to the active precision first. On `h2_1920_night_traffic_filmscan1` that was 60 words, maximum absolute difference 63, and it was not a coefficient or BGRA miss. The decoder was not changed.

## 21. Noise investigation

Same-session forced Hybrid on the source-equivalent decoder, compared with the recorded Phase 3A Hybrid numbers:

| Measure | This session | Phase 3A | Change |
| --- | --- | --- | --- |
| 1080p Noise Hybrid | 4.914 ms | 4.593 ms | +7.0% |
| 4K Noise Hybrid | 8.477 ms | 8.297 ms | +2.2% |
| Sustained 4K Noise, 1000 frames, 3 slots | 129.65 fps | 132.33 fps | −2.0% |

The RC Auto gaps (about +10 percent, +18 percent, and −17 percent throughput) were not reproduced. `git diff` of `src/` and `include/` against `e6e9cb9` is empty. During the run the GPU moved from idle 1455 MHz to 1995 MHz of a 3105 MHz maximum, about 52 to 84 percent utilization, 38 to 55 W, 56 to 60 °C, and at most 778 MiB. No other compute process was listed. The Windows power scheme was Turbo. The conclusion is benchmark variance. Hybrid kernels were not modified, and the numbers were not used to choose a classifier.

## 22. Sanitizer and build results

No production source change, so initcheck, memcheck, racecheck, and synccheck were not repeated. The Phase 3A-RC record remains: CUDA CTest 32/32, CPU-only CTest 6/6, and the four sanitizer logs for those bytes. `benchmarks/results/decode_phase3ad_sanitizers.json` says that explicitly. After the YU64 helper change, `test_decoder` passed in 16.70 s and `test_decode_phase1` passed in 1.87 s. The full CUDA and CPU-only suites were not repeated.

Phase 3B stays blocked. No interlaced inverse, RGB444, RGBA4444, 8-bit or 12-bit expansion, or graphics interop was added.

## 23. Promotion recommendation

**HOLD.** Do not merge.

Metadata-only rules that fit the seen frames did not predict the faster architecture on the frames held out from fitting. The best eligible validation model is the current rule, and that rule captures 14.4 percent of the available hold-out improvement. The shadow model that was allowed to see absolute sizes still reached only 40 percent on validation. Another pass over these features would be fitting the hold-out.

The practical dispatch limitation remains: structured and sparse frames that still look dense are sent to Hybrid and can cost several milliseconds, sometimes as much as the faster path itself. Correctness of the synthetic hold-out is exact. Three photographs show a separate, sparse BGRA mismatch under Lightweight. Auto should stay user-overridable. Replacing it with a new metadata rule is not supported by this evidence.

## Completion checklist

1. Branch: `decode-phase3ad`.
2. Base: RC `0516b90`, decoder source `e6e9cb9`.
3. Commits: research tooling `04f7ea9`, freeze `59631d4`, hold-out provenance `ba83944`, plus the test and docs commits that add this file.
4. Files changed are research tools, tests helpers, benchmarks, and docs. Not `src/` or `include/`.
5. Production decoder changes: none.
6. Entropy code unchanged.
7. Inverse code unchanged.
8. DeviceOutput and the public API unchanged.
9. `upstream/` unchanged.
10. Encoder unchanged.
11. Development timed cases: 69, plus the unsupported RGB control in the manifest.
12. Validation timed cases: 45. `p3a_7680_noise` is assigned and unsupported.
13. Final hold-out cases: 42, all supported.
14. Real images re-encoded, not native camera CineForm: 6 validation, 10 hold-out.
15. Synthetic timed cases: 108 in development/validation and 32 in hold-out, plus 3 bundled YUV 4:2:2 development frames.
16. Split independence: RC and Phase 3A identities are development; validation families and the hold-out generator were committed before measurement; the hold-out file was not used to fit or select.
17. Baseline features: highpass total, level-1 share, maximum band, dense count.
18. Expanded set: the eligible share and bytes-per-coefficient fields in `decode_phase3ad_features.h`. Absolute sizes are shadow-only.
19. Selected features: the baseline set. No new feature shipped.
20. Models tested: baseline, three hand-rule families, depth-2-to-6 tree, weighted and unweighted logistic, three binned lookups, and one ineligible shadow tree.
21. Baseline development capture: 56.44 percent.
22. Baseline validation capture: 29.54 percent.
23. Selected development capture: 56.44 percent. The selected model is the baseline.
24. Selected validation capture: 29.54 percent.
25. Selected hold-out capture: 14.39 percent.
26. Hold-out matches: 28 of 42.
27. Hold-out misses: 14, listed above.
28. Hold-out mean regret: 0.764 ms.
29. Hold-out median regret: 0.
30. Hold-out p95 regret: 3.657 ms.
31. Hold-out worst regret: 4.854 ms.
32. Hold-out catastrophic misses: 10.
33. Worst hold-out case: `h2_1920_night_traffic_filmscan1`, Hybrid instead of Lightweight, relative regret 104 percent.
34. Old RC corpus under the same classifier, new session, not hold-out: capture 36.23 percent, 28 of 35, worst 5.336 ms.
35. Normalization: shares and bytes per coefficient. Absolute byte thresholds cannot be selected.
36. Resolution check: 1080p-only baseline refit captures 31.6 percent of validation 4K. The tree's 80.8 percent on that slice did not make its full validation regret better than the baseline.
37. Selected complexity: the existing four-predicate integer rule.
38. Production arithmetic: integer only.
39. Classifier batch-mean runtime: 66.4 ns on development/validation, 53.1 ns on hold-out.
40. Per-call maximum was not stored. The batch means are the recorded figures.
41. Classifier allocation count: 0.
42. Determinism: 100 identical passes on all 42 hold-out frames.
43. Golden 11/11: unchanged production bytes, RC evidence stands. Not re-run in this phase.
44. Hold-out exactness: 39 of 42 full four-strategy passes. Three photographs fail Lightweight BGRA.
45. Coefficients: exact on the 39 full passes, and exact on the Lightweight pass of the three BGRA failures before that check stopped.
46. LL: exact on those same frames.
47. Planar: exact on the 39 full passes. Not reached on the three BGRA failures.
48. BGRA: exact on the 39 full passes. Three photograph failures, 447, 93, and 33 pixels.
49. CPU-only CTest: full suite not re-run. RC result 6/6 for these bytes.
50. CUDA CTest: full suite not re-run. RC result 32/32 for these bytes. `test_decoder` and `test_decode_phase1` passed after the YU64 helper change.
51. initcheck: not re-run. RC evidence.
52. memcheck: not re-run. RC evidence.
53. racecheck: not re-run. RC evidence.
54. synccheck: not re-run. RC evidence.
55. Mixed Lightweight/Hybrid smoke: not re-run. Dispatch code did not change. RC 470/470 smoke stands.
56. Noise: forced Hybrid within about 7 percent of the historical Phase 3A means. Treated as variance.
57. Peak-Escape: not repeated. The 16 restoration cases and the interlaced coefficient regression remain as documented in Phase 3A-RC. No progressive vector was found there.
58. Frozen before hold-out: yes. Commit `59631d4`, then photograph download, then the single completed `--holdout` file.
59. Final outcome: **HOLD**.
60. Merge recommendation: do not merge.
61. Known limitations: structured frames still take the slow path; three photographs have sparse Lightweight BGRA mismatches; 8K FilmScan3 noise exceeds the 32 MiB sample limit; classifier per-call percentiles were not stored.
62. Phase 3B: remains blocked. No format expansion.

The central question is whether cheap CineForm metadata can predict the faster validated decode architecture on genuinely unseen frames. On this evidence, it cannot do so reliably enough to replace the current rule.
