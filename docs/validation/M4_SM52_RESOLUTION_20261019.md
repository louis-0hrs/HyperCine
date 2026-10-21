# Tesla M4 / SM 5.2 bounded 4K/8K Phase 2C qualification

Status: **PARTIAL / RESOURCE-BOUNDED**, 2026-10-19; corrected 2026-10-21.
Issue: #116.

This note records a bounded native qualification of the Phase 2C encoder path on
a 4 GiB-class device. It closes the "independent SM 5.2 4K/8K and
performance/resolution qualification" item left open by Handoff 5. It is not a
claim of full 8K support or of a Maxwell/CUDA defect.

The 2026-10-21 corrections from independent native review qualify the pipeline
`PASS` label as an execution result that includes a nondeterministic
reference-decoder PSNR gate, state the clean-build prerequisites explicitly, and
record the 8K device-class policy. No measurement, configuration or gate was
changed.

## Exact source and toolchain

| field | value |
| --- | --- |
| Tested commit | `a987f679f7e17a88aebe60375c37c843d7095152` (protected `main`) |
| CMake | 3.31.12, Ninja, Release |
| MSVC | 19.44.35229.0, `/O2 /Ob2 /DNDEBUG /MD` |
| CUDA toolkit | 12.8.93, `CMAKE_CUDA_ARCHITECTURES=52` |
| Device | Tesla M4, compute capability 5.2, 3,953,131,520 B, 8 SMs |
| Driver | 582.78 |
| CPU host class | AMD EPYC 7601 guest, 8 logical processors, 1 processor group, no SMT |
| Correction reruns (2026-10-21) | `f8addc5a8922c31350fe107be20b1e9daa90c5e3` (PR #194 head) |

The same identities were also re-checked at CUDA candidate
`7ed4a82ed97598454c2680bd264dedf259e6e7dc` (PR #187). Results matched; the table
below uses protected `main`.

## Reproduction

Clean-build prerequisites at this head:

- Use CUDA **12.8.x**. CUDA 13.x no longer accepts `sm_52`: CUDA 13.4 returns
  `nvcc fatal: Unsupported gpu architecture 'sm_52'` for `-arch=sm_52`.
- `hypercine_wavelet` compiles C++ sources
  (`src/core/phase2b_encoder.cpp`, `src/core/phase2c_encoder.cpp`,
  `src/core/phase2d_encoder.cpp`) that include `cuda_runtime.h`, but that target
  does not receive `${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}`. A clean
  `cmake --build ... --target hypercine_m4_resolution_probe` therefore stops
  with `fatal error C1083: ... "cuda_runtime.h": No such file or directory`.
  The independent review and the 2026-10-21 reruns supply the toolkit include
  directory through the MSVC `INCLUDE` variable after `vcvars64.bat`, for
  example:

  ```bat
  set "INCLUDE=%CUDA_PATH%\include;%INCLUDE%"
  ```

  This is a build-wiring gap in the CUDA library target, not probe logic, and is
  not changed by this documentation child.

Build the public Phase 2C library and the bounded probe:

```bash
cmake -S . -B build-m4 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_ENABLE_CUDA=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DCMAKE_CUDA_ARCHITECTURES=52
cmake --build build-m4 --target hypercine_m4_resolution_probe --parallel 4
```

Identity gates (CPU versus consolidated, streamed and GPU-lowpass bitstreams):

```bash
build-m4/hypercine_m4_resolution_probe identity 3840 2160
build-m4/hypercine_m4_resolution_probe identity 7680 4320
```

One pipeline configuration (width height slots frames streams host|device):

```bash
build-m4/hypercine_m4_resolution_probe pipeline 3840 2160 3 4 4 host
build-m4/hypercine_m4_resolution_probe pipeline 7680 4320 1 2 4 device
```

The probe consumes only public `hypercine/phase2c.h` entry points. It does not
modify production code, existing tests or frozen `upstream/`.

## Identity results

All CPU/GPU bitstream, lowpass, streamed and multi-stream equality flags were
`1`; `decoded_max_abs_diff=0`. The upstream decoded-pixel comparison also matched
in these bounded calls.

| geometry | sample bytes | sample SHA-256 | decoded SHA-256 | PSNR | repeats |
| --- | --- | --- | --- | --- | --- |
| 3840x2160 | 3,563,684 | `f27b313c6ad7ece0bb04996694920426bc1d673a570a9652cdaac21e3fdecf8a` | `f6e4a4dde2c2e0695c8a575ef32c1e1b239912fe37254be8364ad6b130878ae9` | 44.553943 dB | 3 main + 5 candidate |
| 7680x4320 | 14,009,028 | `a14bf4ec63b82aabd3f5314bd084481ca06d6944f986e9f21295df28f5886ae2` | `91373883fc5361188724dc8c72e37f58a7cf48bf1823405791bb6a4acd314bbd` | 44.582261 dB | 2 main + 2 candidate |

## Pipeline results

Frames below exclude one warmup frame. `bitstream_match` and `decode_ok` are the
public Phase 2C gates and were `1` in every passing row. `slot_vram` and
`slot_pinned` are the API's analytical per-configuration accounting; `vram_used`
is measured while the slots are still resident.

`result` is an **execution result**, not a deterministic pixel verdict.
`RunPhase2C` includes the frozen original-SDK reference-decoder PSNR gate, which
is `ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC` for this platform (#120, #205,
#206). The binding encoder gates in every row are `bitstream_match`,
`decode_ok` and the identity probe's coefficient/payload equality. The reported
PSNR is diagnostic only; a `PASS` row does not certify pixel accuracy.

| width x height | input | slots | streams | frames | execution result | FPS | `vram_used` | share of device |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 3840x2160 | host | 3 | 4 | 4 | PASS* | 21.23 | 1,125,777,408 | 28.5% |
| 3840x2160 | device | 3 | 4 | 4 | PASS* | 16.06 | 1,125,777,408 | 28.5% |
| 7680x4320 | device | 1 | 4 | 2 | PASS* | 5.68 | 2,333,736,960 | 59.0% |
| 7680x4320 | device | 2 | 4 | 2 | PASS* | 4.37 | 3,381,002,240 | 85.5% |
| 7680x4320 | device | 3 | 2 | 2 | PASS* | 3.95 | 3,874,750,464 | 98.0% |
| 7680x4320 | device | 3 | 4 | 3 | **FAIL** | 0 | 3,874,750,464 | 98.0% |

\* All encoder/bitstream gates passed, and the included reference PSNR gate
happened to pass in that run. A `FAIL` row keeps the strict failure visible and
is not retried into a pass.

At this head, an independent native review run recorded one 4K HostInput 3/4/4
run with `bitstream_match=1`, `decode_ok=1` and PSNR **14.325330 dB** against the
35 dB gate; an immediate repeat passed at 44.553943 dB. The 2026-10-21
correction rerun executed the same configuration in six consecutive fresh
processes and observed 44.553943 dB each time. Both are bounded observations of
the same nondeterministic gate; no failure frequency is claimed.

The failing row returned `QueueGpuEntropyFrame failed` before the first encoded
frame. It reproduced **3/3** times at candidate `7ed4a82` and **2/2** times on
`main`. The identical 3-slot configuration passes with 2 entropy streams, and
the 8K identity gates pass at both 4 and 3 occupied slots. That separates a
device-capacity/configuration boundary from a wavelet, entropy-math or ISA
defect.

The public API returns a generic boolean from `QueueGpuEntropyFrame`; it does not
expose the internal CUDA allocation status. The exact failing allocation is
therefore **not claimed**. The measured 98% device occupancy and the 2-stream
pass are the basis for the resource-bound classification.

## Full CTest observation

`ctest -C Release --output-on-failure` at the tested commit: **20/24 passed,
4 failed**. The four failures are the known frozen-reference-decoder
nondeterminism (#120/#160) plus the resource-bound 8K configuration above:

| test | observed failure |
| --- | --- |
| `test_phase2a` | host/device compact streams decoded identically, then decoded PSNR 17.4344 dB against the 35 dB gate |
| `test_phase2b` | 1080p decoded PSNR below 35 dB after all entropy/token checks passed |
| `test_phase2c` | 1080p 1-4 slots and 4K HostInput passed in this run; 8K 3-slot/4-stream returned `QueueGpuEntropyFrame failed` |
| `test_phase2d` | 1080p `Frame1Initial(FSratelimiter=4)` upstream comparison failed |

In `test_phase2c` the 1080p/4K/8K identity gates passed with matching CPU/GPU
SHA-256 in the same process. Across earlier repeats the 1080p slot=4 PSNR gate
also failed (`20.332374`, `12.446088`), which is the same reference-decoder
behavior. These are not encoder-byte, entropy-math or SM 5.2 wavelet failures.

The complete `test_phase2c` executable therefore cannot pass on a 3.8 GiB M4 for
two independent reasons: nondeterministic reference-decoder quality gates and
the unconditional 8K 3-slot/4-stream pipeline. The bounded probe exercises the
4K/8K identity and pipeline configurations directly.

## 8K device-class policy (SM 5.2, 4 GiB class)

On the measured 4 GiB-class device (3,953,131,520 B), the Phase 2C 8K default
configuration of **3 slots x 4 streams** is **`UNSUPPORTED_RESOURCE_BOUND`**.
The configuration fails closed with `QueueGpuEntropyFrame failed` before the
first encoded frame at ~98% measured device occupancy. This classification:

- is **not** a PASS and **not** a silent skip; the strict failure stays visible
  in CTest and in the probe,
- does not change the default slot or stream count and does not weaken any
  bitstream or decode gate,
- does not attribute the failure to SM 5.2 wavelet or entropy arithmetic,
- applies only to this device class. Qualified 8K configurations on it are
  **1-2 slots x 4 streams** or **3 slots x 2 streams**, each with the 8K
  identity gates passing.

A capacity preflight that emits `UNSUPPORTED_RESOURCE_BOUND` with measured
free/total/required bytes and a public API status is the recommended follow-up;
it is not implemented by this documentation child. `test_phase2c` remains
non-passing on this device class because its 8K 3-slot/4-stream pipeline is
unconditional and its reference-decoder PSNR gates are nondeterministic.

## Conclusions

1. **SM 5.2 4K:** the Phase 2C identity gates are qualified and byte-exact at
   `main`. The tested pipeline configurations passed all encoder/bitstream
   gates; their included reference-decoder PSNR gate passed in the recorded runs
   but is nondeterministic and is not a qualified pixel oracle.
2. **SM 5.2 8K identity:** qualified; CPU/GPU samples are byte-identical at
   both 4K and 8K.
3. **SM 5.2 8K pipeline:** works within the device budget (1-2 slots at four
   streams, or 3 slots at two streams); the default 3-slot/4-stream
   configuration is `UNSUPPORTED_RESOURCE_BOUND` on a 3.8 GiB device under the
   policy above.
4. **Release gate:** unchanged. This does not resolve the reference-decoder
   nondeterminism and does not authorize a full 8K product claim.

## Limits

- Not a full CTest pass and not a sanitizer run.
- No exact internal CUDA status for the failing 3-slot/4-stream configuration.
- No 8K performance claim beyond the measured configurations.
- Pipeline `PASS` includes a nondeterministic reference-decoder PSNR gate; only
  encoder/bitstream identity is qualified, never the decoded pixels.
- The documented clean build requires CUDA 12.8.x and the `INCLUDE` workaround
  above; CUDA 13.x cannot build `sm_52`.
- The original GoPro decoder remains nondeterministic under the conditions in
  #120/#160; decoded-pixel equality here is bounded evidence, not a global
  determinism guarantee.
- No protected-main merge, release or tag was produced by this work.
