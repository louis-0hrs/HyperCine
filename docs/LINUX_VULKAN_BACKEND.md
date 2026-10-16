# Linux Vulkan Compute Backend (P1 slice)

This document covers the first **experimental Vulkan compute** slice for
HyperCine, added for issue #31. It is deliberately scoped to the smallest
provable unit: **headless device discovery**, the frozen **BGRA8 -> CG709
Y/Cr/Cb p10** color conversion, and the **L1 2-6 forward wavelet** (horizontal
and vertical). It is **not** a complete CineForm GPU encoder, and no CFHD
compatibility claim is derived from it. Everything beyond L1 (quantization,
entropy, framing, L2/L3) is P2 and out of scope here.

The backend mirrors the already-integrated SDK-free CPU path
(`src/cpu/filmscan3_frontend.cpp`, `src/cpu/wavelet_cpu.cpp`): the same integer
arithmetic is executed in GLSL compute shaders, and an independent harness
compares every sample against the CPU reference bit for bit - zero tolerance.

## Files

| Path | Role |
|------|------|
| `src/vulkan/shaders/color_cg709.comp` | BGRA8 -> CG709 Y/Cr/Cb p10 compute shader |
| `src/vulkan/shaders/wavelet_h26.comp` | 1D horizontal 2-6 forward wavelet |
| `src/vulkan/shaders/wavelet_v26.comp` | 1D vertical 2-6 forward wavelet |
| `src/vulkan/vulkan_frontend.cpp` | headless Vulkan backend (loader via `dlopen`) |
| `include/hypercine/vulkan_frontend.h` | portable public interface (no Vulkan headers) |
| `tools/vulkan_spirv_embed.cpp` | deterministic GLSL -> SPIR-V embedder |
| `tests/test_vulkan_filmscan3.cpp` | CPU <-> Vulkan parity and benchmark harness |

## Build

`HYPERCINE_ENABLE_VULKAN` defaults to **OFF**. When OFF there is no new
dependency, no new target, and the Mac Metal and Windows/Linux CPU paths are
unchanged. When ON the frontend requires the SDK-free standalone build
(`HYPERCINE_BUILD_STANDALONE=ON`, `HYPERCINE_BUILD_UPSTREAM=OFF`,
`HYPERCINE_ENABLE_CUDA=OFF`).

```sh
cmake -S . -B build-vulkan \
      -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
      -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_VULKAN=ON \
      -DHYPERCINE_BUILD_TESTS=ON
cmake --build build-vulkan --target test_vulkan_filmscan3 -j
```

### Shader compilation

SPIR-V is produced at build time by `tools/vulkan_spirv_embed`, which loads the
shaderc **runtime** library (`libshaderc.so.1`) through `dlopen`. This matters on
hosts that ship the shaderc runtime without the `glslang-tools` / `spirv-tools`
command-line packages and without root: the command-line frontends are then not
present, but the C API is. `find_package(Vulkan)` therefore reports
`missing components: glslc glslangValidator` on such a host - that is expected
and does **not** block the build. The Vulkan loader is likewise loaded at
runtime; `VK_NO_PROTOTYPES` removes every link-time Vulkan symbol.

The generated header embeds, per shader, the SPIR-V word array plus its length
and an FNV-1a-64 fingerprint, and the build writes
`generated/vulkan_spirv_manifest.txt`. The manifest is part of the validation
record so a reviewer can detect shader drift.

## Verified environment

Values recorded from the host that produced the results below; no local paths,
share names or hostnames are recorded here.

| Item | Value |
|------|-------|
| GPU | NVIDIA A2, compute capability 8.6, driver 550.163.01 |
| Vulkan instance | 1.4.309 (loader `libvulkan1` 1.4.309.0-1) |
| Physical device API | 1.3.277 (NVIDIA ICD) |
| shaderc runtime | `libshaderc1` 2025.2-1 |
| compiler | GCC 14.2.0 |
| cmake | 3.31.6 |
| libc | glibc 2.41 |

Discovery is honest about absence: if the loader or a compute-capable queue is
missing, `DiscoverDevice()` reports `available=false` with a reason, and the GPU
test fails closed (exit 1) rather than silently using the CPU.

## Bit-exactness contract

The shaders reproduce the CPU reference exactly, including every place where the
CPU code is deliberately non-obvious:

- **Logical vertical inversion**: output row `y` reads source row `height-1-y`.
- **Channel order** Y/Cr/Cb; **Cr and Cb come from the even-x sample only** -
  there is no pair averaging in the FilmScan3 profile.
- **Signed floor division** everywhere the CPU spells it out (`floor_div`,
  `floor_div8`); a signed right shift would be implementation-defined.
- **Saturating int16 and uint16 wrapping.** In `ReferenceChannel` the CPU narrows
  through `uint16_t` *before* the `*4` multiply, so the product wraps modulo
  65536. The shader must mask to 16 bits (`& 0xFFFF`) at the same point; the
  natural 32-bit `uint` would wrap at the wrong modulus. This was the single
  bug found during bring-up and is called out here so it is not reintroduced.
- **No shaderInt16 / 16-bit storage required.** Plane values are carried in
  `int32` and narrowed explicitly, which is provably equivalent for this
  arithmetic and avoids feature-gating on `shaderInt16`.

The wavelet shaders reproduce the lowpass pair sum (saturated), and the exact
left-border, interior and right-border 2-6 highpass taps with `FloorDivide8(sum
+ 4)` rounding.

## Test harness and results

`tests/test_vulkan_filmscan3.cpp` runs the same inputs through the CPU reference
and the GPU, and compares every Y/Cr/Cb sample and every unquantized L1
LL/LH/HL/HH value. On a mismatch it prints
`frame,plane,level,band,x,y,expected,actual` for the first difference and exits
non-zero. Patterns covered: flat, gradient, canonical, impulse, checker/edges,
seeded noise, and extreme signed int16. Both unpadded and padded strides are
exercised (the FilmScan3 BGRA stride must stay 16-byte aligned, matching the CPU
validator). Geometries include 128, 256 and 1920x1088 (the 1088 height carries
the odd L3 geometry already present in the 1080p reference).

`--gpu-required` makes a missing device a hard failure; without it a missing
device is a CTest skip (`SKIP_RETURN_CODE 77`).

Result on the recorded host:

```
color PASS .............. 36 / 36 cases
wavelet_h PASS .......... 42 / 42 cases
wavelet_v PASS .......... 42 / 42 cases
forward_level PASS ...... 12 / 12 cases (LL/LH/HL/HH)
RESULT: PASS (all samples bit-exact)
```

## Benchmark (informational only)

Measured with 1 warmup + 20 alternating CPU/GPU measurements at 1920x1088, same
host, same build. `gpu_kernel_ms` is the timestamped compute window only; it
excludes the host->device upload and device->host download, which are included in
`gpu_wall_ms`. **Vulkan compute P1 is not claimed to be faster end to end** -
buffers are host-visible for P1 (correctness first), so the transfer dominates
the wall time.

```
bench color 1920x1088 reps=21
  cpu_ms        median=373.720 mean=379.955 p95=399.975
  gpu_wall_ms   median=887.214 mean=889.113 p95=902.858   (upload+download+wait)
  gpu_kernel_ms median= 29.067 mean= 29.035 p95= 29.796   (timestamp window only)

bench wavelet_h26 1920x1088 reps=21
  cpu_ms        median= 33.419 mean= 34.243 p95= 38.105
  gpu_wall_ms   median=455.723 mean=460.204 p95=466.610
  gpu_kernel_ms median= 10.515 mean= 10.526 p95= 10.688
```

The kernel-only column shows the compute itself is fast; the wall column shows
the P1 transfer contract, which P2 (resident buffers) is intended to change.
These numbers are **not** comparable to the Mac Metal P1 figures - different
hardware, driver and buffer strategy.

## Shader build fingerprint

```
shader color_cg709  words 1968  fnv1a64 b382a2254df10ccb
shader wavelet_h26  words 2024  fnv1a64 2c3e3eae4d8e5abc
shader wavelet_v26  words 2022  fnv1a64 40fc48953eed6386
```

(Fingerprints depend on the shaderc version; the recorded values correspond to
`libshaderc1` 2025.2-1.)

## Sanitizer status

Host-side sanitizers: a debug build with `-fsanitize=address` was used during
bring-up and found no host memory error after the two defects below were fixed.
**GPU-side tooling was not run**: the Vulkan validation layers
(`VK_LAYER_KHRONOS_validation`) are not installed on the recorded host, and no
compute-sanitizer equivalent was applied to the shaders. This is recorded
honestly: the GPU path is *not* claimed to be sanitizer-clean, only
CPU-parity-clean. Enabling validation layers is a follow-up.

## Deliberate exclusions (not implemented here)

- L2/L3 wavelet levels, quantization, entropy coding, CFHD framing/writer.
- Golden / reference-corpus comparison. The P1 slice is validated against the
  in-repo CPU reference; byte-level CFHD claims require the P2 work and the
  reference corpus, and are explicitly out of scope.
- Performance claims. Only raw alternating measurements are recorded.
- Six quality profiles and any 4K/8K SDK-free codec promotion.

## Bring-up defects (for the record)

Two real defects were found and fixed while standing this up; both are useful
for anyone re-deriving a backend from the CPU reference:

1. **Invalid command recording**: `vkCmdResetQueryPool` was called *before*
   `vkBeginCommandBuffer`, which corrupted the driver heap. Reset is a
   recording-time command and must sit between begin and end.
2. **Wrong wrap modulus**: the color shader used 32-bit `uint` wrapping where the
   CPU reference wraps at 16 bits (see above).

Per-dispatch descriptor sets and command buffers are freed so the pools are
reused across the long parity run (otherwise the pool exhausts after 16
dispatches).
