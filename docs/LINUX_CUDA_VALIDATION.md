# Linux CUDA Validation — NVIDIA A2 (SM 8.6)

Tracking issue: [#33 [Linux/CUDA P1] Windows-CUDA-Encoder auf NVIDIA A2 unter Linux qualifizieren und bitgenau prüfen](https://github.com/louis-0hrs/HyperCine/issues/33)

Status: **partial** — Linux CUDA builds and runs on the real A2 with bit-exact
transform/phase checks and clean sanitizer output. The cross-platform
bit-exactness claim against the Windows GoPro-SDK/CUDA oracle is **not**
established on this host (corpus not present), see "Open items".

All numbers below were produced on the host described in section 1, in one
session on 2026-10-15. No driver, kernel, CUDA or TrueNAS component was
installed or modified: the existing system toolkit was used as is.

## 1. Host inventory (P0)

| Item | Value |
| --- | --- |
| Host / kernel | `Openclaw`, Linux 6.12.100+deb13-amd64 (Debian 13.6 trixie), x86_64 |
| GPU | NVIDIA A2, UUID `GPU-2c023501-e70d-293f-0d41-98842bc468f1`, PCI `00000000:00:07.0` |
| Compute capability | 8.6 (`-DCMAKE_CUDA_ARCHITECTURES=86`) |
| Driver | 550.163.01 |
| GPU memory | 15356 MiB total, 14 MiB used when idle, no compute processes attached |
| PCIe link (current) | generation **1**, width **x8** — virtualised guest, the link never trains higher |
| CUDA toolkit | `nvcc` 12.4.131 at `/usr/bin/nvcc` (Debian-packaged toolkit, no `/usr/local/cuda*`) |
| CMake / Ninja | 3.31.6 / 1.13.0 |
| C/C++ compiler | GCC 14.2.0 (Debian 14.2.0-19) |
| CUDA host compiler | GNU 13.3.0 (selected by nvcc) |
| Display | none required (headless) |

The low PCIe generation is a property of this virtualised host, not of the A2.
It dominates the end-to-end figures in section 5 and is the likely cause of the
~91 % transfer overhead reported there.

## 2. Configure and build

    cmake -S . -B build-linux-cuda -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DHYPERCINE_BUILD_STANDALONE=OFF \
      -DHYPERCINE_BUILD_UPSTREAM=ON \
      -DHYPERCINE_ENABLE_CUDA=ON \
      -DCMAKE_CUDA_ARCHITECTURES=86 \
      -DHYPERCINE_BUILD_TESTS=ON \
      -DHYPERCINE_BUILD_BENCHMARKS=ON
    cmake --build build-linux-cuda -j 4

Configure and build complete on Linux x64. The upstream GoPro SDK builds from
the pristine sources with this toolchain and `hypercine_wavelet` is compiled by
nvcc for `sm_86`, so device code for compute capability 8.6 is really produced
and executed, not replaced by a CPU stand-in.

## 3. Linux portability changes

`upstream/` is untouched. Everything above is done around it:

1. **`cmake/linux_size_t_prefix.h` + root `CMakeLists.txt`** —
   `upstream/Common/CFHDAllocator.h` declares `size_t`-based function pointer
   typedefs without including `<stddef.h>`. MSVC and older libstdc++ supplied it
   transitively; GCC 14 does not. A forced `-include` (C++ translation units only,
   joined argument form because a generator expression cannot expand to two
   arguments and nvcc rejects the joined form) supplies the definition to the
   HyperCine targets. Targets created before the shim — the pristine upstream
   build — keep their original flags.
2. **Root `CMakeLists.txt`** — `CFHDCodecStatic` is a static archive with an
   empty link interface, while `upstream/EncoderSDK/SampleEncoder.cpp` needs
   `uuid_generate`. The upstream tools append `-luuid` themselves; our targets
   link the archive directly, so `libuuid` is attached as an INTERFACE
   dependency (found via `find_library`, hard error if missing).
3. **`include/hypercine/platform_compat.h` + six call sites** — the measurement
   tooling in `tests/` and `benchmarks/` was MSVC-only: `_popen`/`_pclose`,
   `GetSystemInfo`, `_MSC_VER`. The shim keeps the Windows output byte-identical
   (`"MSVC <_MSC_VER>"`) and adds portable equivalents. `tests/test_phase2b.cpp`
   additionally needed `<cstring>` for `std::memcpy`.
4. **`src/CMakeLists.txt`** — `hypercine_wavelet` calls the upstream `CFHD_*`
   entry points but did not declare the dependency, so the link line placed
   `CFHDCodecStatic` before it. GNU ld resolves static archives in a single
   left-to-right pass (MSVC re-scans), which produced undefined
   `CFHD_OpenDecoder`/`CFHD_DecodeSample`/... The dependency is now declared
   `PUBLIC`, which fixes the order for every consumer.

## 4. Test results

    ctest --test-dir build-linux-cuda --output-on-failure -j 1
    # 100% tests passed, 0 tests failed out of 14
    # Total Test time (real) = 148.97 sec

| Test | Result |
| --- | --- |
| test_transform_wavelet | passed |
| test_codec_roundtrip | passed |
| test_yuy2_stability | passed |
| test_avi_sample | passed |
| test_cuda_wavelet | passed — **1680 / 1680 transform checks bit-exact** |
| test_cuda_pyramid | passed — quantization phase 1B bit-exact |
| test_entropy_parallel | passed |
| test_hybrid_roundtrip | passed |
| test_phase2a | passed |
| test_phase2b | passed |
| test_phase2c | passed (17.88 s) |
| test_phase2d | passed (24.49 s) |
| test_entropy_capacity | passed |
| test_phase3a | passed (98.19 s) |

GPU-required tests are not stubs: `test_cuda_wavelet` allocates device buffers
with `cudaMalloc`, dispatches the CUDA reference **and** optimized kernels and
compares them against the CPU reference via `cudaMemcpy` +
`cudaDeviceSynchronize`. Both backends matched the CPU reference bit-exactly for
all 13 patterns at 16x16 ... 1920x1080.

Independent confirmation that the device executed work: sampling `nvidia-smi`
every 200 ms during `test_cuda_wavelet` and `test_cuda_pyramid` showed GPU
utilisation rising to **71 %** with 90 MiB resident.

The 128x128 4-band upstream special case was not emulated or worked around; the
upstream library is linked as-is.

## 5. Benchmark (same A2, benchmarks/benchmark_wavelet)

Raw output: docs/validation/linux-cuda-wavelet-benchmark-2026-10-15.json

| Case | CPU reference | CUDA kernel only | CUDA end-to-end | Kernel speedup |
| --- | --- | --- | --- | --- |
| 1080p, batch 1 (50 iterations) | 9.19 ms (225.7 MP/s) | 0.923 ms (2247 MP/s) | 3.96 ms (524 MP/s) | 9.96x |
| 4K UHD, batch 1 (25 iterations) | 34.57 ms (239.9 MP/s) | 1.368 ms (6064 MP/s) | 11.85 ms (700 MP/s) | 25.3x |
| 8K UHD, batch 1 | 116.6 ms (265.3 MP/s) | 3.185 ms (10417 MP/s) | 44.04 ms (753 MP/s) | 36.6x |
| 1080p, batch 16 (20 iterations) | 107.60 ms (308.3 MP/s) | 3.179 ms (10436 MP/s) | 43.25 ms (767 MP/s) | 33.8x |

The harness runs its own warm-up plus the iteration counts above per case. The
end-to-end column includes host-to-device and device-to-host transfer; on this
guest (PCIe generation 1, x8) that dominates — the reported transfer overhead is
~76 % at 1080p and ~92 % at 8K/batch. A bare-metal or wider-link host would move
these figures; the kernel-only column is the transfer-independent part.

## 6. Compute Sanitizer

    compute-sanitizer --tool memcheck  ./tests/test_cuda_pyramid   # ERROR SUMMARY: 0 errors
    compute-sanitizer --tool memcheck  ./tests/test_cuda_wavelet   # ERROR SUMMARY: 0 errors
    compute-sanitizer --tool initcheck ./tests/test_cuda_pyramid   # ERROR SUMMARY: 0 errors
    compute-sanitizer --tool initcheck ./tests/test_cuda_wavelet   # ERROR SUMMARY: 57748 errors
    compute-sanitizer --tool racecheck ./tests/test_cuda_pyramid   # RACECHECK SUMMARY: 0 hazards
    compute-sanitizer --tool racecheck ./tests/test_cuda_wavelet   # RACECHECK SUMMARY: 0 hazards

`memcheck` and `racecheck` are clean on both binaries, and every run still passed
the full bit-exactness set (1680 / 1680). Nothing was masked or suppressed.

**`initcheck` on `test_cuda_wavelet` is not clean, and is reported as found.** All
15 errors printed with `--print-limit 15` are of one kind:

    Uninitialized access at 0x... on access by cudaMemcpy source

The reported addresses step by 32 bytes (one padded row of the output buffer). The
test copies `in_stride * H` elements back with `cudaMemcpy`, while the kernels only
produce the `width` valid columns of each row, so `initcheck` flags the *stride
padding* of the read-back. `CompareBuffers()` never looks at that padding: it
indexes `buf[y * stride + x]` for `x < width` only. This is therefore a harness
read-back artefact rather than a kernel reading uninitialized global memory, which
is also consistent with `test_cuda_pyramid` (no padded read-back) being clean. The
remaining 57633 errors were not printed and are therefore not individually
classified.

The bit-exactness result itself was unaffected, but the comparison was unnecessarily
coupled to allocator residue in the padding.

**Resolved by PR #40, verified on the A2.** PR #40 replaces the read-back with a
checked `cudaMemcpy2D` over the active columns only; the host vectors keep their
original padded pitch and the CPU-vs-CUDA index and comparison contracts are
unchanged. Verified at commit `b4161b4` on the same host (NVIDIA A2, sm 8.6, driver
550.163.01, CUDA 12.4.131, Compute Sanitizer 2024.1.1.0):

| Run | Before #40 | After #40 |
| --- | --- | --- |
| `initcheck test_cuda_wavelet` | 57748 errors | **0 errors** (exit 0) |
| `memcheck test_cuda_wavelet` | 0 errors | 0 errors (exit 0) |
| `racecheck test_cuda_wavelet` | 0 hazards | 0 hazards (exit 0) |
| `ctest -R 'test_cuda_wavelet\|test_cuda_pyramid'` | 1680/1680 bit-exact | 1680/1680 bit-exact |

`initcheck` was run with `--error-exitcode 99` and a log file, and the summary was
read from that log rather than from the process exit alone. The pre-#40 measurement
above is kept for history; it is not silently superseded.

## 7. Windows golden corpus: received, verified, and what it shows

The archived corpus was supplied through the NAS space as
`~/.openclaw/workspace/nas-openclawspace/HyperCine/golden/windows/qualities/2026-10-15/`
(`//192.168.249.38/Openclawspace`), read-only in this work. Pre-flight was done
before any comparison, as the issue requires:

* `TRANSFER_SHA256.tsv` itself hashes to
  `2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`, exactly the
  value pinned in the issue.
* All **1760 / 1760** listed files match the index in both byte length and
  SHA-256 (`ENTRIES=1760 OK=1760 PROBLEMS=0`). Nothing was written to the share.
* Layout: 20 part-corpora (6 qualities x 1080p-16f / 3840x2160-1f / 7680x4320-1f,
  plus sequence-120f for filmscan2 and filmscan3), 348 encoded frames total.

What the per-part manifests record:

* **All 348 frames have `sdk_normalized_sha256 == cuda_normalized_sha256`.** On
  Windows the GoPro SDK encoder and the Windows CUDA encoder produce identical
  *normalized* samples; the raw samples differ (`raw_equal=0` for all 348) only in
  the masked GUID/DATE/TIME/TIMECODE bytes. Normalized equality is therefore the
  meaningful target for any other platform.
* The Windows CUDA reference was produced on an **NVIDIA GeForce RTX 4090 Laptop
  GPU, compute capability 8.9** — not on an A2 (8.6). Any Linux CUDA comparison is
  cross-architecture as well as cross-platform, so a mismatch would have to be
  attributed before it is called a porting defect.

### Why the full-packet gate cannot be run here

The encoder targets that produce complete CFHD samples
(`hypercine_encode_filmscan3`, `hypercine_reference_probe`) are defined only
inside `if(HYPERCINE_BUILD_STANDALONE)` in the root `CMakeLists.txt`, and that
block hard-fails when CUDA or the upstream SDK is enabled:

    message(FATAL_ERROR "Standalone requires HYPERCINE_BUILD_UPSTREAM=OFF and HYPERCINE_ENABLE_CUDA=OFF")

They are consequently absent from `build-linux-cuda` — verified, not assumed. So
there is currently **no Linux build that has CUDA enabled and can emit a full CFHD
sample**; the CUDA configuration ships the kernel library and its tests, not an
encoder tool. Producing the full bitstream therefore needs either a
Linux-CUDA-capable encoder target or the SDK-free quality core that issue #29
owns. That is a scope decision for the maintainers — issue #33 explicitly forbids
writing a new CUDA engine here, and reusing the standalone encoder is not possible
because of the mutual exclusion above.

**Maintainer decision (2026-10-16):** the shared SDK-free encoder core from #29 /
PR #49 is the intended basis for future Linux CUDA integration. No separate
Linux-CUDA encoder engine is to be developed, and the existing CMake guards are not
to be bypassed. The missing complete CFHD output therefore remains an explicit,
separate integration gate; this branch does not close it and makes no Windows/Linux
bitstream parity claim.
Stage-level comparison against this corpus is also not possible as it stands: it
contains inputs and complete SDK/CUDA samples, but no Windows intermediate dumps
(converted planes, filter stages, HP coefficients), which the earlier macOS work
identified as required for exact HP coordinates.

## 8. Open items

* **Full-packet "Linux CUDA == Windows CUDA" claim — not reachable in this build
  configuration.** See section 7. The corpus is present and verified; the missing
  piece is a Linux encoder target that can produce a complete CFHD sample with
  CUDA enabled. No such claim is made here.
* The 120-frame stateful `FSratelimiter` sequence and the six Windows quality
  profiles were not run.
* Vulkan cross-check (#31) not performed — it must be coordinated and run
  sequentially on the same A2; CUDA `nvidia-smi` output alone says nothing about
  Vulkan.
* An ASan/UBSan host-only build was not run; `memcheck`, `initcheck` and
  `racecheck` results are in section 6 (including the one non-clean finding).
* Bit-exactness of the stage gates was checked via `test_cuda_wavelet` (up to
  1920x1080) and the phase tests; 4K/8K appear in section 5 as performance
  measurements only, not as bit-exactness gates.

## 9. Environment notes for reproducibility

* The toolkit comes from the distribution, so `nvcc` is `/usr/bin/nvcc` and there
  is no `/usr/local/cuda`; `CUDA_PATH` is unset and not needed.
* `ldconfig` is not on `PATH` for this shell profile; library resolution was
  confirmed through the linker instead.
* `build-linux-cuda` is a regular configure; nothing in the build required root
  privileges.
