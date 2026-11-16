<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Standalone gate repeatability and CTest slot check

Bounded infrastructure evidence for #349. Base head
`d8b6adc4b95809776b7d4f635d277405908db13d`; one additive local change on top
(`--replace-owned-output` for the standalone SDK-oracle replay registration).

This is a test-harness repeatability result on one Linux x86-64 host. It is not
codec, SDK, GPU, VC-5 or GoPro qualification, and it does not replace any hosted
lane.

## Defect found on the untouched base head

Running the complete strict standalone gate twice in the **same** build tree,
without rebuilding and without any source change:

| Execution | Result | Failing test | Failure text |
| --- | --- | --- | --- |
| first | 158/158 PASS | none | none |
| second | 157/158, exit 8 | `standalone_windows_oracle_replay` | `FAIL: SDK oracle replay rejected or unavailable` (0.07 s) |

The second failure was not a codec or oracle result. `tests/compare_sdk_oracle.py`
deliberately refuses occupied destinations (`unsafe or occupied oracle
destination`) so that earlier evidence cannot be overwritten, but the CTest
registration used one fixed output directory. Every repeated run therefore
reported a product-looking failure that was caused only by the previous run's
own evidence. Hosted CI starts from a clean workspace, so the defect was
invisible there.

## Fix

`tests/compare_sdk_oracle.py` gains an opt-in `--replace-owned-output` that the
standalone registration passes; no other caller changes.

Replacement is permitted only when the destination directory is provably one
previous run of this harness:

- `comparison.json` must parse as this harness's report and carry every
  `ORACLE_REPORT_KEYS` field;
- every directory entry must be a known destination name, a regular file and
  not a symlink;
- the retained fixture input tree, path escapes and symlinked destinations keep
  refusing.

Any foreign entry, unparsable report or symlink still refuses **before** a
single file is removed. Without the flag the previous strict behaviour is
unchanged, so the existing "never clobber prior independent evidence" tests and
`write_exclusive` semantics still hold.

## Executed evidence

Exact test-level repeats on the fixed tree (no directory cleanup between runs):

- `standalone_windows_oracle_replay`: 3/3 PASS on consecutive executions.
- Full strict gate in one build tree: 158/158 PASS twice (465 s and 465 s).
- Negative control: adding an unrelated file to the replay output directory
  makes the test refuse again, the unrelated file and the previous evidence
  remain byte-identical.

CTest slot check on the same build (full strict gate, `HYPERCINE_STANDALONE_CTEST_JOBS`):

| Slots | Run | Registered | Passed | Skipped | Failed | Wall s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | A | 158 | 158 | 0 | 0 | 464.6 |
| 1 | B (same tree) | 158 | 158 | 0 | 0 | 465.1 |
| 2 | C | 158 | 158 | 0 | 0 | 234.5 |
| 2 | D (same tree) | 158 | 158 | 0 | 0 | 235.8 |

Both settings produced the identical 158-test pass set with no skip, no
parallel-only failure and no repeated-test failure. The two-slot wall time is
about half the serial wall time on this host because the suite is dominated by
independent long tests; that is an observation for this host, not a guaranteed
hosted-runner percentage.

Host facts: Linux x86-64, 48 logical CPUs, GCC 14.2.0, CMake 3.31.6,
Python 3.13.5, FFmpeg/ffprobe 7.1.5, Ninja 1.13.

## Limits

- One host, n=2 per slot setting. Variance on hosted runners and on Windows is
  not measured here; the Windows two-slot lane therefore remains provisionally
  retained, exactly as #349 records.
- Only the fixed-defect repeatability and the slot comparison are claimed. No
  test requirement was relaxed, no test was skipped and no PASS was cached.

## Receipts

Per-run machine-readable receipts, JUnit digests and the exact commands are in
[standalone_repeatability_20261116.json](standalone_repeatability_20261116.json).
The four JUnit files share the identical 158-test pass set; only their
timestamps and durations differ.
