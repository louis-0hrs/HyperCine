# Issue #321 / PR #334: repeated original-SDK witness and build-origin inspection

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Date: 2026-10-31. Replay checker executed from `9e263c2dbea86a8fee4c10e0372924a9346755f0`.
Scope: a fresh native Windows execution and direct byte checks against retained
private originals. This is a new witness in the same machine/trust environment,
not a separate party's attestation. No historical receipt is overwritten.

## Original-artifact and execution confirmation

The three original artifact hashes and lengths were checked again using direct
`hashlib.sha256` reads of the archived files, separately from the replay checker.
They match the original pinned SDK-library and capture-executable bindings.
The existing checker then verified all **818/818** indexed Windows corpus files
and used the original retained capture executables, without rebuilding them.

| New bounded execution | Result |
| --- | --- |
| API capture: Low/Medium/High/FilmScan1, frame 0 | 4/4 normalized packets equal |
| State capture: Low/Medium/High, frames 0..6 | 21/21 normalized packets equal |
| Direct SDK state rows and actual packet-bit counts | 21/21 equal |
| Raw packet equality | 0/25; volatile metadata differs |

All new per-frame observations agree with the 2026-10-30 report. The new
[replay evidence](../evidence/issue321-independent-replay-20261031.json) retains
the checked artifact/record hashes, packet hashes and state observations.
Only the existing checked volatile metadata fields are normalized. This is
bounded replay of seven sessions, not re-export of the full corpus.

## Additional evidence about the build origin

The original SDK build cache still points to a readable source checkout.
Its **currently observed** HEAD is `aec37c1f990a4b3dda3e3103154ec2b4e3bdfa2b`;
that is not asserted to be the commit used at the historical build time.

The pinned SDK compile record contains 67 caret-prefixed source input keys.
All 67 current source files match raw Git blob bytes from pristine GoPro
baseline `11574d0295771edccadd17af14af74a539f924c7`, without needing line-ending
normalization. Windows TLog casing is mapped to the unique baseline Git path.

The original library contains 67 ordinary object members, all marked AMD64,
plus the two linker members and a longnames member. Each ordinary member's
complete payload is byte-identical to its still-present retained build object:
**67/67 exact matches**. Member names may contain private paths and are omitted
from published evidence; member indices, sizes and SHA-256 values are retained.
Parsing follows the [Microsoft COFF archive layout](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#archive-library-file-format).

The [source/object inventory](../evidence/issue321-origin-inspection-20261031.json)
records the two comparisons separately. Equal counts do not establish a
source-to-object causal mapping. Current sources may have changed since the
build, and retained objects/records are not authenticated process history.

Reproduce from this repository on native Windows, binding private locations
locally and choosing a new output file:

```powershell
python -B tools/inspect_issue321_origin.py `
  --sdk-source-root "$env:HYPERCINE_ISSUE321_SDK_SOURCE_ROOT" `
  --compile-record "$env:HYPERCINE_ISSUE321_SDK_COMPILE_RECORD" `
  --library "$env:HYPERCINE_ISSUE321_SDK_LIBRARY" `
  --output build/issue321-origin-new.json
```

The tool reads current sources, baseline blobs, the archive and retained objects;
it does not compile or execute them. Exit zero means observations were written:
inspect all equality fields, machine identifiers and the stated scope. Missing
objects are recorded as unequal; malformed archives or unresolvable source
bindings fail. Four synthetic archive controls cover longnames/odd padding,
truncation, invalid offsets and nonarchives in the registered provenance suite.

Validation: **21/21** standalone provenance/origin controls and **41/41** tests
in the complete configured quality-state-only Release CTest suite passed,
zero failures/skips, 43.23 seconds. The
[validation inventory](../evidence/issue321-independent-validation-20261031.json)
binds the executed source bytes and new evidence files. Hosted CI must evaluate
the new committed head separately from the previously green `9e263c2` head.

## Remaining historical gap

Historical pristine-source-to-binary attestation remains **NOT_ESTABLISHED**.
The 67 compile-key sources exclude transitive headers and generated inputs;
the compiler executable, complete environment and per-unit commands have not
been reconstructed as an authenticated historical build. The next experiment
would freeze all source/header/recipe/toolchain inputs and perform an isolated
rebuild, comparing objects and explaining differences before making a stronger
claim. A matching rebuild would support reproducibility, not prove past execution.

The recorded SDK compiler version remains 19.44.35228.0, distinct from capture
19.44.35229.0. Original generator seed and actual playback cadence remain
unmeasured. No fresh full export, performance, decoder, GPU or VC-5 qualification
is implied. Keep #321 open and PR #334 Draft pending the scoped evidence decision.
