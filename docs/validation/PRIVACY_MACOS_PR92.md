# Native macOS qualification of PR #92

<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE. -->

Date: 2026-10-16. Issue: #54. Reviewed source:
`b21edb17bbe62926cc7dc4ebc4fffaacd5d75d6b` (PR #92), based on
`5c87391e82f307fbe7db0be6a53fd0edbdaad527`.

Native environment: macOS 26.5.2, ARM64, AppleClang 21.0.0
(`clang-2100.1.1.101`), CMake 3.31.5, Apple Git 2.50.1, Python 3.9.6,
FFmpeg/ffprobe n8.0 (native ARM64).

The review used an isolated worktree with worktree-specific pseudonymous
identity, `user.useConfigOnly=true`, executable repository hooks and
`core.hooksPath=.githooks`. The scanner, hooks, codec and fixture bytes are
unchanged from the reviewed source. Release and sanitizer binaries were built
before adding the qualification harness and this report. Raw logs and generated
build evidence remain outside the source tree.

## Results

| Check | Native result |
| --- | --- |
| Original privacy suite | 38 declared and collected; 38 passed |
| Original complete Python suite | 44 passed |
| Added installed-hook integration suite | 12 passed |
| Complete Python suite including the new tests | 56 passed |
| Release full standalone CTest + external decode gate | 28/28 passed, 76.27 seconds |
| ASan+UBSan full standalone CTest + external decode gate | 28/28 passed, 162.02 seconds |
| Reviewed commit, commit range and staged scanner | Passed |
| Shell syntax for both hooks | Passed |
| Upstream changes | None |

The new `ci/test_privacy_hooks.py` executes real commits and pushes to
disposable local bare repositories, with global/system Git configuration and
inherited Git environment overrides isolated. It verifies:

- Explicit human corporate-domain identity, neutral agent identity and GitHub
  noreply agent identity can commit and first-push.
- Agent `useConfigOnly` requirements, unapproved agent domains and sensitive
  author/committer overrides block actual commits without echoing the values.
- Imported sensitive metadata blocks a first push; imported sensitive content
  blocks both a first push and an existing-branch update.
- Tracking the same commit on a different remote cannot hide it from the
  destination's first-push scan.
- A later cleanup commit cannot hide an intermediate MSYS-style path exposure.
- Raw-URL pushes fail closed; rejected pushes leave the public test ref absent
  or unchanged.

Imported history is created in a separate repository before hooks are
configured there. No hook is disabled or bypassed. All remote operations stay
on local disk. The privacy workflow now discovers `test_privacy*.py`, including
these twelve POSIX integration tests; Windows skips this additional POSIX
class. Existing scanner tests remain unchanged.

## Identity policy and review limits

The same public corporate-domain shape passes for an ordinary human name and
is rejected for an agent name. This matches the documented policy. Agent-name
matching is heuristic: a human name containing one of the reserved agent words
can also trigger the stricter policy. This review does not establish zero
false positives for arbitrary names or prove an identity's provenance.

This qualification covers the SDK-free CPU build and installed publication
hooks. It does not qualify CUDA/Metal paths, the frozen SDK, private reference
corpora, historical disclosure erasure or protected-main integration. PR #92
and issue #54 still need the maintainer's policy/history decision.

## Reproduction

Check out the reviewed SHA for the original 38/44-test results. Use this child
branch for the twelve installed-hook tests and combined 56-test result. Put
FFmpeg and ffprobe on `PATH`; keep build outputs outside the source tree.

```sh
python3 -m unittest discover -s ci -p 'test_*.py' -v
python3 ci/privacy_guard.py --commit b21edb17bbe62926cc7dc4ebc4fffaacd5d75d6b
python3 ci/privacy_guard.py --commits 5c87391e82f307fbe7db0be6a53fd0edbdaad527..HEAD
sh -n .githooks/pre-commit .githooks/pre-push

cmake -S . -B ../hypercine-review-release \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build ../hypercine-review-release --parallel 3
python3 ci/run_standalone.py --build-dir ../hypercine-review-release \
  --system Darwin --machine arm64
```

For sanitizer reproduction use a separate build directory, `RelWithDebInfo`,
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` and
`-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'`. Run the same gate with
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. LeakSanitizer is excluded
on Apple ARM64; external FFmpeg binaries are not sanitizer-instrumented.

SHA-256 checks:

| File | SHA-256 |
| --- | --- |
| `ci/privacy_guard.py` | `69048a756d16a904fc612785a13aa14c3a4da449efed1f01d63d1628c82bda7b` |
| `.githooks/pre-commit` | `6e040cb7e0d785e7b7f7e05f07d6b59d6d32a1df4ec003d6780b04d5e47de1ce` |
| `.githooks/pre-push` | `611ceef9b77678637f76fc1ca80ebcac9b677b66d777ec40fd06a183cb7b8610` |
| `tests/fixtures/standalone/canonical.cfhd` | `9cf732846c1b9838c8f5d24f92db0182b82b5b6a05df63ffd5114fc1606db6fd` |
