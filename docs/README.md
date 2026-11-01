# HyperCine documentation

This directory contains **current capability references**, **platform-specific validation reports**, and **historical engineering records**. Those categories are deliberately distinct: an old benchmark or a draft backend design is not evidence of a production-ready feature.

[← Repository overview](../README.md) · [Current compatibility](COMPATIBILITY.md) · [Architecture](ARCHITECTURE.md) · [Contribution rules](../CONTRIBUTING.md)

## Start here

| Question | Reference |
| :--- | :--- |
| Which formats and backends are actually validated? | [Compatibility and verification](COMPATIBILITY.md) |
| Which codec stages run on the CPU, CUDA, or Metal? | [Architecture and component ownership](ARCHITECTURE.md) |
| How is the original GoPro implementation preserved? | [Frozen upstream SDK](UPSTREAM.md) |
| Which measurements should be quoted publicly? | [Benchmark artifact guide](../benchmarks/results/README.md) |
| What are the rules for contributions and autonomous agents? | [Contributing](../CONTRIBUTING.md) · [Agent instructions](../AGENTS.md) |
| How is repository-publication privacy protected? | [Privacy policy and hooks](PRIVACY.md) |

> [!WARNING]
> **Current decoder investigation:** successful `CFHD_DecodeSample` status does not always imply reproducible decoded pixels from identical packets. The decoder root cause and release gate are still under investigation in [#120](https://github.com/louis-0hrs/HyperCine/issues/120) and the [cross-platform decision board #161](https://github.com/louis-0hrs/HyperCine/issues/161). Do not treat SDK-encoder parity as a decoder-safety certification.

## CUDA encoder and performance

**Current reference comparison:** [Phase 3A.2c five-trial benchmark JSON](../benchmarks/phase3a2c_release_candidate.json). It describes a specified Windows/NVIDIA CUDA encode pipeline and a **single-threaded** original SDK CPU baseline, not generic multi-threaded CPU or GPU-resident file transcoding.

- [Sequence correctness](PHASE3A_SEQUENCE_CORRECTNESS.md) — stateful quantizer, entropy and frame-order verification.
- [Entropy coding and bounds](ENTROPY.md) — fused VLC coding, capacity and CUDA error-state contracts.
- [CUDA quality profiles](CUDA_QUALITY_PROFILES.md) — separate profile qualification and its source-pinned evidence; check tested parameters before broadening a claim.
- [Tesla M4 / SM 5.2 legacy support policy](TESLA_M4_SM52_SUPPORT_POLICY.md) — research-only gate separation, the measured 4 GiB capacity failure, three exact 8K points and the PASS/FAIL/SKIP matrix; not a release qualification.
- [Phase 3A benchmark hardening](PHASE3A1_BENCHMARK_HARDENING.md) — **historical** sequence-alignment/provenance repairs and comparisons before Phase 3A.2c.
- [Phase 3B optimization opportunities](PHASE3B_PERFORMANCE_OPPORTUNITIES.md) — ideas from a historical profiling pass; not new implementation or current headline measurements.

## SDK-free CPU, Metal and portable core

These reports often record **specific native machines, fixtures and feature branches**, not universal release gates.

- [Standalone CPU encoder](MACOS_STANDALONE_ENCODER.md) — independent CFHD packet writing and validation.
- [Stateful standalone FilmScan 3](MACOS_FILMSCAN3_CPU.md) — specific 1080p BGRA8 / YUV422 p10 / GOP 1 evidence.
- [Experimental Metal FilmScan 3 frontend](MACOS_METAL_BACKEND.md) — measured color and wavelet frontend, with CPU-hosted subsequent stages.
- [Metal P2](MACOS_METAL_P2.md) and [Metal entropy P3](MACOS_ENTROPY_P3.md) — scoped research and profiling records.
- [SDK-free quality-state model](QUALITY_STATE_SDKFREE.md) — standalone rate/quant-state calculation, not a complete encoder.
- [Mac/Windows bitstream comparison](MACOS_WINDOWS_BITEXACT.md) and [Windows standalone reference decode](WINDOWS_STANDALONE_ORACLE.md) — pinned cross-platform checks.
- [CPU instruction-set capability](CPU_ISA_P0.md) — dispatch/probe boundary; does not by itself establish vectorized codec-wide performance.
- [Encoder capability contract](ENCODER_CAPABILITY_CONTRACT.md) — issue #259 session config and fail-closed query. Experimental CPU attempts are not a reference pass.
- [Internal session quality profiles](ENCODER_PROFILE_SESSIONS.md) — per-backend profile admission matrix inside the #259 contract; six profiles on CPU scalar, FilmScan3 only on Metal/CUDA.

## Decoder, correctness and limits

- [Synthetic decode task graph](DECODE_TASK_GRAPH_147.md) and [wavelet dependency design](WAVELET_DEPENDENCY_SCHEDULING_144.md) — scheduler contracts and tests, **not** a shipping arbitrary-CFHD decoder.
- [Small-chroma reference limitation](SMALL_CHROMA_ORACLE.md) — bounded upstream boundary-read finding and its effect on what constitutes a valid oracle.
- [Frozen upstream implementation and caveats](UPSTREAM.md) — preserved code, verified observations, and unresolved hypotheses.

## Historical research archive

These documents preserve the engineering decisions, experiments and performance reports **from the named development phase**. Some contain old targets, provisional benchmarks, or superseded interpretations.

- [Original Phase 0 reference baseline](REFERENCE_BASELINE.md) and [original GPU acceleration plan](GPU_PLAN.md).
- [CUDA Phase 1A](CUDA_PHASE1.md), [1B](CUDA_PHASE1B.md), [1C](CUDA_PHASE1C.md).
- [CUDA Phase 2A](CUDA_PHASE2A.md), [2B](CUDA_PHASE2B.md), [2C](CUDA_PHASE2C.md), [2D](CUDA_PHASE2D.md).
- [Independent adversarial audit (September 2026)](INDEPENDENT_AUDIT_2026-09-26.md).

**Reading a historical document?** Use its original source commit, benchmark contract, test machine and fixture limitations rather than copying its FPS figures into current marketing or release notes.

## Additional records and evidence

Detailed native run artifacts live under [`docs/validation/`](validation/). They are **individual frozen reports** and may use older source revisions than `main`. Machine-readable JSON, actual successful tests, independent decode results and unresolved gates must be interpreted together.

The [agent dispatch queue #62](https://github.com/louis-0hrs/HyperCine/issues/62) is an operational coordination board. A task marked complete or a PR marked ready for review does **not** mean that its implementation is merged, released or qualified on every platform.
