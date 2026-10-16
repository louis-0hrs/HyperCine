# Width-16 GoPro horizontal-filter oracle diagnosis (#21)

Copyright (c) 2026 HyperCine Contributors. MIT OR Apache-2.0.

## Decision

**Outcome B: `UPSTREAM_ORACLE_UNDEFINED` for the tested width-16 horizontal
row path.** Do not reproduce its prefix reads in HyperCine CPU/CUDA/SIMD code.
128-pixel YUV422 frames reach width 16 at the deepest chroma horizontal input;
the SDK's affected coefficients cannot establish a defined bit-identity oracle.
This is an oracle limitation, not a passing 128px parity result or a general
rejection of 128px CFHD streams.

The experiment links directly to `FilterHorizontalRow16s` from the pristine
GoPro SDK. No copied filter, SDK source edit, production dispatch change or
golden-corpus replacement is involved. Main base: `d6ef272`; upstream baseline:
`11574d0295771edccadd17af14af74a539f924c7`.

## First dependent value and source proof

The active `_PROCESSOR_PENTIUM_4` / `_XMMOPT=1` function begins at
`upstream/Codec/spatial.c:253`. For width 16, `last_column=14` and
`post_column=0`. The SIMD loop does not run. Its left-border coefficient is
computed but never emitted. The scalar tail starts at column zero and reads
`input[column-2]` and `input[column-1]` (lines 539–540).
`ROUNDING(sum,8)` is +4 and `DivideByShift` is arithmetic right shift in the
recorded Windows build (`upstream/Codec/filter.h`). Thus:

```text
highpass[0] = saturate16(floor((-prefix[-2]-prefix[-1]+input[2]+input[3]+4)/8)
                          +input[0]-input[1])
```

For an identical flat logical row containing sixteen 512s:

| Prefix pair | highpass[0] | lowpass (all 8) | highpass[1..7] |
| --- | ---: | ---: | ---: |
| 512, 512 | 0 | 1024 | 0 |
| 0, 0 | 128 | 1024 | 0 |
| 1024, 1024 | -128 | 1024 | 0 |
| -1024, 1024 | 128 | 1024 | 0 |
| 4095, -4095 | 128 | 1024 | 0 |

The correct explicit left-border formula gives zero for this flat row. Only
the first highpass coefficient depends on the two prefix samples; all lowpass
and remaining highpass coefficients remain unchanged across guard variants.

## Boundary and determinism evidence

Each process runs 12 patterns × 12 row widths × 5 prefix pairs = **720 cases**,
each with a repeated identical call. Patterns include flat, ramp, alternating
signed extremes in [-4095,4095], deterministic modular detail, impulses at
0/1/2/13/14/15, and signed constant limits. The impulse coordinate remains
absolute; an impulse beyond a shorter row produces an all-zero row.

| Horizontal row width | Prefix dependence observed |
| --- | --- |
| 12, 14, 16 | Yes, exactly highpass[0] |
| 18, 20, 24, 32, 64, 120, 128, 192, 240 | No |

Full-frame candidate widths 96/112/128/144/160/192/256 map to row widths
12/14/16/18/20/24/32 under the three-level YUV422 chroma geometry. These are
**direct row experiments**, not successful full-frame SDK encode probes.
The result establishes a small-width family, not a defect unique to width 16.
Buffers include initialized SIMD suffix padding; prefix independence at larger
widths is not proof of safety for tightly allocated rows or every geometry.

Five fresh processes produced identical complete row data with fixed guards.
The canonical complete-row JSON SHA-256 and source hashes are recorded in
`tests/reference_formats/small_chroma_oracle_windows.json`; the collector emits
only portable evidence. This establishes determinism *conditional on guards*.
No fresh-encoder heap-perturbation test, Phase3A full-frame intermediate dump,
or CUDA comparison was run. Those broader original tasks are unnecessary to
the updated issue's prefix-dependence decision, and remain unclaimed.

## Sanitizer and validation

Windows x64, MSVC 19.44.35229.0, Release. A separate `/fsanitize=address /Zi`
compile of pristine `spatial.c` reproduces all 720 padded-buffer cases with
identical output. Explicitly poisoning the 16-byte prefix produces the expected
**AddressSanitizer use-after-poison**, read of size 2 in
`FilterHorizontalRow16s`, reported at `spatial.c:540`, exit 1. This controlled
logical-row boundary test is not a full SDK heap sanitizer qualification.
The SDK library is otherwise unchanged; STL container annotations are disabled
only on the diagnostic target to match its uninstrumented dependencies.

The normal CTest regression must keep the observed negative oracle boundary:
changing any additional coefficient, losing the width-16 dependence, or gaining
prefix dependence at a larger tested width fails. It does not suppress an
existing parity test. Production bit-identity claims retain their previously
proven geometry/profile scope; no new scope is granted by this diagnostic.

## Reproduce

Run in an x64 Native Tools PowerShell with the MSVC ASan runtime on `PATH`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_ORACLE_BUILD_REPORT=OFF -DHYPERCINE_SMALL_CHROMA_ASAN=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python tests/collect_small_chroma_evidence.py --probe build/tests/Release/test_small_chroma_oracle.exe --asan-probe build/tests/Release/test_small_chroma_oracle_asan.exe --report build/small-chroma-evidence.json
```

`--asan-prefix` is an intentionally failing manual diagnostic, excluded from
the passing CTest inventory. The collector requires both actual instrumentation
and the expected source/function diagnostic before recording detection; missing
runtime DLLs or an unrelated crash cannot pass. Never publish its raw stacks.

The normal regression is built automatically with the Windows x64 SDK tests.
The ASan diagnostic defaults OFF and does not change core codec dependencies.

Recorded validation on this change: SDK CPU Release **9/9 CTest PASS** (1.96 s);
SDK-free CPU Release **17/17 CTest PASS** (38.37 s), including the existing
16-frame and 120-frame 1080p normalized packet/coefficient manifest gates.
These are manifest-oracle replays; no external corpus bytes were configured.
FFmpeg/ffprobe were unavailable, so external decoder tests were not registered.
No GPU test or performance improvement is claimed. Privacy-guard unit tests:
10 passed, 3 POSIX hook cases skipped on Windows.
