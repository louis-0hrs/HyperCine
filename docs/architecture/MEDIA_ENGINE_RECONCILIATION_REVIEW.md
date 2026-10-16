<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Media engine and FilmScan3 frontend reconciliation review

Status: **PROPOSED decisions; REVIEW required**. Documentation-only deliverable
for [issue #34](https://github.com/louis-0hrs/HyperCine/issues/34), 2026-10-16.
This review does not accept either ADR or authorize production implementation.

## Reviewed revisions and outcome

| Source | Exact revision | Scope |
| --- | --- | --- |
| Protected main | `5c87391e82f307fbe7db0be6a53fd0edbdaad527` | Current internal frontend, scalar math, build and selection contracts |
| [PR #58](https://github.com/louis-0hrs/HyperCine/pull/58) | `f4b9747b643cc4e7645e3c5cc4a34db634545874` | ADR, capability matrix and pipeline gates |
| [PR #53](https://github.com/louis-0hrs/HyperCine/pull/53) | `24c7e4ea149dd4904997d48e3a0a6d9e34157b8c` | CUDA integration proposal |

Read issue #62, #34 and both PR discussions. The existing PR #58 review asks for
explicit separation of internal and proposed public APIs, ownership/fallback
semantics, and a small CPU-only PoC. The decisions below resolve those design
questions for maintainer consideration. Neither proposal branch was modified.

Recommendation: retain the existing internal frontend for incremental CUDA work;
limit the first engine PoC to host layout validation and capability selection.
Correct the following proposal gaps before implementation. CineForm profile,
sequence, geometry and container interoperability gates take priority over adding
generic media ASIC adapters, consistent with the scope correction in issue #34.

## Findings and required corrections

### R1: preserve FilmScan3 prescale and transform order (P1)

PR #53 section 2.1 suggests vertical followed by horizontal CUDA transforms or a
2D helper. These are not established substitutes for `ForwardLevel` in
[`standalone_frontend.cpp`](../../src/cpu/standalone_frontend.cpp).
The scalar path applies horizontal then vertical, with a distinct prescale=2
case. Highpass taps use `floor((sample + 3) / 4)` before filtering, while horizontal
lowpass uses `floor((left + right + 3) / 4)` on the original pair sum. Scaling
each input and summing it changes rounding. Reversing transform order cannot be
assumed exact when intermediate rounding and saturation are present.
For example, a pair (1, 1) gives lowpass floor((1 + 1 + 3) / 4) = 1;
summing individually rounded inputs instead gives 1 + 1 = 2.

[`Frontend::Transform`](../../src/core/filmscan3_backend.cpp) builds three levels
with prescales **0, 2, 0** per channel. The non-quantizing CUDA spatial helper in
[`wavelet_cuda_api.cu`](../../src/cuda/wavelet_cuda_api.cu) has no prescale
parameter. Existing general wavelet parity does not prove this FilmScan3 path.

Required correction: specify horizontal-then-vertical FilmScan3 semantics and
exact L2 pair-sum/tap rounding. Validate signed extrema, negative floor cases,
rounding boundaries and every output band against `ForwardLevel`. Require full
three-level stage and packet comparison before registering a CUDA frontend.
Quantized pyramid helpers cannot replace the unquantized frontend contract
without a separately reviewed ownership and quantization change.

### R2: use the actual internal seam and host result boundary (P2)

PR #53 section 1.1 lists only Convert/Forward/DeviceName. Current
[`Frontend`](../../src/core/filmscan3_backend.h) also has `Transform`, profiling
hooks and `FrontendFrame`; `FilmScan3Encoder::Encode` invokes `Transform`.
The default Transform composes Convert/Forward, so a simple adapter remains
possible. An override can batch GPU work, but must return the same host-readable
color/audit data and L3 LL plus highpass bands needed by CPU quantization/entropy.
Do not assume device residency across calls returning owning host `Plane` values.
Production and audit retention differ; empty discarded LL storage in production
is intentional. Account for upload, readback and waits in either implementation.

Keep these names and the `hypercine::standalone::detail` namespace internal.
`SurfaceLease`, `OperationRequest`, `Completion` and `DeviceKey` in PR #58 are
proposed engine types, not replacements for Frontend or new requirements on the
existing public FilmScan3 API. A future wrapper translates engine requests into
that seam; it does not create a second writer, quality calculator or stage ABI.

### R3: define the build split as a link boundary (P2)

PR #53 section 2.2 proposes `HYPERCINE_COREFORM_CORE` and a legacy alias. Neither
is implemented. Keep this spelling provisional until reviewed; use the existing
`HYPERCINE_BUILD_STANDALONE` option in current reproduction commands.
Removing the root guard alone is insufficient:
[`src/CMakeLists.txt`](../../src/CMakeLists.txt) puts CUDA kernels, upstream
wrappers and SDK-dependent encoder orchestration in `hypercine_wavelet`.
The future SDK-free CUDA target must select only the needed kernel/adapter
sources and avoid upstream headers and transitive SDK linkage. CPU scalar sources
must have one definition per linked image; linking both existing archives is not
a demonstrated solution. Preserve CPU fallback when an optional GPU adapter is
compiled, rather than interpreting 'one frontend' as removing the CPU reference.

Required future configuration tests: CPU core without any platform SDK; CPU core
plus CUDA without GoPro; optional GoPro oracle plus core; unchanged legacy wrapper
build. Inspect target include/link dependencies as well as configure success.
This review changes no CMake file or guard.

### R4: update dependency and qualification wording (P2)

PR #53 sections 1.3 and 4 still require PR #49. The canonical quality-state
implementation landed through PR #67 (`f43df20`); PR #49 is superseded. The
calculator's corpus parity is independent of wiring it into the FilmScan3 writer
and does not certify all six complete encoder profiles.
PR #58's matrix is dated evidence at its stated baseline. Experimental Vulkan
PR #61 now exists; refer to its exact source/evidence rather than treating an old
PLANNED row as a current validation result. No new device qualification was run
by this review, and GPU/decoder holds remain owned by their respective issues.

## Proposed ownership and failure decisions

| Boundary | Proposed contract |
| --- | --- |
| Host input | Retain a storage owner, not just a pointer. Caller relinquishes mutation rights until completion; reject overlapping writable leases. Alignment and pitch remain explicit. |
| Output | One writer per output region until successful completion; failed output is unusable. |
| Completion | Submission owns resources until actual quiescence even if the caller drops its completion handle or requests cancellation. A failed event alone does not prove memory is reusable. |
| Initialization fallback | Cpu is explicit; required Metal/CUDA fails if unavailable. An OrCpu policy can select CPU only at initialization with a recorded reason. Current MetalOrCpu catches factory runtime_error; preserve its API semantics. |
| Runtime failure | Propagate transform/readback/host-writer errors; do not replay automatically on CPU. Record stage and invalidate partial output. |
| Sequence state | Serialize one encoder session. Commit limiter, previous sample bytes and frame index only after complete sample creation. Failure preserves all three; independent sessions can run concurrently. |
| Device loss | Invalidate generation-scoped plans, capabilities and pools. Fail pending work, retain unsafe storage until quiescence, and recover explicitly. |

The current encoder already commits state after sample construction in
[`filmscan3_encoder.cpp`](../../src/core/filmscan3_encoder.cpp). Preserve that
strong exception guarantee when adding scheduling. An engine error category must
not silently broaden the exceptions that trigger legacy initialization fallback.

## Smallest separate CPU-only PoC proposal

Do not implement GPU objects, a scheduler, codecs or an ABI-stable public engine
in the first PoC. Start with internal C++17 types for immutable FrameDesc,
host-plane extents, OperationRequest, ComputeCapability and BackendError. Add a
pure validator and selector with an injected candidate list; no vendor ranking,
runtime SDK lookup or process-global configuration. Public header/versioning
requires a separate maintainer decision. The existing FilmScan3 API is unchanged.

| Deterministic acceptance case | Required result |
| --- | --- |
| Valid aligned BGRA8, bottom-up and padded pitch | Accept exact bounded host view; retain orientation and color facts |
| Empty/null plane, short extent, misalignment, zero rows or arithmetic overflow | Reject before pointer arithmetic, allocation or dispatch |
| Unknown color conversion or unsupported packing/profile/geometry | Unsupported/unknown with a reason; no invented metadata or capability |
| CPU-only candidate and GPU-required request | Reject; CPU candidate cannot satisfy GPU scope |
| GPU unavailable and explicit initialization fallback allowed | Select CPU with a recorded reason; no runtime retry permission |
| Stale device generation or mismatching surface/device | Reject plan; do not reuse cached qualification |
| Candidate order permutations and tied/missing costs | Stable caller-defined preference or CPU default; no fastest-device claim |

Use seed `0x1337BEEF` if generated cases are needed. Keep zero external core
dependencies and run native CTest plus ASan/UBSan where available. A later
submission PoC must add retained-lifetime, cancellation/quiescence, single-writer,
failure rollback and independent-session tests before it introduces async work.
These tests are an acceptance plan, not results from an implemented interface.

## Remaining approval and handoff

Maintainer review must accept or amend R1-R4, the ownership table, internal-first
PoC scope and the public ABI decision. Then assign a separate focused PoC issue
and PR. No PoC issue is created by this review because acceptance is pending.
Keep #34 open and both proposal PRs Draft. No merge, GPU backend allocation,
geometry expansion, performance improvement or NLE acceleration is approved.

## Validation performed for this documentation change

- Windows x64, MSVC 19.44.35229.0, Release SDK-free CPU build completed.
- Full registered suite: `ctest --test-dir build -C Release --output-on-failure`,
  **18/18 PASS**, 37.76 seconds. This includes backend selection and frozen
  fixture/manifest tests; external corpus replay is not implied.
- `python -m unittest discover -s ci -p test_privacy_guard.py`: **10 PASS**,
  **3 platform skips** (POSIX hook tests). Temporary Git integration tests needed
  execution outside the restricted sandbox; no scanner rules were changed.
- FFmpeg/ffprobe unavailable: configured `HYPERCINE_REQUIRE_EXTERNAL_DECODE=OFF`;
  external decoder tests were not registered and remain PENDING.
- Relative source links, balanced fences and staged whitespace checked; staged
  privacy scan passed and the document was manually reviewed.
- No production or upstream files changed; GPU, sanitizers and performance were
  not tested. No new algorithm or executable interface was introduced.

Reproduction from a clean checkout of the review branch (PowerShell):

```powershell
cmake -S . -B build -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=OFF
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python -m unittest discover -s ci -p test_privacy_guard.py
```
