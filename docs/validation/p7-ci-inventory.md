<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Execute P7.A audits in the normal standalone CI gate

Issue: #47. Reviewed source: PR #177,
`3a7e964a82a20e4714f842725f9e0614a3086e45`.

The nested-JSON correction is effective in the separate P7.A project. The normal
standalone CI job registered 32 tests without `p7_plane_contract`,
`p7_json_contract` or `p7_public_sdk_fixture`, so its successful runs did not
execute that regression or the public packet/pixel baseline.

Include the existing P7.A project when standalone tests and external decode tools
are available, and require all three registrations in the CI inventory. Reuse
the exact FFmpeg/ffprobe tools chosen by the outer gate. Preserve the separate
project and the tests-disabled configuration. No Python verifier, fixture,
manifest, codec kernel, production API or frozen upstream file is changed.

The new helper regression fails on the parent for all three missing names.
After correction every individual omission is rejected. Real CMake inventories
confirm the change from 32 registrations with no P7 tests to 35 containing all
three. The registered public-decode command uses the explicitly configured
oracle executables.

## Native validation

macOS 26.5.2 ARM64, AppleClang 21.0.0, CMake 3.31.5,
Python 3.9.6 with optimization enabled, FFmpeg n8.0:

- Original reviewed head: full SDK-free Release CTest **32/32 PASS**;
  separately configured P7.A project **3/3 PASS**.
- Corrected child: full SDK-free Release CTest **35/35 PASS**, zero skips,
  required external decode, including all 24 actual public CFHD packets/frames
  with frozen complete-output and plane hashes.
- Separate P7.A project remains **3/3 PASS**; tests-OFF registers **zero** tests.
- Corrected CI helpers **7/7 PASS**; JSON/plane methods **9/9 PASS**.

Configure/build the existing SDK-free Release configuration with tests and
required external decode enabled, upstream/CUDA/Metal/benchmarks disabled, and
compile commands exported. Supply FFmpeg/ffprobe through PATH or their existing
CMake cache variables. Then run:

```sh
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64
python3 -O ci/test_standalone_gate.py
```

The separate project remains reproducible with FFmpeg/ffprobe on PATH:

```sh
cmake -S tests/macos_decode_portability -B build-p7
PYTHONOPTIMIZE=1 ctest --test-dir build-p7 -C Release --output-on-failure
```

The external oracle is FFmpeg. No original-SDK pixel comparison or HyperCine
runtime decoder is certified. P7.B still requires #13/#16 and independently
matched original SDK output. This is native CPU Release validation; no new
sanitizer, GPU or performance measurement, protected-main merge or release.
Raw logs, media output and machine-specific paths remain outside Git.

## Source SHA-256 after correction

- `CMakeLists.txt`: `37430a609d3fcc9b82e89c3b830102fdd56b2b420639e01dd95974eb4bb14ad7`
- `tests/macos_decode_portability/CMakeLists.txt`: `bc891a3b64a4dd350f211f55fe70534c4894ea47b12d9eb8d7dc26b0985f0732`
- `ci/run_standalone.py`: `9eb72ffb95f16088e08d06480de03f27d2dac62ca670e07fd26fe7f045f8529c`
- `ci/test_standalone_gate.py`: `d8fef6c35c268d12aabdeff879815ab4945924182c56eb2bb1087b28b7abf18d`
