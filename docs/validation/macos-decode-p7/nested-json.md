<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Bounded failure for nested P7 baseline JSON — issue #47

Parent: consolidated PR #172, `131fcc5c23cd4cd8c6556e38857c458ff898a6e4`.
An excessively nested baseline raised an uncaught `RecursionError` from Python's
JSON decoder. This bypassed the existing sanitized failure path and produced a
traceback containing local interpreter and workspace paths.

The verifier now handles recursion exhaustion through the same generic failure
path as malformed JSON and decode errors. The regression invokes the real entry
point, with a captured oracle result and nesting beyond the interpreter's limit;
it checks exit status 1 and the exact sanitized diagnostic. It failed on the
parent. The frozen public manifest and decoder output hashes are unchanged.

Native macOS ARM64, macOS 26.5.2, AppleClang 21, CMake 3.31.5,
Python 3.9.6, FFmpeg n8.0:

- Full SDK-free Release CTest: **32/32 PASS**, zero skips, optimized Python,
  required independent FFmpeg decode.
- Separate complete P7 CTest: **3/3 PASS**, 2.72 seconds, including all actual
  **24 public CFHD packets / 24 decoded frames** and frozen plane hashes.
- P7 JSON/plane unit methods: **9/9 PASS** under optimized Python.

Reproduce the P7 gate with FFmpeg and ffprobe on PATH:

```sh
cmake -S tests/macos_decode_portability -B build-p7
PYTHONOPTIMIZE=1 ctest --test-dir build-p7 -C Release --output-on-failure
```

Full standalone validation uses the same SDK-free options documented in
`consolidation.md`, with `HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON`, followed by
`PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64`.
No native HyperCine decoder, matched original-SDK pixel parity, sanitizer or GPU
claim. P7.B remains gated by #13/#16 and qualified matched SDK output. No upstream
changes, private media, merge or release. Raw logs remain outside Git.
