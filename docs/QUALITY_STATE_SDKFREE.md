<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# SDK-free quality state

This document records the bounded P1 calculator from Issue #29. It predicts
the regular profile's limiter, quantizers, and transform scale values; it does
not encode samples and does not claim that the Windows production encoder has
stopped using the GoPro SDK.

## Interface and supported domain

`include/hypercine/quality_state.h` is a C++17-only public header. Its enum
retains the SDK's six values (`Low=1`, `Medium=2`, `High=3`, `FilmScan1=4`,
`FilmScan2=5`, `FilmScan3=6`), now declared at this SDK-free boundary and
included by the existing phase2d API. `src/core/quality_state.cpp` includes no
SDK, CUDA, Metal, or upstream header. `HYPERCINE_BUILD_QUALITY_STATE_ONLY=ON`
configures just this source and its dedicated CTest suite.

P1 accepts progressive YUV 4:2:2, precision 10, GOP 1, positive frame rate,
even positive widths, and source-byte calculations within the upstream signed
32-bit domain. 1080p multi-frame validation and 4K/8K single-frame validation
are provided by the pinned Windows corpus. Full-resolution chroma, other
precisions, non-progressive transforms, and GOP lengths other than 1 fail
closed. `prescale[3]` is carried explicitly from the negotiated transform
setup; the calculator does not guess a device- or transform-specific prescale.

## Arithmetic and state

The port follows the regular-profile branch in the frozen GoPro
`upstream/Codec/quantize.c` and the quality tables in `quantize.h`. Those files
are references only and remain unchanged. The base quality tables are the
Low/Medium/High arrays in `quantize.h`; FilmScan1 uses the High table with its
quality-4 low-frequency rule. FilmScan2 and FilmScan3 use their separate limiter
thresholds and the High base arrays.

Low/Medium/High also retain the upstream fixed-quality ceiling: at 1080p, the
frame-rate-scaled GOP bitrate is compared with 110/130/150 Mbit/s respectively.
Above the ceiling, the half-step `quantMAX` table is selected and its
`overbitrate` state is carried separately from either FilmScan limiter. The
fixed-quality ceiling is disabled above 1080p, as in the SDK source.

At frame zero, the SDK sets FilmScan2's limiter to 8 and FilmScan3's to 4.
Other regular profiles have no FS rate limiter and report 0. On each later
frame, the prior completed sample byte count is the `lastgopbitcount >> 3`
value that drives the transition. The source byte count is calculated as
integer `width * height * 3 * precision / 8`, converted to `float`, divided by
the previous sample bytes converted through signed 32-bit, then divided by
`1.5f` for 4:2:2. Comparisons and increments are applied in the source order;
the limiter is clamped to `[0,20]`, and the table scale uses
`min(limiter,16)`.

For FilmScan2, compression targets are 4.0:1–5.5:1. FilmScan3 uses
3.0:1–4.5:1. The independent branches preserve all nested thresholds. In
particular, FilmScan2 does not reuse the FilmScan3 state machine. Tables are
36 entries in channel-major, three-wavelet-level, four-band order. The scale
pattern for tested YUV422 p10 is `[4,2,2,1]`, `[16,8,8,4]`, `[64,32,32,16]`
for each channel. Low-pass quantizers are 1. The remaining values follow the
quality table and profile-specific 10-bit adjustments.

## Oracle corpus and reproduction

The read-only Windows corpus is expected at
`\\TRUENAS\DeDub\HyperCine\golden\windows\qualities\2026-10-15\`. Before
replaying any values, `tests/validate_quality_state_corpus.py` checks the
`TRANSFER_SHA256.tsv` digest
`2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`, the
1,760-file inventory, its 5,970,674,259-byte total, and every transferred
file's individual length and SHA-256. It then checks all 6×16 1080p frames,
all six 4K and six 8K one-frame cases, and both 120-frame FilmScan2/3 runs.
The manifests are test oracles only; no expected quantizer table is loaded by
the calculator.

Configure and run the isolated CPU test and oracle replay on the Windows host:

```powershell
cmake -S . -B build-quality-state -DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON `
  -DHYPERCINE_QUALITY_ORACLE_ROOT='\\TRUENAS\DeDub\HyperCine\golden\windows\qualities\2026-10-15'
cmake --build build-quality-state --config Release
ctest --test-dir build-quality-state -C Release --output-on-failure
```

The dedicated `Portable quality state` GitHub Actions workflow runs the same
CPU unit suite on macOS arm64, Windows MSVC x64, and Linux x64. It deliberately
does not add names to the existing 25-test standalone inventory. Oracle
replay is optional in hosted CI because the private NAS is not mounted there.
The first failing comparison reports profile, frame, limiter, previous bytes,
band, and expected/actual value. The report also records the transfer digest,
source commit, and compiler string supplied by CTest.

## Windows validation record

On 2026-10-15, the isolated Release configuration built with MSVC 19.44.35229.0
and passed **2/2 CTest cases** (unit vectors and complete Windows oracle replay).
The replay verified all 1,760 files (5,970,674,259 bytes), all 20 manifests,
and matched 348 rows: 6×16 1080p frames, both 120-frame sequences, and one
frame each for all six 4K and six 8K profiles. Observed transitions were 74
for FilmScan2 and 85 for FilmScan3; these are corpus observations, not
calculator constants. `upstream/` remained clean.

The checked out source was based on main commit
`2a5e94ff6120a7053621c6ab4dcb020c7934e6d4`; the validator reports the exact
calculator commit dynamically for each replay.
Transfer list SHA-256 was
`2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`.
Source SHA-256 values recorded by the passing validator:

| File | SHA-256 |
| --- | --- |
| `include/hypercine/quality_state.h` | `506ca89d489f1f224133a7b487702065e135aaeb34243696218608b5a161bb2f` |
| `src/core/quality_state.cpp` | `016ae138df60b88ebc1094aea4e5b468b37027e3cf23520d32a049fddf62f3b5` |
| `upstream/Codec/quantize.c` | `a02f10aa6fce80df378adda041a15583c90b9f3fe95ec52bb2bc202f53e1dfa5` |
| `upstream/Codec/quantize.h` | `3a956cae47cf398bbad1a4ea20c26e37d90ce74bf5c76a936db4d01b61fa67c9` |
| `upstream/Codec/wavelet.c` | `62b3db074854a3df5ae82a6eeb1291332617706123c377774ccc5330731991fd` |

Hosted macOS/Linux/Windows Actions results remain pending until the draft PR
workflow runs. The local evidence does not claim complete encoder or packet
parity.

The existing standalone Release configuration also built successfully and
passed **15/15 registered tests**, including the frozen FS3 16/120-frame
manifests and Windows reference gates. FFmpeg and ffprobe were absent, so the
nine external-decode cases were not registered; this local run does not claim
the PR #28 25-test CI gate. Three imported Windows reference files had to be
restored byte-for-byte from the Git index to work around the host's
`core.autocrlf=true` checkout behavior before those manifest tests ran.

## Claims and next stage

Passing quantizer/state manifests proves only this calculator's table and
state parity for the checked domain. It does not prove packet, coefficient,
entropy, or whole-bitstream parity, and it does not replace the runtime SDK in
the CUDA encoder. Those checks remain a separate follow-up before any
production switch.
