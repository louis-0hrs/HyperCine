<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Public October 28 RCA records for #160

This publication closes the local-only evidence gap in
[#160](https://github.com/louis-0hrs/HyperCine/issues/160#issuecomment-6472901842).
It is a frozen measurement record, not new decoder implementation. Further
implementation remains in #292 / [Draft #298](https://github.com/louis-0hrs/HyperCine/pull/298).

## Contents

* `sdk-rca-2026-10-28/`: complete 96-call plan and event ledger, pixel-difference
  summaries, original verifier result, source/binary provenance and the historical
  full/focused test inventories. The stale Phase2C/D objects in the initial
  historical run are explicitly recorded, not hidden.
* `sdk-rca-all-channels-2026-10-28/`: complete 28-call plan, event ledger,
  per-snapshot hashes and first row-major signed-coefficient differences,
  original verifier result and source/archive provenance.
* `rca160-public-manifest.json`: SHA-256 inventory of all eleven JSON records.
* `tests/rca160/`: deterministic offline audit and negative tests. Historical
  verifier predicates are retained in readable modules with explicit checks,
  so Python optimization cannot remove them.

The historical JSON bytes are preserved, including their original line endings,
with a scoped `.gitattributes` rule. In particular the all-channel summary and
verifier result retain the SHA-256 values already published in the issue:

```text
summary.json       786a4d468f23754c2fc68d9002742f072fa3c6c547598e24835311052f84adbc
verification.json  f68e73fcca8cb02adc00a05d8e94db364fcd29bd725b7ee9ba6cd700f778b843
```

The historical source maps identify private instrumentation and SDK/archive
versions **by hash only**. Those files are not supplied. The original local
manifest also described experiment script snapshots; it is replaced here with
the explicit public inventory, without changing the measured JSON records.

## Reproduce the public audit

```powershell
python -B tests/rca160/verify_evidence.py
python -B tests/rca160/test_evidence.py
python -O -B tests/rca160/test_evidence.py
```

CTest registers `rca160_public_evidence` when Python is available, including
the SDK-free graph-only configuration. The checker verifies complete arm/call
inventories, file hashes, duplicate JSON keys, call-to-summary joins, recorded
snapshot hash/difference consistency, coefficient coordinates, control equality,
held-producer ordering, publication-before-consumer recovery and the historical
96-call per-channel verdict. Its negative cases falsify status, counts, identity,
publication, first difference and wait witnesses under normal and optimized
Python. No SDK, compiler, packet, GPU or raw plane is needed for these commands.

## What independent acceptance can establish

The audit returns `RECORDED_LEDGER_AUDIT_PASS`: the published records support the
bounded historical conclusion and the counts can be recomputed from them.
It does **not** independently replay decoding or recompute coefficient
differences from raw planes. Those are excluded deliberately, so the checker
reports both capabilities as false. A manifest is an integrity inventory,
not a signature or proof that a measurement happened.

Independent execution requires separately authorized access to the exact
immutable packet and private instrumentation identified by the recorded hashes.
Bind reference media through `HYPERCINE_GOLDEN_DIR` locally; never publish its
value or silently substitute another packet. Media replay and public ledger
audit are separate acceptance steps.

The exact private replay assets have now been preserved in the bound Golden
store, including raw planes, packets, harnesses, binaries and source/build
context. See [the preservation record](GOLDEN_RCA160_PRESERVATION.md) for its
relative location, inventory, verification hashes and replay precautions.

## Historical interpretation and exclusions

The 96-call matrix orders channel V only: its remaining FULL deviations in other
channels are preserved. The later 28-call matrix independently induces Y/V/U,
induces all, orders all actual publications, then reinduces all. First differing
input coefficient is (0,0): Y 2700→0, V 1063→0, U 709→0. The ordered arm matches
all captured coefficient planes and BGRA in 2/2 FULL and 2/2 HALF calls.
Counts describe constructed schedules, not spontaneous production frequency.

The bounded historical `ROOT_CAUSE_CONFIRMED` applies to this spatial packet
and FULL/HALF routes; it does not identify the exact interleaving of every old
spontaneous failure or qualify a decoder/release. Historical source line numbers
refer to the source hashes in the provenance, not necessarily today's main.

Excluded: original/private SDK sources, generated SDK translation units,
libraries, executables, experiment setup/worker/probe scripts, packets,
coefficient snapshots, decoded images, raw logs, private paths and credentials.
No new experiment, codec code, protected merge or release belongs to this PR.
The two reports in `docs/SDK_RCA*_2026-10-28.md` retain the historical explanation;
their private replay instructions describe the original local harness, which is
not shipped here. Use the public audit commands above for repository-only review.
