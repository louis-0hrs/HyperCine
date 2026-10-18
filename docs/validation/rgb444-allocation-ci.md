<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Automatic RGB444 allocation-failure coverage — issue #37

Parent: PR #167, `535fdeaf2df1e04a30322331bb1ad2462235fd3e`.
The existing test injected failures at all three actual aligned plane allocations,
but only its separate CMake project registered it. Normal standalone CI could pass
without running these cleanup, prior-frame preservation and recovery checks.

The standalone test build now includes that same project and requires
`rgb444_allocation_failure` in its checked CTest inventory. Its separate project
still works. Production allocator and frontend source are unchanged.

Native macOS ARM64, macOS 26.5.2, AppleClang 21, CMake 3.31.5,
Python 3.9.6, FFmpeg n8.0:

- Full SDK-free Release CTest: **31/31 PASS**, zero skips, required external
  FFmpeg decode, including the automatically registered allocation test.
- Standalone gate helper methods: **7/7 PASS**; removing the allocation-test
  registration is rejected. The new check failed against the parent inventory.
- Separate actual allocation test with **ASan + UBSan: 1/1 PASS**; leak detection
  disabled. This is scoped sanitizer coverage, not a full-suite sanitizer run.
- Tests disabled: the standalone configuration registers **zero tests**.

Reproduce with FFmpeg/ffprobe available through PATH or their existing cache options:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 2
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64
python3 -O ci/test_standalone_gate.py
cmake -S tests/rgb444_boundary -B build-allocation -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-allocation --parallel 2
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-allocation -C Release --output-on-failure
```

No RGB CFHD packet, SDK parity, GPU execution or throughput claim. Issue #37
remains open for its encoder/reference work. Frozen upstream is unchanged;
raw logs and build output remain outside Git.
