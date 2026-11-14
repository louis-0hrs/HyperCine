<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Compatibility qualification matrix

Machine-readable answer to "what interoperability has actually been measured,
on which exact bytes and which exact decoder builds". It implements the first
finite deliverable of [#381](https://github.com/louis-0hrs/HyperCine/issues/381):
an artifact-role inventory plus one small reproducible runner.

`matrix.json` is the record. `tools/compat_qualification.py` rebuilds and
verifies it.

**What this is not:** a global compatibility claim, a codec test, or a
replacement for the gates in `docs/COMPATIBILITY.md`. A row bounds one packet,
one geometry and one decoder build. Nothing here is merged or released.

## Admitted domain

The only domain this matrix can currently call qualified:

- `yuv422p10le` (10-bit codec precision), `128x128`
- `GOP 1` (single intra packet), progressive scan
- studio range, volatile metadata tags masked at capture
- chroma siting: **not asserted**, because no independent capture exists

Every other family — RGB(A)/4:4:4, RGB48/B64A, Bayer, interlaced, temporal
GOP > 1, stereo layouts, active metadata — is enumerated in `backlog` with a
reason and an owner issue. Backlog entries cannot carry a verdict: the schema
check rejects a backlog item that grows a `status` field.

## Verdict vocabulary

| Verdict | Meaning |
| :-- | :-- |
| `QUALIFIED_IDENTITY` | Every comparison on this row has zero mismatching samples. Bounded to the admitted scope and the named artifacts. |
| `MEASURED_DIFFERENCE` | At least one comparison differs. The exact count, maximum error and PSNR are recorded; a difference is never silently a pass. |
| `NOT_OBSERVABLE` | The artifact exists but no decoded-pixel capture or independent expectation exists in Git. Reported instead of guessed. |

`QUALIFIED_IDENTITY` additionally requires: a non-empty comparison list, no
comparison of an artifact with itself, and — for measurements made on a host —
the SHA-256 of the decoder executable that produced them. A capture recorded in
the repository carries its version string and **no** binary digest, because the
build that produced it is not available here.

Status is derived, never typed in: the loader recomputes it from the comparisons
and rejects the file when the stored status disagrees.

## Evidence basis

| Basis | Both sides | Decoder binding |
| :-- | :-- | :-- |
| `committed_artifacts` | Files in this repository | Version string only |
| `local_decoder` | Repository artifact vs a local decode | Exact executable SHA-256 required |

Errors are counted per **sample** in code values (peak 1023), never per byte: a
p10 buffer is 16-bit little-endian, so a byte diff would overstate both the
count and the magnitude.

## Current verdicts

Measured 2026-11-13 on Linux x86-64 (glibc 2.41), FFmpeg `7.1.5-0+deb13u1`
(local, binary digest `e8a8d46f…`), against the committed captures produced by
FFmpeg `n8.0 140fd653` and the pristine GoPro SDK (`TAG_CPU_MAX=1`).

| Row | Decoder | Against | Verdict | Max err | Mismatches | PSNR dB |
| :-- | :-- | :-- | :-- | --: | --: | --: |
| `hypercine-packet-flat` | FFmpeg n8.0 | `flat.input-p10` | IDENTICAL | 0 | 0/32768 | exact |
| `hypercine-packet-flat` | GoPro SDK | `flat.input-p10` | IDENTICAL | 0 | 0/32768 | exact |
| `hypercine-packet-flat` | FFmpeg 7.1.5 | `flat.p10` (n8.0) | IDENTICAL | 0 | 0/32768 | exact |
| `hypercine-packet-flat` | FFmpeg 7.1.5 | `flat.sdk-p10` | IDENTICAL | 0 | 0/32768 | exact |
| `hypercine-packet-canonical` | FFmpeg n8.0 | `canonical.input-p10` | DIFFERENT | 4 | 18701/32768 | 61.2666 |
| `hypercine-packet-canonical` | GoPro SDK | `canonical.input-p10` | DIFFERENT | 3 | 13040/32768 | 63.7153 |
| `hypercine-packet-canonical` | FFmpeg 7.1.5 | `canonical.p10` (n8.0) | IDENTICAL | 0 | 0/32768 | exact |
| `hypercine-packet-canonical` | FFmpeg 7.1.5 | `canonical.sdk-p10` | DIFFERENT | 3 | 16380/32768 | 62.8427 |
| `hypercine-packet-gradient` | FFmpeg n8.0 | `gradient.input-p10` | DIFFERENT | 5 | 19298/32768 | 60.7313 |
| `hypercine-packet-gradient` | GoPro SDK | `gradient.input-p10` | DIFFERENT | 4 | 15350/32768 | 62.5922 |
| `hypercine-packet-gradient` | FFmpeg 7.1.5 | `gradient.p10` (n8.0) | IDENTICAL | 0 | 0/32768 | exact |
| `hypercine-packet-gradient` | FFmpeg 7.1.5 | `gradient.sdk-p10` | DIFFERENT | 3 | 16633/32768 | 62.7055 |
| `legacy-gopro-packet-control` | GoPro SDK | `canonical.input-p10` | DIFFERENT | 280 | 23143/32768 | 38.8927 |
| `legacy-gopro-packet-control` | FFmpeg 7.1.5 | `control.sdk-p10` | DIFFERENT | 3 | 16821/32768 | 62.7518 |
| `hypercine-metal-filmscan3-packets` | — | — | NOT_OBSERVABLE | — | — | — |

What these numbers do and do not say:

- **Decoder-version stability.** FFmpeg 7.1.5 reproduces all three committed
  n8.0 decodes byte-exactly (0/32768) at this geometry. That is a statement
  about two FFmpeg builds on this format, not about any third decoder.
- **Flat is the only bit-exact roundtrip** in both directions. It is a
  synthetic flat pattern, so it proves the pipeline can be transparent on a
  trivial input — not that the codec is lossless.
- **Canonical/gradient differ from the uncompressed input** by up to 5 code
  values. No independent pixel expectation exists for those patterns, so this
  is recorded as a measured difference, not a failure and not a pass.
- **The legacy control packet** reproduces the numbers already published for
  the frozen control (`38.8927 dB` / max 280 to source, `62.7518 dB` / max 3
  SDK-vs-FFmpeg) on a different host and a different FFmpeg major version.
  Reproducing a claim is not new evidence about GoPro encoding quality.
- **Metal packets** have no decoded-pixel capture in Git, so they stay
  `NOT_OBSERVABLE`. Their existence is not interoperability evidence.

## Reproduce

Hash-only verification, no decoder required (also registered as the
`compat_qualification_committed_matrix` CTest):

```sh
python3 tools/compat_qualification.py --verify tests/compatibility/matrix.json
```

Regenerate with live cross-decoder measurements on a host with FFmpeg:

```sh
python3 tools/compat_qualification.py \
  --ffmpeg /usr/bin/ffmpeg --ffprobe /usr/bin/ffprobe \
  --out tests/compatibility/matrix.json --markdown
```

Guard suite (corpus-free, synthetic fixtures only):

```sh
python3 tests/test_compat_qualification.py
```

The live path wraps each committed packet with the already-reviewed `avi()`
helper from `tests/compare_sdk_oracle.py`, decodes one frame to `yuv422p10le`
and binds the decoder executable by SHA-256. It never writes into the fixture
directories.

## Rules for editing

- Originals are never moved, renamed or rewritten. The runner opens each file
  once, bounded, and binds length and SHA-256 from the committed manifest.
- A new row needs a real artifact plus a real comparison. Rows without evidence
  are `NOT_OBSERVABLE`, and features without a case go to `backlog`.
- Do not widen `admitted_scope` to make a row qualify. The loader rejects an
  identity claim outside the admitted geometry, format, scan or GOP.
