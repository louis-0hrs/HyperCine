# Registered-packet second-decoder triangulation — #120

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

**Status: REVIEW. Test-only measurement; classification and release state unchanged.**

Draft PR on branch `claude/issue120-registered-triangulation`.
Parent: [#120](https://github.com/louis-0hrs/HyperCine/issues/120); campaign:
[#161](https://github.com/louis-0hrs/HyperCine/issues/161); frozen control:
[#138](https://github.com/louis-0hrs/HyperCine/pull/138).

## Why this slice

The earlier second-decoder triangulation reported in #120 ran on a locally
**regenerated** packet because the pre-registered bytes were not present on that
host. The registered packet is now available in the local reference corpus and
was verified byte-for-byte before this run:

| input | bytes | SHA-256 |
| --- | --- | --- |
| packet | 900,396 | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` |
| source BGRA | 8,294,400 | `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1` |

Geometry 1920x1080, FilmScan3, BGRA source. The identity gate aborts before any
decoder runs on a length or digest mismatch.

## Method

- **GoPro:** frozen upstream SDK decoder at baseline
  `11574d0295771edccadd17af14af74a539f924c7`, YU64 output,
  `CFHD_DECODED_RESOLUTION_FULL`, `TAG_CPU_MAX=1`, `TAG_PROCESS_PATH=0xffff`,
  colorspace flags 2/1/0. YU64 is read top-down as `Y0/Cr/Y1/Cb` and each sample
  is shifted `>>6` into tight LE p10.
- **FFmpeg:** pinned `9.0.2-full_build-www.gyan.dev`, binary SHA-256
  `589e50b766d251afdf181dd664d40bd94407e200b019989fd7468c7d118a28d0`;
  ffprobe SHA-256 `02264dfa4f56baaa31e443ece8aa16d397f6b008200e7fd184d22b9009e75fd3`.
  The same packet is wrapped in a minimal single-frame AVI and decoded with
  `-threads 1 -pix_fmt yuv422p10le`; no explicit range or matrix conversion is
  requested.
- **Repeats:** three fresh decoder processes per decoder. All three GoPro hashes
  and all three FFmpeg hashes were identical (`STABLE_3_REPEATS`).
- **Common domain:** tight top-down `yuv422p10le`, LE 16-bit containers, low 10
  bits. FFprobe reports `codec_name=cfhd`, `1920x1080`, `yuv422p10le`;
  `color_range`, `color_space` and `chroma_location` remain absent.
- GoPro and FFmpeg outputs are compared sample-by-sample. Delta is
  **GoPro minus FFmpeg**; every sample, including borders, is included. PSNR
  uses peak 1023. `null` would mean zero MSE. FFmpeg is an independent
  triangulation only, not a quality oracle.

## Measured result

| plane | max abs | mean abs | p95 | RMSE | PSNR | bias | mismatching |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Y | 3/1023 | 0.512183 | 1 | 0.7336 | 62.888 dB | +0.468341 | 1,035,084 / 2,073,600 (49.92%) |
| Cb | 3/1023 | 0.462779 | 1 | 0.6985 | 63.314 dB | +0.395533 | 466,781 / 1,036,800 (45.02%) |
| Cr | 3/1023 | 0.512189 | 1 | 0.7345 | 62.878 dB | +0.467672 | 516,925 / 1,036,800 (49.86%) |

Decoded p10 hashes, identical across the three repeats:

```
GoPro  ca08069a5f6296745948f84257c5a1eacb05f4bb7b62f224316d72d72f9b34ed
FFmpeg 70e62e689dffb9f33ea5ed1ebf17d651b12b1d583a0505aed9d40549f2b3d4ca
```

Border versus interior mismatch fractions (eight-luma-pixel / four-chroma-column
border band):

| plane | border | interior |
| --- | --- | --- |
| Y | 50.4524% | 49.9046% |
| Cb | 48.0647% | 44.9496% |
| Cr | 52.0107% | 49.8070% |

The deltas are therefore not a border-only artifact. The first differences are
single-code steps, for example Y `(x=6,y=0) 676→675`, Cb `(x=0,y=0) 175→174`
and Cr `(x=2,y=0) 258→257`.

## Relation to the earlier NEW-packet run

The aggregate Y/Cb/Cr means and PSNRs reproduce the #120 comment of
2026-10-18 12:23 to the reported precision, and the Y border/interior fractions
match 50.45% / 49.90%. The two packets have different SHA-256 values, so this is
consistency of the measured delta, **not** a claim that the packets are
identical or interchangeable. The registered packet now has its own measured
record.

## Classification and limits

- `reference_stability = STABLE_3_REPEATS`
- `cross_decoder_agreement = MEASURED`
- `encoder_packet_comparison = UNAVAILABLE`
- `release_gate = UNRESOLVED`, `release_blocked = true`

This does **not** identify which decoder is correct, does not localize the first
differing inverse stage, and does not turn FFmpeg into a gate. It does not
reproduce or explain the M4 host divergence; that remains #116/#158 territory.
No BGRA/range/chroma common-conversion audit, no 8K or sequence coverage, and no
production or `upstream/` change was made. No threshold or gate was changed.

## Verification

Full local suite at this branch:

```
ctest --test-dir build -C Release --output-on-failure
```

**16/16 PASS**, including `decoder_differential_yu64_unpack` (the new
`decoder_sdk_dump --self-test`), `decoder_differential_compare` (p10 unpacking,
metrics and group accounting) and `decoder_differential_registered_gate`
(synthetic identity-gate rejection). `git status --porcelain upstream/` is empty.

## Reproduction

Resolve the pinned inputs through `HYPERCINE_GOLDEN_DIR` locally; never publish
its value. The packet and source must match both hashes above and the work
directory must be fresh.

```powershell
cmake -S . -B build -A x64 -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build --config Release --parallel
python -B tests/decoder_differential/registered_packet.py `
  --packet <registered 900396-byte packet> `
  --source <registered BGRA source> `
  --sdk-dump build/tests/Release/decoder_sdk_dump.exe `
  --ffmpeg "$env:FFMPEG" --ffprobe "$env:FFPROBE" `
  --work build/registered-triangulation `
  --output docs/validation/reference_decode_registered_packet_20261018.json
```

Sanitized evidence:
`docs/validation/reference_decode_registered_packet_20261018.json`.
Raw decoded planes, the AVI wrapper output and per-process stdout stay in the
ignored working directory.
