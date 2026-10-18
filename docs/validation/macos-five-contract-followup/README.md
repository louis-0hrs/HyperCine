<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0. -->

# Five macOS issue contract fixes — 2026-10-18

Five separate local branches contain focused validation changes against the
existing reviewed feature work. `sources.json` records each base commit and the SHA-256 of every modified
implementation/test file. Each issue has its own feature branch.
Use `git worktree list` to locate each branch. Existing owner checkouts and the
original working tree changes were preserved. No issue closure, feature acceptance or main merge is implied.

| Issue | Local branch | Implemented change | Complete CTest |
| --- | --- | --- | --- |
| #23 | `codex/macos-five-contract-followup` | Reject duplicate trace JSON keys at every depth, nonfinite numbers, wrong profile/root and empty frame inventories for both encoder trace readers | Release 30/30 PASS, 77.72 s |
| #44 | `codex/p4-sequence-contract-followup` | Reject an empty sequence hash inventory, invalid digests and zero/negative/noninteger frame extents before reading | Release 31/31 PASS, 198.99 s |
| #45 | `codex/metal-budget-contract-followup` | Reject zero/negative/noninteger geometry, odd YUV422 width and invalid padding in the allocation-free resident-buffer model | Contract suite 6/6 PASS, 1.07 s |
| #47 | `codex/p7-json-number-followup` | Reject JSON exponent overflow to infinity in baseline files and tool output, preserving the generic failure response | Audit suite 3/3 PASS, 2.49 s |
| #70 | `codex/macos-corpus-cmake-followup` | Check actual Apple compiler architecture flags, including per-target overrides; match CMake truth rules for optional corpus registrations | Release 30/30 PASS, 73.68 s |

All CTest runs use `PYTHONOPTIMIZE=1`. Targeted optimized Python suites pass:
trace 5 methods; sequence 8; Metal model 4; P7 JSON 6; CI gate 14. New negative
cases failed on the parent behavior before their corresponding fixes. The trace
and architecture additions introduce new helpers; separately reproduced parent
behavior proves the underlying defects rather than treating missing helpers as
bug evidence.

The parent trace reader silently overwrote a duplicated `frame_index` and then
accepted the resulting typed trace. A real native CMake reproducer with global
`CMAKE_OSX_ARCHITECTURES=arm64` and a target-specific `x86_64` override passed the
old cache check and fails the new compile-command check. The actual standalone
compile database contains 17 commands, all accepted as ARM64.

The #44 run independently decoded 19 actual 3840x2160 samples, matched sixteen
ordered sequence frame hashes, and measured minimum finite PSNR
52.32134949615484 dB. Its geometry decode test passed in 119.58 s. No threshold
was changed. The #47 audit decoded all 24 frames of the existing public SDK AVI.

## Reproduction

For #23, #44 and #70, run in the corresponding worktree with available
FFmpeg/ffprobe (or privately configured executable cache paths):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
PYTHONOPTIMIZE=1 ctest --test-dir build -C Release --output-on-failure
```

For #45, configure `tests/macos_contracts` as the source directory. The six-test
run additionally configured `HYPERCINE_P7_AUDIT_ROOT` to the #47 checkout;
without that optional helper the suite has five tests. For #47, configure
`tests/macos_decode_portability` directly. Build and run CTest with the same
Release/optimization settings.

Native environment: macOS 26.5.2 ARM64, AppleClang 21, Python 3.9.6,
CMake 3.31.5, FFmpeg n8.0. This run adds no sanitizer or hosted-CI evidence.
Whitespace checks and a privacy scan of each complete working diff passed;
`upstream/` remained pristine in all five worktrees. Staged privacy scans, manual staged-diff review and every outgoing-commit
scan are required before publication. Local logs and generated media remain
outside Git.

## Remaining feature gates

These changes harden prerequisites, not complete the parent multi-phase issues.
No private original-SDK binary corpus was configured; #23 manifest checks do
not establish a fresh original-binary replay. Original GoPro 4K packet parity
remains `REFERENCE_BLOCKED`; 8K is `PENDING_8K`. #44 experimental acceptance
still gates P5 and NEON implementation. The Metal result is an arithmetic model,
not GPU execution or measured device memory. No NEON kernel, native decoder,
Metal-4K backend, performance improvement, merge or release is claimed.
