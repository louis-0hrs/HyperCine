<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RG48 source-import continuation for issue #37

This extends existing Draft PR #142, preserving its owned branch and the
separate allocation-test children #167/#176. It implements a source importer,
not a complete RGB CFHD encoder. `REFERENCE_ARTIFACTS_PINNED /
SOURCE_IMPORT_PASS`; internal signed-stage oracle and bitstream parity remain
`PENDING_SDK_INTERNAL_STAGE / BITEXACT_PENDING`.

Native macOS 26.5.2 ARM64, AppleClang 21.0.0, CMake 3.31.5,
Python 3.9.6, external FFmpeg n8.0. See `report.json` for exact counts,
source fingerprints and actual suite results. Logs/builds/caches stay outside
Git; no raw source, packet or decoded frame is published.

The importer has 25,518,276 deterministic sample/storage checks (including all
65,536 values per component and the full 1080p/storage boundary) and 21
rejection/state-preservation checks. Six replay-contract methods exercise a
known asymmetric synthetic source through the actual C++ bridge and reject
corrupt artifacts, absent output after a successful repeat, wrong output,
invalid paths and malformed geometry. The CI helper's three missing-test
inventories are rejected; CI Python discovery passes 20/20 methods.

The actual F1 replay compares **100,319,232** imported source samples over
32 cases: eight RG48 patterns, FilmScan1/FilmScan3, 128x128 and 1920x1080.
It verifies **130** stored artifacts by exact size/SHA-256 (32 times four
source/packet/normalized/decoded files plus two manifests) and 96 canonical
source-plane digests. No signed internal SDK stage is present in this corpus.
The original Windows SDK decoded bytes are integrity-checked retained outputs;
no native SDK decode was executed on this Mac. No RGB packet, image PSNR,
GPU, performance, Adobe/Resolve or complete encoder-compatibility claim.

Portable reproduction (existing loose corpus, no copy/export required):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_RGB444_F1_ROOT="$HYPERCINE_GOLDEN_DIR/golden/windows/format-f1"
cmake --build build --config Release
python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64
# The runner executes the full ctest --test-dir build -C Release --output-on-failure.
python3 -m unittest discover -s ci -p 'test_*.py'
```

FFmpeg and ffprobe must be discoverable or specified via their existing CMake
cache options. Omit only `HYPERCINE_RGB444_F1_ROOT` for corpus-free CI: the
three new mandatory tests still run, while actual F1 replay is not registered.
A configured missing/changed artifact fails; it is never a skip/pass.

For the separate sanitizer build, add
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"`
and `-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"`, then run with
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. All 19 C++ compilation
entries carry sanitizers; Python/external FFmpeg do not. Leak detection and
allocation-failure injection on this new entry point were not performed.

Before encoder math, the next owner must obtain qualified original signed
RGB internal-stage dumps, then pin G/R/B order, prescale, wavelet, band count,
quantization and framing independently. F1 source integrity clears the source
access blocker only. Parent #37 remains open for that codec work.
