# AGENTS.md — HyperCine AI Agent & Contributor Guidelines

This document specifies mandatory rules, design constraints, and operational protocols for any AI agent or software engineer contributing to the **HyperCine** repository.

Every agent operating in this repository must strictly adhere to these rules.

---

## 1. Upstream Immutability (Absolute Rule)
- The entire contents of the `upstream/` directory are **frozen reference code**.
- **NEVER** edit, refactor, reformat, modernize, or patch any file inside `upstream/`.
- Do not fix compiler warnings, change CRT calls, or modernize CMake inside `upstream/`. Suppress warnings or configure flags in root `CMakeLists.txt` or wrapper layers instead.
- If upstream behavior has a quirk or bug, document it in `docs/UPSTREAM.md` and handle or bypass it in the `hypercine/` wrapper layer.
- Upstream baseline commit: `11574d0295771edccadd17af14af74a539f924c7`.

---

## 2. Dual Licensing and Copyright Preservation
- GoPro's original code is dual-licensed under **MIT** or **Apache-2.0** (`Copyright (c) 2017 GoPro, Inc.`).
- Preserve all existing license files and copyright headers untouched in `upstream/`.
- All new files created for HyperCine must also be dual-licensed under MIT OR Apache-2.0.
- Standard file header for new HyperCine source files:
  ```cpp
  // Copyright (c) 2026 HyperCine Contributors.
  // Dual-licensed under the MIT License or Apache License, Version 2.0.
  // See LICENSE-MIT and LICENSE-APACHE for details.
  ```

---

## 3. Zero External Dependencies for Core Codec
- The core HyperCine codec libraries (`include/hypercine/`, `src/`) must have **zero external third-party dependencies**.
- Do not introduce Boost, Abseil, OpenCV, Conan, vcpkg, or GoogleTest for core functionality.
- Rely strictly on:
  - ISO C++17 standard library.
  - Platform-native GPU SDKs when enabled (NVIDIA CUDA Toolkit, Khronos Vulkan SDK, Apple Metal).
  - CMake 3.25+ and CTest for build and test harnesses.

---

## 4. Strict Bitstream Compatibility
- Any bitstream emitted by HyperCine must conform to the SMPTE VC-5 / CineForm `CFHD` standard.
- Any bitstream produced by HyperCine must decode correctly in:
  - The pristine upstream GoPro CineForm SDK (`upstream/Example/TestCFHD`).
  - Adobe Premiere Pro / After Effects.
  - DaVinci Resolve.
  - FFmpeg (`cfhd` decoder).
- Any valid bitstream produced by upstream tools must decode correctly in HyperCine.

---

## 5. Test-Driven Verification (TDD)
- Never merge or commit changes without running the full test suite via CTest:
  ```powershell
  ctest --test-dir build -C Release --output-on-failure
  ```
- Any new algorithm, color conversion kernel, or GPU acceleration path must have a corresponding test vector in `tests/`.
- Lossy compression tests must verify PSNR thresholds ($> 35\text{ dB}$ for FilmScan 3); lossless modes must verify mathematical bit-exactness.

---

## 6. Deterministic Execution
- All test patterns, benchmarks, and mathematical verifications must be deterministic.
- Use explicit seed values (such as `0x1337BEEF`) for test generators; do not use non-deterministic sources like `std::random_device` without a reproducible seed.
- Wavelet transforms and color conversions must produce consistent, reproducible output across runs.

---

## 7. SIMD & Memory Alignment Safety
- The CineForm codec relies on 16-byte (and 32-byte) aligned vector memory for SSE2, AVX2, and GPU transfers.
- Never pass unaligned standard heap allocations to codec buffers.
- Use `AlignedAllocator<T, 16>` / `AlignedVector<T>` (defined in `tests/test_vectors.h`) or platform-specific aligned allocators (`_aligned_malloc` / `posix_memalign`).
- Ensure line pitch / stride is padded to a multiple of 16 bytes.

---

## 8. Modular Multi-Backend Architecture
- Maintain strict separation of concerns across directories:
  - `include/hypercine/`: Public C and C++ API headers.
  - `src/core/`: Common frame handling, format conversion, and bitstream orchestration.
  - `src/cpu/`: CPU-based fallback and reference wrappers.
  - `src/cuda/`: NVIDIA CUDA acceleration backend (Phase 1).
  - `src/vulkan/`: Cross-vendor Vulkan compute backend (Phase 2).
  - `src/metal/`: Apple Metal compute backend (Phase 3).
  - `tools/`: Command-line utilities (`hypercine_transcode`, `hypercine_info`).
  - `tests/`: Lightweight, standalone CTest suites.
  - `benchmarks/`: Automated performance measurement harnesses.

---

## 9. Safe Memory & Resource Management
- In modern C++ code, use RAII for all resource lifecycles:
  - Wrap encoder/decoder handles (`CFHD_DecoderRef`, `CFHD_EncoderRef`, `CFHD_MetadataRef`) in smart pointers with custom deleters.
  - Wrap GPU streams, device buffers, and pinned host allocations in RAII wrappers.
- Never leak raw handles or invoke raw `free()` on memory allocated with `_aligned_malloc()`.

---

## 10. Performance Regression Protection
- Before claiming optimization improvements or completing acceleration milestones, run `benchmark_codec` and compare results against the recorded baseline in:
  ```
  benchmarks/results/reference_windows_msvc.json
  ```
- Record FPS, Megapixels/sec, latency (avg/min/max), and peak memory footprint.
- Do not commit changes that cause unexplained throughput drops or latency spikes.

---

## 11. Git & Commit Conventions
- Use **Conventional Commits** formatting:
  - `feat(...)`: New features, acceleration backends, or tools.
  - `fix(...)`: Bug fixes or compatibility corrections.
  - `perf(...)`: Proven performance optimizations.
  - `test(...)`: New test cases or test framework updates.
  - `docs(...)`: Documentation updates.
  - `build(...)`: Build system or CMake adjustments.
- Ensure `git status --porcelain upstream/` is always 100% empty before committing.


---

## 12. Public Repository: Privacy and Pre-Publication Gates (Mandatory)
- Treat **everything** in GitHub (branch commits, draft PRs, issues, comments, logs,
  CI artifacts and validation reports) as public immediately, even before merge.
- **Never publish** actual private IPs, internal DNS/hostnames, SMB/NFS/NAS
  locations, machine-specific absolute home directories, personal usernames,
  credentials, tokens, API keys, SSH configuration, or environment dumps.
- Use repository-relative paths in public commands and reports. Access external
  reference corpora through a locally configured variable such as
  `HYPERCINE_GOLDEN_DIR`; publish the *variable name*, not its value.
- Keep local host/mount paths and raw verbose tool logs outside the Git tree.
  For public validation evidence, provide portable reproduction instructions,
  test counts, test results, GPU/OS/compiler versions, checksums and timings.
  Sanitize generated JSON, XML, CMake diagnostics and logs before staging.
- Before **each commit**, run `python ci/privacy_guard.py --staged` and review
  `git diff --cached`. Before **each push**, run the privacy guard over
  **every outgoing commit**; do not rely on a final tree diff.
- Install and verify hooks explicitly in every participating worktree.
  With `extensions.worktreeConfig=true`, use
  `git config --worktree core.hooksPath .githooks` rather than changing the
  shared `--local` configuration. Hooks and the scanner must be present in
  the given worktree and an actual Python interpreter must work. See
  `docs/PRIVACY.md` for platform-specific details.
- A scanner pass does not replace human review. If the guard fails or Git/Python
  is unavailable, **stop** and report the blocker; never use `--no-verify`,
  disable a hook, or quietly suppress a hit just to publish.
- If potentially sensitive information was *already pushed*, STOP further
  publication of that content, notify the maintainer without reproducing the
  value, and propose a targeted remediation. Do not rewrite shared history,
  force-push active branches or rotate secrets without the maintainer's approval.
- Issue/PR instructions must contain no workstation-specific values. Existing
  public examples or old issues are **not** authorization to repeat such data.
- GitHub CI runs **after** data is pushed. Required status checks can block a
  merge, but only local pre-push controls can block a first public disclosure.
  See `docs/PRIVACY.md` for the limits and response process.

---

## 13. Issue-First Agent Coordination & Handoff (Mandatory)
- The shared **dispatch index** is public GitHub Issue
  [#62](https://github.com/louis-0hrs/HyperCine/issues/62); each linked
  platform-specific issue is the authoritative scope and test contract.
- At the beginning of a new task or resumed session, **read the latest**
  issue #62, the selected issue, and open PR discussion, before selecting
  code files to modify. If the GitHub CLI or connector is unavailable,
  report `BLOCKED_GITHUB_ACCESS`; do not claim an invented issue status.
  CLI example: `gh issue view 62 -R louis-0hrs/HyperCine`.
- **Claim before editing:** post a short comment to the specific issue with
  agent platform, existing worktree/branch, status `IN_PROGRESS`, and
  expected first verifiable deliverable. If a different live agent already
  owns the same task/files or shared GPU test window, coordinate instead of
  duplicating work. One agent may own multiple queued issues but must not
  start a blocked dependency.
- **Claim is not a permission request:** for an unclaimed `READY` issue within
  the agent's assigned lane and existing tool permissions, post `IN_PROGRESS`
  and begin work immediately. Do not request maintainer approval to claim,
  create an isolated branch/worktree, run tests or open a privacy-checked
  Draft PR. After claiming, re-check other comments for races; never duplicate
  another active owner. If simultaneous claims conflict, stop overlapping
  work and coordinate ownership before proceeding. Continue an existing
  own claim and update its Draft PR instead of re-claiming or creating
  duplicates. Existing claims and blocked dependencies remain binding; a
  claim grants no tool permissions.
- **Approval boundary:** approval is required for merges into branches owned
  by others (especially protected `main`), releases/tags/deployments,
  force-push/history rewrites, destructive/external service changes, and any
  special gate in the task. App/tool permission prompts are not bypassable
  by this document; if rights are missing, report `BLOCKED_TOOL_PERMISSION`
  with the needed permission rather than inventing a maintainer-approval
  requirement. All publication privacy checks in section 12 still apply
  before pushing a branch or opening/updating a Draft PR; verify
  pseudonymous Git author and committer identities before committing.
- Respect issue states `READY`, `IN_PROGRESS`, `BLOCKED`, `REVIEW`,
  `DONE`. A green historical check is not a substitute for testing the
  current commit or correct hardware. Explicitly distinguish CPU-only,
  Windows CUDA, Linux CUDA, Vulkan and macOS Metal evidence.
- Use one focused branch and Draft PR per independent deliverable. Do not
  overwrite another worktree, change unrelated branches, silently replace
  upstream reference bits or start a consumer before its required shared
  interface has passed review.
- At **every handoff or blocker**, update the assigned issue with:
  status; PR URL; exact head SHA; tests actually run with platform and
  result; files/interfaces modified; dependency/blocker; and one specific
  next action. Keep lengthy telemetry on local disk; publish only sanitized
  evidence and hashes. When a task completes, link the PR to the
  originating issue and return its status to `REVIEW`.
- Unless explicitly authorized by the maintainer, **do not autonomously
  merge, release, force-push or rewrite history**. Historical privacy
  violations require issue #54 remediation rather than disabling checks.
- This is a coordination **protocol**, not a remote-execution system:
  public GitHub issues/comments alone do not wake local agents.
  Unattended discovery needs a separately configured local agent poller,
  with user-controlled schedules, permissions and approval boundaries.
