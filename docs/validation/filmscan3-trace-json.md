<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# FilmScan3 trace JSON contract — issue #23

Parent: PR #151, `6e6fe42734f8f2dd9900aa89ce7a1ff8e4448b4c`.
The trace reader used ordinary JSON decoding, which silently kept the last
duplicate key. A duplicate frame inventory or state field could therefore erase
conflicting evidence before the existing strict row checks ran.

Both the primary and optional comparison encoder now use the same strict reader.
It rejects duplicate keys at every depth, nonstandard numeric constants, incorrect
profiles and unexpected envelope fields. Parser diagnostics contain no evidence
values. Existing row/state/table/hash comparisons remain in place.

Native macOS ARM64, macOS 26.5.2, AppleClang 21, CMake 3.31.5,
Python 3.9.6, FFmpeg n8.0:

- Full SDK-free Release CTest: **30/30 PASS**, zero skips, optimized Python,
  required external FFmpeg decode and existing 16/120-frame manifest replay.
- Trace contract: **5/5 methods PASS**, including nested duplicate keys,
  wrong profiles/root types/constants and diagnostic redaction.
- Existing standalone gate helpers: **6/6 methods PASS**.

Reproduce with FFmpeg/ffprobe on PATH (or their existing CMake cache options):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 2
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64
python3 -O tests/test_trace_contract.py
```

This changes test evidence parsing only. Private original SDK packet and internal
stage comparisons were not supplied. Issue #23's remaining production/reference
gates stay open. No new GPU, sanitizer or performance measurement; upstream is
unchanged. Raw logs/build output remain outside Git.
