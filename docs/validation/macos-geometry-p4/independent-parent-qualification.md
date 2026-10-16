<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Independent qualification of the merged P4 parent

Issue #44 requests fresh native qualification of P4 source commit
`56fbe550d2f88ebf6cc3a3e81b0d37a3ae8e084f` after PR #73 was merged into
PR #50's feature branch. This report uses a separate clean checkout and fresh
build directories. Earlier uncommitted qualification reports were left untouched.
The accompanying `independent-parent-qualification.json` records this run's
actual results, test inventory, source digest and ordered decoded-frame hashes.

The CPU Release and ASan/UBSan suites require functional external FFmpeg and
ffprobe, the original 16-frame SDK corpus, and the original 120-frame SDK
sequence corpus. The standalone runner checks that every registered test ran
and passed without skips. A separate native Metal build runs the seven
Metal/backend-selection tests, including both original SDK corpora and
external decoding. Metal tests require native GPU access.

The 4K gate independently decodes all 19 packets and the complete changing
16-frame sequence to `yuv422p10le`. It checks each sequence frame against its
independent decode in order. The evidence comparison requires exact equality
of all recorded frame fields between Release and sanitizer builds, including
input/sample/decoded hashes, coefficient identities, quantization/scaling
tables, payload/framed-band hashes and rate state. The ordered sequence hashes
are also compared with the checked-in child integration baseline.

## Reproduction

Check out the source commit above. Configure the private corpus roots through
`HYPERCINE_GOLDEN_DIR`, exposing `tier-a` and `sequence` directories; adjust
those relative suffixes to the local extraction layout when needed. Put a
functional FFmpeg and ffprobe on PATH, or provide the two CMake executable
cache variables explicitly. Keep all build/media/log output outside Git.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_REFERENCE_CORPUS_ROOT="$HYPERCINE_GOLDEN_DIR/tier-a" \
  -DHYPERCINE_FS3_SEQUENCE_ROOT="$HYPERCINE_GOLDEN_DIR/sequence"
cmake --build build --parallel 3
python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64
python3 -m unittest discover -s ci -p 'test_*.py'
```

Repeat in `build-sanitized` with `RelWithDebInfo`,
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`, and
`-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'`. Run its standalone
gate with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
For `build-metal`, use Release with `HYPERCINE_ENABLE_METAL=ON`, then run:

```sh
ctest --test-dir build-metal -C Release -R 'metal|backend_selection' \
  --output-on-failure --no-tests=error
```

Compare `geometry-p4/geometry.json` from both full CPU builds: all 19 `frames`
records and the sixteen `sequence_decoded_sha256` entries must be identical.
Compare those sequence hashes with `current-main-integration.json` as well.
Raw CMake/JUnit/geometry reports may contain local paths or tool configuration;
publish only the selected portable evidence in the accompanying JSON.

## Scope and remaining gates

ASan/UBSan instruments the C++ CPU encoder and tests. External FFmpeg and
Python are uninstrumented; leak detection is disabled on Apple arm64.
Metal 1080p regression is separate from CPU 4K qualification. Test durations
and peak RSS describe correctness runs; no performance improvement is claimed.
No codec, API, GPU source or frozen `upstream/` file changed.

Original-GoPro SDK **4K raw-packet parity remains REFERENCE_BLOCKED**.
8K/12K/17K encoding, Metal 4K, CUDA/Vulkan and production acceptance are outside
this qualification. The next action is a maintainer decision on experimental
P4 acceptance. No protected-main merge or P5/NEON implementation is authorized
by these results alone.
