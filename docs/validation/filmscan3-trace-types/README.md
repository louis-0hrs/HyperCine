<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0. -->

# FilmScan3 trace evidence types

Refs #23. Child of #143, base `2d0b396ba323eacd40dfefbd7665339f95c4d73b`.

Encoder trace comparisons used Python equality, accepting floats for integer state/tables. The extracted checker also accepted an empty expected/actual inventory. New negative tests fail seven subcases on the parent. Validate exact integer types, table/hash cardinality, hex digests, nonempty inventory and record fields before any equality comparison. Validate the optional comparison encoder trace independently as well.

Native macOS ARM64 / AppleClang 21 / Python 3.9.6 / FFmpeg n8.0: complete Release standalone CTest under PYTHONOPTIMIZE=1, 30/30 PASS, no skips, 79.08 s. Includes 16/120-frame manifest replay and actual independent FFmpeg decode. No private Windows binary corpus configured; no new SDK raw-packet parity or GPU qualification claim.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build
PYTHONOPTIMIZE=1 ctest --test-dir build -C Release --output-on-failure
```

Only validation and regression registration changed; upstream and runtime codec sources unchanged. No merge, release or performance claim. Raw logs excluded.
