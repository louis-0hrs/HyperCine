# Wiring the Linux CUDA compute path into the shared SDK-free codec core

Integration proposal for the maintainer decision of 2026-10-16 (issue #33, PR #35):
the shared SDK-free encoder core from #29 / PR #49 is the basis for future Linux CUDA
integration. That quality-state calculator has since landed as PR #67. No separate
Linux-CUDA encoder engine, and the existing CMake guards are not bypassed. Refs #29,
#33, #38, PR #49, PR #67.

This is a **proposal**, not an implementation. Nothing here is merged, and no
Windows/Linux bitstream parity is claimed.

## 1. What already exists (verified by reading the tree, not assumed)

### 1.1 The stage seam is already there and already has two implementations

`src/core/filmscan3_backend.h` declares an internal stage boundary:

    struct Frontend {
        virtual ~Frontend() = default;
        virtual Planes Convert(const uint8_t*, std::size_t, int, int, std::size_t) = 0;
        virtual WaveletLevel Forward(const Plane&, int) = 0;
        virtual std::string DeviceName() const = 0;
        virtual FrontendFrame Transform(const uint8_t*, std::size_t, int, int, std::size_t, bool audit=false);
        virtual bool ProfileHost() const { return false; }
        virtual void RecordHostTiming(const HostTiming&) {}
    };
    std::shared_ptr<Frontend> CpuFrontend();
    std::shared_ptr<Frontend> MetalFrontend();   // throws if disabled/unavailable
    std::shared_ptr<Frontend> SelectFrontend(FilmScan3Backend, MetalFactory = MetalFrontend);

with `enum class FilmScan3Backend { Cpu, Metal, MetalOrCpu }` in
`include/hypercine/filmscan3.h`. `Transform`, `ProfileHost` and `RecordHostTiming`
were added in `db98869`. Only `Convert`, `Forward` and `DeviceName` are pure virtual;
`Transform` has an out-of-line default in `src/core/filmscan3_backend.cpp`. The enum
and `SelectFrontend(FilmScan3Backend, MetalFactory)` are unchanged since `8631ef5`.
Two `Frontend` implementations exist today: the scalar CPU frontend
(`src/cpu/filmscan3_frontend.cpp`, `src/cpu/wavelet_cpu.cpp`) and the Metal frontend
(`src/metal/filmscan3_metal.mm`). Selection is explicit, and `MetalOrCpu` already
encodes a fallback policy; the Metal factory is injectable, which is what makes
deterministic failure/fallback tests possible. The Vulkan compute class on main
(`include/hypercine/vulkan_frontend.h`) is not a `detail::Frontend` and is not in
the enum.

**Consequence: a CUDA backend does not need a new architecture. It needs a third
implementation of an interface that already exists, registered in the same switch.**

### 1.2 The CUDA stages already expose the same contract

`include/hypercine/wavelet.h` is a stable C API over the kernels in `src/cuda/`:

- `HyperCine_IsCUDAAvailable()`
- `HyperCine_ForwardHorizontal26_CUDA` / `HyperCine_ForwardVertical26_CUDA`
- `HyperCine_ForwardSpatial2D_CUDA` / `HyperCine_ForwardSpatialQuant2D_CUDA` /
  `HyperCine_ForwardPyramid_CUDA`
- the inverse counterparts

and `src/cpu/wavelet_cpu.cpp` is compiled into **both** `hypercine_wavelet` (CUDA
configuration, `src/CMakeLists.txt`) and `hypercine_standalone` (SDK-free
configuration). The non-CUDA `hypercine_wavelet` stub target compiles it too.
`include/hypercine/wavelet.h` is unchanged since `8631ef5`. The CPU and CUDA sides
therefore already implement one stage contract. Draft PR #35's A2 evidence, which is
not in this tree, records 1680/1680 bit-exact transform checks. The current
`tests/test_cuda_wavelet.cpp` matrix is still 2 backends x 10 dimensions x 14 patterns
x 6 checks. Main does not contain that PR's active-pixel readback used for initcheck.

### 1.3 What PR #49 contributes

`include/hypercine/quality_state.h` + `src/core/quality_state.cpp`: the SDK-free
quality/state calculator — six regular profiles, the 36 quant/scale entries, p10 GOP-1
state transitions and separate FilmScan2/FilmScan3 limiter thresholds, validated by a
fail-closed corpus validator. It is on main as `hypercine_quality_state` (PR #67,
`f43df20`), linked from `hypercine_wavelet` and not from `hypercine_standalone`.
`FilmScan3Encoder` still builds its tables with `ComputeFilmScan3Tables` in
`src/core/filmscan3_encoder.cpp`. The calculator does not encode samples and does not
switch the production encoder away from the SDK, nor claim bitstream parity
(`docs/QUALITY_STATE_SDKFREE.md`).

### 1.4 The structural blocker

`HYPERCINE_BUILD_STANDALONE` and `HYPERCINE_ENABLE_CUDA` are mutually exclusive by
explicit `FATAL_ERROR`. Because of that conflation, there is currently **no Linux build
that has CUDA enabled and can emit a complete CFHD sample** (draft PR #35,
`docs/LINUX_CUDA_VALIDATION.md` section 7; that file is not on main) — the CUDA
configuration builds kernels and tests, not an encoder. The guard is unchanged in
wording and now sits at `CMakeLists.txt` after the quality-state and Vulkan options.

The guard treats two different things as one: *the SDK-free CineForm core* and *the
pristine GoPro SDK that is used as a reference oracle*. CUDA belongs to the first; it
has nothing to do with whether the GoPro oracle is built.

## 2. Proposed architecture

    +--------------------------------------------------------------+
    | shared CineForm core (SDK-free)                              |
    |   quality state / quant tables   <- #29 / PR #67 (landed)    |
    |   entropy + bitstream writer     <- single source, existing  |
    |   rate control / metadata        <- single source, existing   |
    +----------------------------+---------------------------------+
                                 |  detail::Frontend
         +----------------+------+------+----------------+
         |                |             |                |
     CpuFrontend    CudaFrontend   MetalFrontend   VulkanFrontend
     (exists)       (proposed)     (exists)        (separate class, not this seam)

The core owns quality/quantisation/entropy/bitstream/rate-control. A backend owns only
`Convert()` (BGRA -> planes) and `Forward()` (one wavelet level). Nothing about
entropy, bitstream framing or quality tables is duplicated per backend.

### 2.1 New code, and deliberately nothing else

1. `src/cuda/filmscan3_cuda_frontend.cu` — a `Frontend` whose `Convert()` calls the
   existing CG709/CUDA colour path and whose `Forward()` calls
   `HyperCine_ForwardVertical26_CUDA` + `HyperCine_ForwardHorizontal26_CUDA` (or the 2D
   variant when the geometry allows). It owns device buffers and the stream, and throws
   `std::runtime_error` when no usable device exists — the same contract the Metal
   frontend has, so `CudaOrCpu` fallback behaves exactly like `MetalOrCpu`.
2. `include/hypercine/filmscan3.h` — extend the enum with `Cuda, CudaOrCpu` only.
3. `src/core/filmscan3_backend.cpp` — extend `SelectFrontend` with a `CudaFactory`
   parameter modelled on the existing `MetalFactory` (injectable for tests, no global
   switch).
4. CMake — split the guard as described in section 2.2.

No wavelet kernel, entropy routine or bitstream writer is reimplemented. The adapter
is the whole new surface.

### 2.2 CMake: split one conflated guard into two orthogonal ones

    HYPERCINE_COREFORM_CORE     # SDK-free core + selected frontends (cpu | cuda | metal | vulkan)
    HYPERCINE_BUILD_UPSTREAM    # pristine GoPro SDK, oracle/parity use only

Rules:

- `HYPERCINE_COREFORM_CORE=ON` with any single frontend, including CUDA.
- `HYPERCINE_BUILD_UPSTREAM=ON` stays independent and unchanged; it is an oracle, and
  combining it with a core build enables the existing same-input comparison harness.
- `upstream/` remains untouched, exactly as today.
- `HYPERCINE_BUILD_STANDALONE` keeps working as the current SDK-free core switch, so
  nothing in the existing CI breaks. It is not CPU-only: Metal and Vulkan are compiled
  into `hypercine_standalone` and both require this switch. A legacy alias has to keep
  those frontends; it is not `frontend=cpu`.

This removes the false choice that currently blocks the encoder without weakening any
safety guard: the guard that matters ("do not silently substitute a CPU fallback for the
Windows oracle") is expressed by `*OrCpu` selection plus the existing explicit-failure
contract, not by forbidding CUDA next to the SDK.

## 3. Validation plan (gate before any production claim)

1. **Reuse the existing harness.** `hypercine_encode_filmscan3` does not take
   `--cpu-encoder` and does not compare packets. It accepts `--reference-yuv` and
   `--metadata-binary` and writes samples with the default CPU `FilmScan3Encoder`
   (Metal only when that tool is rebuilt with `HYPERCINE_METAL_AUDIT`). Full raw
   packet comparison is `tests/validate_filmscan3.py --cpu-encoder <cpu-binary>`,
   which CMake uses when the encoder under test is `hypercine_encode_filmscan3_metal`.
   A CUDA check is a second binary on that harness. The encoder target is still
   created only inside `HYPERCINE_BUILD_STANDALONE`, so no CUDA build emits it.
2. **Stage parity first.** The A2 result recorded in draft PR #35 (1680/1680 bit-exact
   transform checks, CPU vs CUDA reference *and* optimized) is the foundation; extend to
   quant/entropy stages with the same input SHA discipline. It has not been re-run for
   this document.
3. **Normalized corpus parity second.** The verified Windows corpus
   (`qualities/2026-10-15`, 1760/1760 files, index SHA-256
   `2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`) shows
   `sdk_normalized_sha256 == cuda_normalized_sha256` on all 348 frames. That is the
   acceptance target for the new backend: full normalized sample equality over the six
   qualities, then the 120-frame stateful sequence.
4. **Cross-architecture caveat, stated up front.** The corpus's CUDA reference came from
   an RTX 4090 Laptop (sm 8.9); this host is an A2 (sm 8.6). Because integer wavelet and
   entropy paths should be architecture-independent, a mismatch would have to be
   attributed (A2-CUDA vs A2-CPU first) before it is called a porting defect. The plan
   above does that attribution by construction.
5. **Safety gates per backend**, on the A2, with `--error-exitcode 99` and log files:
   `memcheck --leak-check full`, `initcheck`, `racecheck`, `synccheck`. The
   `test_decode_phase3b2a_temporal` result in `docs/DECODE_PHASE3B2A_LINUX_SANITIZER.md`
   (issue #38, draft PR #52) is the working template. That file is not on main.
6. **GPU-required tests stay fail-not-skip.** A backend test must FAIL without a usable
   device rather than silently reporting a CPU result.

## 4. Suggested order of work

1. The SDK-free quality-state calculator has landed (PR #67). Wiring it into
   `FilmScan3Encoder` in place of `ComputeFilmScan3Tables` is still open; this proposal
   does not do that.
2. Split the CMake guard (section 2.2) as its own small, reviewable change with no
   behaviour change for existing configurations.
3. Add `Cuda` / `CudaOrCpu` to the enum plus a throwing `CudaFrontend()` stub, so the
   selection and fallback paths are testable without a device — mirroring how
   `MetalFrontend()` throws when Metal is not built.
4. Implement `src/cuda/filmscan3_cuda_frontend.cu` against the existing CUDA stage API.
5. Run the same-input CUDA-vs-CPU comparison, then the normalized corpus gate, then the
   sanitizer battery; publish results the same way draft PR #35 published its A2 evidence.
6. Vulkan compute already exists off this seam (`include/hypercine/vulkan_frontend.h`).
   Moving it onto `SelectFrontend` stays separate from the CUDA adapter.

## 5. What this proposal does not claim

- No Windows/Linux bitstream parity, and no production switch of the encoder away from
  the GoPro SDK.
- No new CUDA kernel and no entropy/bitstream duplication.
- No bypass of the CMake guards: they are *split*, not removed, and the oracle guard
  keeps its meaning.
- The Metal frontend is not modified.
