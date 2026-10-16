<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# FilmScan3 entropy P3 — Issue 42

Base: PR41 `db98869012a75fa628110e14fb25f93697a78754`, still draft at start.
Isolated branch `codex/macos-vlc-entropy-p3`; no edits to P2's checkout or upstream.

## Stage A: frozen instruments

`tests/entropy_legacy.h` independently freezes the complete old Bits and
PackHighpass source from PR41. It is only compiled into tests/diagnostics.
The test compares complete payloads, exact capacity and rejected short capacity,
all signed int16 values, cubic/saturation boundaries, long RLE thresholds,
padded pitch, invalid geometry and 1001 reused bands. It also compares the real
27 quantized bands for each 1080p flat/gradient/noise input against the oracle.

`validation/metal-p3/p2-before.json` is a fresh native run from the unchanged
PR41 encoder, before production edits. The benchmark executable SHA256 is
`7194990bd1b6b79300d6487c4387b1ae35320803810c45eb97baeb880a4fdf22`.
The existing benchmark records 30 warm CPU/Metal pairs per fixed input after
two initial frames, plus separate changing 16/120-frame stateful sequences.
Inputs, packets and limiter state are retained per frame. Timings include the
unchanged quantization, all public coefficient hashes and full packet writer.

`validation/metal-p3/profile_entropy.cpp` is a separate diagnostic harness:
it reports all 27 band sizes and nonzero counts for three real patterns,
complete PackHighpass duration and split-pass classification/mapping/RLE/legacy
bit packing with reserved temporary vector bytes. Split passes add temporary
storage and change cache behavior: their durations must not be summed and
presented as production latency. No per-symbol clocks enter production.

Reproduce the diagnostic after building the standalone library:

```sh
c++ -O3 -DNDEBUG -std=c++17 -arch arm64 -Iinclude \
  docs/validation/metal-p3/profile_entropy.cpp build/libhypercine_standalone.a \
  -framework Metal -framework Foundation -o /tmp/p3-profile
/tmp/p3-profile > /tmp/p3-profile.json
```

The standalone CMake test automatically runs on existing native CPU CI for
Apple ARM64, Linux x64 and Windows MSVC x64. GPU gates run locally on the Mac.
No optional band parallelism is introduced in Stage A/B. Encoder sequence
state and framing order remain sequential.
