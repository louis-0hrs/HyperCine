<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0. -->

# p7-frame-geometry-types

Refs #47. Base `8f547c6719f4ec7a529764d0b0525b5ee9f9afcf`.

The planar summary accepted boolean height True as 1; float/string/null geometry could raise TypeError rather than the documented validation error. Require exact integer width and height before arithmetic and slicing. Regression covers six malformed argument pairs.

Native macOS ARM64, AppleClang 21, Python 3.9.6, CMake 3.31.5; 3/3 CTest PASS, no skips. FFmpeg n8.0 where used.

```sh
cmake -S tests/macos_decode_portability -B build-p7
PYTHONOPTIMIZE=1 ctest --test-dir build-p7 -C Release --output-on-failure
```

Includes real independent FFmpeg decode of the public 24-frame SDK fixture against unchanged pinned packet/pixel hashes. This is not a native HyperCine decoder or SDK output parity; P7.B remains blocked.

Upstream unchanged. No runtime codec changes, performance claim, merge or release. Raw logs and local paths excluded.
