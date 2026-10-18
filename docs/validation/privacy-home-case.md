<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Case-insensitive home-path publication checks — issue #96

Parent: PR #131, `adcdb14bffe59166df98f61916c7f97eb1768df6`.
The UNIX and Windows home-path patterns were case-sensitive, while the existing
MSYS pattern already ignored case. Common macOS/Windows case variants could pass
through ref-name and annotated-tag checks.

Both home-path patterns now ignore case. Two disposable-local-remote reproductions
published their synthetic branch/tag metadata on the parent and are blocked after
the fix. Each new installed-hook test also checks redacted diagnostics and actual
non-publication, in both SHA-1 and SHA-256 repositories. No real private values or
public test pushes were used.

Native macOS ARM64, macOS 26.5.2, AppleClang 21, CMake 3.31.5,
Python 3.9.6, Apple Git 2.50.1, FFmpeg n8.0:

- Complete privacy Python discovery: **150/150 PASS**, zero skips, 90.032 seconds.
  This includes **96 real installed-hook scenarios**, 48 per object format.
- Full SDK-free Release CTest: **28/28 PASS**, zero skips, optimized Python and
  required independent external decode.

```sh
python3 -m unittest discover -s ci -p 'test_privacy*.py'
```

For the full codec regression gate, configure `HYPERCINE_BUILD_STANDALONE=ON`,
`HYPERCINE_BUILD_UPSTREAM=OFF`, `HYPERCINE_ENABLE_CUDA=OFF`,
`HYPERCINE_BUILD_TESTS=ON`, `HYPERCINE_BUILD_BENCHMARKS=OFF`,
`HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON`, Release and compile-command export;
build and run `PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64`.

This remains a child of the existing security stack. Native Windows/Linux/Git 2.55
qualification and historical remediation remain separate gates. No history
rewrite, protected-main merge, release, GPU or performance claim. Upstream is
unchanged; raw logs and disposable repositories remain outside Git.
