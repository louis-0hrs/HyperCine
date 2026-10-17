<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Real SHA-256 publication-hook qualification — #96

Child of PR #125, parent aa0c61373e34d945bb084a6150d65e76534fb930.
The scanner accepts 40- and 64-digit object IDs, but the installed-hook suite
previously constructed only default SHA-1 repositories. The same 46 actual hook
scenarios now run in a second fixture using explicit Git SHA-256 object format
for every working/private/public repository. Fixtures verify actual object format
and hash length; this is not a string-only simulation or a production guard change.

Both formats exercise commit identities/content, refs and annotated/nested/replaced
tags, custom fetch mappings, legacy remote configurations, deletion and rejection
with actual non-publication checks. Native macOS Apple Git 2.50.1: all 151 Python
methods PASS, zero skips, including 92 installed-hook scenarios (46 per format).
Total Python runtime: 96.059 seconds. Native Windows/Linux/Git 2.55 runs remain
pending; the existing POSIX fixture remains explicitly skipped on Windows.
No shared history was changed and historical privacy remediation remains open.

```sh
python3 -m unittest discover -s ci -p 'test_*.py'
ctest --test-dir build -C Release --output-on-failure
```

Native test environment: macOS ARM64, AppleClang 21, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Full Release configuration uses HYPERCINE_BUILD_STANDALONE=ON,
HYPERCINE_BUILD_UPSTREAM=OFF, HYPERCINE_ENABLE_CUDA=OFF,
HYPERCINE_BUILD_BENCHMARKS=OFF and HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON.
Supply FFmpeg/ffprobe through PATH or the existing CMake executable cache options.
No protected-main merge or release; frozen upstream remains unchanged.

Full native SDK-free Release required-external-decode CTest: 28/28 PASS,
zero skips, 77.36 seconds.
