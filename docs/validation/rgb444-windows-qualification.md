<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RGB444 source boundary: independent Windows qualification

Issue #37, parent PR #142 at exact source commit
`eb9fe2ed101f7e470022b8632abd155df90e99e0`.
No actionable source defect found in the bounded storage implementation.
Production code, parent tests and upstream remain unchanged.

The parent explicitly left allocation-failure injection untested. The independent
`tests/rgb444_boundary` CTest project compiles the actual frontend and replaces
aligned allocation only inside its test executable. It fails allocation 1, 2
and 3 in turn, verifies `bad_alloc`, checks all samples and geometry of the
previous frame, and verifies that partial allocations return to the baseline.
A successful retry and final zero live aligned allocations are required.
Aliased read-only source planes are also exercised. This counter is scoped to
the test's aligned allocations; it is not a process-wide leak detector.

Native Windows x64, MSVC 19.44.35229, CMake 3.31.6-msvc6, Python 3.10.9:

| Check | Result |
| --- | --- |
| Complete registered SDK-free Release CTest, external decode disabled | 20/20 PASS, zero skips |
| Independent allocation-failure Release CTest | 1/1 PASS |
| Independent allocation-failure MSVC AddressSanitizer CTest | 1/1 PASS |
| CI Python discovery | 19 tests: 16 PASS, 3 existing POSIX-only skips |

FFmpeg and ffprobe are unavailable on this host. The ten additional external
decode registrations and the required-external-decode full gate are **NOT RUN**;
20/20 is not the parent's 30/30 external-decoder configuration. No RGB packet,
SDK parity, GPU execution, benchmark, UBSan or full-suite ASan claim.
ASan covers the actual frontend and new fault-injection executable only.
The initial ASan launch lacked its runtime DLL on PATH; adding the compiler's
runtime directory resolved launch and the test passed. Python's temporary Git
fixture creation needed normal temporary-repository write access; the rerun
passed without changing or bypassing tests.

Reproduce from the repository root (MSVC environment on Windows):

```powershell
cmake -S . -B build -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=OFF
cmake --build build --config Release --parallel 1
ctest --test-dir build -C Release --output-on-failure
cmake -S tests/rgb444_boundary -B build-boundary
cmake --build build-boundary --config Release
ctest --test-dir build-boundary -C Release --output-on-failure
python -m unittest discover -s ci -p 'test_*.py'
```

For the scoped MSVC ASan build, use a separate build directory with
`-DCMAKE_CXX_FLAGS="/EHsc /fsanitize=address /Zi"` and
`-DCMAKE_EXE_LINKER_FLAGS="/INCREMENTAL:NO /DEBUG"`; make the matching MSVC ASan
runtime available on PATH before CTest. No installation or host settings change
is needed. The test also has a POSIX aligned-allocation implementation, which
was not executed in this Windows qualification.

Next action: review this test-only child together with the bounded parent
source API. Issue #37 remains partially implemented: actual RGB transform,
quantization, packet export and original-SDK correctness need separate evidence
and implementation. No protected-main merge, release or issue closure.
