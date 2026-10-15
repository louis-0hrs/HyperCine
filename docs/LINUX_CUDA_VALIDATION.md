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

    compute-sanitizer --tool memcheck ./tests/test_cuda_pyramid   # 0 errors
    compute-sanitizer --tool memcheck ./tests/test_cuda_wavelet   # 0 errors

Both runs reported `ERROR SUMMARY: 0 errors` while still passing the full
bit-exactness check set (1680 / 1680). No error was masked or suppressed.

## 7. Open items

* **Windows GoPro-SDK/CUDA oracle comparison — blocked.** The archived Windows
  golden corpus referenced by the issue (`//TRUENAS/DeDub` ->
  `HyperCine/golden/windows/qualities/2026-10-15/`, 20 part-corpora, 1760 files,
  ~5.97 GB) is not present on this host. Only the small in-repo fixtures under
  `tests/fixtures/windows-oracle/` are available, which is not the 6-profile
  corpus. -> `BLOCKED: SDK_ORACLE_UNAVAILABLE` for any full-file
  "Linux CUDA == Windows CUDA" CFHD bitstream claim. No such claim is made here.
  Section 4 establishes CPU-versus-CUDA equality *within Linux*, which is a
  necessary but not sufficient condition.
* The 120-frame stateful `FSratelimiter` sequence and the six Windows quality
  profiles were not run.
* Vulkan cross-check (#31) not performed — it must be coordinated and run
  sequentially on the same A2; CUDA `nvidia-smi` output alone says nothing about
  Vulkan.
* `racecheck`/`initcheck` and an ASan/UBSan host-only build were not run.
* Bit-exactness of the stage gates was checked via `test_cuda_wavelet` (up to
  1920x1080) and the phase tests; 4K/8K appear in section 5 as performance
  measurements only, not as bit-exactness gates.

## 8. Environment notes for reproducibility

* The toolkit comes from the distribution, so `nvcc` is `/usr/bin/nvcc` and there
  is no `/usr/local/cuda`; `CUDA_PATH` is unset and not needed.
* `ldconfig` is not on `PATH` for this shell profile; library resolution was
  confirmed through the linker instead.
* `build-linux-cuda` is a regular configure; nothing in the build required root
  privileges.
