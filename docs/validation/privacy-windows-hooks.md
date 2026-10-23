<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Native Windows installed-hook qualification

Issue: #96. This test-only child follows PR #179 at
`2d2f4d0d09f90d6d6a4076b33e2e0f8b2de350d5`.

Native Windows discovery previously skipped all 96 installed-hook cases through
one POSIX-only class decorator. Removing that decorator lets Git for Windows
invoke the real pre-commit and pre-push shell hooks from native Python. Both
48-case object-format matrices (SHA-1 and SHA-256) now run normally. No hook or
scanner rule changes. A dedicated Windows Actions job keeps this coverage active.

## Reproduction

Prerequisites: Git for Windows and Python 3.8+ available to its shell hooks.
Run from the repository root:

```powershell
python -m unittest discover -s ci -p 'test_*.py' -q
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF `
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF `
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=OFF
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

All hook remotes are disposable local bare repositories. Hook tests check
rejection, diagnostic redaction and non-publication of sensitive synthetic refs,
tag annotations and identities; clean tags, repointed tags, multi-ref pushes,
replacement objects and unusual target types retain their existing assertions.
Test Git commands require permission to create their disposable repositories.

## Native results and limits

Platform: Windows x64, Git 2.45.1.windows.1, Python 3.10.9,
CMake 3.31.6-msvc6, MSVC 19.44.35229.0 Release.

- Unmodified parent discovery: 150 tests, 37 passed, 113 skipped.
- SDK-free CPU CTest: 18/18 passed, zero skips, 33.94 seconds.
- Full Python discovery: 156 tests, 139 passed, 17 existing POSIX-only skips,
  zero failures, 223.675 seconds. Includes all 96 installed-hook cases:
  48 SHA-1 and 48 SHA-256, zero hook skips.
- Workflow YAML parsed successfully; dedicated Windows job configuration checked.

FFmpeg/ffprobe were unavailable, so external-decode tests were not registered.
This is the complete configured CPU suite, not the required-external-decode
acceptance gate. Git 2.55, native POSIX shell stubs, Ubuntu CI-shell behavior,
hosted new-head CI and ordered security-stack integration remain separate.
The route-shape false-positive policy reported on #179 remains unchanged.
No codec changes, GPU qualification, sanitizer or throughput claim; frozen
`upstream/` remains unchanged. No main merge, tag publication or history rewrite.
