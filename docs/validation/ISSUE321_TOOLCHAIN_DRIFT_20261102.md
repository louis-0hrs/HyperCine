# Issue #321 / PR #334: compiler drift at the retained SDK build path

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Date: 2026-11-02. Continued from `1554d403ff9b16dbbaa6bdace7853df5f9e5ab6a`.

The compiler executable named by the retained SDK CMake record still exists,
but executing it with `/Bv` now reports **19.44.35229.0**. The retained record
states **19.44.35228.0**. File existence at that path therefore cannot establish
availability of the historical compiler. This is an observation of current
installed bytes versus retained records, not proof of when or why they changed.

| Observation | Result |
| --- | --- |
| Retained SDK compiler-record SHA-256 | `a5c5754928e40e21838b2182d534a58cf6724a42251b864672dac513f899f233` |
| Current compiler executable SHA-256 | `fe251ef50a1545b1b0835ee17b1e785459712b38d79e45b5c1d3d28970a36619` |
| Recorded / current reported version | 19.44.35228.0 / 19.44.35229.0 |
| Probe result / exit | `TOOLCHAIN_VERSION_DRIFT` / 2 |
| Historical compiler binary identity | `NOT_ESTABLISHED` |

The new stdlib-only `tools/verify_issue321_toolchain.py` reads the explicitly
selected local CMake record and invokes its trusted compiler with `/Bv`, without
compiling. It hashes the record and current compiler and checks both remain
unchanged during the probe. It rejects missing/ambiguous records, missing or
ambiguous versions, failed processes and timeouts. Public JSON excludes private
paths and raw diagnostics. Exit 0 means version agreement only; exit 2 writes
the measured drift report; exit 1 means the probe failed. Existing reports and
outputs inside the locally configured corpus are rejected before execution.

Reproduce on native Windows with a locally bound path to the retained record:

```powershell
python -B tools/verify_issue321_toolchain.py `
  --compiler-record "$env:HYPERCINE_ISSUE321_COMPILER_RECORD" `
  --output build/issue321-toolchain-new.json
```

The recorded compiler path is executed, so use a locally trusted record. The
CLI checks the configured corpus boundary and therefore needs read access to
that location even though the probe performs no corpus writes. Synthetic tests
are corpus-free and invoke no actual compiler.

The [machine-readable receipt](../evidence/issue321-toolchain-drift-20261102.json)
binds this measurement and the checker source. Ten added deterministic controls
cover version agreement, drift at the same path, missing compiler, invalid or
ambiguous records, failed or ambiguous probes, timeout, changed artifacts,
nonzero drift exit, existing output and corpus output rejection. They join the
21 existing provenance/origin controls in the registered CTest contract.

Native validation: Release build succeeded; the complete configured
quality-state-only CTest suite passed **41/41**, zero failures/skips, 43.12 s.
The [validation inventory](../evidence/issue321-toolchain-validation-20261102.json)
binds executed source hashes and individual test outcomes. This CPU-only
configuration does not qualify a full codec or GPU build. Hosted CI must
evaluate the revised committed head separately.

A fresh [retained-artifact replay](../evidence/issue321-retained-replay-20261102.json)
also verified all **818/818** indexed Windows A/B/C files and **17/17** indexed
private companion files. The unchanged companion index remains SHA-256
`dabb98270e8b8a11ebb02001bebd5f8077d8cc2d315732a4783862ff65e1c424`.
The original executables reproduced **25/25 normalized packets** (four profile
frame-0 cases and three seven-frame state sequences), with **21/21** direct
SDK states equal. Raw equality remains **0/25** because volatile metadata
differs. This is another execution in the same Windows machine/trust
environment, not independent third-party attestation. All new replay files
remain in local scratch; no original or companion corpus file was written.

This finding does not invalidate the original archived library hash or the
original-executable replay. It does prevent assuming that the compiler at the
retained path is the version in the historical SDK record. No purported exact
historical rebuild was performed with the different compiler. A rebuild with
the installed version would be a separately versioned experiment.

Next action: recover and hash-bind the recorded compiler/toolchain (including
headers, libraries and commands) for a controlled rebuild, or make an explicit
maintainer decision accepting the documented historical provenance limits.
Neither current-version agreement nor a matching rebuild would authenticate
past execution. Seed and actual playback cadence remain unmeasured. Keep #321
open and PR #334 Draft; no bulk exports or product qualification are implied.
