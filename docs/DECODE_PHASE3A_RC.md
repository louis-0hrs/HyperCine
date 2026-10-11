# Decode Phase 3A-RC promotion validation

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

**Promotion Gate: HOLD — RC Outcome C — HOLD FOR CLASSIFIER WORK.** Do not merge; conduct separate offline classifier research with fresh development and new held-out splits.

Starting Phase 3A HEAD: `e6e9cb905aee848808682942d13f07f2ff505a2d` (Phase 3A base `dded4d8`). Branch: `decode-phase3a-rc`. RC changes only tests, their CMake registration, evidence and documentation. Production sources, public API, classifier thresholds, encoder, upstream and `main` are unchanged. No merge, tag, push or release occurred.

## Corpus and independence

The predeclared [corpus manifest](../tests/decode_phase3a_rc_corpus.json) fixes families, settings, seed and source SHA256 fingerprints before the primary timing window. All eleven development source/sample identities are excluded. Generated content uses a new integer-only generator and seed `0x6A09E667`; it is synthetic validation imagery, not camera footage. External samples were previously CPU decode fixtures, not data used for selecting Auto thresholds.

There are 36 cases: 32 generated (15 families at 1080p, 15 at 4K, two practical low-detail 8K frames) and four external cases. Three external frames (0, 12, 23) come from one bundled progressive 10-bit YUV422 clip; camera acquisition and scene category are unknown. The bundled 12-bit RGB444 frame is an unsupported control, excluded from Oracle statistics. Total resolution counts: 19 at 1080p, 15 at 4K, two at 8K. This is broad synthetic entropy coverage with limited external-video coverage. Source hashes, decoded/source frame hashes, sample sizes, identified profile/precision and support status are in [holdout JSON](../benchmarks/results/decode_phase3a_rc_holdout.json); no new media is committed.

## Primary measurements and offline Oracle

Each supported case has five warmups and 120/60/30 measured frames per strategy at 1080p/4K/8K, two configured slots, one outstanding ticket, equivalent Queue→Ready→Release windows and device-resident output. L/H order alternates by case; Auto is measured separately. Process CPU time is kernel plus user time across threads. GPU workloads run sequentially. Correctness downloads are outside timing. Auto never consumes Oracle timing.

OracleFrame is `min(Lightweight mean, Hybrid mean)`. Regret uses the forced latency of Auto's selected strategy. Capture uses exactly Phase 3A's definition: `(min(sum L, sum H) - sum selected) / (min(sum L, sum H) - sum Oracle) × 100`, equal case weights. The primary corpus captures **26.79%**, with **23/35 matches** and **12 misses**. Mean/median/worst regret: **0.871/0.000/5.378 ms**; worst case `1920_text_ui`. Summed selected/L/H/Oracle latency: **306.831/443.489/317.982/276.363 ms**.

| Case | Auto choice | L ms | H ms | Auto ms | Oracle | Regret ms | Regret % |
| --- | --- | --- | --- | --- | --- | --- | --- |
| CFHDsample422.AVI:frame0 | lightweight | 2.804 | 5.990 | 2.632 | lightweight | 0.000 | 0.0 |
| CFHDsample422.AVI:frame12 | lightweight | 1.914 | 4.480 | 1.863 | lightweight | 0.000 | 0.0 |
| CFHDsample422.AVI:frame23 | lightweight | 2.301 | 3.643 | 2.318 | lightweight | 0.000 | 0.0 |
| CFHDsample444.AVI:frame0 | unsupported | — | — | — | — | — | — |
| 1920_foliage_proxy | hybrid | 9.088 | 5.821 | 5.830 | hybrid | 0.000 | 0.0 |
| 1920_water_proxy | hybrid | 7.837 | 5.292 | 5.144 | hybrid | 0.000 | 0.0 |
| 1920_architecture_graphics | hybrid | 4.292 | 9.355 | 9.448 | lightweight | 5.063 | 117.9 |
| 1920_text_ui | hybrid | 5.418 | 10.796 | 10.804 | lightweight | 5.378 | 99.3 |
| 1920_cgi_geometry | hybrid | 5.471 | 5.675 | 5.632 | lightweight | 0.204 | 3.7 |
| 1920_dark_noise | hybrid | 9.081 | 5.792 | 5.329 | hybrid | 0.000 | 0.0 |
| 1920_clean_sky | lightweight | 1.877 | 3.730 | 1.856 | lightweight | 0.000 | 0.0 |
| 1920_heavy_grain | hybrid | 10.600 | 5.692 | 5.719 | hybrid | 0.000 | 0.0 |
| 1920_curved_gradient | lightweight | 1.934 | 4.138 | 1.944 | lightweight | 0.000 | 0.0 |
| 1920_diagonal_edges | hybrid | 8.976 | 5.658 | 5.551 | hybrid | 0.000 | 0.0 |
| 1920_woven_fabric | hybrid | 8.819 | 8.436 | 9.027 | hybrid | 0.000 | 0.0 |
| 1920_skin_proxy | hybrid | 6.271 | 5.384 | 5.352 | hybrid | 0.000 | 0.0 |
| 1920_mixed_detail | hybrid | 8.874 | 13.532 | 13.503 | lightweight | 4.659 | 52.5 |
| 1920_motion_blur_proxy | hybrid | 9.583 | 5.417 | 5.434 | hybrid | 0.000 | 0.0 |
| 1920_low_quality_detail | hybrid | 4.782 | 4.057 | 4.070 | hybrid | 0.000 | 0.0 |
| 3840_foliage_proxy | hybrid | 34.893 | 11.482 | 9.994 | hybrid | 0.000 | 0.0 |
| 3840_water_proxy | hybrid | 19.732 | 8.050 | 7.936 | hybrid | 0.000 | 0.0 |
| 3840_architecture_graphics | hybrid | 10.779 | 13.871 | 13.255 | lightweight | 3.092 | 28.7 |
| 3840_text_ui | hybrid | 13.995 | 15.753 | 15.681 | lightweight | 1.758 | 12.6 |
| 3840_cgi_geometry | hybrid | 25.517 | 26.885 | 26.420 | lightweight | 1.368 | 5.4 |
| 3840_dark_noise | hybrid | 25.062 | 8.912 | 8.875 | hybrid | 0.000 | 0.0 |
| 3840_clean_sky | hybrid | 3.857 | 6.193 | 6.300 | lightweight | 2.336 | 60.5 |
| 3840_heavy_grain | hybrid | 40.572 | 10.438 | 10.694 | hybrid | 0.000 | 0.0 |
| 3840_curved_gradient | hybrid | 4.176 | 6.411 | 6.373 | lightweight | 2.235 | 53.5 |
| 3840_diagonal_edges | hybrid | 23.475 | 9.083 | 9.095 | hybrid | 0.000 | 0.0 |
| 3840_woven_fabric | hybrid | 25.232 | 16.073 | 15.993 | hybrid | 0.000 | 0.0 |
| 3840_skin_proxy | hybrid | 15.853 | 8.105 | 8.359 | hybrid | 0.000 | 0.0 |
| 3840_mixed_detail | hybrid | 22.830 | 24.422 | 25.522 | lightweight | 1.592 | 7.0 |
| 3840_motion_blur_proxy | hybrid | 34.319 | 8.920 | 9.018 | hybrid | 0.000 | 0.0 |
| 3840_low_quality_detail | hybrid | 11.802 | 6.240 | 6.251 | hybrid | 0.000 | 0.0 |
| 7680_clean_sky | hybrid | 10.591 | 12.295 | 11.652 | lightweight | 1.705 | 16.1 |
| 7680_curved_gradient | hybrid | 10.880 | 11.960 | 11.750 | lightweight | 1.080 | 9.9 |

## Misses and generalization

Every >2 ms absolute or >20% relative regret is surfaced here (6 cases). These thresholds flag engineering concerns, not incorrect reconstruction. Content labels are explanatory and never become selector special cases.

- `1920_architecture_graphics`: 5.063 ms (117.9%), Auto hybrid / Oracle lightweight.
- `1920_text_ui`: 5.378 ms (99.3%), Auto hybrid / Oracle lightweight.
- `1920_mixed_detail`: 4.659 ms (52.5%), Auto hybrid / Oracle lightweight.
- `3840_architecture_graphics`: 3.092 ms (28.7%), Auto hybrid / Oracle lightweight.
- `3840_clean_sky`: 2.336 ms (60.5%), Auto hybrid / Oracle lightweight.
- `3840_curved_gradient`: 2.235 ms (53.5%), Auto hybrid / Oracle lightweight.

The confirmation window retained every source hash and decision: capture 27.33%, 23/35 matches. It does not replace the primary window. The original corpus and all primary misses remain in the report. Thresholds were never tuned. The poor improvement capture and material misses warrant a separate dispatch research step; decoder correctness remains exact.

## Classifier metadata and determinism

All 35 supported cases produced identical strategy and metadata across 100 repeated passes (3500 classifications), matched normal production Auto, and retained decisions in independent validation and timing windows. Production classifier observations range 100–300 ns; mean 177.1 ns. Standalone repeated classifier cost is also recorded per case. Neither observation affects dispatch.

Level payload columns are level 0/1/2; coefficient count includes padded rows. Concentrated and distributed flags use the unchanged integer predicates.

| Case | HP bytes | Padded coefficients | Largest band | Dense bands | Level bytes 0/1/2 | Concentrated | Distributed |
| --- | --- | --- | --- | --- | --- | --- | --- |
| CFHDsample422.AVI:frame0 | 570820 | 4082400 | 54504 | 0 | 212132/261432/97256 | False | False |
| CFHDsample422.AVI:frame12 | 210784 | 4082400 | 21004 | 0 | 75836/93724/41224 | False | False |
| CFHDsample422.AVI:frame23 | 170220 | 4082400 | 50704 | 0 | 96792/58844/14584 | False | False |
| 1920_foliage_proxy | 3007408 | 4082400 | 765076 | 9 | 2170608/664756/172044 | False | True |
| 1920_water_proxy | 1289552 | 4082400 | 422100 | 3 | 708204/431832/149516 | False | True |
| 1920_architecture_graphics | 837984 | 4082400 | 216680 | 1 | 460232/274808/102944 | False | True |
| 1920_text_ui | 1187472 | 4082400 | 285216 | 1 | 729004/347140/111328 | False | True |
| 1920_cgi_geometry | 1327480 | 4082400 | 284588 | 4 | 740732/429380/157368 | False | True |
| 1920_dark_noise | 2152900 | 4082400 | 537660 | 1 | 1596140/464212/92548 | False | True |
| 1920_clean_sky | 185548 | 4082400 | 42592 | 0 | 81392/80664/23492 | False | False |
| 1920_heavy_grain | 3519508 | 4082400 | 898736 | 9 | 2641128/731220/147160 | False | True |
| 1920_curved_gradient | 233952 | 4082400 | 42592 | 0 | 119112/88100/26740 | False | False |
| 1920_diagonal_edges | 2252664 | 4082400 | 493972 | 6 | 1324352/694424/233888 | False | True |
| 1920_woven_fabric | 2212948 | 4082400 | 542776 | 4 | 1398812/636596/177540 | False | True |
| 1920_skin_proxy | 1359328 | 4082400 | 328420 | 0 | 984824/312432/62072 | False | True |
| 1920_mixed_detail | 1998776 | 4082400 | 494704 | 2 | 1420548/462072/116156 | False | True |
| 1920_motion_blur_proxy | 2264888 | 4082400 | 780520 | 5 | 1610208/527928/126752 | False | True |
| 1920_low_quality_detail | 1154088 | 4082400 | 232956 | 6 | 484288/544076/125724 | False | True |
| 3840_foliage_proxy | 12023456 | 16329600 | 3060248 | 9 | 8678528/2658108/686820 | False | True |
| 3840_water_proxy | 5152076 | 16329600 | 1688000 | 3 | 2829432/1725308/597336 | False | True |
| 3840_architecture_graphics | 3369316 | 16329600 | 877244 | 1 | 1855036/1100424/413856 | False | True |
| 3840_text_ui | 4799992 | 16329600 | 1172496 | 1 | 2941324/1409724/448944 | False | True |
| 3840_cgi_geometry | 8382864 | 16329600 | 2044764 | 6 | 5039108/2598168/745588 | False | True |
| 3840_dark_noise | 8606708 | 16329600 | 2149660 | 1 | 6379976/1856456/370276 | False | True |
| 3840_clean_sky | 698660 | 16329600 | 170516 | 0 | 302000/310464/86196 | False | True |
| 3840_heavy_grain | 14072192 | 16329600 | 3595160 | 9 | 10562232/2921876/588084 | False | True |
| 3840_curved_gradient | 786456 | 16329600 | 170516 | 0 | 385328/307108/94020 | False | True |
| 3840_diagonal_edges | 9005108 | 16329600 | 1976248 | 6 | 5295120/2775276/934712 | False | True |
| 3840_woven_fabric | 8849292 | 16329600 | 2172928 | 4 | 5595904/2543516/709872 | False | True |
| 3840_skin_proxy | 5421976 | 16329600 | 1311296 | 0 | 3932876/1244128/244972 | False | True |
| 3840_mixed_detail | 7900548 | 16329600 | 1954492 | 1 | 5621236/1825240/454072 | False | True |
| 3840_motion_blur_proxy | 9055384 | 16329600 | 3121864 | 5 | 6439292/2109628/506464 | False | True |
| 3840_low_quality_detail | 4613356 | 16329600 | 931552 | 6 | 1933660/2175884/503812 | False | True |
| 7680_clean_sky | 2717076 | 65318400 | 681628 | 0 | 1169872/1220172/327032 | False | True |
| 7680_curved_gradient | 2895484 | 65318400 | 681628 | 0 | 1395192/1169804/330488 | False | True |

## Progressive Peak-Escape search and exactness

**Peak-Escape Production Vector: NOT FOUND.** Six patterns (alternating extremes, checkerboard, impulses, hard transitions, random extremes, saturated chroma), six qualities (Low through FilmScan3), YUY2 and YU64 containing true 10-bit values, and 64×64 / 1920×1080 form 144 completed progressive configurations. The unchanged encoder emitted zero Peak-Escape tables. No progressive bitstream is manufactured and progressive Peak-Escape CPU/LL/planes/BGRA exactness remains unobserved.

The default-capacity pilot completed 100 configurations then faulted on 1920×1080 YUY2 random extremes, FilmScan2, flags 0 (exit `0xC0000005`). The existing public `LARGER_OUTPUT` flag (2048) let all 144 configurations finish with the same patterns/qualities; it changes capacity, not coding policy. Full pilot and completed-search metadata are preserved in [peak JSON](../benchmarks/results/decode_phase3a_rc_peak_escape.json), with the observation documented in [UPSTREAM.md](UPSTREAM.md).

Direct tests call the unchanged restoration kernel: 16 exact cases cover positive/negative signed-16 replacements, inclusive threshold boundaries, coefficient order, inactive bands, odd zero padding, null/bounds/count failures, retained frame errors and restoration before horizontal differencing. Omitting restoration differs in four coefficients. These are metadata/kernel units, not a progressive production-vector claim. The original valid interlaced regression remains coefficient-only: CPU/HostStreaming/Hybrid all 30 bands exact, max error 0; production returns `UnsupportedEncodingMode` and publishes no output.

## Golden, smoke, sanitizers and CPU-only gates

All 35 supported held-out cases match CPU in 30/30 coefficients, both LL levels, signed planar output, YU64 and DeviceBGRA for forced L, forced H, Auto and HostStreaming. All **11/11 frozen golden samples** retain original coefficients, LL, planar/YU64/BGRA hashes and Auto choices. Expected hashes were not regenerated.

The shortened held-out smoke passes **470/470 frames** (300 1080p, 150 4K, 20 8K), holding two outputs and releasing out of order, with exact output bytes and frame/band decisions and stable owned capacities/allocation counts. Full production CTest also repeats lifecycle, malformed-input, multi-instance, allocation and determinism coverage. The previous 16000-frame run is referenced because production bytes are unchanged.

CUDA CTest: **32/32 PASS**, 703.22 s. CPU-only configure/build with `HYPERCINE_ENABLE_CUDA=OFF`: **6/6 PASS**, 4.67 s. Untouched baseline initially passed 29/31: historical encoder `test_phase2a` and `test_phase2b` failed, then both passed immediate unchanged retries. Both logs are retained; this intermittent baseline limitation is not hidden or repaired inside decoder RC.

The first final CUDA run spanned an overnight interruption: 31/32 passed, with a historical `test_decode_phase2fa` timeout. Its 44922.45 s full-run log is retained as `decode_phase3a_rc_interrupted_cuda_ctest.txt`. A fresh complete run produced the passing final result above. Neither test logic nor timeouts were relaxed.

The four clean Phase 3A Compute Sanitizer logs remain applicable to byte-identical production code. Focused RC sanitizer results and log hashes are in [validation JSON](../benchmarks/results/decode_phase3a_rc_validation.json). New RC units and bounded generation are exercised under instrumentation; no old result is presented as covering changed production code.

## Existing production benchmark regression

The unchanged 120-frame Phase 3A matrix and 1000-frame streaming runs were rerun in an isolated directory. All eleven Auto choices and owned device/pinned/pageable capacities are unchanged. Median Auto latency change versus the final Phase 3A report: **+1.61%**. Every >5% row is flagged in [regression JSON](../benchmarks/results/decode_phase3a_rc_regression.json); CPU changes, min/max latency, FPS, MP/s, working-set peaks, mixed throughput and the unchanged `benchmark_codec` comparison with `reference_windows_msvc.json` are retained there. Source equality excludes an RC runtime implementation change; laptop timing differences are observations, not claimed optimizations.

| Width | Pattern | Phase 3A Auto ms | RC Auto ms | Change | CPU ms/frame old → RC |
| --- | --- | --- | --- | --- | --- |
| 1920 | flat | 0.992 | 1.008 | +1.6% | 0.91 → 1.69 |
| 1920 | gradient | 2.714 | 2.687 | -1.0% | 4.17 → 2.86 |
| 1920 | noise | 4.587 | 5.060 | +10.3% | 12.76 → 18.75 |
| 1920 | texture | 7.454 | 7.655 | +2.7% | 7.55 → 9.64 |
| 1920 | edges | 3.532 | 3.567 | +1.0% | 6.77 → 8.20 |
| 3840 | flat | 1.543 | 1.572 | +1.9% | 1.30 → 1.56 |
| 3840 | gradient | 4.121 | 4.217 | +2.3% | 10.16 → 8.85 |
| 3840 | noise | 8.229 | 9.702 | +17.9% | 40.89 → 46.09 |
| 3840 | texture | 19.947 | 18.264 | -8.4% | 21.35 → 20.05 |
| 3840 | edges | 5.063 | 5.070 | +0.1% | 11.20 → 16.15 |
| 7680 | flat | 4.324 | 4.346 | +0.5% | 4.69 → 4.69 |

Systematic Noise slowdown flag: **True**. The timing cause is unresolved and warrants follow-up; source equality is evidence of unchanged implementation, not proof of a particular clock or scheduling explanation. The following streaming comparison also surfaces CPU and process working-set changes. Working-set high-water includes process/harness and runtime, while owned decoder capacities are exactly equal.

| Width | Workload | Slots | FPS change | Ready latency change | CPU cost change | Peak WS change bytes |
| --- | --- | --- | --- | --- | --- | --- |
| 1920 | equal_five_pattern | 2 | -1.9% | +2.0% | +16.3% | -1429504 |
| 1920 | equal_five_pattern | 3 | +1.2% | -1.2% | +9.8% | -1974272 |
| 1920 | equal_five_pattern | 4 | +3.7% | -3.6% | -0.1% | -1695744 |
| 1920 | low_latency | 1 | -3.4% | +3.5% | +13.5% | -1695744 |
| 1920 | high_throughput | 3 | +2.4% | -2.3% | +2.7% | -1695744 |
| 3840 | equal_five_pattern | 2 | -0.5% | +0.5% | +6.4% | -1695744 |
| 3840 | equal_five_pattern | 3 | -3.5% | +3.6% | +5.2% | -1695744 |
| 3840 | equal_five_pattern | 4 | -3.5% | +3.7% | +10.6% | -1273856 |
| 3840 | low_latency | 1 | -2.5% | +2.5% | +15.8% | -1273856 |
| 3840 | high_throughput | 3 | -1.5% | +1.5% | +4.8% | -1273856 |
| 3840 | noise_sustained | 3 | -17.3% | +20.9% | +32.4% | -1273856 |

The unchanged CPU codec harness is compared with the preserved original baseline below. Timing changes are not attributed to RC decoder improvements.

| Case | Encode baseline → RC FPS | Decode baseline → RC FPS | Encode/decode MP/s | Encode avg/min/max ms | Decode avg/min/max ms | Peak WS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| 1080p | 77.70 → 80.75 | 394.15 → 458.06 | 167.4/949.8 | 12.01/10.93/13.60 | 2.18/1.87/2.57 | 91.78 |
| 4K_UHD | 21.84 → 22.02 | 125.31 → 122.23 | 182.6/1013.8 | 44.28/41.85/50.42 | 8.18/7.79/8.83 | 228.50 |
| 8K_UHD | 4.28 → 5.82 | 27.96 → 30.02 | 193.0/995.8 | 167.49/159.36/179.29 | 33.32/30.21/37.08 | 613.27 |

## Reproduction and repository state

```powershell
cmake -S . -B build -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\tests\Release\test_decode_phase3a_rc.exe --benchmark . benchmarks/results
.\build\tests\Release\test_decode_phase3a_rc.exe --benchmark . build/phase3a-rc/repeat
# Run benchmark_decoder.exe 120 from build/phase3a-rc/regression (isolated output).
cmake -S . -B build-cpu -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
python benchmarks/summarize_decode_phase3a_rc.py
```

Run GPU tests, benchmarks and sanitizers sequentially. The frozen declaration intentionally makes the summarizer reject changed evaluation source; research on these now-observed cases needs a new split. Existing Phase 3A result artifacts are untouched. Upstream status is empty and its tree remains `837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`; `main` remains `51435890ba5853e87e8cf1ddc7cc747954bd9e80`. Encoder and public API are unchanged. Commit identities and final working-tree status are recorded in [completion JSON](../benchmarks/results/decode_phase3a_rc_completion.json) and the delivery message.

## Completion checklist

| # | Item | Result |
| --- | --- | --- |
| 1 | Branch | decode-phase3a-rc |
| 2 | Base commit | e6e9cb905aee848808682942d13f07f2ff505a2d |
| 3 | Commits | 8876921 test(decoder): add Phase 3A held-out promotion validation; documentation commit containing this report: docs(decoder): record Phase 3A release-candidate validation |
| 4 | Production source changed | No |
| 5 | Files changed | Complete path list in completion JSON |
| 6 | Upstream clean | Yes, frozen tree and empty status |
| 7 | Encoder unchanged | Yes |
| 8 | Main unchanged | 51435890ba5853e87e8cf1ddc7cc747954bd9e80 |
| 9 | Held-out cases total | 36 |
| 10 | Real external cases | 4; 3 supported from one clip |
| 11 | Generated held-out cases | 32 |
| 12 | 1080p cases | 19 |
| 13 | 4K cases | 15 |
| 14 | 8K cases | 2 |
| 15 | Unsupported cases | 1: 12-bit RGB444 |
| 16 | Auto/Oracle matches | 23 |
| 17 | Auto/Oracle misses | 12 |
| 18 | Oracle improvement capture | 26.79% |
| 19 | Mean regret | 0.870527 ms |
| 20 | Median regret | 0.000000 ms |
| 21 | Worst regret | 5.377827 ms |
| 22 | Worst-regret case | 1920_text_ui |
| 23 | Catastrophic reporting flags | 6; every case listed above |
| 24 | Classifier determinism | 100/100 per supported case; validation and repeat decisions match |
| 25 | Classifier cost | 100–300 ns; mean 177.1 ns |
| 26 | Thresholds unchanged | Yes |
| 27 | Progressive Peak-Escape sample found | No |
| 28 | Peak source patterns | alternating_extremes, checkerboard, impulses, hard_transition, random_extremes, saturated_chroma |
| 29 | Progressive Peak-Escape count | 0 |
| 30 | Evidence old behavior differs | 4 unit coefficients; valid interlaced CPU regression retained |
| 31 | CPU coefficients exact | 30/30, max error 0 |
| 32 | Lightweight exact | Yes |
| 33 | Hybrid exact | Yes |
| 34 | Auto exact | Yes |
| 35 | LL exact | Yes |
| 36 | Planar exact | Yes, all four strategies |
| 37 | DeviceBGRA exact | Yes, all four strategies |
| 38 | Frozen golden result | 11/11; original hashes and decisions unchanged |
| 39 | Interlaced coefficients | 30/30 CPU/HostStreaming/Hybrid exact, max error 0 |
| 40 | Interlaced production rejection | UnsupportedEncodingMode; no output |
| 41 | Benchmark regression | Median Auto change +1.61%; Noise slowdown flagged; owned memory unchanged |
| 42 | Long-run/smoke | 470/470 held-out smoke PASS; full production regression; previous 16000-frame evidence referenced |
| 43 | Sanitizers | Four focused RC tools clean; four Phase 3A logs cover unchanged production |
| 44 | CPU-only | Configure/build and 6/6 CTest PASS |
| 45 | Production API unchanged | Yes |
| 46 | Working-tree status | Evidence generation precedes its own docs commit; final clean status confirmed in delivery message |
| 47 | Final RC outcome | RC Outcome C — HOLD FOR CLASSIFIER WORK |
| 48 | Merge recommendation | Do not merge |
| 49 | Known limitations | No progressive peak vector; limited external provenance; historical encoder flakiness; unresolved Noise timing slowdown |
| 50 | Phase 3B start | Resolve promotion dispatch gap in a separate research step first; then profile capability matrix and field-aware interlaced inverse with frozen real difference-coded end-to-end vectors. |

## Promotion decision

**RC Outcome C — HOLD FOR CLASSIFIER WORK. Do not merge; conduct separate offline classifier research with fresh development and new held-out splits.** Known gaps are limited external-camera provenance, no real progressive Peak-Escape vector, and intermittent historical encoder baseline tests. Phase 3B should start with a profile capability matrix and field-aware interlaced inverse reconstruction after a separate dispatch study resolves the promotion gap. No format expansion or decoder redesign belongs in this RC.
