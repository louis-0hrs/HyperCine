<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0. -->

# Validate both sides of the 4K p10 quality gate

Refs #44. Base `5939a40b80358ffa93733b0fdd673476903cefcd` (sequence-validation child of P4).

The 4K quality gate checked the decoded p10 range but omitted the reference range. Invalid reference samples of 1024 against decoded 1023 reported 60.20 dB and passed. Extract the metric into a checked helper; reject empty/odd storage, unequal plane lengths and values above 1023 on either side before calculating PSNR. Two new failing negative cases on the parent now pass. The existing >35 dB threshold is unchanged.

Native macOS ARM64 / AppleClang 21 / Python 3.9.6 / FFmpeg n8.0: full Release standalone CTest under PYTHONOPTIMIZE=1, 31/31 PASS, no skips, 206.88 seconds. Includes 19 real 4K single-frame independent FFmpeg decodes, 16 ordered decoded sequence hashes and 1080p 16/120-frame manifest regressions. Minimum measured 4K PSNR 52.32135 dB. Original SDK 4K packet parity remains REFERENCE_BLOCKED; no private corpus configured.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build
PYTHONOPTIMIZE=1 ctest --test-dir build -C Release --output-on-failure
```

No runtime/geometry/Metal changes or P4/P5 acceptance. Upstream unchanged. No merge, release or performance claim. Raw logs and build artifacts excluded.
