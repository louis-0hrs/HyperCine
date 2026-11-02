# Issue #144: original coefficient stages through the existing S3 graph

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Status: **REVIEW — bounded research-stage validation**, native Windows x64,
2026-10-30; base main `04c348f848de1eb3a47d3b90fe191722c1ab559e`.

The old compiler/SIMD-provenance task is already completed, and #292's S3
attachment is merged through #298. This follow-up tests their combination:
the six existing original coefficient pairs pass through `MakeSpatialLowpassTask`
and the graph-owned publication path, rather than a direct scalar-only call.
It does not implement the separately proposed next inverse stage or a decoder.

## Exact stage scope

Four authenticated, entropy-completed/dequantized wavelet5 bands produce
wavelet4 band0 for the same SDK channel. The existing
`SCALAR_INVERSE_STAGE_PROPOSAL_144.json` binds all five original files in each
case to the same original packet and producer/capture identity:

| Original packet geometry | SDK channel | Input stage geometry | Output stage geometry |
| --- | --- | --- | --- |
| 1920x1080 | 0 Y | 240x135 | 480x270 |
| 1920x1080 | 1 V | 120x135 | 240x270 |
| 1920x1080 | 2 U | 120x135 | 240x270 |
| 3840x2160 | 0 Y | 480x270 | 960x540 |
| 3840x2160 | 1 V | 240x270 | 480x540 |
| 3840x2160 | 2 U | 240x270 | 480x540 |

The existing original-sidecar verifier authenticates **all 30 payloads and 60
JSON/SHA sidecars before any probe executes**, then revalidates payload hashes
and sidecar bindings afterward. These are the existing loose originals under
the local `HYPERCINE_GOLDEN_DIR` binding. Nothing is exported, resealed,
renamed or overwritten; no SDK encoder/decoder is run here.

The test driver reads compact signed-16 LE rows into the existing aligned
`OwnedSignedPlane` storage. The graph copies the padded allocations; scalar
arithmetic and original-byte comparison use only visible coefficients. No scaling,
clipping, unsigned reinterpretation or output-derived expected values are added.

## Graph execution and negative controls

The actual existing S3 factory and scheduler execute every case at workers
**1/2/4/16**. LL is queued after the three highpasses; immediately before LL
publication the spatial consumer must still be Pending. Every producer publishes
its graph-owned copy and then overwrites its own caller buffer with a sentinel.
Exact results therefore also exercise the owned-copy lifetime path.

The output sink admits the same generation/channel/wavelet/band, nonzero version,
signed16 LE encoding and doubled geometry. It copies visible bytes into owned
signed objects after typed admission; workers join before output is serialized.
The independent Python wrapper compares both SHA-256/length and actual output
bytes to the authenticated original LL buffer.

Two identity controls accompany every positive execution:

- Exchange the first two declared band edges. The consumer must reject the
  mismatch before the output sink is entered.
- Cross legal channel identities: **Y->V, V->U, U->V**. Both directions of the
  V/U permutation reach the actual typed consumer guard; invalid channel 3 is
  never used as a shortcut.

Controls require exit **2**, the specific `S3_IDENTITY_REJECTED` witness,
`no_output_access=1`, a drained graph, preserved callback error and no output
directory. An accidental crash, allocation failure, exit 1, missing marker or
an output publication does not count as a rejected control. These are input
identity controls, not newly executed source guard-removal mutation claims.

## Native evidence

Windows x64, MSVC 19.44.35229.0, CMake 3.31.6, Python 3.10.9; SDK/GPU/benchmarks
OFF, graph-only/tests ON, original corpus explicitly bound.

| Final configuration | Full configured CTest | Original positive outputs | Identity controls |
| --- | --- | --- | --- |
| Release | **32/32 PASS**, no skips | **24/24 byte-exact** | **48/48 rejected** |
| MSVC AddressSanitizer, Release | **32/32 PASS**, no skips | **24/24 byte-exact** | **48/48 rejected** |

Every six-case/channel/worker combination is present. The two final replays
produce the same six original hashes, and both probe hashes are recorded.
Existing T1/T2/S2/T4/T5 and S3 attachment tests remain registered and pass.
No ASan report is observed in the final suite. This is not UBSan, TSan, leak
sanitizer, big-endian, GPU or full-frame qualification.

An initial ASan configuration incorrectly replaced CMake's default C++ flags
with `/fsanitize=address /Zi`, dropping `/EHsc`. Nine C4530 warnings identified
missing exception-unwind semantics. Its full suite returned **16 PASS / 16 FAIL**,
including unchanged allocation/exception contract tests; that result is retained,
not presented as a passing sanitizer run. Restoring the baseline compiler flags
and appending ASan flags, with `/DEBUG /INCREMENTAL:NO` at link, fixes the
configuration. The final full suite above runs with `/EHsc` intact, ASan flags
verified on graph/attachment/replay targets and the ASan runtime dependency
verified. No assertion, error path or test registration is removed to get green.

The new protocol contract has **eight SDK/tool-free methods**: actual synthetic
DC replay and identity controls at every worker/channel, wrong expected bytes,
truncated input, existing-output preservation, accidental failures versus
controlled rejection, missing originals before probe execution, post-execution
sidecar revalidation, and invalid worker/geometry admission. Its synthetic
expected values are explicitly not original-SDK evidence.

Public computed evidence:

- [Original graph replay, Release](../evidence/issue144-original-graph-release-20261030.json)
- [Original graph replay, ASan](../evidence/issue144-original-graph-asan-20261030.json)
- [Full test inventories, flags, source/probe hashes and initial invalid configuration](../evidence/issue144-native-validation-20261030.json)

Tests run before the evidence commit on a modified worktree based at `04c348f`;
the report binds the final test-source bytes. **All `src/`, public headers and
`upstream/` remain unchanged**, including inverse arithmetic and scheduler.
Only test code, its CMake registration and sanitized evidence are added.

## Reproduction

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DHYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY=ON `
  -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF `
  "-DHYPERCINE_S3_ORIGINAL_ROOT=$env:HYPERCINE_GOLDEN_DIR"
cmake --build build --config Release --parallel 4 -- /nodeReuse:false
ctest --test-dir build -C Release --output-on-failure `
  --output-junit results.xml --test-output-size-passed 200000
```

For native MSVC ASan, use a separate `build-asan`, preserve the standard flags
including `/EHsc`, and append `/fsanitize=address /Zi` to `CMAKE_CXX_FLAGS`.
Set `CMAKE_EXE_LINKER_FLAGS` to `/INCREMENTAL:NO /DEBUG`, make the installed
MSVC ASan runtime available on that test process's PATH, and run with
`ASAN_OPTIONS=halt_on_error=1`. The exact measured flags are in the JSON.

The private gate is registered only when `HYPERCINE_S3_ORIGINAL_ROOT` is
explicitly supplied. Missing/corrupt originals then fail with exit 1; no skip
or synthetic fallback is permitted. Without that binding, the protocol remains
registered but no original equality is claimed. For an optional new report:

```powershell
python -B tests/validate_spatial_attachment_144.py `
  --probe build/tests/decode_task_graph/Release/replay_original_s3_144.exe `
  --golden-dir "$env:HYPERCINE_GOLDEN_DIR" --output new-s3-observations.json
```

The report must be new and outside the corpus. Raw temporary outputs are private
and removed after comparison; the CTest JSON contains no private paths or media.

## Remaining scope and next action

`GraphProfile` is still `Synthetic`. Supplied original coefficients do not
create a supported codec profile or parser/entropy path. No temporal inverse,
color conversion, GOP1/GOP2 frame decoder, public API or original SDK race fix
is implemented. Existing #120/#16 pixel-release gates are not changed.

Review the replay transport, immutable-input binding, both chroma identity
directions and exact source-bound native results. Keep #144/#47 open. Any next
inverse unit or actual bitstream attachment needs its own reviewed source,
numeric oracle and admission scope; this result authorizes no phase/main merge
or production release.
