<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Private Golden preservation of the October 28 RCA artifacts

Status: **COMPLETE_SHA256_VERIFIED**, 2026-10-28.

The previously local-only replay artifacts have been copied to the bound private
Golden store as native loose files. This is artifact preservation, not a new
decoder experiment, independent decoder acceptance or release qualification.
Public evidence remains in #299; decoder implementation remains #292 / #298.

## Location

Use the locally configured `HYPERCINE_GOLDEN_DIR`. The Golden-relative folder is:

```text
rca160-2026-10-28-preservation-01/
```

The resolved storage path is private and is intentionally not published.
This directory was reserved exclusively; existing Golden files were not
overwritten. Original local artifacts were copied, not moved or deleted.

## Preserved inventory

**2,034 source files / 1,713,257,925 bytes**, plus the generated manifests,
completion receipts and private README files. No ZIP/TAR/7z was created.

| Relative directory | Contents |
|---|---|
| `original-checkout/build-rca-all/` | Complete three-channel experiment: setup, hooks, worker, runner, private generated decoder TU, executable, build context/logs and all measured raw snapshots |
| `original-checkout/build-rca-all/matrix/` | Canonical 28-call FULL/HALF run: 600 coefficient snapshots, 28 BGRA images, plans, ledgers and summaries |
| `original-checkout/build-rca/` | Complete earlier experiment build, stock/probe executables, exact SDK library, diagnostics, exploratory and canonical runs |
| `original-checkout/build-rca/matrix-final/` | Canonical 96-call run and all 96 BGRA images |
| `original-checkout/.worktrees/issue120-187-release-gate/build-native/guard-smoke/capture/` | Both immutable 900396-byte host/device input packets |
| `original-checkout/upstream/`, `include/`, `src/`, `cmake/`, `tests/` | Frozen SDK source and the actual wrapper/test/build source context needed to interpret or rebuild the harness |
| `original-checkout/build/rca*` | Retained original build, diagnostic and validation logs/records |
| `capture-generator/` | Original capture-generator test source/executable, guard-smoke runner, build settings and safety-test fixtures/logs |
| `public-review-299/` | Published sanitized measurement evidence, reports and SDK-free offline audit |
| `preservation-tool/` | The one-way copy/verification tool used for the primary preservation |

The capture-generator's `oversized.cfhd` and `wrong_geometry.cfhd` are deliberate
invalid guard fixtures, not valid decode golden samples. Historical
`matrix-v1`, `matrix-v2` and pilot/failed-build results are retained for
forensics only; they must not be added to the canonical acceptance counts.
Source Git databases, credentials and Python bytecode caches were not copied.

## Integrity receipts

`MANIFEST.json` covers the primary 2,018 files. The separate
`SUPPLEMENTAL_MANIFEST.json` covers 16 generator/build/guard artifacts.
`PRESERVATION_INDEX.json` combines their counts and verification status.
The primary README describes the primary copy; `README_SUPPLEMENT.md`
explains the additional generator context.

| Receipt | SHA-256 |
|---|---|
| `MANIFEST.json` | `4cda2e52379014b3ba6c70c6d3de5aee06a037d6147c88e664b06d41f25b9dcd` |
| `SUPPLEMENTAL_MANIFEST.json` | `4036b7ba9d226508262d2b6f2398ba2398331b8ba758af4a4cc7928192aee089` |
| `PRESERVATION_INDEX.json` | `c9a686881f445d87d90e3c5eb3ea3232f36c4d9bf7f73e6471922788856c6eee` |

Every copied file was created exclusively and independently read back from the
destination to verify size and SHA-256. Source changes during copying were
checked. The preserved 600 coefficient snapshots, 28 all-channel BGRA files,
96 earlier BGRA files, input packet, harness sources, three executables and SDK
source/library dependencies additionally match their historical recorded hashes.
The published offline ledger audit also passed directly against the NAS copy.
The sanitized receipt is `golden_rca160_preservation_receipt.json`.

## Use for independent acceptance

Resolve the location locally; do not post the variable value:

```powershell
$preserved = Join-Path $env:HYPERCINE_GOLDEN_DIR 'rca160-2026-10-28-preservation-01'
(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $preserved 'PRESERVATION_INDEX.json')).Hash
python -B (Join-Path $preserved 'public-review-299/tests/rca160/verify_evidence.py')
```

For a full integrity review, hash every path in both manifests against its
recorded byte count and SHA-256. Raw planes and exact instrumentation are now
available privately, so an authorized reviewer can independently recompute
snapshot differences or repeat the decoder experiment; that further acceptance
has not been performed merely by copying the files.

Do not execute or regenerate in the preservation directory. Copy the required
loose sources and inputs to a fresh local working directory. Historical CMake
caches contain old machine paths; regenerate a clean build using the preserved
setup and source context. The original runner refuses an existing matrix
directory. External MSVC/CMake/Windows runtime installations were not copied.

The SDK source, generated translation unit, executables, raw planes/images,
packets and private build logs remain **private Golden data**, not public
repository attachments. Their presence in this preservation is not permission
to upload them or disclose the resolved storage path.
