# Contributing to HyperCine

HyperCine is an experimental CineForm (`CFHD` / SMPTE VC-5) engineering project. Contributions are welcome, but each platform and format has its **own verification scope**. Start with the [documentation index](docs/README.md), [architecture](docs/ARCHITECTURE.md), [compatibility matrix](docs/COMPATIBILITY.md), and [open work queue](https://github.com/louis-0hrs/HyperCine/issues/62).

> [!IMPORTANT]
> The GoPro SDK under [`upstream/`](upstream/) is a **frozen reference**. Do not patch, refactor, reformat or update any file there. Handle integration quirks outside that directory; document reference behavior in [`docs/UPSTREAM.md`](docs/UPSTREAM.md).

## Before you start

Read [`AGENTS.md`](AGENTS.md) for mandatory repo-wide rules, including agent claims, independent worktrees, branch ownership, pre-publication privacy gates and handoffs. Use a focused issue and an isolated branch; coordinate against [issue #62](https://github.com/louis-0hrs/HyperCine/issues/62) before editing files an agent already owns. Do not autonomously merge or release.

New HyperCine source files require the repository's dual **MIT OR Apache-2.0** copyright/license header. Keep core codec dependencies within the project's published C++17/platform-native SDK constraints.

## Show correctness before speed

Changes to format conversion, wavelet math, rate control, bitstream assembly or entropy paths require deterministic test vectors. Where valid, compare **actual compressed bytes and state** with the frozen upstream encoder. Record precisely which metadata is normalized. Pixel quality and round-trip decoding supplement, but do not replace, bitstream equality.

**The upstream GoPro decoder can produce different pixels for identical packets under some worker configurations.** Its successful return code is not sufficient evidence of correct pixels. Keep failures classified as encoder discrepancy, decoder error, or decoder reproducibility; do not weaken release gates or silently skip failing tests. See [#120](https://github.com/louis-0hrs/HyperCine/issues/120) and [`docs/UPSTREAM.md`](docs/UPSTREAM.md).

Run all **applicable** build and CTest gates for the touched implementation. Identify platform and test command; do not call an unrun/unsupported build a pass. For cross-platform changes, report verified platforms individually and explicitly record outstanding native verification. [CPU reference CI](.github/workflows/ci-cpu.yml) and [SDK-free standalone CI](.github/workflows/ci-standalone.yml) are different contracts.

## Publish measurements responsibly

The historical Phase 3A.2c CUDA headline uses `ProductionEndToEnd_v2`, five-trial median FPS, an explicitly identified **single-threaded** upstream CPU baseline, aligned warmup/state equivalence, and a clean worktree. `HostPageableEndToEnd` is the ordinary host-to-host benchmark. `HostPinnedApi` changes the memory contract. `DeviceInput` begins in VRAM and **must never be described as file-transcoding throughput**.

Report source commit, benchmark JSON, platform, input residency, CUDA architecture, resolution, slot policy, clocks and representative latency. Label exploratory or modified-tree runs as noncanonical. See the [canonical JSON](benchmarks/phase3a2c_release_candidate.json) and [benchmark guide](benchmarks/results/README.md).

## Privacy and pull-request checklist

Public branches, draft PRs, issue comments, logs and artifacts are **public before merge**. Do not upload credentials, personal machine/user paths, private network or NAS locations, internal DNS, or raw environment logs.

Before every commit, review the staged diff and run `python ci/privacy_guard.py --staged`. Before pushing, use the configured pre-push guard to scan **every outgoing commit** and review any generated evidence. A hosted CI check is not a pre-publication substitute. Follow [`docs/PRIVACY.md`](docs/PRIVACY.md) for hook/worktree setup and incident handling; never bypass a failing guard just to publish.

Open a **Draft PR** linking its issue. Include the actual test commands, OS/compiler/GPU and driver versions where relevant, sample/commit hashes without private paths, PASS/FAIL/SKIP/BLOCKED classifications, scope boundaries, and unresolved risks. The [pull-request template](.github/pull_request_template.md) provides the final checklist. Leave protected-main integration to the maintainer's explicit review process.
