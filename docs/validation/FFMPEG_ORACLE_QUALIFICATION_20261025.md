<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# External FFmpeg oracle — candidate qualification contract (2026-10-25)

Status: test-only evidence. Issue: #120, recommended action 4 of
`docs/validation/REFERENCE_SDK_PIXEL_CALLSITES_20261024.md`.
Base: protected `main` `471da206e02c9373b9a30cd2cc2d77af87c7b93c`.

This note and its harness turn one-off measurements into a repeatable,
fail-closed qualification. They do not replace the original GoPro SDK gate, do
not change callsite #3 or #5, and do not flip
`tools/check_reference_release.py` `independent_pixel_oracle.qualified`,
which stays `false` until the maintainer reviews an adapter.

## What is and is not claimed

Qualified here: one external decoder build reproduces the pinned reference
bytes for the **three committed 128×128 standalone fixtures**, in fresh
processes, on one host, and the comparison still rejects a different packet and
a mutated packet.

Not claimed: any other packet, resolution (1080p/4K), chroma format, GOP
structure, FIELDPLUS, BGRA conversion equivalence, GPU path, or parity between
FFmpeg and the original GoPro SDK. A stable decode of a wrong packet would
still pass the parity check; the encoder regression gate remains
`standalone_oracle_fixture_hashes`.

## Candidate oracle identity

- Build banner: `ffmpeg version 9.0.2-essentials_build-www.gyan.dev`.
- Host: Windows 11 x64 CPU-only QEMU guest (AMD EPYC 7601, 8 LP, no SMT),
  Python 3.12. The harness is standard-library only.
- The pinned reference bytes were produced by `FFmpeg n8.0`
  `140fd653aed8cad774f991ba083e2d01e86420c7` (fixture manifest).

Only builds listed in `QUALIFIED_BUILD_PREFIXES` are evaluated. Any other
build, or a missing tool, exits `77` with `ORACLE_QUALIFICATION_BLOCKED`; it is
never reported as qualified. Adding a build is a reviewed change: re-run the
harness, record the measured evidence, then extend the list.

## Adapter and normalization contract

1. Wrap exactly one packet in the minimal unindexed AVI from
   `tests/validate_standalone.py:avi` (`vids`/`CFHD`, one `00dc` chunk).
2. Require `ffprobe` to report one stream with
   `codec_name=cfhd`, `width=128`, `height=128`, `pix_fmt=yuv422p10le`;
   anything else fails the qualification rather than being coerced.
3. Decode with a fresh process per repeat:
   `ffmpeg -v error -xerror -y -i <avi> -frames:v 1 -c:v rawvideo
   -pix_fmt yuv422p10le -f rawvideo <out>`.
4. Output domain: top-down 128×128, planes `Y | Cb | Cr` as 16-bit
   little-endian, `Y` 128×128 and each chroma plane 64×128, total 65,536 bytes,
   every sample within 0…1023. No row inversion, no alpha, no color
   conversion, no padding.
5. Hash the whole output buffer with SHA-256 and compare with the manifest-bound
   `.p10` bytes. The packet bytes themselves are verified against the manifest
   before decoding, so a replaced fixture cannot qualify.

## Bound inputs

| Packet | Packet SHA-256 | Reference SHA-256 |
| --- | --- | --- |
| flat | `236cacbac9c5f39a5fbd1a4b939619276ccfc622ac486bf5a2ba967bcf28392b` | `427618421e3b4c84e8be9a39381f7607b294859dd37b91f439c81b0090e9593a` |
| gradient | `4086e8c7ec3cc0b287ab637a486354eaa89623d048ac9d79d290291099b767fb` | `30a73209e9ca96bd4b052012c69b733572f8d179a9cfe4d8588e73d6d154513d` |
| canonical | `9cf732846c1b9838c8f5d24f92db0182b82b5b6a05df63ffd5114fc1606db6fd` | `0f55d646a41090ac2cbed1b6d7d83815f6e747187f7533ab2eb707d78c3de84f` |

## Pre-registered protocol

Fixed before the run:

- N=5 fresh decoder processes per packet (15 decodes total); a determinism
  claim needs at least 2 and the harness rejects less.
- Every decoded buffer must be byte-identical to the pinned reference and all
  repeats must share one SHA-256.
- Positive control: all three packets qualify.
- Negative control A (`cross_packet`): the `flat` decode must not equal the
  `gradient` reference.
- Negative control B (`mutated_packet`): byte `500` of `canonical.cfhd` is
  XORed with `0xFF` in memory **after** the manifest check. The control passes
  only if the decoder rejects the packet or its pixels differ from the pinned
  reference; an unchanged decode means the oracle cannot discriminate.
- Adapter self-check: a truncated image and an out-of-range sample are
  rejected before any external tool runs.

## Measured result (this host, this build)

`python -B tests/qualify_ffmpeg_oracle.py` → exit `0`.

| Packet | Distinct outputs / 5 | Output SHA-256 | Reference match |
| --- | ---: | --- | --- |
| flat | 1 | `427618421e3b4c84e8be9a39381f7607b294859dd37b91f439c81b0090e9593a` | exact |
| gradient | 1 | `30a73209e9ca96bd4b052012c69b733572f8d179a9cfe4d8588e73d6d154513d` | exact |
| canonical | 1 | `0f55d646a41090ac2cbed1b6d7d83815f6e747187f7533ab2eb707d78c3de84f` | exact |

- `control=cross_packet outcome=REJECTED`
- `control=mutated_packet outcome=PIXELS_DIFFER` (the mutated packet still
  decodes; its pixels change)

Failure paths exercised on the same host: running with FFmpeg absent from
`PATH` prints `ORACLE_QUALIFICATION_BLOCKED reason=ffmpeg is not on PATH` and
exits `77` (CTest reports it as Skipped); the contract test refuses an unlisted
build token such as `9.1.0`; a one-byte change to `canonical.p10` or
`canonical.cfhd` exits `1` at the manifest check.

Full native Release CTest at the commit that adds this harness: **25/25
passed** (129.07 s), including `ffmpeg_oracle_qualification` (8.00 s) and
`ffmpeg_oracle_qualification_contract` (0.56 s).

## Exit-code contract

| Code | State | Meaning |
| ---: | --- | --- |
| 0 | `QUALIFIED` | Every positive and negative control passed for this build |
| 1 | `QUALIFICATION_FAILED` | Bound input tampered, repeat divergence, reference mismatch, or a control did not discriminate |
| 77 | `ORACLE_QUALIFICATION_BLOCKED` | Tool absent or build not on the measured list; never a pass |

CTest registers `ffmpeg_oracle_qualification` with `SKIP_RETURN_CODE 77` and
the `reference-oracle` label. A hosted or developer machine without FFmpeg
therefore reports a blocked gate, not a green one.

## Remaining work before a pixel-quality gate may cite this oracle

1. Maintainer review of this adapter and of the build allowlist.
2. A normalization contract for every additional output format or resolution
   the gate would cover, plus the same negative controls for each.
3. An explicit decision that a qualified external decoder replaces or
   supplements the original-SDK pixel signal at callsite #3, which remains
   owned by Draft #237, and at callsite #5, which remains with the phase lane.
4. Only then may `tools/check_reference_release.py` report
   `independent_pixel_oracle.qualified=true`.
