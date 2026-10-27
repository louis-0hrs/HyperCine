<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Combined publication guard qualification — #54 and #96

This isolated child starts at PR #123 `89c4c45d2ccdec5119ef98a98799fbdc7cf108cc`
and applies the direct/CI original-object protection and workflow regression
suite from PR #122 `87b4799d9ebd292c8517063ec92c01ef444e5976`.
The existing parents remain untouched; this provides a concrete combined head
for review instead of inferring that two independently green heads compose.

## #54: original-object and CI protection

All scanner entry modes set `GIT_NO_REPLACE_OBJECTS=1` before examining Git
objects. CI sets it before resolving its range/ancestry as well. The imported
13-method workflow suite executes the checked-in workflow shell against local
synthetic histories. This includes replacement-hidden staged content, direct
commit/range identity inspection, actual PR/first-push ranges, both commit
envelope identities, intermediate disclosure and missing-base rejection.
No real sensitive values are used or published in the regression fixtures.

## #96: ref/tag behavior on the combined head

The #123 guard and all 46 actual installed-hook methods are retained: nested
remote names are built as legacy configuration without relying on `remote add`,
custom fetch mappings remain fail-closed, and both outgoing ref names, nested
annotated tags, tagger metadata, replaced tag objects, mixed-ref pushes and
non-publication assertions remain binding. No test skip, deletion, destination
branch update or production codec change is introduced by this combination.

## Actual native verification

macOS ARM64, Apple Git 2.50.1, Python 3.9.6, AppleClang 21, CMake 3.31.5,
FFmpeg n8.0. Python discovery: **105/105 PASS**, zero skips, 58.244 seconds
(40 guard + 46 installed-hook + 13 workflow + 6 standalone-gate methods).
Fresh full SDK-free Release CTest with required FFmpeg/ffprobe external decode:
**28/28 PASS**, zero skips, 82.91 seconds. Frozen upstream is unchanged.
Shell syntax and executable-hook checks pass. Raw build/test logs stay local.

```sh
python3 -m unittest discover -s ci -p 'test_*.py'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_ENABLE_CUDA=OFF \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_BUILD_STANDALONE=ON \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

FFmpeg and ffprobe must be discoverable or configured through their existing
CMake executable variables. This native result is not Git 2.55, Windows or
Linux hook execution. Hosted checks and native Windows/Linux qualification
of the resulting combined head remain necessary before ordered integration.
Pre-existing remote tracking state, fetch/push destination identity, arbitrary
binary content and historical disclosures retain the documented trust limits.
Neither parent issue is closed, and no protected-main merge, release or history
rewrite is performed.
