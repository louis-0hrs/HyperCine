<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Standalone gate repeatability and CTest slot check

Bounded infrastructure evidence for #349. Base head
`d8b6adc4b95809776b7d4f635d277405908db13d`; the earlier destructive candidate `0c80a9f768bcac5dd6195b314db004e0820286ea`.
The current correction replaces that approach; see below.

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

## Replacement: non-destructive isolated scratch (17 Nov 2026)

The original candidate HEAD \`0c80a9f768bcac5dd6195b314db004e0820286ea\`
used \`--replace-owned-output\` and was **unsafe**: a forged JSON object
containing known report keys could cause deletion of unrelated files with
known destination names. The earlier claimed ownership check is withdrawn.

This correction removes that flag, restores the original strict,
create-exclusive oracle and its old contract tests byte-for-byte, and runs
the CTest replay through \`tests/run_isolated_oracle_replay.py\`.
Each invocation creates a new private, random temporary subdirectory using
the standard library's exclusive creation, runs the existing oracle there in
the same Python process, and only cleans up **that new subdirectory**.
Previously existing \`comparison.json\`, \`control.avi\`, other receipts
and arbitrary files in the parent are never examined, overwritten or removed.
This avoids the repeat-run collision without a forged ownership heuristic or
a second process supervisor. The retained fixture source directory cannot
be selected as a scratch parent, and a symlinked scratch parent is refused.

Five new executable safety controls cover repeated invocations with old and
foreign entries, failure cleanup without clobber, fixture overlap, symlinked
parent and CMake/strict-inventory wiring. A new mandatory CTest registers
these controls. The wrapper keeps the oracle's stdout/exit verdict; outputs
are ephemeral scratch, while CI/CTest logs remain the published receipt.
This intentional change means no persistent replay JSON is uploaded from
the original fixed directory; no PASS or prior evidence is restored.

The benchmarks and repeatability counts below are **historical results from
the superseded destructive candidate**; they are not acceptance evidence
for this new implementation. New exact-head Windows/Linux/ASan and native
repeat-run results must be checked before merge.


## Executed evidence

Historical test-level repeats on the superseded candidate (not the repaired HEAD):

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
