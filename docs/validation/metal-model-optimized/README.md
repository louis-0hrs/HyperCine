<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0. -->

# metal-model-optimized

Refs #45. Base `59da896391137d50ba147df617c763e40f51ddfb`.

Resident Metal buffer and padding checks used assert and disappeared under Python -O. Replaced them with explicit ValueError checks. Optimized fault injection corrupts the padding accounting by one byte and must fail.

Native macOS ARM64, AppleClang 21, Python 3.9.6, CMake 3.31.5; 6/6 CTest PASS, no skips. FFmpeg n8.0 where used.

```sh
cmake -S tests/macos_contracts -B build-contracts -DCMAKE_BUILD_TYPE=Release
cmake --build build-contracts
PYTHONOPTIMIZE=1 ctest --test-dir build-contracts -C Release --output-on-failure
```

The measured run enabled the optional P7 helper against the separately tested #136-derived branch. Without that optional source the suite has five tests. No Metal device execution or measured RAM limits; P4/P5 gates stay open.

Upstream unchanged. No runtime codec changes, performance claim, merge or release. Raw logs and local paths excluded.
