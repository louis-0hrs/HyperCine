<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Original-object privacy inspection — issue #54

Parent: PR #114, `1e6626811567d77f50a27c486204f1156ef7de8a`.

Local Git replacement refs can substitute safe content or identities for an
unsafe commit during `--commit`, `--commits` and staged-baseline inspection.
The existing push path already disabled replacement objects; direct and CI
entry points did not. The scanner now disables replacement lookup for every
mode. The workflow also exports the setting before selecting its merge base.
No replacement ref is deleted and no published history is rewritten.

Synthetic disposable repositories exercise original content through actual PR
and first-push workflow shell, original author/committer through both direct
commit modes, and staged additions against a replaced HEAD. Diagnostics must
remain redacted. Removing the fix reproduces four failing subcases in the
initial two added methods; all corrected cases pass, including the subsequently
added staged-HEAD case. This is a newly reproduced local-control bypass, not
proof of any new real disclosure or historical erasure.

Reproduce from the repository root:

```sh
python3 -m unittest discover -s ci -p 'test_*.py'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_STANDALONE=ON \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

FFmpeg and ffprobe must be discoverable, or supplied with the documented CMake
executable variables. Raw outputs remain outside Git. Native macOS ARM64,
AppleClang 21, CMake 3.31.5, Python 3.9.6, Apple Git 2.50.1 and FFmpeg n8.0.
Platform-specific results are recorded in the PR handoff. No native Windows,
Linux, CUDA, sanitizer or performance qualification is inferred from this run.
The remaining security stack and historical #54 remediation stay open.
