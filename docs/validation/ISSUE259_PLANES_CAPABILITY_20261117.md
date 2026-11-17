# Issue 259: native YUV422p10 planes as a distinct source domain

2026-11-17. Bounded capability-contract slice of #259 (lane L1, Linux x86_64
CPU/core). It gives the *internal* capability preflight an explicit
representation of the native 10-bit YUV422 planar source that
`FilmScan3Encoder::Encode(const Planes&)` already consumes, and makes every
unsupported combination a typed refusal instead of a BGRA reinterpretation or a
silent CPU substitution. It does not change the writer, the rate law, the public
API/ABI, any constructor, a workflow, the frozen `upstream/` tree, or any
qualification status.

## The gap this closes

#397 added the public native planes entry (`Encode(const Planes&)`, same
session, tables, rate state and writer as the BGRA overload). The internal
contract that decides *what a session may allocate* had no word for it:
`EncoderSessionConfig::pixel_format` could only describe `Bgra8` (implemented),
`Yuy2`/`Rgb48` (typed unimplemented), and every storage rule was written in
terms of an interleaved byte stride (`width*4`, 16-byte aligned, `input_bytes`).
Any attempt to describe the planes domain therefore landed on one of two bad
answers:

- it was refused as "only BGRA8 input is implemented", which is false for the
  CPU scalar session, or
- an interleaved byte binding was applied to a planar frame, i.e. the query
  budgeted storage the encoder cannot address.

Neither answer is a capability statement about the domain the caller has.

## Contract actually implemented

Internal headers only (`src/core/encoder_session_config.h`,
`src/core/encoder_capability.cpp`); no public header, installed type or public
class layout changed.

- `SourcePixelFormat::Yuv422p10Native` is its own enumerator. It is not an alias
  of `Yuy2`/`Rgb48`, and the test asserts the three cannot coincide: an alias
  would let one format answer for the other.
- `NativePlaneStorage { luma_stride_samples, chroma_stride_samples }` declares the
  storage in **int16 samples**, matching `Plane::stride`. This field exists so
  the preflight can refuse a wrong planar stride *before* any `Plane` is
  allocated, without ever reusing the byte fields.
  - both zero = shape-only query (no planes allocated yet);
  - one zero = refusal (a half-bound planar layout cannot be addressed);
  - negative = refusal (a sample count cannot be negative);
  - a bound layout must cover the row (`luma >= width`, `chroma >= width/2`),
    be 16-byte aligned in samples (`% 8`), and stay inside
    `PlaneStorageFits(stride, height)`.
- The two addressing domains are exclusive. A native config carrying
  `input_stride_bytes`/`input_bytes`, or a BGRA config carrying plane strides, is
  a **format error**, not a preference, because the query would otherwise budget
  a buffer the selected entry cannot address.
- Two capability entries, one implementation (`Evaluate(..., native_domain)`), so
  the geometry, pixel-budget, storage, profile-admission and evidence axes cannot
  drift apart:
  - `CheckEncoderCapability(...)` is the interleaved BGRA session entry. It
    refuses a native config with `ValidButNotImplemented` and says why, instead
    of budgeting bytes for it. This is the "never silently fall back to BGRA"
    direction.
  - `CheckNativePlaneCapability(...)` is the planes entry. It requires the
    native source format (a BGRA config is a format error there), and otherwise
    reuses the whole existing chain.
- Backend rule, decided on the **effective** backend after the existing presence
  policy is resolved:
  - CPU scalar: admitted inside the existing evidence geometry table and the
    existing pixel/plane budgets. No new geometry, no new profile matrix. A
    bound native layout is costed in the same static planning ceiling as a BGRA
    buffer: `2 bytes * height * (luma_stride + 2*chroma_stride)` (overflow-guarded),
    which is 4 bytes/pixel for the canonical YUV422p10 layout. Without that term
    the native domain would have admitted requests the BGRA domain refuses at the
    same geometry and budget.
  - Metal / CUDA / Vulkan: `CapabilityReason::ValidButNotImplemented` with a
    detail that names the missing planes frontend. The refusal is
    **unconditional** and precedes the geometry validator
    (`geometry_validator_invoked` stays false), any factory and any allocation;
    cheap pre-checks that already ran keep their flags on the result, but the
    primary reason names the domain gap. The base `Frontend::TransformPlanes`
    still refuses, so no backend can silently reinterpret BGRA storage as planes.
  - The existing `allow_cpu_scalar_fallback` flag is a **presence** policy, not a
    source-domain policy. With a genuinely absent device and the flag set, the
    admitted CPU session is observable through `fell_back_to_cpu_scalar`; with
    the backend present, a native request is refused even when the flag is set,
    because a permitted fallback cannot conjure a planes frontend.

## Wiring boundary

The admitted domain is consumed by the existing public planes entry
(`FilmScan3Encoder::Encode(const Planes&)`, unchanged), which the test drives
with real planes at an admitted geometry. `EncoderSession`'s storage entry is
interleaved BGRA only, so a native configuration is refused there instead of
being reinterpreted; adding a planes intake to that session is a follow-up that
touches `src/core/encoder_backend_selection.*`, which this lane does not own.
This slice therefore claims a contract/audit deliverable, not a new runtime path.

## What is preserved

- Six-quality shared rate state, the single shared writer and the session
  profile lock: no file in the writer, quality or frontend path was touched. The
  test re-encodes the same planes through the BGRA and planes entries and
  requires identical packet bytes, tables and rate state, plus the existing
  "profile is fixed by the first successful frame" rule.
- Every strict registration that existed on `main` `309f007c` (160 measured
  before this slice); this slice adds two additive registrations.
- `upstream/` untouched, public API/ABI/constructors unchanged.

## Verification actually run

Linux x86_64, GCC 14.2.0, CMake 3.31.6, Ninja, Python 3.13.5, FFmpeg 7.1.5;
strict standalone configuration (`STANDALONE=ON`, `UPSTREAM=OFF`, `CUDA=OFF`,
`TESTS=ON`, `REQUIRE_EXTERNAL_DECODE=ON`), build directory outside the Git tree.

- Registration inventory measured before the slice on `main` `309f007c`: **160**.
- This head: **162 registered / 162 passed / 0 failed / 0 skipped**, 462.19 s,
  CTest parallelism 1 (JUnit SHA-256 `ad4e693724142858435d44e4aab3b4ee131ee764735e8967843006b4a7f09838`,
  local evidence, not committed). The two additive registrations are the new
  test in its normal and optimized build modes.
- New focused test: **252 checks PASS** in both modes (normal compiles its own
  non-NDEBUG capability translation unit; optimized consumes the Release
  library copy). The planning-ceiling boundary is source-bound: at
  `36P + 1MiB + 1` both the bound native layout and the equivalent BGRA buffer
  are refused, and at the exactly computed ceiling
  (`36P + 1MiB + 4*w*h`) both are admitted.
- `ci/test_standalone_gate.py`: **46/46 PASS** normal and **46/46** under
  `python3 -O`.
- Focused ASan + UBSan + LeakSanitizer build of the two new registrations:
  **2/2 PASS**. Not a whole-suite sanitizer claim.
- Registration/removal negative: deleting the new registration block drops the
  inventory to 160 and `ci/run_standalone.py` fails closed with
  `missing/duplicate CTest registrations: ['encoder_native_planes_normal',
  'encoder_native_planes_optimized']` before any test executes. The block was
  then restored byte-identically.
- Mutation controls, each scratch-only and reverted (original blob restored and
  re-hashed): disabling the GPU-native rule turns both modes RED (the refusal
  degrades to a misleading backend-storage/geometry cause); removing the native
  luma stride check turns both modes RED (`native refusal admitted allocation`);
  disabling the interleaved-session refusal turns both modes RED (`BGRA session
  query admitted a native planes source`); removing the native storage term from
  the planning ceiling turns both modes RED at the new boundary negative
  (`native refusal admitted allocation: planning ceiling`).

## Non-claims

No GPU execution of any kind, no Metal/CUDA planes implementation, no original
GoPro SDK, corpus or golden replay, no ST 2073 conformance, no product
interoperability, no performance claim. `#259`, `#46`, `#321` remain open; the
production GPU session matrix and the `reference_qualified=false` status are
unchanged. Native Windows/MSVC and macOS/ARM64 execution of the new test is
**NOT_RUN** on this host; hosted CI results are reported separately and are not
a native-platform qualification.
