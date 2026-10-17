<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Commit identity checks through the publication CI command

Refs #54. Test-only child of PR #109, exact parent
`f497234b12be6382164c2c8576c0efc5b3edc109`.

`ci/test_privacy_ci.py` extracts and executes the actual Bash `run` block of
the publication workflow's commit-inspection step. Each test creates a local,
disposable Git repository containing the current scanner. Global/system Git
configuration and inherited Git environment overrides are excluded. No test
pushes to GitHub or reproduces actual contributor metadata.

Ten test methods exercise 22 workflow executions:

- PR and first-push ranges reject unapproved synthetic agent author and
  committer identities, together and independently, with redacted diagnostics.
- GitHub noreply and neutral agent identities pass, as do explicit human
  identities on a public domain; this preserves the existing policy.
- A later safe commit cannot conceal an intermediate rejected identity.
- Existing-branch pushes use their before SHA; empty or unavailable before
  SHAs use the main merge base.
- Missing PR bases or first-push merge bases fail closed; empty ranges pass.

The existing workflow discovery pattern automatically includes this file.
The scanner, hooks and workflow are unchanged. This checks the existing
agent-name heuristic, not whether arbitrary human-looking names belong to
agents. CI still runs after publication and cannot prevent initial disclosure.

## Reproduce

Run from the repository root on Linux or macOS with Bash, Git and Python 3.8+:

```sh
python3 -m unittest discover -s ci -p test_privacy_ci.py -v
python3 -m unittest discover -s ci -p 'test_*.py' -v
```

The new tests intentionally skip native Windows because the workflow runs on
Ubuntu. Native Git-for-Windows hook qualification remains a separate gate.

## Native macOS verification, 2026-10-17

ARM64, AppleClang 21.0.0, CMake 3.31.5, Python 3.9.6, FFmpeg n8.0:
new workflow suite **10/10 PASS**; all Python suites **99/99 PASS**, zero skips.
Standalone Release build succeeded and full CTest **28/28 PASS**, zero skips,
75.57 s, including the required external FFmpeg decode tests.
A temporary mutation removing commit-envelope inspection made the PR rejection
test fail as expected. The mutation was never applied to the repository.

For the native codec regression gate, put FFmpeg and ffprobe on `PATH` and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

No codec, frozen upstream, sanitizer, performance or historical privacy
remediation claim follows from these publication-control tests. The security
stack still needs its separate integration and platform acceptance gates.
