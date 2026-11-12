<!--
Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under the MIT License or Apache License, Version 2.0.
See LICENSE-MIT and LICENSE-APACHE for details.
-->

# Encoder session ownership (#371)

`FilmScan3Encoder` and the internal `detail::EncoderSession` are move-only.
Their copy constructors and copy assignment operators are deleted. A move
transfers the existing session, including its frontend, geometry, storage
limit, CPU ISA policy, quality/profile lock, rate-control history and diagnostic
identity. It does not construct another backend or start another stream of
frames. Both move operations are `noexcept` and self-move is a no-op.

This is an intentional C++ source-API change. Existing code that copied an
encoder must construct a separate session with the desired configuration, or
use `std::move` when transferring ownership. There is no implicit clone or
snapshot operation. The reviewed repository callers create separate sessions,
use references or transfer `unique_ptr` owners; none requires encoder copies.
Adding the inactive-state flag also changes the public C++ object layout, so
consumers must rebuild against the updated header and library together.

## Why copies were unsafe

The previous implicit copy duplicated frame/rate/profile state and the
diagnostic session/serial identifiers while sharing `frontend_`. A Metal
frontend holds mutable workspace, timing and completion-failure state. CUDA
frontends also hold session-specific counters and failure-injection state.
Copying the encoder therefore did not create an independent session.

This was observable without GPU hardware: copying a CPU encoder after its first
diagnostic-off frame made both copies emit identical diagnostic tombstones on
their next frames. The previous implicit move had a different problem: the
source retained its old state and diagnostic identity after its frontend moved
away. Its next operation could select the default 1920x1080 CPU frontend.
An internal session additionally retained a successful selection through its
`const` member copy, while its actual encoder had moved elsewhere.

## State after a move

The source becomes **inactive**, and `active()` reports `false`.

| Operation | Inactive `FilmScan3Encoder` | Inactive `detail::EncoderSession` |
| --- | --- | --- |
| `Encode` | Throws `std::logic_error` before any backend dispatch | Throws `std::logic_error` before storage validation or dispatch |
| `geometry()` | Width, height and sample capacity are zero | Same zero geometry |
| `wavelet_isa()` | `Auto`, denoting no bound execution policy | Same |
| `state()` | Default frame/rate state; transferred diagnostic identity cleared | No public state accessor |
| `session_quality()` / `coefficient_hashing()` | Default FilmScan3 / hashing enabled, with no active session | Policies are represented by the default config |
| `SetSessionQuality` | Throws `std::logic_error` | No such setter |
| `SetCoefficientHashing` / `Reset` | No-op; cannot reactivate the encoder | `Reset` is a no-op and `noexcept` |
| `config()` / `selection()` | Not applicable | Default config, empty frontend and refused selection |
| Move assignment from an active owner | Installs the transferred session | Installs its config, selection and encoder together |
| Move from another inactive owner | Remains inactive | Remains inactive |

The internal frontend-injection seam also refuses an inactive encoder. A
fresh session must be constructed before ownership can be assigned back into
the old owner. A session's diagnostic identity continues at its destination;
constructing a separate replacement session creates a new identity when its
first diagnostic-off frame completes.

The internal config and selection remain externally read-only. Their private
members can now change during an ownership transfer so the old owner does not
retain a selected backend or a misleading capability result.

## Concurrency and qualification boundary

Concurrent operations on the **same** encoder/session, including `Encode`,
`Reset`, policy changes and moves, are unsupported. The caller must serialize
such operations. Separate sessions may be used concurrently according to the
existing backend contracts. Moving an owner does not expand a backend's thread
or device-context guarantees.

The internal shared frontend pointer remains available for diagnostic and test
observers. This change prevents implicit copies of active encoders; it does not
make arbitrary intentional sharing through `detail::FrontendAccess` safe, nor
does it turn the same frontend into multiple independent GPU sessions.

`encoder_move_ownership` is a mandatory SDK-free CPU CTest. It checks compile-time
copy refusal and nonthrowing moves, complete packets/tables/rate state against
an independently constructed scalar reference, profile-lock and diagnostic
continuity, inactive sources, both self-move cases, lazy default sessions,
replacement ownership, and exact frontend release. A stateful frontend wrapping
real CPU transforms preserves a pending completion failure through a move and
checks rollback and recovery without executing GPU code.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF
cmake --build build
ctest --test-dir build -C Release --output-on-failure
```

These tests do not qualify native Metal/CUDA resource destruction, cross-thread
moves or CUDA device switching. Backend-required native packet, failure and
multisession tests remain necessary on their actual supported hardware. CUDA
device selection is the separate A10 follow-up. No codec-quality, performance
or original-SDK compatibility claim follows from ownership tests alone.

## Recorded Linux qualification

The final main-based source passed the complete strict standalone gate:
**128/128 CTests, zero failure/error/skip**, with real FFmpeg/ffprobe required.
All nine implementation/build/test fingerprints matched before and after the
run. The CTest wall time was 355.83 seconds during concurrent local qualification;
this is not a throughput benchmark. An independent source review found no
functional blocker in the copy/move contract or its required gate registration.

The baseline CPU probe reproduced identical diagnostic tombstones from copied
owners and a moved-from owner retaining its frame index while reverting to
1920x1080 geometry. Three separately compiled actual-source mutations failed
the intended assertions: a still-active source, reset diagnostic identity and
stale successful backend selection. Those earlier probe/mutation outputs were
observed, but separate raw logs were not retained. The full-suite log is retained
outside Git and its hash is recorded with the source fingerprints in
[`ENCODER_OWNERSHIP_371_20261112.json`](validation/ENCODER_OWNERSHIP_371_20261112.json).
