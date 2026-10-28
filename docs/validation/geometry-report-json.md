<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Strict recorded C0 geometry evidence — issue #48

Parent: PR #129, `813e2c8f2f4ce6f0c1489ef9eaac157830a7e66e`.
The arithmetic entry points already rejected noninteger sizes, but the recorded
report check used ordinary Python object equality after permissive JSON parsing.
It accepted floating-point sizes equal to integers and silently discarded earlier
duplicate keys. Boolean zero aliases could likewise pass report equality.

The recorded report now has a reusable verifier and `audit.py --verify` command.
Duplicate keys, nonstandard constants, numeric type aliases, changed geometry
order and altered pending statuses are rejected. Canonical comparison preserves
insignificant object-key order and whitespace. CLI failures use a generic
diagnostic without echoing report values or local paths. The existing arithmetic
model and checked-in C0 report are unchanged.

Both the arithmetic tests and actual recorded-report CLI check are registered in
CTest and mandatory in the standalone CI inventory.

Native macOS ARM64, macOS 26.5.2, AppleClang 21, CMake 3.31.5,
Python 3.9.6, FFmpeg n8.0:

- Full SDK-free Release CTest: **30/30 PASS**, zero skips, optimized Python,
  required external FFmpeg decode.
- Arithmetic/report unit methods: **7/7 PASS**; standalone gate helpers:
  **7/7 PASS**, including rejection when either geometry registration is missing.
- Recorded report verified directly with `audit.py --verify`.

```sh
python3 -O tests/geometry_extreme/test_audit.py
python3 -O tests/geometry_extreme/audit.py --verify docs/validation/extreme-geometry-c0.json
python3 -O ci/test_standalone_gate.py
```

For full standalone regression validation, configure Release with
`HYPERCINE_BUILD_STANDALONE=ON`, `HYPERCINE_BUILD_UPSTREAM=OFF`,
`HYPERCINE_ENABLE_CUDA=OFF`, `HYPERCINE_BUILD_TESTS=ON`,
`HYPERCINE_BUILD_BENCHMARKS=OFF`, `HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON` and
compile-command export; build, then run
`PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64`.
FFmpeg and ffprobe must be discoverable on PATH or supplied through their existing
CMake executable cache options.

C0 mathematical evidence only: no extreme frame allocation, new format support,
GPU measurement or C1/C2 qualification. Original SDK and extreme-resolution
independent decode oracles remain pending. Upstream is unchanged; raw logs and
build output remain outside Git. No merge or release.
