<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RGB444 frame P0 validation

Base: protected main `e87ef0258ba37f5c78f1557c59d9c7331df5521c`.
Scope: canonical source-frame storage and rejection contracts only (issue #37).

Run from the repository root with CMake 3.25+ and FFmpeg/ffprobe on `PATH`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 3
python3 ci/test_standalone_gate.py
python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64
```

On Apple ARM64 also configure `-DCMAKE_OSX_ARCHITECTURES=arm64`. On other native
platforms supply the matching `--system`/`--machine`. For ASan/UBSan use a fresh
build directory with `-DCMAKE_BUILD_TYPE=RelWithDebInfo`,
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` and
`-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined`. Run the full gate with
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` (Apple LeakSanitizer unavailable).

The machine-readable report records actual counts and proof limits. Raw logs
remain outside Git. Existing YUV manifests and real independent FFmpeg decode
are regression checks; they are not RGB444 bitstream evidence. Private external
SDK corpora were not configured. No benchmark/performance or GPU claim.
