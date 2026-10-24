# Phase 3B-2A production decode fixture

SPDX-License-Identifier: MIT OR Apache-2.0

`motion_1080p_progressive.cfhd` is the reproducible 1920×1080 progressive
FIELDPLUS/GOP-2 FilmScan3 motion probe generated for Phase 3B-2R. It is retained
here so production correctness checks do not depend on ignored build outputs.

| File | Size | FNV-1a 64-bit | SHA-256 | Frame 0 BGRA | Frame 1 BGRA |
|---|---:|---|---|---|---|
| `motion_1080p_progressive.cfhd` | 718,396 bytes | `c59a1ca30aa101a9` | `7FBE3AF9EFDCBCC8DBA8CF904FD21F422FF9608F78AFB8D79C5202015B0DF8B8` | `7f633a5358642abd` | `6d174c1993f61015` |

The sample was generated from deterministic 1920×1080 YUY2 moving-box/edge
frames using seed `0x1337BEEF`, FilmScan3 quality, and progressive encoder flag
`2`. The unchanged upstream reference hashes are recorded in
`benchmarks/results/decode_phase3b2r_reference_frames.json`.
