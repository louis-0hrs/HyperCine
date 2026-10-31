# Issue #321: retained Windows build artifacts and bounded replay

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Status: **REVIEW**, native Windows x64, 2026-10-30. Base main `04c348f`.
This follow-up supplies the three previously absent historical artifacts,
checks their actual bytes against the original pinned manifests, and executes
the retained programs on a small subset of the existing immutable inputs.

## Artifact recovery and retained build records

All **818/818 Windows A/B/C indexed files** pass SHA-256, length, index-digest
and exact-inventory verification before any capture executable runs. The
original A/B/C manifests bind these recovered files:

| Retained artifact | Bytes | Actual SHA-256, equal to original manifest |
| --- | ---: | --- |
| SDK static library | 2778866 | `65ba13a8f7d26f5c94cc18520a6e7ce1e4ca5b35ae084d65b5b16b022b7fc84f` |
| A/B API capture executable | 1021440 | `6b1b88ddbfc45f56390e77e63ac4bcbb496723b86af292ac788a1531f6924d53` |
| C state capture executable | 1021440 | `6e218944b0a5724c3da4e3482a9efe11fdfd3447a204b81d4456b03847e7106a` |

Both retained capture sources and their CMake recipes also match the indexed
corpus copies byte for byte. Retained compile records name those source files;
retained link records name this SDK library, resolving parent components and
case differences. This is evidence from retained local records, **not a
cryptographic source-to-binary or historical execution attestation**.

The retained CMake compiler records distinguish two compiler versions:

| Build | Recorded compiler | Selected tokens found in compile records |
| --- | --- | --- |
| A/B and C captures | MSVC 19.44.35229.0 | `/O2 /Ob2 /MD /std:c++17` |
| Original SDK archive | MSVC 19.44.35228.0 | `/O2 /Ob2 /MD /std:c++17` |

Tokens are selected observations across each target's retained records, not
complete per-translation-unit command lines. No ISA dispatch qualification is
inferred. Compiler/compile/link record digests and source/recipe digests appear
in the [computed audit](../evidence/issue321-retained-provenance-20261030.json).

## Minimal controlled replay

The hash-bound original executables are run without rebuilding or modification.
Outputs go to a new private scratch directory, never into the corpus. Each
invocation creates a fresh encoder session and receives the pinned source bytes
in manifest order. Process errors, timeouts, state or packet differences fail
the audit; no retry selection or skip result is accepted.

| Executable | Inputs selected | Normalized original packet equality |
| --- | --- | ---: |
| A/B API capture | Frame 0 of Low, Medium, High, FilmScan1 | 4/4 |
| C state capture | Frames 0..6 of Low, Medium, High | 21/21 |

All **25/25** normalized packets match; **0/25** raw packets match because
volatile capture metadata differs. Normalization uses the existing bounded
parser's checked GUID/DATE/TIME/TIMC fields. Coefficients and quant/scale tables
remain part of the compared packet bytes. This is a new bounded execution
witness for these inputs, not authentication of every historical invocation.

All **21/21** C trace rows additionally match the original SDK overbitrate,
FSratelimiter and lastgopbitcount. For each profile, overbitrate is
`0,2,3,2,1,1,0`, FSratelimiter is zero, and lastgopbitcount equals actual emitted
packet bytes multiplied by eight. Frames 3/4 are in the 80–100% band under the
source-fallback 30-fps calculation while the counter descends; frame 5 is the
constant-state hold, and frame 6 leaves. These remain distinct observations.

## Private durable companion archive

The recovered files and retained raw build records are stored separately under
`HYPERCINE_GOLDEN_DIR`, relative root
`golden/windows/qualities/2026-10-30-issue321-retained-build-provenance`.
This new companion contains **17 indexed files**, plus its own transfer index.
Full destination readback checked all **18/18** file lengths and SHA-256 values
and exact inventory. Existing corpus directories and transfer indexes were
not changed. No new bulk reference export was performed.

Transfer-index SHA-256:
`dabb98270e8b8a11ebb02001bebd5f8077d8cc2d315732a4783862ff65e1c424`.
The [archive receipt](../evidence/issue321-retained-archive-20261030.json)
also pins its manifest. Raw retained compiler/compile/link records contain
private paths: keep them and the binaries private, outside GitHub.

Independent archive verification uses the existing generic index verifier:

```python
import os, sys
from pathlib import Path
sys.path.insert(0, 'tools')
from verify_windows_sdk_corpus import verify_index
root = Path(os.environ['HYPERCINE_GOLDEN_DIR']) / (
    'golden/windows/qualities/2026-10-30-issue321-retained-build-provenance')
verify_index(root, 17,
    'dabb98270e8b8a11ebb02001bebd5f8077d8cc2d315732a4783862ff65e1c424')
```

## Reproduction and validation

### 2026-10-31 CI normalization correction

Review #5719980168 on PR #334 identified a host-dependent link-input check:
only the expected library path used `Path.resolve()`, while retained record
paths used lexical Windows normalization. An alias, junction or symlink in a
temporary root can therefore change only the expected spelling.

The checker now applies `ntpath.normcase(ntpath.normpath(...))` to both
spellings, without filesystem resolution in this comparison. It requires
full-path equality under Windows case/separator/parent-component rules;
matching basenames alone is insufficient. Artifact SHA-256 binding remains
a separate required gate. This policy does not establish filesystem identity
between differently spelled aliases or historical execution authenticity.

The contract now has 17 deterministic controls, including simulated root
canonicalization, parent components on either side, Windows case/separators,
and rejection of the same basename in another directory. The root-alias
control failed with `ValueError: link input record differs` before the fix.
Both real retained capture link records were also checked against the exact
SDK input and expected parent-component spelling; wrong-directory controls
were rejected. The unchanged companion index verified all 17 indexed files.

Revised-source validation is recorded separately in
[the correction inventory](../evidence/issue321-path-normalization-20261031.json).
The 2026-10-30 results and hashes below remain historical evidence for the
previous checker. Fresh hosted Windows/macOS/Linux CI on a committed revised
head is still required before treating PR #334 as integration-ready.

Bind the private original capture-build directory and SDK library locally via
`HYPERCINE_ISSUE321_CAPTURE_BUILD` and `HYPERCINE_ISSUE321_SDK_LIBRARY`.
The audit command below uses the original retained build layout, including its
compile/link records; copied companion files have a separate portable index
verification above. Use a new scratch/output name for each execution.

```powershell
python -B tools/verify_issue321_provenance.py `
  --historical-root "$env:HYPERCINE_ISSUE321_CAPTURE_BUILD" `
  --sdk-library "$env:HYPERCINE_ISSUE321_SDK_LIBRARY" `
  --replay-dir private-scratch/issue321-new-replay `
  --output issue321-new-audit.json
```

Without `--replay-dir`, the command verifies artifacts/records without executing
the binaries. It refuses existing reports and replay destinations and rejects
destinations inside the corpus. Process diagnostics and private paths are not
included in public JSON.

On native Windows x64, MSVC 19.44.35229.0, CMake 3.31.6 and Python 3.10.9:
configure `HYPERCINE_BUILD_QUALITY_STATE_ONLY=ON`, upstream/benchmarks/CUDA OFF,
tests ON; build Release; run `ctest --test-dir build -C Release
--output-on-failure`. Complete configured suite: **41/41 PASS, zero failures or
skips**, 43.72 seconds. The registered provenance contract contains **13**
synthetic controls, including altered artifact bytes, absent/ambiguous records,
wrong compile/link inputs, private-command exclusion, packet/state/order/length
divergence and rejection before process execution for a bad input hash.
The [sanitized validation inventory](../evidence/issue321-native-validation-20261030.json)
records every CTest result and hashes of the final checker, tests and audit receipts.

## Remaining boundaries and next action

Recovery resolves the previous missing-artifact gap, not every provenance gate.
The exact pristine-source-to-archive build has not been independently reproduced.
Retained records cannot prove the original process history, original input
generator seed, source-to-binary authenticity or actual playback cadence. No
FRMR measurement has been added; 30 fps remains a source-fallback calculation.
No Linux executable reproduction, independent pixel decoder, GPU, performance,
product or SMPTE/VC-5 conformance qualification is claimed.

Next: review the recovered artifact bindings and private companion index together
with the separately published exact-head #322/#331 consumer evidence. Decide
which remaining historical provenance limits are acceptable or require a new
source-controlled build experiment. Keep #321 open; no merge/release is implied.
