<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Original SDK GOP-2 reference contract

**Current Phase B:** both previously missing stage families are extracted from
the original SDK and compared exactly (180/180 new checks). See
[the Phase-B path qualification and owner handoff](../../docs/validation/GOP2_ISSUE189_PHASEB.md).
The Phase-A indices below remain immutable historical records.

See [the measured #189 report](../../docs/validation/GOP2_ISSUE189.md) for inventory,
producers, byte layouts, actual pool sizes, independent comparisons, commands and
remaining blocked gates. Raw bytes live only under the private
`HYPERCINE_GOLDEN_DIR` binding. Repo JSON files are sanitized indices of measured
files, not substitute fixture bytes.

Schemas are versioned by `schema` strings: `hypercine.gop2.inventory/1` is
read-only discovery; `observations/1` records original SDK calls and post-inverse
resident stages; `artifact/1` and `manifest/1` pin the original files, producers
and input hashes; `comparison/1` and `comparison_index/1` retain exact candidate
comparisons and tool/artifact hashes. The validator rejects unknown schemas,
unsafe paths, length/hash/sidecar mismatches, and incomplete comparison matrices.
Hashes alone do not authenticate a producer. `AVAILABLE` denotes the specific
bounded original-SDK control, not general SDK correctness or release readiness.

Every operation keeps `release_gate=BLOCKED`; successful inventory/verification
CLI exit 2 makes that state explicit. No missing final-signed-plane or pre-inverse
oracle can become an implicit pass. No output file or corpus export is overwritten.
