# `test_phase2a` host/device attribution — 2026-10-20

Scope: attribute the intermittent `device-resident decode differs from host compact`
failure reported for Draft #187 without changing the equality gate, the skip count or
any production decoder path. Related issues: #205, #120, #160.

## Environment

- Windows x64 QEMU guest, AMD EPYC 7601, 8 LP, no SMT.
- NVIDIA Tesla M4, compute capability 5.2, 3,840 MiB, driver 582.78.
- CUDA 12.8, `CMAKE_CUDA_ARCHITECTURES=52`; MSVC 19.44 Release.
- Synthetic canonical 1920x1080 BGRA frame only; no private corpus, no `upstream/` change.

## Instrumentation

`tests/test_phase2a.cpp` now prints, without changing pass/fail semantics:

- size and SHA-256 of the host-compact and device-compact packets;
- parsed sample-header fields and the parsed tag-stream length;
- every differing packet byte with hex context;
- separate open, `CFHD_PrepareToDecode` and `CFHD_DecodeSample` statuses, geometry,
  pitch, decoded size and BGRA SHA-256;
- first differing pixel between the two decoded images and the differing-pixel count.

Two bounded diagnostic options support fresh-process trials:

- `HYPERCINE_PHASE2A_CAPTURE_DIR=<dir>` writes `phase2a_host.cfhd` and
  `phase2a_device.cfhd` once;
- `test_phase2a --decode <packet>` performs exactly one decode per process and prints
  the same diagnostics plus PSNR and first difference against the synthetic source.

## Packet comparison

Both packets are 900,396 bytes. In a single encoder process they differ in exactly one
byte, offset 71: `0x0b` (host) vs `0x0e` (device). The parsed sample header reports
`frame=11` and `frame=14` respectively, with identical width, height, quality, encoded
format and parsed tag-stream length. Offset 71 lies inside the 800,268-byte parsed header.
No coefficient-payload byte differs. Both packets decode to identical pixels in the
modal case below.

Methodological note: samples produced in different processes also differ in embedded
GUID and DATE/TIME metadata, so the matrix below uses one captured immutable pair rather
than re-encoding per trial. This metadata variation is not the host/device mismatch.

## Fresh-process decode matrix

One decode per fresh OS process; no in-process warmup. The modal output for both packets
is `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0` at
44.466 dB PSNR against the synthetic source.

| Packet | Immutable SHA-256 (prefix) | Fresh-process decodes | Modal output | Corrupted variants |
| --- | --- | --- | --- | --- |
| host compact | `1ed21728fbad690a` | 8 + 6 | 9/14 | 5: `1bba277a…`, `4cb1d6f4…`, `855393bb…` (28.99 dB), `f333ebdc…` (19.64 dB), `03fd458d…` (17.94 dB) |
| device compact | `66eb97a658366b4f` | 8 + 6 | 13/14 | 1: `45460a8d…` |
| host compact, targeted probe | same bytes | 2 | 1/2 | 1: `8937c3f0…` (14.57 dB) |

Corrupted variants differ from the source in up to 2,012,894 of 2,073,600 pixels. The
same immutable packet therefore yields different BGRA output across fresh processes.

## Classification

`ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC` — the frozen original GoPro decoder varies for
identical immutable packet bytes. The host/device packet difference is a sample-header
frame number only, not a coefficient or entropy payload mismatch. A three-retry pass, or
any single pass, does not characterize this behavior; the full-run `test_phase2a` failure
must not be read as evidence of a CUDA payload defect.

## Non-coverage

- No production decoder, entropy, wavelet or CMake change.
- No equality relaxation, test skip, 45-test rerun, sanitizer claim or release claim.
- The bounded quarantine/reliable-oracle design remains #120/#160 scope; this document
  only attributes the observed failure.
