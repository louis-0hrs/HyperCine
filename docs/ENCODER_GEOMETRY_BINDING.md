<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE. -->

# Encoder geometry binding after frontend injection

This is the bounded follow-up to issue #259 and PR #359. The reviewed baseline
is `dca0c0707a0d460fb8cb7ca7d198c0a7aa4be8e7`. It closes an internal session
contract gap, without changing the public encoder layout or codec arithmetic.

## Problem and correction

Previously `FrontendAccess::Use` validated a frontend's geometry at injection,
but both `FilmScan3Encoder::geometry()` and later `Encode` calls queried its
virtual declaration again. An injected frontend could therefore change its
answer after validation. Comparing two current virtual answers did not preserve
the geometry originally accepted for the session.

The encoder now holds an internal `BoundFrontend` adapter containing an immutable
copy of the accepted geometry. Before calling the original frontend's
`Transform`, it compares the live declaration with that copy. Changed width,
height or sample capacity throws before backend transformation and packet
assembly. The accessor continues reporting the accepted snapshot. A replacement
injected through `Use` must match the session's original dimensions; a refusal
leaves its previous frontend and state in place.

The default constructor had a related gap: successful encoding used a temporary
CPU frontend while the encoder's stored pointer remained empty. Later injection
could change resolution while retaining the earlier rate state. A successful
first default encode now commits its geometry binding together with completion.
A failed first encode leaves the default encoder unbound, preserving exception
safety. `Reset` resets rate state and retains the binding.

## Compatibility boundaries

No member or declaration in `include/hypercine/filmscan3.h` changes. The snapshot
lives behind the existing private `shared_ptr`, and CPU/Metal/CUDA factories keep
returning their original objects. `BackendSelection::frontend` also remains the
original object. Existing callers of `ReadMetalTiming`,
`FailNextMetalCompletion`, `CudaSpatialDispatchCount` and
`ArmCudaMidframeFailureAfter` therefore retain the native object identity used by
their casts. They do not receive the encoder's private adapter.

The adapter forwards backend/ISA identity, device name, host profiling and
timing recording to the original frontend. CPU `Auto` continues following the
existing global ISA preference. This change introduces no backend fallback or
new GPU qualification and does not define concurrent calls on one encoder or
independent GPU state for copied encoders.

## Regression evidence

The existing `filmscan3_geometry` CTest now includes a mutable synthetic frontend:

- Inject `128x24`, then change the declaration to `192x24`, `256x24`, `128x32`,
  or a different sample capacity. Each encode is refused with zero calls to the
  original `Transform` or timing recorder.
- Verify unchanged frame/rate/hysteresis state, diagnostic identifiers and caller
  bytes. Refuse a replacement that matches only the changed live declaration.
- Restore the original declaration, encode the same control packet, reset and
  retry, and replace it with another frontend of the original dimensions.
- Verify failed default initialization remains recoverable, while a successful
  default encode and subsequent `Reset` prevent resolution replacement.

On Linux x86-64, the expanded test fails against the baseline headers/libraries
with `live frontend declaration replaced the bound geometry`. The repaired
`filmscan3_geometry` test passes. An isolated build with only the live-versus-
snapshot guard removed fails with `invalid submission accepted`; neither the
repository source nor the normal build is modified for that negative control.

The full strict Release gate registered and completed **127/127 PASS**, with
zero failure or skip, including real external FFmpeg/ffprobe decoding. The
execution used GCC 14.2.0, CMake 3.31.6, Python 3.12.14 and FFmpeg 7.1.5 on
Linux x86-64. No private reference corpus was configured.

Reproduce the full portable gate with an out-of-tree Release build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 2
python3 ci/run_standalone.py --build-dir build --system Linux --machine x86_64
```

Native Windows, ARM64/Metal, CUDA and private original-SDK gates remain separate.
Portable synthetic tests and external FFmpeg decoding do not establish those
qualifications or SMPTE VC-5 conformance. `upstream/` remains unchanged.
