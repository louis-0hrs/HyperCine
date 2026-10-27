<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Windows reference stack refresh, 2026-10-27

PR #277 is reconciled forward with main
`45700478cbf088b6768c45d93a91ba87a082a014`, preserving its original parent
`0c84006bc75ee168bc8b13a5a036c9f5aa21a5ab`. No history is rewritten.
The tested precommit tree is `9f29eff1b040c2ddbc84121c31fddbf63d6d1a44`;
only this report and sanitized evidence are added after that source snapshot.
The publication comment identifies the resulting merge commit. Configure-time
HEAD was the original child commit; this distinction is retained rather than
claiming that an uncreated commit identifier was embedded in the binaries.

The only manual merge resolution is `docs/UPSTREAM.md`. Both the Windows
fresh-process observation section and main's issue #48 sized-chunk overflow
section survive verbatim. The CMake merge retains the reference-process
contract and current main's session-ISA and geometry tests. Frozen upstream
has no changes. No new codec algorithm or public interface change is authored
by this reconciliation; API changes inherited from main remain main's work.

## Environment and checks

Windows 11 Home 10.0.26200, VS2022 x64, MSVC 19.44.35229.0, Windows SDK
10.0.26100.0, CMake 3.31.6, Release. CUDA uses NVCC 12.8.61, SM89,
RTX 4090 Laptop GPU and driver 610.62. Standalone CPU/CUDA builds have
upstream OFF, tests ON and required FFmpeg/ffprobe ON. The original-SDK
configuration has standalone OFF, upstream ON and CUDA OFF. All three
complete native builds pass; large CTest suites run sequentially.

| Full suite | PASS | FAIL | SKIP | Time |
|---|---:|---:|---:|---:|
| Original SDK | 44 | 0 | 2 | 65.15 s |
| SDK-free CPU | 78 | 0 | 1 | 337.13 s |
| SDK-free CUDA | 84 | 0 | 1 | 304.96 s |

Per-test results are exported under
[`windows-stack-refresh-20261027/`](windows-stack-refresh-20261027/).
Raw logs, JUnit, subprocess output and local paths are excluded. SDK skips
are `standalone_sdk_decode_3_packets` and `ffmpeg_oracle_qualification`;
the standalone platform skip is `cuda_filmscan3_output_dir_negative`, whose
live symlink checks need Windows symlink privilege. These remain blocked
evidence, never passes.

All 19 export-contract tests pass: exact integer types and field bounds,
duplicate/nested duplicate keys, NaN/Infinity, typed inventory allowlists,
path/host/credential canaries and fixed redacted failure documents. All four
native file/input protection negatives pass without changing the source
packet or overwriting protected outputs.

The fresh-process reference matrix runs the fixed 21 processes with no
retries. The existing immutable input SHA256 is
`5e8ea4785c00dd6406e85be7ddee426dd093e1007aca7effa9e1aff4f95aa1ca`.
This run reports `BLOCKED_CONFIGURATION_DEPENDENT_REFERENCE`, exit 77,
with two output hashes. This does not erase historical within-cell
nondeterminism. Requested thread counts remain distinct from unobserved
decoder workers; neither result qualifies a pixel oracle or identifies a
causal mechanism. No new original corpus export is made.

CPU/CUDA packet parity, independent FFmpeg decodability and unresolved original
SDK parity remain separate. PR #274 was independently tested at
`b6148d82e96de6b0aed2fa2282397e78bfa05e7d`: full CUDA CTest 81 PASS / 1 FAIL /
1 SKIP. Windows command quoting and CRLF parsing block that registered test;
its Linux-owned source is unchanged here. Manual FFmpeg decode of its retained
1080p and 4K pairs agrees, without converting the failed CTest into PASS.

No sanitizer or performance milestone is claimed by this documentation and
integration refresh. Existing benchmark measurements are historical only.
The child is offered for parent review; neither protected main nor the parent
branch is automatically merged. Next action: review the refreshed child into
the parent reference stack while preserving the original-SDK blocker.

