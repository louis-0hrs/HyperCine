<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE. -->
# Decode Phase 3A-F — conservative Auto policy gate

**Policy outcome: B, accept with a limitation.** On the fresh primary corpus the frozen Auto rule is a useful conservative dispatch policy. It is a weak Oracle predictor, and the Phase 3A-D HOLD on that prediction question stays in force.

Branch `decode-phase3af`, created from the clean Phase 3A-E HEAD `e105ee5b8578e22873373bfb2a3ff3c3265e8c49`. The signed-color repair `5f839527b67a94aa5c48e5a7c7d50d0e3c1ee2e7` is an ancestor. `main` was `51435890ba5853e87e8cf1ddc7cc747954bd9e80` and was not moved. Upstream remains the GoPro baseline `11574d0295771edccadd17af14af74a539f924c7`, with an empty `upstream/` status. `src/core/decode_dispatch.cpp` still matches Phase 3A `e6e9cb905aee848808682942d13f07f2ff505a2d`. No production source, encoder, threshold, or feature changed. Nothing was merged, tagged, or pushed.

Measured on one NVIDIA GeForce RTX 4090 Laptop GPU, Windows, MSVC Release, CUDA 12.8. These are measurements from that machine. They are not a frame-rate guarantee.

## 1. Motivation

Phase 3A-D showed that cheap CineForm metadata does not predict the exact faster decode architecture well enough to act as an Oracle. The misses that study recorded were asymmetric: Auto chose Hybrid when Lightweight was faster, and the expensive mistake, Lightweight on a frame where Hybrid is much faster, did not appear.

High-entropy frames can make Lightweight tens of milliseconds slower than Hybrid. The reverse mistake, Hybrid on structured material, had stayed within a few milliseconds. Phase 3A-F asks whether the frozen rule is already useful because it avoids the expensive mistake. Oracle capture is reported and is not the promotion metric.

## 2. Difference from Phase 3A-D

Phase 3A-D fit and compared metadata rules, then held the candidate because validation captured 29.5 percent of the Oracle improvement and the untouched hold-out captured 14.4 percent. That HOLD is unchanged: metadata still cannot reliably name the faster path.

Phase 3A-F does not fit a rule, does not move a threshold, and does not add a second hold-out for rescue. It freezes the Phase 3A integer classifier, builds a new corpus, and scores four outcomes of each frame:

| Direction | Meaning |
| --- | --- |
| Correct Lightweight | Auto selects Lightweight and Lightweight is strictly faster, or the forced means are equal |
| Correct Hybrid | Auto selects Hybrid and Hybrid is strictly faster, or the forced means are equal |
| Conservative miss | Auto selects Hybrid and Lightweight is strictly faster |
| Dangerous miss | Auto selects Lightweight and Hybrid is strictly faster |

An exact tie is correct for whichever strategy Auto chose. Phase 3A-D treated a non-strict tie as Hybrid. That difference is applied only in this report.

Dangerous severity, nested, uses the forced means:

| Level | Rule |
| --- | --- |
| Any dangerous miss | Hybrid is strictly faster and Auto selects Lightweight |
| Material | Absolute regret above 1 ms, or relative regret above 20 percent |
| Severe | Absolute regret above 5 ms, or Lightweight above 1.5 times Hybrid |
| Catastrophic | Absolute regret above 10 ms, or Lightweight above 2 times Hybrid |

Regret is the forced mean of the architecture Auto chose, minus the faster forced mean. The separate Auto wall-clock window is not the regret.

## 3. Asymmetric-cost rationale

On the 100 primary frames where Hybrid is strictly faster, the Lightweight/Hybrid ratio has median 1.566, 95th percentile 4.425, and maximum 8.041. Sending those frames through Lightweight would add 592.095 ms across the corpus.

On the 130 primary frames where Lightweight is strictly faster, the Hybrid/Lightweight ratio has median 1.522, 95th percentile 2.286, and maximum 2.652. The largest conservative miss is 4.142 ms. None exceeds 5 ms.

The expensive tail points one way. A wrong Lightweight choice can cost about eight times the Hybrid time. A wrong Hybrid choice stayed under 2.65 times and under 4.2 ms on this corpus.

## 4. Frozen classifier statement

`ClassifyDecodeFrame` is the Phase 3A integer rule. Hybrid is selected only when the frame is not concentrated and it has either a dense band or distributed high-pass data.

* A band is dense when its payload is at least 32,768 bytes and `payload * 100 >= coefficients * 105`.
* The frame is concentrated when level-1 bytes are at least 75 percent of the high-pass bytes.
* The frame is distributed when high-pass bytes are at least 262,144 and the largest band is at least 65,536 bytes.

The arithmetic is integer. The call does not read pixels, clocks, or a learned model. `git diff e6e9cb9 -- src/core/decode_dispatch.cpp` is empty. The gate recomputed the label for every supported frame and required it to match the label stored in the manifest before timing. One hundred repeated calls per frame stayed on that label. The mean of those 100-call batches is 72.180 ns, under one microsecond.

## 5. Corpus

The manifest is `benchmarks/results/decode_phase3af_corpus.json`. It was committed in `22bec70` with `timing_performed: false` before the gate ran. The gate re-encoded every case and aborted unless the encoded hash, byte count, source hash, support flag, and frozen Auto label matched. No case was replaced after the timings were known.

| Population | Count |
| --- | --- |
| Manifest rows | 233 |
| Supported | 233 |
| Primary gate | 230 |
| Previously timed, excluded from the primary rates | 3 |
| Over the 32 MiB production sample limit | 0 |

Provenance of the 233 rows: 155 synthetic, 72 re-encoded real photographs, 6 native frames from `upstream/Example/videos/CFHDsample422.AVI`. The primary gate keeps 155 synthetic, 72 re-encoded, and the three native frames 3, 6, and 18. Frames 0, 12, and 23 were already timed in earlier phases and stay in the file as `previously_timed`.

Resolution of the primary rows: 166 at 1920x1080, 52 at 3840x2160, 12 at 7680x4320. Quality labels on the primary rows: low 39, medium 65, high 21, filmscan1 22, filmscan2 22, filmscan3 58, native 3. Encoded size runs from 153,176 to 17,658,536 bytes. There are 96 distinct source hashes and 233 distinct encoded hashes.

Synthetic pixels use seed `0x3AF0BEEF` and 22 classes that are not the Phase 3A, RC, or 3A-D generators. Each class is encoded at 1920x1080 in low, medium, and filmscan3, and at 3840x2160 in medium and filmscan3. Even class indexes also get 1920 high and filmscan1. Odd class indexes also get 1920 filmscan2. Six classes (`open_sky`, `radial_ramp`, `sparse_marks`, `flat_graphics`, `cgi_solids`, `skin_smooth`) are also encoded at 7680x4320 in low and medium. All twelve 8K samples fit under the unchanged 32 MiB limit.

Photographs come from `benchmarks/phase3af_media/photos.tsv`: 32 sources, two 1920 encodes each, and a 3840 encode for the first eight sources whose original is at least 3200x1800. The encode seed is `0x3AF51A77`. Two titles are imperfect matches to their query and were kept because the manifest was already frozen: family `portrait` is an urn whose title contains "face", and family `water_surface` is a bridge photographed from a lake. Workload tags were assigned from the generator class or the photograph query before any timing. They are not speed labels. Primary tags: high-entropy 77, sparse/structured 119, neutral 34.

Frozen Auto labels: 170 Hybrid and 63 Lightweight on all 233 rows, and 170 Hybrid and 60 Lightweight on the 230 primary rows. The three excluded native frames are Lightweight.

## 6. Benchmark methodology

`benchmark_decode_phase3af` times device-resident decode from queue through ready and release, with two slots. Each supported frame receives five Lightweight warmups and five Hybrid warmups. Forced Lightweight and Hybrid are then measured in alternating case order. Five Auto warmups follow, and Auto is measured in its own window. `Measure` itself does not warm up. Repetitions are 64 at 1080p, 32 at 4K, and 16 at 8K. Each forced series records mean, median, minimum, maximum, and sample standard deviation with divisor n-1.

The Oracle for a frame is the minimum of the forced Lightweight mean and the forced Hybrid mean. It is not used at runtime. Percentiles are nearest-rank, matching Phase 3A-D: the index is `min(n-1, max(0, ceil(fraction * n) - 1))`.

A near-tie is an absolute gap under 0.25 ms, or a relative gap under 5 percent of the faster mean. Near-ties are reported and are kept in the primary dangerous rate. Twelve primary frames are near-ties. Excluding them leaves 218 frames and the same one dangerous miss. No material dangerous miss sits inside a near-tie. The analyzer found zero primary gaps that fall inside two standard errors, so the repetition counts were not increased and the gate was not rerun.

Mixed workloads were declared before timing. Equal weight shuffles the 230 primary frames with an LCG seeded by `0x3AF0BEEF`. Natural-photo-heavy repeats `reencoded_real` four times and shuffles with seed `0x3AF0BEEF ^ 0x10001` (446 entries). High-entropy-heavy repeats that predeclared tag four times, seed `0x3AF0BEEF ^ 0x20002` (461 entries). Sparse/structured-heavy does the same for its tag, seed `0x3AF0BEEF ^ 0x30003` (587 entries).

The Always-Lightweight and Always-Hybrid columns use forced means. The Auto column of the workload table uses the separate Auto wall-clock window. Directional regret does not. Those two Auto numbers answer different questions and are labeled separately below.

## 7. Historical directional-error audit

Retained files were relabeled and were not retimed. They are seen data. They support the asymmetry claim and they are not the promotion gate.

The machine-readable check agrees with the published direction. Every retained miss is Hybrid when Lightweight was faster. Dangerous misses are zero on the Phase 3A development matrix, the original RC oracle file, the RC rows remeasured in Phase 3A-D, the Phase 3A-D validation split, the Phase 3A-D hold-out, and the Phase 3A-D development split. The original RC file's catastrophic flags are conservative as well. The original RC match rows do not store the slower path, so winner ratios are omitted there.

| Set | Frames | Dangerous | Conservative | Conservative total / worst ms | Cost avoided if Lightweight were used on Hybrid winners, ms |
| --- | ---: | ---: | ---: | --- | ---: |
| Phase 3A original matrix | 11 | 0 | 0 | 0 / 0 | 76.443 |
| Phase 3A rows inside the 3A-D session | 16 | 0 | 0 | 0 / 0 | 82.232 |
| RC original oracle file | 35 | 0 | 12 | 30.468 / 5.378 | unavailable |
| RC remeasured in 3A-D | 35 | 0 | 7 | 17.831 / 5.336 | 221.245 |
| 3A-D development split | 69 | 0 | 10 | 21.962 / 5.336 | 352.871 |
| 3A-D validation | 45 | 0 | 9 | 25.938 / 5.587 | 300.756 |
| 3A-D hold-out | 42 | 0 | 14 | 32.091 / 4.854 | 187.189 |

On the sets that store both paths, the Lightweight/Hybrid ratio on Hybrid winners reaches 4.1 to 8.1 at the maximum. The Hybrid/Lightweight ratio on Lightweight winners stays near 2.1 to 2.4. The original RC worst conservative miss remains `1920_text_ui` at 5.378 ms. Phase 3A-D's published totals match this relabeling.

## 8. Fresh directional-error results

Primary gate, 230 untouched supported frames:

| Result | Count |
| --- | ---: |
| Correct Lightweight | 59 |
| Correct Hybrid | 99 |
| Conservative miss | 71 |
| Dangerous miss | 1 |
| Accuracy | 0.6870 |
| Oracle capture, secondary | 0.5011 |

Sums of forced means: Lightweight 1635.345 ms, Hybrid 1254.205 ms, the forced path Auto selected 1148.499 ms, Oracle 1043.250 ms. Auto's selected forced time beats both fixed policies. It captures about half of the gap between the better fixed policy and the Oracle. That capture figure is context. It is not a reason to reject the policy, and it does not reopen the Phase 3A-D HOLD.

The same single dangerous miss is present if the three previously timed frames are put back. That sensitivity set is 233 frames, 71 conservative misses, 1 dangerous miss, and 0 catastrophic misses.

## 9. Dangerous miss distribution

One primary frame, `syn_7680_skin_smooth_low`: synthetic, 7680x4320, quality low, predeclared tag sparse/structured. Auto selects Lightweight. Forced Lightweight mean 12.344 ms, forced Hybrid mean 9.059 ms, absolute regret 3.285 ms, relative regret 0.363, Lightweight/Hybrid ratio 1.363.

The miss is material. It is not severe, because 3.285 ms is not above 5 ms and 1.363 is not above 1.5. It is not catastrophic. It is not a near-tie.

| Rate on 230 frames | Count | Rate |
| --- | ---: | ---: |
| Dangerous | 1 | 0.4348% |
| Material dangerous | 1 | 0.4348% |
| Severe dangerous | 0 | 0 |
| Catastrophic dangerous | 0 | 0 |

Dangerous total regret, mean regret, and worst regret are the same 3.285 ms. There is no second dangerous id. Excluding 12 near-ties leaves the rate at 1/218.

## 10. Conservative miss distribution

Seventy-one primary frames. Total regret 101.964 ms. Mean 1.436 ms, median 1.205 ms, 95th percentile 3.392 ms, worst 4.142 ms. Mean relative regret 0.320. Frames above 1 ms: 47. Above 2 ms: 17. Above 5 ms: 0.

Dangerous regret divided by conservative regret is 0.0322. The 3.285 ms dangerous regret is about 0.55 percent of the 592.095 ms that Lightweight would add on the Hybrid-faster frames.

The worst conservative miss is `syn_3840_ink_script_filmscan3`: Lightweight 8.313 ms, Hybrid 12.455 ms, regret 4.142 ms, relative 0.498. The next rows are the same shape, structured or sparse synthetics, plus two `pcb_traces` frames whose predeclared tag is high-entropy and whose faster forced path is still Lightweight. The tag is not the Oracle.

| Rank | Frame | Regret ms | Lightweight ms | Hybrid ms |
| ---: | --- | ---: | ---: | ---: |
| 1 | `syn_3840_ink_script_filmscan3` | 4.142 | 8.313 | 12.455 |
| 2 | `syn_3840_ink_script_medium` | 3.776 | 5.387 | 9.162 |
| 3 | `syn_1920_hard_contrast_medium` | 3.483 | 3.509 | 6.992 |
| 4 | `syn_7680_cgi_solids_medium` | 3.392 | 5.102 | 8.494 |
| 5 | `syn_3840_hard_contrast_medium` | 3.306 | 7.993 | 11.298 |
| 6 | `syn_3840_brick_course_medium` | 2.960 | 9.552 | 12.512 |
| 7 | `syn_1920_ink_script_filmscan2` | 2.878 | 3.465 | 6.343 |
| 8 | `syn_1920_brick_course_medium` | 2.765 | 4.239 | 7.004 |
| 9 | `syn_1920_brick_course_high` | 2.707 | 4.584 | 7.291 |
| 10 | `syn_1920_ink_script_filmscan3` | 2.538 | 3.555 | 6.093 |
| 11 | `syn_1920_pcb_traces_low` | 2.485 | 4.088 | 6.572 |
| 12 | `syn_1920_lattice_repeat_low` | 2.280 | 4.273 | 6.554 |
| 13 | `syn_1920_hard_contrast_high` | 2.243 | 4.513 | 6.755 |
| 14 | `syn_1920_lattice_repeat_medium` | 2.140 | 4.748 | 6.888 |
| 15 | `syn_1920_pcb_traces_high` | 2.053 | 5.037 | 7.089 |
| 16 | `syn_1920_hard_contrast_low` | 2.031 | 3.219 | 5.250 |
| 17 | `syn_1920_lattice_repeat_high` | 2.028 | 4.980 | 7.008 |
| 18 | `syn_1920_brick_course_low` | 1.918 | 3.598 | 5.516 |
| 19 | `syn_1920_hard_contrast_filmscan3` | 1.896 | 4.795 | 6.692 |
| 20 | `syn_1920_brick_course_filmscan3` | 1.888 | 5.248 | 7.136 |

Full-precision rows are in `benchmarks/results/decode_phase3af_directional_regret.json`.

## 11. Strategy ratios

| Population | Frames | Median | 95th percentile | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Lightweight / Hybrid on Hybrid-faster frames | 100 | 1.566 | 4.425 | 8.041 |
| Hybrid / Lightweight on Lightweight-faster frames | 130 | 1.522 | 2.286 | 2.652 |

The Hybrid-faster population includes the one dangerous miss and the 99 correct Hybrid frames. The Lightweight-faster population includes the 59 correct Lightweight frames and the 71 conservative misses.

## 12. Mixed workload results

Means below for Always Lightweight and Always Hybrid are forced means. The Auto column is the separate Auto wall-clock window. Times are milliseconds. Exceedances are counts of sequence entries above 8.33 ms, 16.67 ms, and 33.33 ms.

Unweighted primary, 230 frames:

| Policy | Mean | p95 | p99 | Worst | FPS | MP/s | Exceed 8.33 / 16.67 / 33.33 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Always Lightweight | 7.110 | 21.554 | 41.076 | 59.013 | 140.64 | 717.68 | 58 / 17 / 5 |
| Always Hybrid | 5.453 | 9.534 | 13.901 | 23.104 | 183.38 | 935.78 | 22 / 2 / 0 |
| Auto window | 4.996 | 9.635 | 13.631 | 23.737 | 200.17 | 1021.42 | 18 / 2 / 0 |

Natural-photo-heavy, 446 entries:

| Policy | Mean | p95 | p99 | Worst | FPS | MP/s | Exceed 8.33 / 16.67 / 33.33 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Always Lightweight | 6.482 | 16.948 | 34.714 | 59.013 | 154.28 | 612.59 | 97 / 23 / 5 |
| Always Hybrid | 5.517 | 8.592 | 13.104 | 23.104 | 181.26 | 719.68 | 28 / 2 / 0 |
| Auto window | 5.169 | 8.648 | 13.382 | 23.737 | 193.45 | 768.09 | 24 / 2 / 0 |

High-entropy-heavy, 461 entries:

| Policy | Mean | p95 | p99 | Worst | FPS | MP/s | Exceed 8.33 / 16.67 / 33.33 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Always Lightweight | 8.694 | 26.122 | 50.349 | 59.013 | 115.02 | 500.79 | 160 / 50 / 17 |
| Always Hybrid | 5.539 | 9.086 | 17.610 | 23.104 | 180.54 | 786.08 | 37 / 5 / 0 |
| Auto window | 5.274 | 9.214 | 17.817 | 23.737 | 189.61 | 825.57 | 33 / 5 / 0 |

Sparse/structured-heavy, 587 entries:

| Policy | Mean | p95 | p99 | Worst | FPS | MP/s | Exceed 8.33 / 16.67 / 33.33 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Always Lightweight | 5.871 | 16.265 | 28.740 | 59.013 | 170.33 | 1026.50 | 106 / 29 / 5 |
| Always Hybrid | 5.537 | 11.298 | 13.901 | 23.104 | 180.59 | 1088.33 | 73 / 5 / 0 |
| Auto window | 4.898 | 11.731 | 13.631 | 23.737 | 204.18 | 1230.52 | 57 / 5 / 0 |

Auto has the lowest mean on every mix, including the high-entropy mix and the sparse/structured mix. On the sparse/structured mix the mean advantage over Always Hybrid is the conservative behavior doing its job: Hybrid is avoided on frames where Lightweight is faster. On the high-entropy mix the Auto mean still beats Always Hybrid, while the Auto p95 and p99 sit a fraction of a millisecond behind the Hybrid window.

## 13. Tail latency

Tail latency is the production question next to the dangerous-miss rate. Always Lightweight is the tail risk. Its unweighted worst frame is 59.013 ms, its 99th percentile is 41.076 ms, and five primary frames exceed 33.33 ms. The high-entropy-heavy sequence puts 17 entries over 33.33 ms and 50 over 16.67 ms.

Auto and Always Hybrid both keep every primary frame under 33.33 ms. Each has two unweighted frames over 16.67 ms. Auto's unweighted 99th percentile, 13.631 ms, is slightly inside Hybrid's 13.901 ms. Auto's 95th percentile is 9.635 ms against Hybrid's 9.534 ms.

The Auto worst window, 23.737 ms, is `syn_3840_pcb_traces_filmscan3`. Auto selects Hybrid on that frame. The forced means are Lightweight 22.156 ms and Hybrid 23.104 ms, so the frame is a conservative miss of 0.949 ms and it is also Always Hybrid's worst forced mean. The 0.632 ms difference between 23.737 and 23.104 is the separate Auto window against the forced Hybrid window on that same frame. The series standard deviation there is about 1.16 ms over 32 repetitions. It is not a second architecture failure.

No 120 fps guarantee is claimed. A mean under 8.33 ms still leaves 18 primary Auto windows above 8.33 ms.

## 14. Correctness

The gate checker calls `DecodeRC::Exact`. Strategy order is Lightweight, Hybrid, Automatic, then HostStreaming. The first failing stage throws, so a Lightweight packed-YU64 miss never reaches Hybrid.

Of 233 supported frames, 219 passed every stage of all four strategies: 30 coefficients, both LL levels, BGRA against the CPU reference, signed planar planes, and packed YU64. Fourteen frames threw `RC YU64 exact strategy=1` after Lightweight coefficients, LL, BGRA, and planar planes had already matched. Every recorded pair was packed word 65472 versus CPU YU64 word 65535, maximum absolute difference 63.

`ConvertPlanarToYU64` saturates a 10-bit sample to 1023 and then shifts by 6, which is 65472. Phase 3A-D already documented that clamping after the shift maps a full-scale code to 65535, and that the helper was aligned with the saturate-then-shift value. The decoder was not changed in that note, and it was not changed here. The CPU reference's packed YU64 buffer still stores 65535 for samples the planes hold above 1023.

`--stage-audit` reloaded the fourteen cached samples and continued all four strategies independently. The result is `benchmarks/results/decode_phase3af_stage_audit.json`, status `PASS_IMAGE_EXACT_WITH_FULLSCALE_YU64_PACK_CONVENTION`.

| Check on all 14 frames and all four strategies | Result |
| --- | --- |
| Coefficient mismatches | 0 |
| LL mismatches | 0 |
| BGRA byte mismatches | 0 |
| Planar sample mismatches | 0 |
| BGRA hashes agree across strategies | yes |
| Packed YU64 hashes agree across strategies | yes |
| Path-dependent pixel difference | 0 |
| YU64 word diffs that are not 65472 versus 65535 | 0 |
| Differing words whose plane sample is 1023 or below | 0 |

Word-diff counts match the original checker, from 1 word to 898 words. The first differing plane samples run from 1024 through 1063. The fourteen identities are seven re-encoded photographs (`real_books_1920_low`, `real_brick_wall_1920_low`, `real_concrete_1920_medium`, `real_night_city_1920_high`, `real_painting_1920_filmscan1`, `real_painting_1920_low`, `real_snow_1920_low`) and seven `noise_field` encodes. The strict correctness file remains status FAIL. That status is the packed-buffer compare. Image outputs agree with the CPU reference and agree across Lightweight, Hybrid, Auto, and HostStreaming.

Manual overrides remain Lightweight, Hybrid, and Auto. Public API names were not changed.

Because production bytes match `e105ee5`, the full CUDA, CPU-only, and sanitizer suites were not repeated. The relevant regression that was run, Release, one test at a time:

| Test | Result | Time |
| --- | --- | --- |
| `test_decode_phase3ae` | Passed | 0.95 s |
| `test_decoder` | Passed | 18.07 s |
| `test_decode_phase1` | Passed | 1.96 s |
| `test_color_conversion_cuda` | Passed | 0.31 s |

## 15. Policy comparison

Always Lightweight is the slow, heavy-tailed policy on this corpus. Always Hybrid removes that tail and pays a bounded tax on Lightweight-faster frames. Auto keeps the Hybrid tail behavior and recovers most of that tax: the forced path it selects sums to 1148.499 ms, against 1254.205 ms for Always Hybrid and 1635.345 ms for Always Lightweight.

The price of Auto on this corpus is one material dangerous miss of 3.285 ms, ratio 1.363, on one 8K low-quality synthetic, plus 71 conservative misses whose worst case is 4.142 ms. The Auto wall-clock window tracks the chosen forced path closely enough that its mean, 4.996 ms, sits next to the selected-forced mean, 4.993 ms.

Outcome A would require a material dangerous rate that is extremely low and a correctness record with nothing left to explain. This corpus has one material miss, and the strict YU64 file is FAIL until the pack convention is read beside it. Outcome C would require a meaningful pattern of dangerous Lightweight selections, a severe or catastrophic miss, or an Auto tail that becomes unpredictable. One 3.285 ms miss, zero frames over 33.33 ms, and a worst Auto window that is the same conservative frame as Hybrid's worst forced mean do not meet that description. Outcome D would require Auto to show no practical advantage. The mean and the Lightweight tail say otherwise.

## 16. Final Auto policy recommendation

**Outcome B. Accept the frozen Auto rule with a limitation.**

Use Auto as the conservative general-purpose default. Keep explicit Lightweight and explicit Hybrid available. Auto must not become the only behavior.

The limitation to publish with that default:

* One fresh dangerous miss, `syn_7680_skin_smooth_low`, 3.285 ms, 1.363 times Hybrid, material, not severe, not catastrophic.
* Conservative misses are common and bounded: 71 of 230, worst 4.142 ms, none above 5 ms.
* Oracle capture on the fresh corpus is 50.1 percent. That is a weak predictor. Phase 3A-D's HOLD on Oracle prediction remains the correct conclusion for that question.
* Fourteen frames disagree with the CPU packed YU64 buffer by the full-scale 65472-versus-65535 convention. Coefficients, LL, BGRA, and planar samples match, and the four strategies match each other.

No new classifier was written after these numbers. No threshold moved.

## 17. Phase 3A promotion recommendation

Promote the existing Auto rule as a conservative policy, with the limitation in section 16 written next to it. Do not promote it as an Oracle, and do not replace it with a newly fit rule. Phase 3A-D, Phase 3A-RC, and Phase 3A-E keep their published conclusions. Those reports were not rewritten.

Phase 3A-E performance work stays closed. This phase did not change production bytes and did not measure a repair regression above 5 percent.

## 18. Phase 3B recommendation

Three answers, kept separate:

| Question | Answer |
| --- | --- |
| Is the progressive decoder core ready? | Yes for the current progressive 10-bit YUV 4:2:2 path. This corpus found no path-dependent pixel difference. The signed-color repair from Phase 3A-E is present. The packed YU64 discrepancy is the documented full-scale convention after the planes already match. |
| Is Auto acceptable as a conservative production policy? | Yes, with the Outcome B limitation. Explicit Lightweight and Hybrid overrides stay. |
| Does the Oracle-prediction failure still block Phase 3B? | No. Format expansion can proceed while Auto remains a conservative default with a documented miss, or while a caller forces Hybrid or Lightweight. Phase 3B was not implemented in this phase. |

Remaining limitations: the policy evidence is one GPU; 8K coverage is the twelve samples that fit under 32 MiB; the portrait and lake-bridge photographs are imperfect query matches and were retained; Pillow resampling of the photograph sources is not a promise of bit-exact YUV across machines, so the committed hashes plus the local YUV are the corpus identity; the strict YU64 checker remains FAIL on fourteen frames; Oracle capture remains about 50 percent.
