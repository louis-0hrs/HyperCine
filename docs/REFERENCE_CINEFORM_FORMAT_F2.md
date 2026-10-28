<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under the MIT License or Apache License, Version 2.0. See LICENSE-MIT and LICENSE-APACHE for details. -->

# CineForm Format Golden Corpora (Issue #36, stage F2)

This document records the **F2** stage of [issue #36](https://github.com/louis-0hrs/HyperCine/issues/36):
a bounded **3840×2160** real-SDK corpus for the compressed families that the
[F0 capability matrix](REFERENCE_CINEFORM_FORMAT_MATRIX.md) actually qualified. It reuses the
[F1 exporter](REFERENCE_CINEFORM_FORMAT_F1.md); F1 remains the 128 px / 1920×1080 stage.

- Family scope: `RGB_444` (coded 12-bit), `RGBA_4444` (coded 12-bit), `BAYER` (12-bit log-coded).
- Geometry: one 3840×2160 frame per source/quality case, one canonical pattern per family.
- Out of scope: 8K, stereo/dual-channel, `YUV_INTERLACED`/`YUV_2FRAME_GOP`, `KEYING`,
  uncompressed flags, CFA variants, sequences, and any production-codec claim.

Upstream reference (frozen, unmodified): `11574d0295771edccadd17af14af74a539f924c7`.

## 1. Tier label and unchanged F1 behaviour

The exporter gained one metadata-only option:

```
--tier f1|f2        corpus tier label (default f1; f2 = 4K stage)
```

- The default is `f1`, so every existing F1 artifact path, test and manifest keeps its previous
  meaning. `--list` previews now include the tier segment.
- With `--tier f2` the artifact path becomes `<family>/f2/<width>x<height>/...` and the manifest
  records `"stage":"F2"`. No source generation, encode, decode, comparison or normalization logic
  changed.
- An unknown tier is rejected with `unsupported tier: <value>` and exit code 2; no directory or
  frame is created.

## 2. Running the bounded F2 matrix

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build --config Release --target format_corpus_export

$root = "$env:HYPERCINE_GOLDEN_DIR/format-f2/2160p"
build/tests/Release/format_corpus_export.exe --out "$root/RGB_444" --tier f2 `
  --width 3840 --height 2160 --family RGB_444 --pattern asymmetric `
  --quality filmscan1,filmscan3
build/tests/Release/format_corpus_export.exe --out "$root/RGBA_4444" --tier f2 `
  --width 3840 --height 2160 --family RGBA_4444 --pattern alpha_ramp `
  --quality filmscan1,filmscan3
build/tests/Release/format_corpus_export.exe --out "$root/BAYER" --tier f2 `
  --width 3840 --height 2160 --family BAYER --pattern detail `
  --quality filmscan1,filmscan3

build/tests/Release/format_corpus_export.exe --verify "$root/RGB_444"
build/tests/Release/format_corpus_export.exe --verify "$root/RGBA_4444"
build/tests/Release/format_corpus_export.exe --verify "$root/BAYER"
```

Each family writes its own `manifest.json` and `TRANSFER_SHA256.tsv`; do not point several
invocations at the same output directory because the later manifest replaces the earlier one.
Raw corpora stay outside Git. The repository publishes only the variable name
`HYPERCINE_GOLDEN_DIR`, never its value.

## 3. Measured corpus

Host: native Windows x64 QEMU guest, AMD EPYC 7601 (8 LP), MSVC 19.44.35229.0,
`/O2 /Ob2 /DNDEBUG`, Visual Studio 17 2022 generator, CMake 3.31.12, CUDA disabled.
Generator seed `0x1337BEEF`; source generation is deterministic.

| Corpus | Cases | Exported | Encode failures | Decode failures |
| --- | --- | --- | --- | --- |
| `RGB_444` (RG48, RG24; FS1+FS3; `asymmetric`) | 4 | 4 | 0 | 0 |
| `RGBA_4444` (RG64, BGRA; FS1+FS3; `alpha_ramp`) | 4 | 4 | 0 | 0 |
| `BAYER` (BYR4; FS1+FS3; `detail`) | 2 | 2 | 0 | 0 |

`--verify` re-checked **43 manifest artifacts** and **40 `TRANSFER_SHA256.tsv` rows**
(17/16 for RGB, 17/16 for RGBA, 9/8 for Bayer), all hashes and lengths matching. The local raw
corpus occupied approximately 900 MB. Exact per-case values:

| Case | Source bytes | Packet bytes | Output | PSNR (dB) | Max abs | Exact ratio |
| --- | ---: | ---: | --- | --- | ---: | ---: |
| `RGB_444/RG48/filmscan1` | 49,766,400 | 904,816 | RG48 | r 76.720 / g 73.421 / b 77.377 | 29 | 0.000001 |
| `RGB_444/RG48/filmscan3` | 49,766,400 | 904,816 | RG48 | r 76.720 / g 73.421 / b 77.377 | 29 | 0.000001 |
| `RGB_444/RG24/filmscan1` | 24,883,200 | 1,102,644 | RG48 | r 52.516 / g 51.895 / b 52.533 | 557 | 0.000004 |
| `RGB_444/RG24/filmscan3` | 24,883,200 | 1,340,724 | RG48 | r 52.561 / g 52.219 / b 52.683 | 520 | 0.000007 |
| `RGBA_4444/RG64/filmscan1` | 66,355,200 | 1,215,348 | b64a | r 76.720 / g 73.421 / b 74.316 / a 67.453 | 42 | 0.000000 |
| `RGBA_4444/RG64/filmscan3` | 66,355,200 | 1,217,372 | b64a | r 76.720 / g 73.421 / b 74.316 / a 68.132 | 42 | 0.000000 |
| `RGBA_4444/BGRA/filmscan1` | 33,177,600 | 1,493,544 | b64a | r 52.516 / g 51.895 / b 52.636 / a 51.590 | 719 | 0.000004 |
| `RGBA_4444/BGRA/filmscan3` | 33,177,600 | 1,622,352 | b64a | r 52.561 / g 52.219 / b 52.626 / a 51.681 | 476 | 0.000004 |
| `BAYER/BYR4/filmscan1` | 16,588,800 | 8,756,612 | BYR4 | cfa 34.855 | 9,873 | 0.000931 |
| `BAYER/BYR4/filmscan3` | 16,588,800 | 13,101,848 | BYR4 | cfa 42.089 | 5,514 | 0.001935 |

SHA-256 values for the same run (source, normalized packet, decoded output):

| Case | Source | Normalized packet | Decoded |
| --- | --- | --- | --- |
| `RGB_444/RG48/filmscan1` | `2682f4a25ca743e7ab462b8243173800a0c5db1984bfd86441f8898053caa1c3` | `56812ad1f76072238790771c7f03409fe243d61bc2166a0989e98607e79f8352` | `b42fcf7fad9d17310db404c07ca40e43298cebcb57f2218fe889dd6bb82bf00a` |
| `RGB_444/RG48/filmscan3` | `2682f4a25ca743e7ab462b8243173800a0c5db1984bfd86441f8898053caa1c3` | `df95a8e2c13f77bb95d89d0cc7c86aaef3f3e6eb13928b2879e8091f708276c5` | `b42fcf7fad9d17310db404c07ca40e43298cebcb57f2218fe889dd6bb82bf00a` |
| `RGB_444/RG24/filmscan1` | `7722a35aa3ef2e6465bde82cc8a6733d4ebb25ec7f1acac9eaec5dbe28bb70ff` | `cfcd64fcafe6a71da98b166541f48985d4b387fb242aa72e16f7fc012061af06` | `2aff3172180ee66205584d30440b3115720f92e43b92606202ae090f530cd3e2` |
| `RGB_444/RG24/filmscan3` | `7722a35aa3ef2e6465bde82cc8a6733d4ebb25ec7f1acac9eaec5dbe28bb70ff` | `1db14dcae49c792e2e3cca7082c04da07822afc00d409f3d923433a06734ec44` | `f0817dcd126df0ce854e08c4ad00d3c4ed0cc5626a524defd38576ecb0f9832f` |
| `RGBA_4444/RG64/filmscan1` | `eec5f30a40ae83f1ba582f7cf7f41399f22b9996999304dde6175dcf0d655bb1` | `9d0dd2a2f1b18ccb0ab5e838e2524d0cca7b5862dee269f2af7b603776dfd322` | `d99cbfec5ea8792c76a4d35c07962ca1541f3b1fbd40a63eea46f22721ceae68` |
| `RGBA_4444/RG64/filmscan3` | `eec5f30a40ae83f1ba582f7cf7f41399f22b9996999304dde6175dcf0d655bb1` | `91b88a6ef215629b2289a19717d27c3acbf3bd309cd2173a533ed9d7f7d8f5f4` | `d23bad64b0aae57e9eb9f1a70189fa3eb2d75411d5816ca680375671c38552d4` |
| `RGBA_4444/BGRA/filmscan1` | `7d0a2e617d18f6d8688ac4e110d77f19620ca5970cb6deac7ba95cacd495dc69` | `4820d040513b07e38a7ffbc2a5f5de1f40b50693bfde51a98920c1a79a0b9e6f` | `eff01e5e92264d695d2a96328b4d063d8448aa8b7e8c36eeb12b8999e3c44a6b` |
| `RGBA_4444/BGRA/filmscan3` | `7d0a2e617d18f6d8688ac4e110d77f19620ca5970cb6deac7ba95cacd495dc69` | `4753b9b9feb132080e8ba45a94c7f662046c0aecc3f7ad69bfa358c3aafeaa82` | `85beb971a6e894379b385f77ec2fe69a48c7be0a435a96eb361a7c6bfab29209` |
| `BAYER/BYR4/filmscan1` | `319a579433ffeed830c86fb63eb23996ce31064253dec96c08d0d443e61def55` | `d6348dd21d959775d27242cf1bc06b6a955a2a8ea4b237a5c218b04fc8be7d4c` | `4f5fd804931e56a4903d2f3674b43e648fcda52f2724223edbdf0fa125122749` |
| `BAYER/BYR4/filmscan3` | `319a579433ffeed830c86fb63eb23996ce31064253dec96c08d0d443e61def55` | `886e5fb57ffab2471f5c8be06c34e91fe84be4cccbe722ae834ed016c701bb4d` | `93d8d8d33818bd3154354dcecccaef8b0ffe1f2a1b9dd55b5821466a6f33b878` |

FilmScan 1 and FilmScan 3 produce different normalized packets for the same source but, in these
cases, the same decoded RGB/RGBA bytes. Do not infer that the two qualities are equivalent in
general; the packet differs and only the decoded result matched here.

## 4. Repeatability at 4K

The whole matrix was generated twice in separate exporter processes. In this sample every
source SHA-256, normalized-packet SHA-256 and decoded SHA-256 reproduced exactly, including the
two `BAYER/BYR4/detail` cases.

This does **not** retract the F1 observation that 128 px Bayer `noise` and `flat` cases varied
between runs. The F1 explanation remains upstream `rand()` seeding/dithering
([UPSTREAM.md](UPSTREAM.md) quirk 12). The 4K `detail` result is two runs at one pattern and
geometry, so it is reported as a measured sample only. Continue to treat **all** BAYER hashes as
per-run evidence until a larger, pre-registered repeatability matrix exists; do not use them as a
cross-run byte gate.

## 5. Limits

- No claim that HyperCine's own encoder or decoder implements these modes.
- No 8K, stereo, interlace/GOP2, `KEYING`, uncompressed, alternative CFA, sequence or visual
  fidelity claim. This is a single-frame corpus fact from the pristine SDK.
- `TAG_BAYER_FORMAT` remains unavailable in this static build; every Bayer case uses the SDK
  default CFA (`PENDING_SDK_CFA_METADATA`).
- Output formats are the SDK's actual choices: `RG48` for both RGB sources, `b64a` for both RGBA
  sources, `BYR4` for Bayer. The 8-bit sources are stored bottom-up as measured in F1; see that
  document for the orientation and alpha-curve details.
- Raw corpora are not part of the Git tree. Verify transferred copies with `--verify`.
- The CTest additions cover only the tier label (list and invalid-value rejection); the 4K matrix
  itself stays an on-demand run because of its size.

## 6. Storage and transfer

Create the corpus locally, run `--verify`, then copy it to the authorized external corpus
location without overwriting the YUV corpora from issues #10/#24 or the F1 trees. Re-run
`--verify` at the destination. Only this document, the exporter, the tests and the schema live in
Git.
