# PR #337: Linux x64 integration reconstruction of the original-S3 test-only change

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Status: **REVIEW — Linux integration reconstruction complete**, Linux x64
native, 2026-11-01; target base `main` `a7853be2bd387a232838f0e48b583c2b14ad1ba9`.

This is the "*Linux integration owner*" half of the 2026-11-01 maintainer next
action on PR #337. It is not a second Windows receipt, not a merge, and not a
Draft promotion.

## Final head and reconstruction

A normal `git merge origin/main` was performed; no rebase and no force-push.

- Previous reviewed head: `0429eebe99cbe92551e960dc49d705f4003f1442`
- Merge commit / final head: `27a3a91aa0f2e135db379be031161ff903303447`
- Parents: `0429eeb` (branch) and `a7853be` (current `main`)
- Branch: `codex/issue144-original-graph-20261030`
- Remote read-back `refs/heads/codex/issue144-original-graph-20261030` matches
  the final head exactly.

The merge introduced zero conflicts and zero content resolution: the merge
tree is a straightforward union.

## Landing inventory and untouched trees

Eight files, `+1788/-0` against `a7853be`. No `src/`, `include/`, `tests/`
interface, `upstream/` or build-system entry outside
`tests/decode_task_graph/CMakeLists.txt` is touched; `git status --porcelain --
upstream/` is empty before and after all building.

| Blob | Bytes | Path |
| --- | --- | --- |
| `c741f572a3cd33d2ccf7492582469995d9aae392` | 12922 | `docs/evidence/issue144-native-validation-20261030.json` |
| `903d5539b298c9b81a71e2e97352458bc2873f40` | 10667 | `docs/evidence/issue144-original-graph-asan-20261030.json` |
| `9688d44d2b1485b9aea55a8562fb0402e2630a19` | 10667 | `docs/evidence/issue144-original-graph-release-20261030.json` |
| `1ff8f0b2c95c98963238c203969bdc666cf28497` | 8410 | `docs/validation/ISSUE144_ORIGINAL_S3_GRAPH_20261030.md` |
| `2cd31e9d57495a65c49aacab1e1f2463c4af93a3` | 9387 | `tests/decode_task_graph/CMakeLists.txt` |
| `cd8c5f62852a5989217e767e94342cb51f06b956` | 6939 | `tests/decode_task_graph/replay_original_s3_144.cpp` |
| `37061c17ac3a1979f55cf9ce93c70c3f9f8ceb0e` | 4874 | `tests/test_spatial_attachment_replay_144.py` |
| `1bbbe4b7333536fd188ff8331892f6fa3d1ecd53` | 5421 | `tests/validate_spatial_attachment_144.py` |

**All eight blobs are byte-identical between `0429eeb` and `27a3a91`.** This is
the stated justification for carrying the Windows receipt forward: the landing
diff did not change, so the `0429eeb` native results apply to the unchanged
test/registration bytes. They are still recorded as results **of `0429eeb`**
and are not relabelled as `27a3a91` native evidence.

## Host and toolchain

Linux x64, AMD EPYC 7601, 48 logical CPUs, Debian 13 (kernel 6.12.107),
GCC 14.2.0, CMake 3.31.6, Ninja, CPython 3.13.5. CPU-only: no CUDA, no Metal,
no Vulkan.

## Configuration and registration

Two configurations from the exact head in a clean out-of-tree build directory.

| Configuration | Configure | Build |
| --- | --- | --- |
| Release, `HYPERCINE_BUILD_TESTS=ON` | exit 0 | exit 0, 145/145 targets |
| RelWithDebInfo, `-fsanitize=address,undefined`, `-fno-omit-frame-pointer` | exit 0 | exit 0, 145/145 targets |

Registration is conditional, exactly as designed:

- `s3_original_replay_protocol` is registered whenever a Python interpreter is
  found (test #10).
- `s3_original_stage_144` is registered **only** when
  `-DHYPERCINE_S3_ORIGINAL_ROOT=<dir>` is set (test #11).

A clean configure without the corpus root reports 60 tests; with a corpus root
it reports 61. No test is silently dropped and none is silently added.

### Fail-closed probe of the conditional registration

Configuring with `-DHYPERCINE_S3_ORIGINAL_ROOT` pointed at a **nonexistent**
directory keeps the registration and fails the test rather than skipping it:

```
11: S3 original replay BLOCKED or MISMATCH; no pass report; details redacted
2/2 Test #11: s3_original_stage_144 ............***Failed    0.10 sec
```

That is the required behaviour: an unresolvable original corpus is a failure,
not a green skip, and the message carries no private path.

## CTest results

Full configured suite, no `-E` exclusion, no `-R` filter.

| Configuration | Tests | Passed | Failed | Skipped | JUnit SHA-256 |
| --- | --- | --- | --- | --- | --- |
| Release | 60 | 59 | 0 | 1 | `d891c7168a465059255af15b6c9a1e3748715698db8801703d715124ac5b223d` |
| ASan+UBSan | 60 | 57 | 2 | 1 | `bc818b4279dd39681b86a9614a23f384e21bc511178bd758de58238694218872` |

Probe binary `replay_original_s3_144`, SHA-256
`7833064b8496304a5479238350487a572dd4687dfb15734cc356ddf5005139da`
(identical in the Release and corpus-root build trees).

### The single Release skip is pre-existing, not this PR

`ffmpeg_oracle_qualification` (#58) skips with a stated reason:

```
ORACLE_QUALIFICATION_BLOCKED reason=decoder build 7.1.5-0+deb13u1 is not on the
measured list (n8.0, 9.0.2)
```

This host carries FFmpeg 7.1.5, which is not a measured oracle version. The
test is unrelated to this PR and blocks itself honestly.

### The two ASan failures are pre-existing on clean `main`

`test_codec_roundtrip` and `test_yuy2_stability` fail under ASan+UBSan at the
exact head. I rebuilt **clean `origin/main` `a7853be`** in a separate worktree
with identical flags and reproduced both failures with identical stack roots:

```
SUMMARY: AddressSanitizer: heap-buffer-overflow upstream/Codec/spatial.c:537
         in FilterHorizontalRow16s
runtime error: left shift of 16 by 27 places cannot be represented in type 'int'
         upstream/Codec/codebooks.c:1141
runtime error: null pointer passed as argument 2, which is declared to never be
         null  upstream/Codec/encoder.c:728
```

Baseline result: `0% tests passed, 2 tests failed out of 2`. Both are in the
frozen `upstream/` SDK tree, are byte-identical before and after this PR, and
are **not** introduced or affected by this change. They are attributed as a
pre-existing frozen-tree finding, not as a blocker of #337, and I did not touch
`upstream/` to make them green.

## S3-specific evidence at this head

`s3_original_replay_protocol` — **PASS**, 8/8 controls, in both configurations.
Because the private six-pair original corpus is not on this host, the
corpus-dependent `s3_original_stage_144` was exercised only as the fail-closed
negative above. I am **not** claiming the 24/24 byte-exact original outputs or
48/48 identity rejections on Linux; that stays the Windows receipt's result.

## Adversarial non-decoder crash control (new, native)

The maintainer asked for a crash control if the contract does not already
distinguish it. The existing contract does distinguish it via return-code
gating, and I proved that here on a real process rather than by mocking.

A tiny C program prints a **well-formed** rejection witness and then dies by
`SIGSEGV`:

```
S3_IDENTITY_REJECTED no_output_access=1 drained=1
raw exit = 139
```

Driven through the unchanged harness at this head:

- `normal`: rejected — `graph replay failed`
- `wrong-band`: rejected — `identity control did not produce its required rejection`
- `wrong-channel`: rejected — `identity control did not produce its required rejection`
- no output directory was created in any mode

**0/3 false positives.** A forged witness plus a crash cannot pass as `exit 2`
identity rejection, because the harness requires the distinct `returncode == 2`
*and* the witness text *and* the absence of output. The control deliberately
forges the status text only; it does not perform an actual invalid memory
access.

## Hosted CI on the final head

Four workflow families are required; runs were observed for
`27a3a91aa0f2e135db379be031161ff903303447`. See the issue comment for the
final observed conclusions, since hosted runs were still in progress when this
file was written.

## What this does and does not establish

- Established: the test-only change merges cleanly onto current `main`, touches
  no production or frozen tree, registers conditionally, fails closed, passes
  the full Release suite with the same pre-existing single skip, and the two
  ASan failures are provably pre-existing on clean `main`.
- Established: the harness rejects a forged-witness crashing process natively.
- Not established here: the private-corpus 24/24 and 48/48 counts on Linux.
  Those remain bound to the Windows receipt at `0429eeb`, justified by
  byte-identical landing blobs.
- Unchanged: #337 stays Draft, #144/#47 stay open,
  `GraphProfile::Synthetic` and the pixel/release HOLD stay intact. No merge,
  no release, no rebase, no force-push.
