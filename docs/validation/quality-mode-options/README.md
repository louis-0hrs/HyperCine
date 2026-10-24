<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0. -->

# quality-mode-options

Refs #46. Base `e87ef0258ba37f5c78f1557c59d9c7331df5521c`.

Quality-state-only returned before validating requested Metal, CUDA, standalone and external-decode modes, silently accepting all four without providing them. It also registered a test with HYPERCINE_BUILD_TESTS=OFF. Reject incompatible requests explicitly and honor the test switch while retaining the probe tool.

Native macOS ARM64, AppleClang 21, Python 3.9.6, CMake 3.31.5; 3/3 CTest PASS, no skips. FFmpeg n8.0 where used.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON
cmake --build build
ctest --test-dir build -C Release --output-on-failure
```

This is a build-mode fix, no P6 adapter or profile algorithm change. Actual six-profile SDK corpus and Metal profile parity were not tested.

Upstream unchanged. No runtime codec changes, performance claim, merge or release. Raw logs and local paths excluded.
