# SDK golden refresh (#46, Draft #322)

This is Linux x64 CPU replay of original SDK packets, not production or VC-5
conformance qualification. No original packets or private paths are in Git.

## Current-main continuation (2026-10-30)

The existing draft forward-integrates main
`04c348f848de1eb3a47d3b90fe191722c1ab559e` after #325/#329 landed.
The merge retains the main implementation byte-for-byte; no duplicate ladder
implementation hunk remains and no original tests were deleted. The tables
below remain the historical measurement snapshot, not current-head results.
Current complete suite evidence is recorded separately in
`SDK_GOLDENS_CURRENT_46.json` once completed. Corpus pins and historical
FAIL 78/120 artifacts remain unchanged; the original 40f ramp remains negative.

This lane owns #322 gates/docs only. #331 independently verifies packet-table
replay; it is complementary, not a second implementation to copy into this PR.
#320 canonical integration is separately owned. Windows-native execution is
deferred because its node is unavailable; original Windows A/B/C replay here
is Linux CPU evidence only. Original Windows compiler/ISA, seed, explicit
FRMR, native GPU, sanitizers and product/conformance admission remain unproved.

## Source and history

- The first forward merge brings in main `9909bfccf220ec914a3c1ebf3e57fc9d95696f99`,
  including the RGB444 p12 transform and its tests; no conflicts or test deletion.
- The corrected candidate also forward-integrates the exact #325 dependency
  `0be5bef090906ac047fb018b7c10d6e23da6212b` into this test branch only.
  This is **not** a merge of #325 or #322 on GitHub or into protected main.
  Acceptance of the #325 dependency remains a maintainer gate.
- `SDK_GAP_PARITY_46.json` and `SDK_GAP_TRAJECTORY_46.json` are **unchanged
  historical evidence**, never updated with corrected results. Medium and High
  remain recorded as FAIL, 78/120, with 42 divergent packets each.
- `p6_sdk_gap_historical_pre325` pins the historical expected-to-fail-before-#325
  outcome and all seven runs of mismatches. A passing preservation check does
  not change any historical FAIL into a qualification PASS.

## Independent, read-only datasets

Each inventory is independently SHA-256 pinned, and **every** indexed artifact
is checked for SHA-256 and byte length before replay. Inputs and packets are
also checked against their frame manifests at the comparison site.

| Dataset | Indexed files | Bytes | Transfer index SHA-256 |
|---|---:|---:|---|
| Linux SDK gaps | 776 | 2,024,958,339 | dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb |
| Windows group A | 606 | 1,520,292,514 | 62c6f4c9297d760f1f14bde73ad15f62ca254371fa8b05f55c7ddf68e1d2e0bc |
| Windows group B | 177 | 390,885,680 | 72e7c314e0e28a9831d1e9270045642b28ade24aec9363f550c825a6a6fa5857 |
| Windows group C | 35 | 59,385,771 | a718243eb1f0d9efdd1c8fb030720eff7d2321d20d24fad5860b26bb8f91a4e5 |

Windows group A input bytes are independently compared to the pinned Linux
120-frame originals. Derived group B/C amplitudes are replayed byte-for-byte
from the same verified frame 6 using the captured integer RGB formula; alpha
is preserved. Group B contains three 43-frame ramps plus a separate 16-frame
Low calibration, not the older negative 40-frame experiment.

Group C directly reads original SDK internal state after encode, rather than
inferring state from packet quant tags. All three profiles must have exactly
seven frames and counters `0,2,3,2,1,1,0`. Frames 3 and 4 must each use an
actual preceding SDK packet whose size × 240 lies in the 80–100% band. The
gate checks the original trace against the manifest, all packet quant/scale
fields, profile tag -80, normalized **whole packets**, writer overbitrate,
limiter and the previous completed sample size consumed for this frame. The
writer result trace carries pre-encode previous bytes (zero on frame 0), whereas
the original SDK lastgopbitcount after encode carries the current packet bits;
these differently timed fields are checked against their own correct domains. Only parsed GUID/DATE/TIME/TIMC
value ranges are normalized; all other bytes remain significant.

## The original 40-frame ramp stays NEGATIVE

`p6_sdk_gap_negative_ramp` succeeds only if all 120 packets match AND the
fixed experiment is rejected as `NON_QUALIFYING_NO_HOLD`, with zero in-band
frames and zero hold witnesses in every profile. Unexpected hold evidence,
packet mismatch or missing coverage fails this negative control. It never
admits a profile. Positive qualification is the **separate** Windows group C
gate; `--require-hold` still rejects the original negative ramp.

## Portable reproduction

Bind the two read-only corpus roots privately. No paths are written to public
reports. Without these options CTest covers only public fixtures/contracts;
absence of a private test registration is not private-golden evidence.

~~~sh
cmake -S . -B build-regular -DCMAKE_BUILD_TYPE=Release
cmake --build build-regular --parallel 4
ctest --test-dir build-regular --output-on-failure

cmake -S . -B build-goldens -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_SDK_GAP_CORPUS_ROOT="$HYPERCINE_SDK_GAP_CORPUS_ROOT" \
  -DHYPERCINE_SDK_GAP_INDEX_SHA256=dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb \
  -DHYPERCINE_WINDOWS_SDK_ROOT="$HYPERCINE_WINDOWS_SDK_ROOT"
cmake --build build-goldens --parallel 4
ctest --test-dir build-goldens --output-on-failure
~~~

## Actually measured results

Linux x86_64, GCC 14.2.0, CMake 3.31.6, Python 3.13.5.
[Sanitized machine-readable evidence](SDK_GOLDENS_REFRESH_46.json) binds tested
implementation/test blobs and both encoder executables, without raw logs/paths.

| Complete run | Passed / total | Failed | Skipped | Wall seconds |
|---|---:|---:|---:|---:|
| Main implementation, regular Release | 55/56 | 0 | 1 | 74.27 |
| Corrected implementation, required regular Release | 55/56 | 0 | 1 | 75.50 |
| Corrected standalone, no private corpus | 118/118 | 0 | 0 | 199.18 |
| Corrected standalone, both private corpus roots | 125/125 | 0 | 0 | 432.69 |

The regular skip is `ffmpeg_oracle_qualification`, not a passed decoder oracle.
The two standalone counts are distinct configurations, not an aggregate.

The **current-main implementation was actually rerun before the ladder**:
Linux 120f Low/Medium/High/FS1 = 120/78/78/120 matched; Windows group A gives
the same 120/78/78/120, first Medium/High divergence at frame 17. Windows B
Low/Medium/High ramps = 43/39/41 out of 43 and Low calibration 16/16.
Windows C has 21/21 identical packets but **fails direct state parity**:
main counters `0,1,1,1,1,1,0` vs original `0,2,3,2,1,1,0` for every profile.
This is a concrete example of why equal packets alone cannot prove SDK state.
All four baseline gate invocations exit 1; these failures are not waived.

On the **corrected candidate**, locally executed positive tests are:

- `p6_sdk_gap_sequences`: Linux original SDK 480/480 packets, four ×120.
- `p6_windows_sdk_group-a-sequences`: independent Windows originals 480/480.
- `p6_windows_sdk_group-b-hysteresis`: 145/145 (3×43 +16 calibration).
- `p6_windows_sdk_group-c-sdk-state`: 21/21 packets, all 21 exact direct SDK
  counter/limiter/previous-input-size comparisons; enter, hold and leave.

| Direct original SDK profile | Prior packet bitrate at hold frames 3/4 | Counter at 3/4 |
|---|---:|---:|
| Low | 105,835,200 bps | 2 / 1 |
| Medium | 124,896,000 bps | 2 / 1 |
| High | 126,132,480 bps | 2 / 1 |

Historical `p6_sdk_gap_historical_pre325` preserves both original FAIL records
byte-for-byte using SHA-256 pins. `p6_sdk_gap_negative_ramp` verifies 120/120
packet parity while rejecting all three 40f ramps: zero in-band frames,
zero hold witnesses. The ramp is **still negative**.
`p6_windows_sdk_comparison_negatives` kills truncated/reordered traces,
wrong state, wrong previous bytes, same-length packet mutation and missing
output; its verified copied-packet baseline is a harness control, not encoder
proof. All seven comparison-site controls also passed under Python `-O`.
Public Windows gate contracts reject wrong/missing profiles, order, count,
state source, direct state/rate, hold coverage and index pins.

## Test-environment recovery

An initial public-suite attempt hit FFmpeg `ENOSPC / No space left on device`
in `filmscan3_geometry_external_decode` because the shared temporary filesystem
was full. That attempt was stopped and is **not** a completed passing suite.
The concurrent private attempt was also stopped rather than exposing later
external-decode tests to the same full temporary filesystem. Both complete
suites were restarted with `TMPDIR` bound to dedicated disk-backed test scratch;
no codec threshold, skip rule or reference requirement was changed. A prior
private attempt was separately stopped while correcting this new helper's
pre-encode previous-bytes vs post-encode current-bits timing-domain comparison.
Only the final complete runs count below. Raw attempt logs remain outside Git.

For a small tmpfs, set `TMPDIR="$HYPERCINE_TEST_TMP_DIR"` before CTest. The final
standalone runs use `ctest -j4 --output-on-failure` and include the full registered
inventory, not merely the previously failing subset.

## Qualification limits

The Windows manifest reports MSVC 19.44.35229, frozen SDK commit
`11574d0295771edccadd17af14af74a539f924c7` and static-library hash
`65ba13a8f7d26f5c94cc18520a6e7ce1e4ca5b35ae084d65b5b16b022b7fc84f`.
Those are pinned provenance declarations; this Linux replay does not rebuild
or independently audit the original Windows compiler invocation/ISA dispatch.
Original generator seed and explicitly set SDK FRMR remain NOT_MEASURED;
30 fps is the documented SDK fallback, not evidence of explicit FRMR.
No fresh Windows-native run, Metal/CUDA, 4K/8K, alternate timebase, sanitizer,
performance improvement, product admission or standards-conformance claim.
The regular full suite has an explicit unconfigured FFmpeg-oracle skip; it
must not be counted as a passed reference decode. Maintainer review and
exact-head hosted CI remain separate acceptance gates. Issues #46/#321 stay open.
