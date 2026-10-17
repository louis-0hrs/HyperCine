<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Five macOS validation hardening tasks

Base: `e87ef0258ba37f5c78f1557c59d9c7331df5521c` (main).
These are five completed validation subtasks of the linked GitHub issues.
The parent features and their separate safety, geometry and production gates
remain open. This change does not implement blocked P5/P6/NEON/decoder work.

| Task | Defect and resulting behavior |
| --- | --- |
| [#23](https://github.com/louis-0hrs/HyperCine/issues/23) reference gate | Python equality accepted integral floats and booleans as integer fields, and truthy strings as normalized equality. Validate exact integer types, canonical digest syntax, table and band fields, and literal `true` before generation/comparison. Pinned manifest hashes remain mandatory. |
| [#46](https://github.com/louis-0hrs/HyperCine/issues/46) shared quality prerequisite | Probe booleans could compare equal to integer tables; frame order was assumed rather than checked. Validate required probe fields/types and both 36-entry tables, suite dimensions and consecutive frame indices before replay. No profile adapter or new quantization math. |
| [#7](https://github.com/louis-0hrs/HyperCine/issues/7) external decode acceptance | `assert` checks disappeared under `python -O`/`PYTHONOPTIMIZE`. All geometry, storage, p10 range, repeatability, golden and quality checks now raise explicitly. PSNR remains strictly above 35 dB and flat remains exact. |
| [#11](https://github.com/louis-0hrs/HyperCine/issues/11) Oracle import | Log record names became output paths before final inventory validation. Only eight known filenames are accepted; enforce record framing, unique names, declared/encoded lengths and a 16 MiB per-record ceiling (pixel buffers exactly 65,536 bytes). Check the complete inventory and resolved output parents before writing. Fixture hashes and comparison gates also survive `-O`. |
| [#54](https://github.com/louis-0hrs/HyperCine/issues/54) publication provenance | Two validators embedded the interpreter and literal local arguments in JSON. Reports now contain an explicitly named `command_template` with known switches, frame indices and named path-variable placeholders. Unknown arguments fail closed. Shared validation code is included in source fingerprints. This does not certify other report fields or arbitrary tool logs as safe to publish. |

`tests/test_validation_contracts.py` supplies fault injection, including fresh
optimized Python processes with changed packets, wrong stream identity,
out-of-range p10, low-quality pixels and unequal plane lengths. It also checks
successful extraction against all eight checked-in Windows fixtures and rejects
traversal, duplicates, nested/unmatched/truncated/oversized records without
writing output. No private corpus or media is used.

The new test is registered in the regular standalone CTest suite and required
by `ci/run_standalone.py`; it is not an opt-in side project. Native ARM64 Release
and full external decode ran with Python optimization enabled. See
[report.json](report.json) for actual counts, tools and patched-source hashes.
Existing 16/120-frame manifest replay and checked-in Windows decoder fixtures
ran; a private SDK binary corpus and six-profile external corpus were not
configured. No native Metal GPU, Windows, CUDA, Linux or sanitizer execution is
claimed for this Python/test-only change. Codec math, samples and `upstream/`
are unchanged; no performance improvement is claimed.

Reproduce from the repository root (CMake 3.25+, C++17 toolchain, Python 3.8+,
FFmpeg and ffprobe on PATH):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py \
  --build-dir build --system Darwin --machine arm64
python3 -m unittest discover -s ci -p 'test_*.py'
python3 -O tests/test_validation_contracts.py
```

The command template schema intentionally replaces the old literal `command`
field. Supply `HYPERCINE_PROBE`, `HYPERCINE_ENCODER`, `HYPERCINE_GOLDEN_DIR`,
`HYPERCINE_FFMPEG`, `HYPERCINE_OUTPUT_DIR` and related placeholders privately
when reproducing a reported invocation; their values are never in the template.
Raw local build/test logs remain outside the Git tree. Public evidence must
still undergo manual review and the privacy guard before publication.
