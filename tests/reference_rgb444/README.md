<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Original F1 RG48 source pins

`rg48_f1_pins.json` contains sizes and SHA-256 of actual stored F1 artifacts
from issue #36 / PR #156, exporter head
`f17355f2f391410fc2cf27065e54dcffe1ae45e7`, pristine SDK baseline
`11574d0295771edccadd17af14af74a539f924c7`. All paths are relative to the
F1 directory beneath `HYPERCINE_GOLDEN_DIR`. Source, original packet,
normalized packet and native decoded output remain loose private files.
The two original manifest hashes pin the stored export snapshots; exporter
re-runs with new volatile metadata are different snapshots.

For eight RG48 patterns at 128x128 and 1920x1080 and FilmScan1/3, the three
canonical output hashes describe **source** samples, tightly packed top-down
little-endian unsigned p12 uint16, separately R, G and B. They were calculated
independently in Python from each original source by integer division by 16,
without invoking the C++ importer. Replay also compares every component.
They are not dumps of SDK signed internal planes or expected lossy decoded
pixels. Original SDK outputs are verified only as stored artifacts.

The core source importer has no dependency on this corpus, Python or SDK.
The optional replay requires the configured existing immutable snapshot;
normal CTest/CI runs deterministic importer and synthetic replay-contract tests.
