# HyperCine compatibility and verification

HyperCine has **multiple independently qualified implementations**. A passing test for one does not certify every codec profile, operating system, GPU, decoder, or editing application. This page describes the checked-in `main` source and preserves the Phase 3A.2c CUDA benchmark scope rather than combining results from unrelated development branches.

> [!IMPORTANT]
> **Bitstream equivalence is not decoder correctness.** Phase 3A.2c demonstrates upstream-encoder equivalence for specified inputs and states. The frozen GoPro decoder has separately reproduced nondeterministic output on identical packets; its release-safety investigation is still open ([#120](https://github.com/louis-0hrs/HyperCine/issues/120), [#159](https://github.com/louis-0hrs/HyperCine/issues/159), [#160](https://github.com/louis-0hrs/HyperCine/issues/160)). Neither successful API return nor a shared CFHD FourCC establishes application-wide interoperability.

## Status at a glance

| Path | Implemented scope | Evidence / boundary |
| :--- | :--- | :--- |
| **CUDA Phase 3A.2c encoder** | Windows x64 + NVIDIA CUDA; progressive BGRA8 → CFHD YUV 4:2:2, FilmScan 3, GOP 1 | Historical canonical upstream **encode-byte** equivalence at 1080p, 4K UHD and 8K UHD on an RTX 4090 Laptop. [Benchmark artifact](../benchmarks/phase3a2c_release_candidate.json). |
| **GoPro SDK CPU reference** | Frozen upstream encoder and decoder built in the reference configuration | Reference implementation, **not** a HyperCine-accelerated decoder. Multithreaded decode reproducibility remains a known investigation. |
| **SDK-free standalone CPU encoder** | Separate C++17 fixed FS3-Q2 path; separately selectable stateful FilmScan 3 profile | Profile-specific CFHD packet and decoder validation. FilmScan 3 corpus qualification is scoped to 1920×1080 BGRA8 / YUV422 p10 / GOP 1. [Standalone](MACOS_STANDALONE_ENCODER.md) · [FilmScan 3](MACOS_FILMSCAN3_CPU.md). |
| **Experimental Apple Metal frontend** | Optional Metal color/wavelet frontend for the **SDK-free FilmScan 3** encoder, not the Windows CUDA pipeline | Native macOS stage/packet tests exist. This is not general Metal codec, decode, 4K/8K, or NLE qualification. [Metal evidence](MACOS_METAL_BACKEND.md). |
| **Portable decode task graph** | Synthetic scheduler and dependency tests | **No production CFHD decoding** from this module; entropy, inverse wavelet and output implementation require separate integration. [Task-graph record](DECODE_TASK_GRAPH_147.md). |
| **Vulkan / complete GPU decoder** | Experimental design or independent work | Not an integrated release-candidate capability. Open branches and proposals are not equivalent to merged and qualified features. |

## What the CUDA encoder actually supports

The **release-candidate accelerated contract** is limited to progressive `CFHD_PIXEL_FORMAT_BGRA` source frames, `CFHD_ENCODED_FORMAT_YUV_422`, `CFHD_ENCODING_QUALITY_FILMSCAN3`, and `GOP = 1`.

| Input / encoded family | Frozen CPU SDK | Phase 3A.2c CUDA acceleration |
| :--- | :--- | :--- |
| BGRA8 → YUV 4:2:2, FilmScan 3 | Reference supported | **Implemented; canonical byte-equivalence measurements** at 1920×1080, 3840×2160, 7680×4320 |
| YUY2 or 2vuy → YUV 4:2:2 | Existing SDK paths | No corresponding release-candidate CUDA qualification |
| RGB / RGBA → RGB 4:4:4 or RGBA 4:4:4:4 | Existing SDK paths | No corresponding release-candidate CUDA qualification |
| RGB48 / B64A high-bit-depth inputs | Existing SDK paths | No corresponding release-candidate CUDA qualification |
| BYR2 / BYR4 Bayer formats | Existing SDK paths | No corresponding release-candidate CUDA qualification |
| FilmScan 1/2 and Low/Medium/High quality settings | SDK supports additional settings | Not qualified under the Phase 3A.2c CUDA benchmark gate |
| Interlaced and multi-frame GOP profiles | Depends on SDK format/profile | Not implemented in the Phase 3A.2c CUDA path |

These are **CUDA release-candidate** assertions, not an exhaustive inventory of prototype work. For independently measured additional families, inspect the corresponding issue, branch, tests, and platform evidence before upgrading a status.

**Precision terminology:** BGRA8 is the *source pixel layout*. The GoPro encoder prepared by `src/core/phase3a_encoder.cpp` uses **10-bit codec precision**; the CUDA pipeline can hold intermediate values in wider integer buffers. Do not interpret an internal intermediate width as proof of 12-bit *encoded* YUV output. The SDK-free FilmScan 3 evidence expressly uses `YUV422 p10`. See [Architecture](ARCHITECTURE.md) for stage boundaries.

## How compatibility is measured

**CUDA ↔ frozen GoPro encoder.** The Phase 3A.2c `ProductionEndToEnd_v2` gate aligns frame sequence and prior-bitcount/`FSratelimiter` state, verifies quantization and masked CFHD sample hashes, and uses **five-trial medians** for performance claims. Four volatile metadata fields (clip GUID, encode date/time and timecode) are masked in cross-session byte comparisons. Reference sample identity does not mean the external decoder is free of races. See [benchmark methodology](PHASE3A1_BENCHMARK_HARDENING.md) and [upstream limitations](UPSTREAM.md).

**SDK-free standalone ↔ real decoders.** The standalone CI matrix runs independent FFmpeg decode checks and validation tests using `HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON` when configured. Separate Windows GoPro SDK oracles and real-corpus reports cover **specific fixtures/profiles**. A pass here does not broaden the Windows CUDA release-candidate test matrix. See [CI contract](../ci/STANDALONE.md) and [standalone workflow](../.github/workflows/ci-standalone.yml).

**Third-party editors.** Adobe Premiere Pro, After Effects, DaVinci Resolve and Final Cut Pro import/playback, including AVI/MOV muxing and complete metadata behavior, are **not certified by the listed automated gates**. FFmpeg decoding of a sample is useful independent evidence, not a substitute for validating each host application.

## Reference-decoder reproducibility and release gate

The original upstream `CFHD_DecodeSample` can return `CFHD_ERROR_OKAY` and expected geometry while decoded pixels differ for identical input on some CPU/thread configurations. Issue [#120](https://github.com/louis-0hrs/HyperCine/issues/120) tracks test classification and safe verification; [#159](https://github.com/louis-0hrs/HyperCine/issues/159) tracks Linux behavior, and [#160](https://github.com/louis-0hrs/HyperCine/issues/160) tracks causal investigation. A one-worker decoder configuration is **a diagnostic constraint, not a general repair or release certification**. The actual root cause is not declared solved here.

## What to read next

[Architecture and component ownership](ARCHITECTURE.md) · [SDK-free FilmScan 3 limits](MACOS_FILMSCAN3_CPU.md) · [Experimental Metal](MACOS_METAL_BACKEND.md) · [Frozen upstream](UPSTREAM.md) · [Current CUDA benchmark artifact](../benchmarks/phase3a2c_release_candidate.json) · [Contribution and test expectations](../CONTRIBUTING.md)
