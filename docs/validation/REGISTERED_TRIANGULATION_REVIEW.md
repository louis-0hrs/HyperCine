# Independent registered-packet harness review — #120 / #170

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

Review date: 2026-10-19 (Europe/Berlin).
Reviewed [Draft #170](https://github.com/louis-0hrs/HyperCine/pull/170) head
`3e369f7a10857062c8d007737cf6338808129d5b`, based on frozen #138 head
`97c34500324b550da5f6444fe9d28c8729dea8d2`.
This child contains test-only corrections. Neither existing branch was changed.

## Findings and corrections

1. **Repeat freshness:** `compare.py::audit_case` used the same `gopro.p10` and
   `ffmpeg.p10` paths for every process. A tool returning zero without producing
   another output could reuse the previous file and be reported as stable.
   A synthetic executable adapter reproduces this false stability result on the
   reviewed head: the first invocation writes pixels, later invocations write
   nothing, yet all three recorded hashes match. Every repeat now requires a
   distinct, previously absent output file. Missing output fails the run.
2. **Pinned-tool preflight:** `registered_packet.py::main` accepted arbitrary
   FFmpeg/ffprobe executables and recorded their identities after decoding.
   It did not enforce the documented pinned decoder. The registered protocol
   now checks both SHA-256 values from the original measured report before any
   sample decode. Missing or different tools return exit 2 with explicit
   `ALTERNATE_DECODER_UNAVAILABLE`, `UNAVAILABLE` and `release_blocked=true`.
   Tool failures are caught without publishing a traceback or local paths.
   Each tool command has a 20-second timeout instead of 300 seconds.
3. **SDK output lifetime:** `sdk_dump.cpp::main` converted and wrote pixels
   before closing the decoder; its output allocation was also destroyed before
   the decoder during stack unwinding. This differs from #138's explicit
   close-before-inspection policy (`reference_decode/worker.cpp`). Output now
   outlives the handles, and successful metadata/decoder teardown precedes pixel
   conversion. Close errors fail without publishing pixels. The native test
   compiles the actual dump entry point against a synthetic SDK which finalizes
   pixels at close. It fails on the original ordering and passes after the fix;
   injected decode and close errors also fail without an output file.
   This tests the diagnostic's lifecycle contract, **not** a claim that the real
   SDK always writes after `CFHD_DecodeSample` returns or a new race diagnosis.
4. **Report provenance:** the old report says `requested_format=BGRA` although
   `sdk_dump.cpp` requests YU64; BGRA describes the input source. Protocol v2
   distinguishes `source_format=BGRA` and `requested_format=YU64`, includes
   `compare.py`'s hash and records whether the source checkout is dirty. The old
   source commit is the parent and does not contain the diagnostic script;
   the recorded individual script/SDK-source hashes do match the reviewed
   working files. Do not describe that parent SHA alone as the complete
   measurement source lineage.

The existing v1 JSON and report are retained unchanged as historical observations.
No v2 native registered-packet measurement is claimed by this review. In
particular, this patch cannot retroactively establish output freshness or
post-teardown pixels for the earlier run.

## Independent checks on the committed numerical record

The v1 result identifies the registered packet SHA-256
`2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52`.
Its three hash pairs contain one GoPro digest and one FFmpeg digest each.
Recomputing counts, mismatch counts, mean absolute error, RMSE and PSNR from
the signed histograms agrees with the committed record. Each disjoint group
also sums to the plane's sample and mismatch counts:

| Plane | Samples | Mismatches | Max absolute error | Recomputed PSNR |
| --- | ---: | ---: | ---: | ---: |
| Y | 2,073,600 | 1,035,084 | 3/1023 | 62.887945 dB |
| Cb | 1,036,800 | 466,781 | 3/1023 | 63.313910 dB |
| Cr | 1,036,800 | 516,925 | 3/1023 | 62.878247 dB |

This establishes internal arithmetic consistency. It does not independently
authenticate retained raw outputs, establish decoder correctness, resolve
range/matrix/chroma interpretation or explain the divergent BGRA case.

## Verification

Native Windows x64, MSVC 19.44.35229.0, CMake 3.31.6, Python 3.10.9.
CPU-only SDK Release configuration; CUDA and benchmarks disabled. No new
registered-packet decode, regeneration, affinity matrix, GPU, 4K/8K, sanitizer
or performance qualification was run.

```powershell
cmake -S . -B build -A x64 -DHYPERCINE_ENABLE_CUDA=OFF `
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_ORACLE_BUILD_REPORT=OFF
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

Full suite: **17/17 PASS, zero skips**. Differential Python suites: 6 metric/
freshness methods and 9 identity/preflight methods. The new native synthetic
lifetime test covers normal, decode-error and close-error paths. Before the
fix, the freshness regression and native lifetime regression failed. Missing
tool and pin preflight were absent in the original implementation.

`git diff --check` passes; frozen `upstream/` is unchanged. No production code,
encoder-byte check, PSNR threshold or existing quality gate was changed.

## Acceptance remains unresolved

The current coordination dispatch in #62/#161 requests this independent review,
not another reproduction sweep. The first faulty 1080p stage and a causal
intervention remain with #160; native Windows validation can follow a precise
stage hypothesis. #120 remains open and release-blocked. FFmpeg remains an
independent observation, never an accepted quality oracle. Existing classifier
rules keep bitstream mismatches, decoder API errors and stable bad quality as
FAIL, and nondeterministic reference pixels as UNRESOLVED. No merge or release
is authorized or performed by this test-only child.
