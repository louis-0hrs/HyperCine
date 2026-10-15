# macOS / Windows bit-exact reference bridge — issue 23

Copyright (c) 2026 HyperCine Contributors. MIT OR Apache-2.0.

**Draft diagnostic milestone; production gate BLOCKED.** This branch is stacked on
PR #22 (`codex/macos-standalone-variable-size`, `d1c341d`). It does not implement
an SDK-compatible FilmScan3 encoder or claim Windows bitstream identity. The
experimental FS3-Q2 encoder and its packet bytes are unchanged.

## Measured evidence

| Gate | Result | Scope |
|---|---|---|
| Tier A input BGRA SHA-256 | PASS, 16/16 | Actual regenerated 1920×1080 bytes, including stamps |
| Sequence input BGRA SHA-256 | PASS, 120/120 | Each sequence index checked against the imported manifest |
| CPU diagnostic replay, frames 0/3/6/13 | FAIL | 0/30 coefficient FNV and 0/30 payload SHA matches per frame |
| Windows SDK/CUDA binary files | BLOCKED | No local corpus root supplied; hashes are not sample files |
| Earliest causal stage / signed coordinate | PENDING | Windows conversion/filter/coefficient dumps required |
| SDK-compatible FS3 profile / dynamic state | PENDING | Exported quant/scale/state retained, no state evolution port |
| Mac vs Windows normalized full sample | BLOCKED | SDK-compatible encoder and actual binaries required |
| 4K/8K production parity | PENDING | No supported oracle/data or geometry release |

The machine-readable results are in
[`validation/macos_windows_bitexact.json`](validation/macos_windows_bitexact.json).
The first *observed* mismatch for all four probed frames is channel 0, subband 0
(Y L3 LL, 240×135, quant=1). For frame 0 the Mac coefficient FNV is
`e3e8b01a9160e0d3`. Expected hashes are recorded alongside actual hashes in the
JSON. This is a coefficient-hash observation, not proof of the earliest causal
stage or a particular differing coefficient. No PSNR threshold is used here.

The replay uses the existing Q2 conversion/wavelet implementation with the
**exported per-band divisors**. It does not apply the SDK scale table as a new
transform, reproduce SDK rate control, or emit an SDK-compatible packet. Changing
just the divisors does not establish FilmScan3 compatibility. Its FAIL is an
expected measurement of the compatibility gap, separate from passing CTests.

## Input provenance and exact generator semantics

`tests/reference_corpus/{manifest.json,manifest.schema.json,
phase3a_1080p_120_manifest.json}` are byte-for-byte read-only imports from Windows
branch commit `addd8f46ca78a968b9c235baf3f7e3d53d8238f7`. Their SHA-256 values,
the complete imported source hash, and the portable helper hash are pinned in
`tests/reference_corpus/import_provenance.json`. The validator refuses changed
manifest/helper bytes. Original Windows generator/build provenance, including the
dirty-tree capture flag and captured source file hashes, stays intact. Imported
source bytes and captured build hashes are separate provenance facts.

`tests/reference_frame_generator.h` isolates the real integer generator and
stamps without SDK/CUDA declarations. Negative arithmetic shifts in the
photographic pattern use explicit floor division for portable C++17 semantics.
The tool uses aligned storage and explicitly serializes little-endian BGRA.

Crucially, `VerifyPhase3ASequence120` first generates each archetype at
**frame_index = numeric pattern id**. For sequence frame k it copies that
archetype and applies `StampFrameIdentityBlocks(..., k, pattern)` again. Calling
`GeneratePhase3AFrame(..., k, pattern)` directly fails the input hashes for most
frames. The probe implements the former sequence semantics. Pattern names alone
and the unrelated Mac Canonical pattern are never accepted as input identity.

All 136 input buffers are regenerated and checked for exact byte length and
SHA-256. Immediately before any diagnostic codec invocation the input is checked
again. Input mismatch raises an error and prevents that invocation.

## Reproduce on Apple Silicon without the GoPro SDK

Prerequisites: CMake 3.25+, C++17 compiler, Python 3 standard library for the test
harness. FFmpeg/ffprobe are optional external validation executables; neither is
linked into the codec. The recorded run used Apple Clang 21.0.0, CMake 3.31.5,
arm64, and FFmpeg n8.0. Exact compiler version, source hashes, probe executable
hash, host and diagnostic invocation are included in the result JSON.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF \
  -DHYPERCINE_FFMPEG_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffmpeg \
  -DHYPERCINE_FFPROBE_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffprobe
cmake --build build -j4
ctest --test-dir build -C Release --output-on-failure
python3 tests/validate_windows_reference.py \
  --probe build/hypercine_reference_probe --inputs-only
python3 tests/validate_windows_reference.py \
  --probe build/hypercine_reference_probe \
  --output docs/validation/macos_windows_bitexact.json
```

The input-only command exits 0 after verifying all inputs. A full diagnostic run
exits **2** because the production compatibility gate remains blocked, even if
all inputs pass. Invalid input/provenance/artifacts or a failed subprocess exit
**1**; these errors must never be converted into a parity PASS.

Retain intermediate files outside Git with `--stage-dir /private/tmp/mac-stages`.
Each selected Tier A frame gets its own directory. Dumps include converted
Y/Cr/Cb planes, all four bands at each wavelet level, all 30 quantized bands in
channel/subband order, and entropy payloads. Signed coefficients use tight
row-major little-endian int16 (no stride padding); LL payload is big-endian
int16; HP payload uses the existing Table17 writer. Quant plans contain exactly
30 validated divisors; full quant[36]/scale[36] and previous-frame state remain
in the result JSON. These diagnostic files are not SDK-compatible samples.
The horizontal and vertical internal filter passes are not separately dumped
in this draft. A future instrumented stage interface must expose those too.
`first_i16_difference` supports exact signed x/y/value reporting once verified
Windows intermediate bytes are available; hashes alone cannot provide that.

```sh
cmake -S . -B build-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined \
  -DHYPERCINE_FFMPEG_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffmpeg \
  -DHYPERCINE_FFPROBE_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffprobe
cmake --build build-sanitized -j4
ctest --test-dir build-sanitized -C Release --output-on-failure
python3 tests/validate_windows_reference.py \
  --probe build-sanitized/hypercine_reference_probe \
  --output /private/tmp/mac-sanitized-reference.json
```

## Minimal Windows transfer request

Supply these original files under the paths indexed by the imported Tier A
manifest, ideally frames **0000 (FlatBlack), 0003 (gradient), 0006 (noise)**:

- `frames/tier-a-1080/frame_NNNN/input.bgra`
- `sdk.cfhd`, `cuda.cfhd`, and the separately exported `*.normalized.cfhd` in each
  of those frame directories (their hashes/lengths are already pinned)
- The original manifest and, if possible, tight signed conversion/filter and
  coefficient dumps with per-file SHA-256, shape, endianness, row orientation,
  quant/scale and encoder state provenance.

Three input frames require about 25 MB uncompressed, plus compressed samples.
The supplied ZIP is also acceptable outside Git: original archive SHA-256
`fdc9cd0e881db417e23423c8cd3f9f1e27161f2c946e40edfa2bcb8b56d58dcf`.
No ZIP/BGRA/CFHD files are tracked in the Windows reference branch; its README
explicitly identifies them as local artifacts. The 120-frame binaries also stay
outside Git. With an extracted root, add `--corpus-root /path/to/corpus` to the
validator. Existing files must match original artifact lengths/SHA-256; absent
files remain BLOCKED, corrupt files fail. File verification alone does not
change the Mac production gate.

This draft performs **no metadata masking**. Later full-sample comparison must
parse and validate only GUID/DATE/TIME/TIMECODE value ranges before normalizing
copies. Never trust arbitrary offsets or mask size/codec tags. Compare original
files separately when metadata is identical. Do not claim a normalized sample
was the untouched original.

## Remaining implementation and release gates

1. Obtain and verify the minimal actual Windows files and intermediate dumps.
   Compare BGRA → conversion → horizontal/vertical stages → levels → quantized
   coefficients to locate the earliest differing signed value. Resolve the
   conversion/transform gap based on that evidence.
2. Add a **separate** SDK-compatible FS3 profile with exported fixed quant/scale
   replay first, then the full quantization and FSratelimiter / previous GOP
   byte-count state machine. Include state trajectory tests; leave FS3-Q2 bytes
   unchanged. Replay of exported tables is not dynamic rate-control testing.
3. Port/verify entropy, scale/size tags, channel/frame framing and side metadata.
   Require 30/30 actual coefficient bands, 30/30 payloads and full normalized
   sample identity for at least two genuine 1080p samples; then 16 frames and
   the complete 120-frame stateful sequence in its original order.
4. Add checked Windows intermediate dump comparisons and metadata parsing tests.
   CPU mathematics must be usable independently from entropy/framing by future
   Metal kernels. No Metal implementation is part of this branch.

128×128 SDK/CUDA output remains an unreliable oracle under issues #20/#21:
possible SIMD out-of-bounds reads at chroma width 16 must never be reproduced.
The 128/Q2 hashes and the 256/1080 Q2 decoder tests protect the existing Mac
encoder separately. Pixel-quality/FFmpeg decode PASS does not imply encoder
bitstream parity. Metal #7 can research CPU-vs-Metal FS3-Q2 parity, but SDK/Windows
identity and production release remain gated on this issue. No optimization or
acceleration milestone is claimed, and no Windows/CUDA/upstream files are edited.

## Validation recorded for this draft

Release CTest: **16/16 PASS**. Debug ASan+UBSan CTest: **16/16 PASS**.
Both runs include all existing native FFmpeg decode checks. The additional input
CTest regenerates all 136 corpus inputs, and the guardrail CTest contains ten
unit cases. Independently compared all **27/27** Q2 sample packet SHA-256 values
against `macos_variable_size_2026-10-15.json`; all remain unchanged, including
128×128, 256×256 and 1920×1080. See the separate machine-readable
`validation/macos_windows_checks.json` for regression and sanitizer evidence.
These PASS results cover the local tests and Q2 regressions only; they do not
change the FAIL/PENDING/BLOCKED encoder-parity gates above.
