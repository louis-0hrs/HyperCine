<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Frozen reference and CUDA trigger gates — Issue #363

## Result and source binding

**Local Linux Release validation: PASS, 129/129 CTests, zero failures, errors or
skips; 343.55 seconds.** The complete Standalone runner also accepted its
registered/executed inventory, architecture and required external decode gates.

The tested worktree starts at `dca0c0707a0d460fb8cb7ca7d198c0a7aa4be8e7`.
The [machine-readable receipt](frozen_reference_gates_363.json) binds the exact
11 changed implementation, test, workflow and baseline files by SHA-256. Those
fingerprints were checked again after the final run. These two documentation
files were added afterward; no bound source file changed. The receipt does not
invent a commit identity for the then-uncommitted worktree.

The original SDK remains untouched. Its 222-file tree is
`837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`, identical to the root tree of original
commit `11574d0295771edccadd17af14af74a539f924c7`.

## Changes verified

The former `git status` checks could accept a committed reference change because
the checkout would be clean. `ci/check_upstream_immutable.py` now checks the
committed tree against the original tree pin, the complete staged inventory and
the actual materialized files. It rejects missing, changed, staged, conflicting,
untracked and ignored reference files. Git replacement objects are disabled;
case folding cannot hide an extra file. Native executable modes are checked
when `core.filemode` enables them. Index `assume-unchanged` and `skip-worktree`
flags cannot hide changed bytes.

Raw blob identity is preferred. A legitimate text checkout may differ only in
LF/CRLF representation, verified directly against pinned blob bytes. NUL and
other binary control-byte content cannot use that exception. The verifier does
not execute Git clean filters, diff drivers or filesystem-monitor hooks.

The verifier runs in CPU/CUDA workflows and the Standalone preflight. Its direct
check and real temporary-Git regression suite are registered in CTest and
required by the Standalone inventory. The real repository's `upstream/` is
never changed by the negative tests.

Both CUDA workflow path filters now include `cmake/**`; the root
`CMakeLists.txt` was already covered. Routing rule R15 and negative controls
require the CUDA build and its inventory self-test for PR/main changes confined
to those CMake modules. Documentation-only edits remain excluded.

The historical non-Mac job-step hashes are retained. An explicit #363 record
permits only the three reviewed immutability-command substitutions; the
comparison restores just those command texts before checking the original
hashes. All other step fields, runner choices and Mac fallback policy remain
under the original checks. Branch rules, services, permissions and production
runner configuration are outside this change.

## Executed checks

| Scope | Final local result |
| --- | --- |
| Complete Standalone Release gate | 129/129 CTests; zero skips/failures/errors |
| Immutable-reference controls | 26/26; repeated under `python -O` |
| Package imports plus Standalone guardrails | 58/58 unit tests |
| Routing, runner capability and Mac-scope checks | 66/66 unit tests |
| Static routing model | 7 workflows, 8 events, 66 job instances, 15 rules |
| Independent focused re-review controls | 6/6; zero skips; scoped acceptance |
| Final source fingerprint recheck | All 11 files unchanged |

Independent review found three bypasses in the initial local candidate before
publication: a clean filter could emit pristine text for changed bytes, an
assume-unchanged flag could conceal a mode change, and case folding could hide
an additional file on a case-sensitive filesystem. All three were corrected and
received discriminating regressions. A canary proves the final checker does not
execute a clean filter, including on successful verification. The independent
review also reran legitimate CRLF, original-CRLF and binary-EOL controls.

The final JSON/JUnit artifacts were read directly. The 4K artifact contains 19
independent FFmpeg decodes and a 16-frame sequence with matching per-frame pixel
hashes. The lowest finite p10 PSNR is **52.32134949615484 dB**, above the existing
35 dB threshold. The 120-frame manifest gate passed. Recorded SDK fixtures were
replayed; this is not a fresh original-SDK execution. The geometry report keeps
`source_sdk=REFERENCE_BLOCKED` and `eight_k=PENDING_8K`.

## Environment and limits

Linux x86-64; GCC 14.2.0; CMake/CTest 3.31.6; Python 3.12.14; Git 2.52.0;
FFmpeg 7.1.5. Build: Unix Makefiles, Release, Standalone/tests/external decode ON,
upstream/CUDA/benchmarks OFF. Temporary synthetic media used a dedicated
filesystem-backed directory with sufficient capacity.

Windows/MSVC/PowerShell and native macOS were not executed. Windows CRLF handling
was exercised through real Git fixtures on Linux. The native executable-bit
control is POSIX-specific; the case-collision control requires a case-sensitive
filesystem. CUDA compilation, GPU algorithms, Metal/Vulkan, ASan/UBSan and private
original-corpus replay were not qualified by this run. No performance improvement,
release qualification, GitHub workflow execution or branch-protection change is
claimed.

An earlier full-suite attempt encountered a temporary-file write failure during
the 120-frame manifest test and was stopped. A subsequent attempt was stopped
to apply the independent review corrections. **Neither is a passing receipt.**
The result above comes only from the complete final run after those corrections.

## Reproduction

Run from the repository root with CMake/CTest, a C++17 toolchain and FFmpeg/ffprobe
in PATH. Configure `HYPERCINE_AUDIT_TMPDIR` privately to an existing disposable
directory with sufficient disk capacity; do not publish its local value.

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 2
TMPDIR="${HYPERCINE_AUDIT_TMPDIR:?configure a private temporary directory}" \
  python3 ci/run_standalone.py --build-dir build --system Linux --machine x86_64
python3 -m unittest ci.test_upstream_immutable ci.test_standalone_gate
python3 -O ci/test_upstream_immutable.py
python3 -m unittest ci.test_workflow_event_matrix ci.test_runner_capability \
  ci.compute_policy.tests.test_routing_scope
```
