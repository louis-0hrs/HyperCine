<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Zero-exit transient FFmpeg installation failures — #99

Child of PR #100, parent 459aacd865bca2b4bd9780de61253041d0895324.
Chocolatey can exit zero while reporting a transient feed failure. Previously,
functional ffmpeg/ffprobe binaries left from an earlier installation made that
attempt succeed immediately. Recognized transient failure text now prevents
installer success independently of binary probes. The existing three-attempt
limit applies even when both binaries already work; an error-free retry and both
functional probes are required to pass.

The new regression failed before the correction and now verifies recovery on the
second attempt and nonzero exit after three failed attempts with existing tools.
No package manager or network is invoked by these synthetic unit tests. Raw
installer output remains private. A real Windows installer/hosted run remains
required; native macOS testing does not establish that platform gate.

```sh
python3 -m unittest discover -s ci -p 'test_*.py'
ctest --test-dir build -C Release --output-on-failure
```

Native test environment: macOS ARM64, AppleClang 21, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Full Release configuration uses HYPERCINE_BUILD_STANDALONE=ON,
HYPERCINE_BUILD_UPSTREAM=OFF, HYPERCINE_ENABLE_CUDA=OFF,
HYPERCINE_BUILD_BENCHMARKS=OFF and HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON.
Supply FFmpeg/ffprobe through PATH or the existing CMake executable cache options.
No protected-main merge or release; frozen upstream remains unchanged.

Native CI Python discovery: 36/36 PASS, including all 17 installer methods.
Full SDK-free Release required-external-decode CTest: 28/28 PASS, zero skips,
78.18 seconds.
