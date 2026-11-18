<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Issue 259: capability-gated native planes sessions

## Scope

This follows the native source-domain preflight integrated in #402 on main
`86a88ab5ffc4345c17b2e95e2e606cada8bc13d7`. The internal `EncoderSession` now
consumes native YUV422p10 planes through that preflight and the existing
`FilmScan3Encoder::Encode(const Planes&)`. There is no BGRA conversion, new
writer, public API change, GPU planes implementation or expanded qualification.

The earlier [capability report](ISSUE259_PLANES_CAPABILITY_20261117.md) remains a
historical record of its own scope. This follow-up supplies the runtime session
entry that report explicitly left for later.

## Runtime contract

- The immutable session `pixel_format` selects `CheckNativePlaneCapability` or
  the existing BGRA capability query before the frontend factory runs. All
  geometry, quality, reference and memory refusal axes remain in force.
- `EncoderSession::Encode(const Planes&)` accepts only a native session;
  `Encode(bytes, size, stride)` accepts only a BGRA session. A wrong entry
  refuses before consuming input or advancing frame/rate/diagnostic state.
- Native strides are sample counts. A configured layout binds both luma and
  chroma strides; actual planes must match it. Shape-only sessions bind the
  actual strides per call and recheck the existing static planning ceiling.
  The two chroma planes must have equal strides because the session contract
  declares one chroma stride.
- Each native vector must contain **exactly** `stride * session_height`
  elements. The CPU pyramid copies whole vectors, so an inactive vector tail
  would otherwise allocate bytes not covered by the stride-based plan. This
  stricter storage contract belongs to the internal session; the existing
  public `FilmScan3Encoder` entry retains its previous behavior.
- After storage admission, the existing planes encoder validates geometry,
  alignment, extent and active sample codes before transformation. Its sole
  writer, actual quality tables, rate state, profile lock and diagnostics are
  reused without modification. Padding values are not active samples.
- Only CPU scalar native intake is implemented. A genuinely absent backend may
  use the caller's explicit scalar fallback; a present Metal/CUDA/Vulkan request
  remains a typed refusal before a factory call. The fallback origin remains
  visible. Actual-frame checks use the already selected effective backend.
- Move and reset preserve the same ownership contract as the existing session.
  Reset cannot reactivate a moved-from object. Diagnostic-off tombstones retain
  their session identity and serial; they are never compared as coefficient
  hashes across different sessions.

The independent `CheckEncoderCapability` and `CreateWireSession` boundaries
remain BGRA-only. Native wire-request admission is a separate follow-up; this
change does not silently widen the reviewed wire contract.

## Regression coverage

The existing `encoder_native_planes_normal` and
`encoder_native_planes_optimized` registrations exercise the new path. The
normal executable compiles its own non-NDEBUG capability, selection/session and
wire translation units; the optimized executable uses the actual Release
library. No existing registration or strict inventory requirement was removed.

The tests cover all six quality profiles over ordered distinct native frames,
exact packet/table/rate-state/full-hash agreement with the shared encoder,
caller-config isolation, reset, both move forms, diagnostic-off ownership,
factory refusal, explicit fallback, source-domain refusal, wrong shape, invalid
sample codes, truncated storage, bound-stride mismatch, unequal chroma strides,
exact packed/padded budget boundaries and excess inactive vector tails. Failed
encodes are followed by comparison with an uninterrupted session to prove that
state was not consumed.

## Validation status

Local Linux x86_64: GCC 14.2.0, CMake 4.4.5, Python 3.12.14, FFmpeg 7.1.5.
Both targeted native-session CTests passed: **702 checks in each mode** at the
pre-correction head, **705 checks in each mode** with the allocation counter
described below.
The separately instrumented ASan+UBSan build also passed both native-session
CTests, with `halt_on_error=1`; leak checking was disabled and is not claimed.
An independent review ran the same native tests plus the existing wire,
backend-selection, profile and ownership gates successfully.

Four independent scratch mutations were rejected in **both** compilation modes:
removing exact vector extent, removing the actual-frame budget refusal,
removing BGRA-on-native source refusal, and removing declared-stride binding.
Each produced the matching named regression failure; baseline scratch links
passed 702 checks in each mode. The runtime source was unchanged after the
scratch runs, SHA-256
`5aed401055a8b8f54751ff357582f1dba43c76de05fe84b1e4378d8d41677ca6`.
The independent large-tail probe also refused. The in-repo no-allocation
windows now count the aligned allocation family as well, not only the plain
global `operator new`; the positive control described below is what makes the
"before any allocation" wording evidence rather than an assumption.

The full strict Linux standalone gate passed **162 registered / 162 passed /
0 failed / 0 skipped**, two CTest slots, 144.52 seconds. It executed all existing
external-decode/fixture/registration gates as well as both native-session tests.
JUnit SHA-256:
`02212ce528412924762d1a83f646629a4f9c0570c01465d642d10faa7a4d1ac1`.
Focused ASan+UBSan JUnit SHA-256:
`ec6b2c602d41e069976f2b01fa9049e58c223c73bb8e4c864a5481ebc0cb17e7`.

The first full run was **161 passed / 1 failed / 0 skipped**: the unchanged
`filmscan3_geometry_external_decode` test exhausted the host's 4.9 GiB temporary
filesystem while FFmpeg wrote its 4K sequence. That failed receipt is retained,
SHA-256 `21bedf65adaaf42adcbac03d34593414ec599f96870ebabebce20e80526636f9`.
The complete successful rerun used a task-owned `TMPDIR` on storage with enough
space. No test, decoder option or source changed to obtain that result.

### Allocation-counter correction

The earlier counter replaced only `operator new`/`operator new[]` and their
deletes. `Plane` storage is over-aligned, so an aligned allocation could pass
through every "no allocation" window unseen: `before == after` did not by
itself evidence that a refusal happened before any allocation, and the previous
wording said more than the instrumentation could show.

This head closes that gap in the test only:

- The replacement family now also covers `operator new(std::size_t,
  std::align_val_t)`, its array form and the matching aligned deletes, using
  `posix_memalign`/`_aligned_malloc`. The previous `operator new[]`, which
  delegated to `::operator new` and therefore recursed into itself, now
  allocates directly.
- `AllocationInstrumentation()` is a positive control that calls all four
  allocation functions directly (so no compiler elision can substitute for a
  real allocation), requires the counter to move by exactly 4, requires the
  aligned pair to be 64-byte aligned, and requires the four deletes not to
  change the counter. It snapshots the counter before each `Require` because
  building a failure message is itself an allocation.
- Three mutations of the counter are rejected in both compilation modes
  (**6/6 RED**), each with its own named failure: aligned path not counted,
  array path not counted, deallocation counted as allocation. The mutation
  "aligned path not counted" reproduces exactly the previous instrumentation and
  fails at 705-check parity, so the old wording is now demonstrably
  unsupported rather than merely reworded.
- The source was restored byte-identical after every arm
  (`d5ea777d89e80ada77ef4fceafc45760b7b1538dcde8607d3f6b0a9e43e6e544`).
- No production decode, encode, capability or budget path changed; every
  existing refusal window still reports `before == after` under the wider
  counter.

Tested source SHA-256 identities:

| File | SHA-256 |
| --- | --- |
| `CMakeLists.txt` | `7d29cc66788a0d7b247830804703d060ab36bfb5cf8bbada00e090fce3e66b4b` |
| `src/core/encoder_backend_selection.h` | `1cdd82e0059e1f82d4cc4565bc867fd8824572e3027a25e7e38c2d37d64d964d` |
| `src/core/encoder_backend_selection.cpp` | `5aed401055a8b8f54751ff357582f1dba43c76de05fe84b1e4378d8d41677ca6` |
| `tests/test_encoder_native_planes_capability.cpp` | `d5ea777d89e80ada77ef4fceafc45760b7b1538dcde8607d3f6b0a9e43e6e544` |

The pre-correction identity of that test file was
`6495012de99f8a4dd66173583d693a8a1076b26c3023fbdd63b6fa920350ca3a`; the other
three files are unchanged by this correction.

No Windows/macOS runtime, native GPU, GoPro SDK, product or SMPTE VC-5
qualification is claimed by this local result. `upstream/`, retained reference
fixtures and codec arithmetic are unchanged. #259 and #46 remain open.

Portable reproduction after configuring the usual SDK-free build:

```sh
cmake --build build --parallel 3
ctest --test-dir build -C Release -R '^encoder_native_planes_' --output-on-failure
python ci/run_standalone.py --build-dir build --system Linux --machine x86_64
```
