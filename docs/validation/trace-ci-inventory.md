<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Required FilmScan3 trace regression in standalone CI

Issue: #23. Reviewed source: PR #175,
`6ff4aafe48cf34d888a3eb7020580f70d24be335`.

The strict trace reader rejects ambiguous primary and comparison encoder JSON.
Its registered `filmscan3_trace_contract` regression was absent from the
standalone runner's required inventory. Removing that registration from the
actual 30-test inventory still passed `registered_tests`, allowing a green gate
without the new JSON regressions.

Require this existing test explicitly. A regression removes the registration
and checks rejection; it failed against the reviewed parent (one failed method
out of seven), then passed after the inventory correction. No trace parser,
encoder, fixture, manifest or production API changes.

Source SHA-256 after correction:

- `ci/run_standalone.py`: `bdb20f62d688b170fc293468753eae37ab43912d455681b7176bdb47617acde2`
- `ci/test_standalone_gate.py`: `373b7790a1bed0213cb4d4080557a354b0b30ca7e97c45cd3e5035d07b61a869`

## Native validation

macOS 26.5.2 ARM64, AppleClang 21.0.0, CMake 3.31.5,
Python 3.9.6 with optimization enabled, FFmpeg n8.0.

- Original reviewed head: full SDK-free Release CTest **30/30 PASS**.
- Corrected child: full SDK-free Release CTest **30/30 PASS**, zero skips,
  required external FFmpeg decode and existing 16/120-frame manifest checks.
- Corrected CI helper suite: **7/7 PASS**; strict trace methods: **5/5 PASS**.

Configure and build the existing SDK-free Release configuration with tests and
required external decode enabled, upstream/CUDA/Metal/benchmarks disabled,
FFmpeg/ffprobe available through PATH or the documented CMake cache variables,
and compile commands exported. Then run:

```sh
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64
python3 -O ci/test_standalone_gate.py
python3 -O tests/test_trace_contract.py
```

The initial configure correctly rejected unavailable FFmpeg executables on PATH;
the existing local validator executables were explicitly configured before the
successful runs. No decode gate was disabled. Raw logs and machine-specific
paths remain outside Git.

Actual private SDK packets/internal-stage dumps were not supplied. Original
packet parity and all remaining #23 feature gates stay open. This is CPU test
inventory validation, with no new sanitizer/GPU/performance claim or protected
main merge/release.
