# FIELDPLUS temporal reference fixtures

SPDX-License-Identifier: MIT OR Apache-2.0

These four 64×64 CFHD samples are deterministic research vectors created by
`tests/test_decode_phase3b2r.cpp` with seed `0x1337BEEF`. They supplement, and do
not replace or regenerate, the two canonical 3B-0 fixtures in
`tests/fixtures/phase3b0/`.

| File | Profile | Pattern | SHA-256 |
|---|---|---|---|
| `temporal_asymmetric_64_progressive.cfhd` | Progressive FIELDPLUS | dark sparse frame 0; bright detailed frame 1 | `4BDE9C000975B09BC5A444BA9E53EDB30E265C0CA400E2F3AC970E42F567650C` |
| `motion_64_progressive.cfhd` | Progressive FIELDPLUS | translated box and moving edge | `8678804C907B6692409A12E6A902DCFC71B5B467B577F01AFDD63DACDB12C1F0` |
| `chroma_temporal_64_progressive.cfhd` | Progressive FIELDPLUS | strong U/V change, patterned luma | `F72AD3F4AFFA3F51FB3B4E44B45095E3799B76944B5E28158607FA7B86D964FE` |
| `field_parity_temporal_asymmetric_64_interlaced.cfhd` | Interlaced FIELDPLUS | parity-specific Y/U/V plus frame change | `D68B3FBF0D3544CFA59F6AE2E22F85FAB7D193FEB1B7DEE001BA333ED816ABFC` |

`benchmarks/results/decode_phase3b2r_reference_frames.json` stores independent
frame 0 and frame 1 BGRA and deinterleaved Y/U/V output-word hashes for these
vectors and the canonical Phase 3B-0 fixtures. Its hashes use FNV-1a 64-bit;
the table above uses SHA-256 over the complete compressed sample.

The two 1920×1080 probes are reproducible from the same generator and are kept
in the ignored build directory rather than committed. Their source/sample hashes,
encoder flags, quality and upstream frame references are in the machine results.
