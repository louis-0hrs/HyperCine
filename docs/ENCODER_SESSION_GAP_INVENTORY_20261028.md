# Encoder session / profile / geometry / backend — current-main gap inventory

Issue #259, parts A–E. Evidence-only inventory at a single exact commit.
**Not** a product-support claim, **not** a GoPro pixel-oracle claim, **not** a release note.

- Base: protected `main` = `f1591d246512f41b8bba1fbad2455ef1a764491a` (#298 squash).
- Method: every statement below was re-read from the tree at that SHA (`grep -n` / source),
  not carried over from PR prose. Measured test results are in §8 and name the host.
- Related issues: #34, #45, #23, #33, #37, #36, #48, #70, #29, #46.

## 1. What already landed for #259

| PR | Advance | Explicitly does **not** prove |
| --- | --- | --- |
| #263 | `CheckEncoderCapability` fail-closed capability contract, parts A/B prototype | No wiring into `FilmScan3Encoder`; CPU-scalar only |
| #266 | `ValidateFilmScan3Geometry` separates wavelet shape from allocation budget | No new qualified geometry |
| #261 | Optional per-session coefficient-hash diagnostic | Side channel only, not embedded in the sample |
| #262 | 30 fps metadata split and 65535-frame stop | No other timebase |
| #269 | Per-session CPU ISA policy (`WaveletIsa` pinned per session) | CPU only; no GPU ISA policy |
| #271 | Metal geometry matrix | macOS only, synthetic input |
| #269 (contains #272) | Concurrent scalar+NEON sessions, per-session ISA | CPU only; `21afbf08` (#272) is not an ancestor of main — its content arrived via #269's squash `74f8952` |
| #273 | Metal session isolation | macOS only |

So parts A/B exist **as an internal prototype**, part C is half-done (CPU), part D is
partially covered, part E is untouched.

## 2. Public vs internal surface (exact anchors)

| Item | Location | State |
| --- | --- | --- |
| `enum class FilmScan3Backend { Cpu, Metal, MetalOrCpu }` | `include/hypercine/filmscan3.h:70` | Only **public** backend selector; there is no `Cuda` and no `Vulkan` value |
| `FilmScan3Encoder(FilmScan3Backend, FilmScan3Geometry)` | `include/hypercine/filmscan3.h:83` | Public; resolves through `detail::SelectFrontend` |
| `FilmScan3Encoder(FilmScan3Geometry, WaveletIsa)` | `include/hypercine/filmscan3.h:89` | Public CPU session with pinned ISA |
| `geometry()`, `wavelet_isa()` | `include/hypercine/filmscan3.h:90,94` | Read-back for the caller |
| `EncoderSessionConfig`, `EncoderDevice`, `CapabilityReason`, `EncoderCapabilityResult` | `src/core/encoder_session_config.h:60,95,110,120` | Internal, not installed as public header |
| `CheckEncoderCapability(...)` | `src/core/encoder_session_config.h:142`, `src/core/encoder_capability.cpp:101` | Internal; **no production call site** |
| `SessionBackend { CpuScalar, CpuSimd, Metal, Cuda, Vulkan }` | `src/core/encoder_session_config.h:36` | Capability vocabulary only — not the encoder API |
| `detail::SelectFrontend(FilmScan3Backend, FilmScan3Geometry)` | `src/core/filmscan3_backend.cpp:61` | Three cases only (see §6) |
| `detail::CudaFrontend(FilmScan3Geometry, CudaTestMode)` | `src/core/filmscan3_backend.h:57` | Declared, implemented, **unreachable from public API** |
| `VulkanFrontend` | `include/hypercine/vulkan_frontend.h:57`, `src/vulkan/vulkan_frontend.cpp` | Standalone class, **not** a `detail::Frontend` and **not** in `SelectFrontend` |
| `Rgb444Frame::Copy` / `CopyRg48` | `include/hypercine/rgb444.h`, `src/cpu/rgb444_frontend.cpp` | Source-plane storage/importer only — no writer, no encoder |

## 3. The seven axes: implemented, but only in the prototype

| Axis | Implemented at | Wired into the real encoder? |
| --- | --- | --- |
| Format legality | `ValidateFilmScan3Geometry` `src/cpu/filmscan3_frontend.cpp:16` | **Yes** — every frontend factory calls it |
| Backend implemented | `SelectFrontend` `src/core/filmscan3_backend.cpp:61` | Yes, but for three enum values only |
| Backend qualified for geometry | `kCpuExperimentalEvidence[]` `src/core/encoder_capability.cpp:23` | **No** — capability prototype only |
| Hardware present | `BackendPresent` `src/core/encoder_capability.cpp:51` | **No** — caller-supplied booleans, never probed |
| Memory budget | `PlanningCeiling`/`LowpassPayloadBytes` `src/core/encoder_capability.cpp:69,76` | **No** — writer only enforces its own capacity |
| Reference data | `result.reference_qualified = false;` `src/core/encoder_capability.cpp:104` | Hard-false everywhere; no oracle exists |
| Explicit fallback | `allow_cpu_scalar_fallback`, `src/core/encoder_capability.cpp:206,210,297` | Only `MetalOrCpu` degrades, at the `SelectFrontend` level |

`CheckEncoderCapability` call sites at this SHA are exactly two, both tests:
`tests/test_encoder_capability.cpp:56` and `tests/windows_review/review_frame_rates.cpp:13`.
**There is no code path that constructs a `FilmScan3Encoder` from a validated
`EncoderSessionConfig`.** That missing seam is the core #259-A deliverable.

Fail-closed behaviour that *does* hold today:

- `allocation_permitted` is false unless the primary reason is `ExperimentallySupported`
  (`src/core/encoder_capability.cpp:344`).
- A missing backend blocks unless `fell_back_to_cpu_scalar` (`:270`, `:297`, `:313`).
- Explicit `Metal` never degrades — `MetalFrontend` throws
  (`src/core/filmscan3_backend.cpp:66`, `:83-85`).

## 4. Geometry, pitch and budget arithmetic

| Limit | Value | Enforced at |
| --- | --- | --- |
| Width/height shape | `width % 128 == 0`, `width >= 128`; `height % 8 == 0`, `height >= 24` | `src/cpu/filmscan3_frontend.cpp:17` |
| Wire dimension field | `kMaxWireDimension = 65535` | `include/hypercine/standalone.h:41`, used `filmscan3_frontend.cpp:18` |
| Pixel budget | `kMaxPlaneElements = 3840*2160 = 8 294 400` | `include/hypercine/standalone.h:47`, `filmscan3_frontend.cpp:22` |
| Sample ceiling | `8*pixels + 16384` | `src/cpu/filmscan3_frontend.cpp:30`, `filmscan3_encoder.cpp:159` |
| CFHD chunk word count | 24-bit words ⇒ ≤ 67 108 860 payload bytes | `Writer::End` overflow check, `src/core/filmscan3_encoder.cpp:56` |
| Input storage | 16-byte aligned, `stride % 16 == 0`, overflow-checked | `ValidateFilmScan3Input`, `src/core/filmscan3_backend.cpp:88` |
| Legacy pixel cap | `kMaxPixels = 2 097 152`, `kMaxDimension = 4096` | `include/hypercine/standalone.h:45,55` — legacy path only |

Two findings worth separating from PR prose:

1. **Default-1080p leak.** `FilmScan3Geometry` defaults to `1920x1080`
   (`include/hypercine/filmscan3.h:14`) and the base `Frontend::Geometry()` returns `{}`
   (`src/core/filmscan3_backend.h:25`). An injected frontend that does not override
   `Geometry()` therefore reports a 1080p geometry it was never validated for. All three
   real frontends do override it (CPU `filmscan3_backend.cpp:36`, CUDA
   `src/cuda/filmscan3_cuda_frontend.cpp:69`, Metal `src/metal/filmscan3_metal.mm:125`),
   so this is a **latent** contract hole, not a live mis-encode.
2. **Stale FilmScan3 size audit, not the legacy documentation.** The historical
   `docs/CAPABILITY_8K_17K_AUDIT.md:12` describes the old `[128,4096]` FilmScan3
   axis limit without distinguishing the post-#266 pixel budget. Current FilmScan3
   uses `kMaxWireDimension = 65535` and refuses oversized frames on the *pixel
   budget* instead (`git log -S kMaxWireDimension` → `cbeab83`). By contrast,
   `docs/MACOS_STANDALONE_ENCODER.md:228` explicitly documents legacy fixed-Q2
   `EncodeBgra` with `kMaxPixels = 2,097,152` and `kMaxDimension = 4096`;
   that legacy limit is intentional and must **not** be changed by #259.

## 5. Profile and format separation

| Concept | Declared | Actually implemented in the single writer |
| --- | --- | --- |
| `CodecFamily::FilmScan3` | Yes | **Yes** — one writer, `src/core/filmscan3_encoder.cpp:139,145` |
| `CodecFamily::Rgb444` | Yes | No — source storage only (`src/cpu/rgb444_frontend.cpp`) |
| `CodecFamily::LegacyFixedQ2` | Yes | No — separate legacy path, cap unchanged |
| `EncodingQuality` (6 values, `include/hypercine/quality_state.h:14`) | Yes | Only `FilmScan3`; the rest are `ValidButNotImplemented` (`src/core/encoder_capability.cpp:120`) |
| `SourcePixelFormat::Bgra8` | Yes | **Yes** |
| `Yuy2`, `Rgb48` | Yes | No |
| `GopMode::IntraGop1` | Yes | **Yes** |
| `Gop2`, interlaced | Yes | No |
| `DiagnosticMode::CoefficientFnv64` | Yes | Yes, side channel |
| `EmbedHashesInSample` | Yes | No — would change the bitstream |

The shared bit-exact seam for every backend that exists is: `Frontend::Transform`
(color + 3-level 2-6 wavelet), then host-side `ComputeFilmScan3Tables` → `QuantizeFilmScan3`
→ `PackHighpass` → one `Writer`. Backends can only differ in what `Transform` produces.

## 6. The real backend-selection gap (what part C actually needs)

`SelectFrontend` has exactly three cases and one hard-coded fallback rule:

```text
case Cpu        -> CpuFrontend(geometry)
case Metal      -> MetalFrontend(geometry)      // throws, never degrades
case MetalOrCpu -> try Metal, catch runtime_error -> CPU
otherwise       -> throw invalid_argument
```

Consequences, all verifiable at this SHA:

- Adding `Cuda` (or `Vulkan`) to the public `FilmScan3Backend` without a new
  `SelectFrontend` case yields `invalid_argument` at runtime, not a typed refusal.
- `CudaFrontend` can only be injected through `FrontendAccess::Use`
  (`src/core/filmscan3_backend.h:72`) — a test seam, not a selector.
- `VulkanFrontend` is a different class hierarchy: it does not implement
  `detail::Frontend`, so "CUDA and Vulkan selectable on the same contract" (#259-C) is
  **not** satisfied even though both code paths exist.
- `SessionBackend::CpuSimd` has no `SelectFrontend` case and no frontend at all; AVX2/AVX-512
  exist as *detection* only (`src/cpu/cpu_isa.cpp`). #278 is an integration question.

## 7. Session isolation and global state

| State | Scope | Effect on #259 |
| --- | --- | --- |
| `t_vector_forced_unsupported` (`src/cpu/standalone_frontend.cpp:103`) | `thread_local` | Test seam only; cannot perturb a concurrent encode |
| NEON enable default (`HyperCine_IsNEONWaveletEnabled`) | process-global | Read once per session when an explicit ISA is requested (`ResolveWaveletIsa`, `standalone_frontend.cpp:175`); `Auto` stays live by design |
| `CpuFrontend()` shared singleton (`filmscan3_backend.cpp:52`) | process-global | Used by the **backend-only** constructor, which carries no geometry — the 1080p default of §4 |
| `Writer` capacity / rate limiter | per session (`FilmScan3State`) | Correct: ordered frames and two concurrent sessions covered by #272 |

Ordered-session contract: `FilmScan3State::frame_index` and `fs_rate_limiter` advance per
session; a failed `Encode` does not stamp the diagnostic identity
(`src/core/filmscan3_encoder.cpp`, after sample close).

## 8. Test inventory and measured results

Registered CTest entries that touch the encoder contract at this SHA (root
`CMakeLists.txt` + `tests/CMakeLists.txt`, conditional on build flags):

`encoder_capability_contract`, `filmscan3_api`, `filmscan3_geometry`,
`filmscan3_geometry_budget`, `filmscan3_geometry_external_decode`,
`filmscan3_external_decode`, `filmscan3_backend_selection`, `filmscan3_fixture_integrity`,
`filmscan3_geometry_validation`, `filmscan3_geometry_reference_p10`,
`filmscan3_autonomous_manifest`, `filmscan3_sequence_manifest`,
`filmscan3_actual_tier_a`, `filmscan3_actual_sequence`, `cpu_isa_selection`,
`neon_wavelet_bitexact`, `neon_concurrency_contract`, `wavelet_isa_session_policy`,
`wavelet_isa_parallel_sessions`, `rgb444_frame_storage`, `rgb444_frame_rejection`,
`rgb444_rg48_import`, `rgb444_rg48_rejection`; GPU-gated: `filmscan3_metal_required`,
`filmscan3_metal_multisession`, `filmscan3_metal_geometry_matrix`,
`filmscan3_metal_4k_encoder`, `filmscan3_metal_4k_ffmpeg_gate`,
`vulkan_filmscan3_cpu_parity`, `cuda_filmscan3_complete_packet`,
`cuda_filmscan3_multigeometry_sequence`, `cuda_filmscan3_ffmpeg_decode`,
`cuda_filmscan3_sequence_ffmpeg_decode`, `cuda_filmscan3_output_dir_negative`.

`tests/test_encoder_capability.cpp` contains 37 capability assertions plus a six-geometry
encode+FFmpeg-decode matrix. `SKIP_RETURN_CODE 77` on the GPU tests means a skip is **not**
a GPU proof.

Hosted CI families at this SHA (`.github/workflows/`): `ci-cpu.yml` (windows-latest MSVC),
`ci-standalone.yml` (matrix), `ci-cuda-build.yml` (ubuntu-24.04 build + registered-test
inventory gate), `quality-state.yml`, `privacy-guard.yml`.

### 8a. Measured on-host results (Linux x86_64, NVIDIA A2)

Run at `f1591d246512f41b8bba1fbad2455ef1a764491a` in an isolated worktree built outside
the Git tree. `git status --porcelain upstream/` was empty before and after. Both builds
required `HYPERCINE_BUILD_UPSTREAM=OFF`, because `CMakeLists.txt:195` rejects
`HYPERCINE_BUILD_STANDALONE=ON` together with `BUILD_UPSTREAM=ON`; that is a build-flag
contract, not a code defect.

| Build | Configure | Result |
| --- | --- | --- |
| CPU standalone, Release, Ninja | `-DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON` | 93/93 objects, success |
| CUDA standalone, Release, Ninja | same, plus `-DHYPERCINE_ENABLE_CUDA=ON` | 105/105 objects, success |

| CTest selection | PASS | FAIL | SKIP | Wall |
| --- | --- | --- | --- | --- |
| Full CPU suite | 105 | 0 | 0 | 142.41 s |
| `-L cuda` | 9 | 0 | 0 | 34.66 s |

No test returned `SKIP_RETURN_CODE 77`, so the CUDA numbers are real executions, not skips.
`cuda_filmscan3_multigeometry_sequence` ran resident on the A2: six geometries with packet
parity over 12 CUDA launches; FFmpeg decode covered 1920x1080 and 3840x2160 only.

One transient failure is recorded rather than hidden: `filmscan3_geometry_external_decode`
failed once with `ENOSPC` on a tmpfs scratch **after** the encoder had already printed its
4K pass (143.82 s); re-run against a large scratch directory it passed in 140.69 s. A full
scratch filesystem must not be read as a codec failure.

The geometries that encode and decode end-to-end on this host are exactly the six in
`kCpuExperimentalEvidence` (`src/core/encoder_capability.cpp:23-29`). That is still not
original-SDK qualification.

No `filmscan3_*4k*` test is registered on Linux: those names are Metal-gated
(`CMakeLists.txt:614`).

### 8b. Three evidence traps found while measuring

**The "Metal 4K FFmpeg gate" does not run Metal.** `CMakeLists.txt:615-619` registers
`filmscan3_metal_4k_ffmpeg_gate` inside `if(HYPERCINE_ENABLE_METAL)`, but passes
`--encoder $<TARGET_FILE:hypercine_encode_filmscan3>` — the plain CLI. The Metal-aware
binary is a different target, `hypercine_encode_filmscan3_metal` (`CMakeLists.txt:412`),
compiled with `HYPERCINE_METAL_AUDIT=1`, which is the only thing that installs the Metal
audit frontend through `FrontendAccess::Use` (`tools/hypercine_encode_filmscan3.cpp:72-76`).
Without that macro the CLI calls `FilmScan3Encoder(geometry)`, the CPU constructor
(`src/core/filmscan3_encoder.cpp:112`). **A green `filmscan3_metal_4k_ffmpeg_gate` is
therefore not Metal evidence today.** This is a test-selection defect, not a codec defect,
and it bears directly on the #45/#23 parity claim.

**No workflow enables Metal.** `HYPERCINE_ENABLE_METAL` occurs in `.github/workflows/`
only as `=OFF` (`ci-cuda-build.yml:117`). Metal evidence remains owner-host evidence.

**CI binding is partial, not absent.** `ci/run_standalone.py:REQUIRED` does contain
`filmscan3_backend_selection`, `filmscan3_geometry`, `filmscan3_geometry_external_decode`,
`filmscan3_geometry_validation`, `filmscan3_api`, `cpu_isa_selection` and the four
`rgb444_*` tests. It does **not** contain `encoder_capability_contract`,
`filmscan3_geometry_budget`, `wavelet_isa_session_policy`,
`wavelet_isa_parallel_sessions`, `neon_wavelet_bitexact` or `neon_concurrency_contract`.
The capability contract and the per-session ISA proofs can therefore be deleted from the
inventory without turning CI red. That is the concrete, narrow CI gap — not "nothing of
#259 is bound".

## 9. Reconciliation map — no per-resolution or per-backend forks

| Issue | What it needs from #259 | Where it plugs in |
| --- | --- | --- |
| #45 / #23 Metal | Same-input CPU↔Metal packet parity at 4K **and** noncanonical geometry, session isolation, explicit GPU-required vs. fallback | `SelectFrontend` case `Metal`; capability axis 3 must gain a Metal qualification record (today hard `false`) |
| #33 CUDA (Linux/A2) | Public selector only **after** #249 integration, plus multigeometry and ordered-session evidence to existing goldens | New `SelectFrontend` case `Cuda` returning either a frontend or a typed refusal; `CudaFrontend` already takes geometry |
| #37 / #36 RGB444 p12 | A **separate** verified format family; must not retarget the FilmScan3 writer | `CodecFamily::Rgb444` + `Rgb444Frame` storage as the input side; needs its own writer semantics after F1 reference |
| #70 NEON | Per-session ISA policy (landed) | Already session-scoped; `CpuSimd` backend still unimplemented |
| #48 extreme resolution | Do **not** raise `kMaxWireDimension`; separate R&D gate | Capability refusal path already reports `MemoryBudgetExceeded` + `BackendNotQualifiedForGeometry` |

## 10. Smallest scoped next step (Draft PR, review-only)

One internal function, no writer change, no public header change in the first commit:

```cpp
// src/core/encoder_backend_select.h (internal)
struct BackendSelection {
    std::shared_ptr<detail::Frontend> frontend;  // null on refusal
    EncoderCapabilityResult capability;          // typed refusal reason
};
BackendSelection SelectBackend(const EncoderSessionConfig&, const EncoderDevice&);
```

It composes what already exists — `CheckEncoderCapability`, `ValidateFilmScan3Geometry`,
`BackendPresent`, and the three frontend factories — and returns either a frontend or a
typed refusal. `FilmScan3Backend` gains **no** new public value in this step; a `Cuda`
case is added only once #249's geometry/evidence gate is expressed in the capability table.

Follow-up commits, each separately reviewable: (a) route the public
`FilmScan3Encoder(backend, geometry)` constructor through `SelectBackend` with an
`EncoderDevice` built from the caller; (b) make `Frontend::Geometry()` non-defaulting or
assert it was validated; (c) correct the stale **FilmScan3** size-audit claim of §4,
without changing the intentional `[128,4096]` legacy fixed-Q2 contract.

## 11. What this inventory does not prove

- No GoPro original-SDK oracle, no complete bitstream parity, no product release.
- `SUPPORTED_EXPERIMENTAL` remains experimental: `reference_missing` is hard `true`.
- Metal/CUDA/Vulkan qualification is **not** established here; skipped GPU tests are not
  evidence.
- The six `kCpuExperimentalEvidence` geometries are attempts, not qualified modes;
  `4096x2024` is deliberately absent.
- Nothing here changes `upstream/`, the legacy Q2 path, or any rate-state semantics.

## 12. Owner actions that block progress

1. **Metal qualification** needs a native M2 run of the geometry matrix and 4K FFmpeg gate
   on the current SHA — macOS hardware window.
2. **CUDA public selector** needs a decision to accept #249's evidence as the gate, plus a
   free A2 window for multigeometry and 4K packet parity.
3. **RGB444 p12** needs the verified F1 reference corpus before a writer exists.
4. **Reference oracle** (GoPro original SDK pixel comparison) is unavailable and remains
   the reason `reference_qualified` cannot become true.
