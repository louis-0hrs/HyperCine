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
| Legacy Q2 diagnostic replay, frames 0/3/6/13 | FAIL | 0/30 coefficient FNV and 0/30 correctly framed payload SHA matches per frame |
| Frozen SDK arithmetic replay | PASS, 16/16 | 480/480 coefficient hashes and entropy blocks; reference band headers reused only for diagnostic wrapping |
| Windows Tier A binary files | PASS | 280/280 actual files verified from the NAS archive |
| Windows SDK/CUDA Tier A normalized samples | PASS, 16/16 | Actual bytes equal after parsed whitelist normalization; 480/480 framed band pairs equal |
| Windows 120-frame binary files | BLOCKED | Not included in the supplied Tier A archive |
| First differing LL signed coordinate | FAIL | Frame 0: Y L3 LL (0,0), expected 1888, actual 13936; first entropy byte at sample offset 788 |
| Earliest causal stage | PENDING | Windows conversion/filter dumps still required |
| SDK-compatible FS3 profile / dynamic state | PENDING | Exported quant/scale/state retained, no state evolution port |
| Mac vs Windows normalized full sample | BLOCKED | SDK-compatible encoder and actual binaries required |
| 4K/8K production parity | PENDING | No supported oracle/data or geometry release |

The machine-readable results are in
[`validation/macos_windows_bitexact.json`](validation/macos_windows_bitexact.json).
The first *observed* mismatch for all four probed frames is channel 0, subband 0
(Y L3 LL, 240×135, quant=1). For frame 0 the Mac coefficient FNV is
`e3e8b01a9160e0d3`. Expected hashes are recorded alongside actual hashes in the
JSON. The NAS-backed follow-up now also compares the actual big-endian LL
coefficients against the Mac signed dumps; the first differing coordinate and
entropy byte offsets are recorded for every probed band. This still does not
establish the earliest causal stage. No PSNR threshold is used here.

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
`first_i16_difference` reports exact signed x/y/value differences for the three
LL bands extracted from actual samples. HP coefficients cannot be recovered
bit-for-bit from lossy companded entropy; Windows coefficient dumps are still
required for exact HP coordinates. Raw and reframed entropy differences are
reported separately.

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

`tests/reference_sample.py` now parses size-bounded sample/level/subband chunks,
raw channel indices and typed metadata records. It normalizes copies of only
parsed GUID/DATE/TIME/TIMECODE values, checks the exact mask ranges against the
original manifest, and validates normalized sample SHA-256. Arbitrary mask
ranges, unexpected types/sizes, truncated records/chunks, malformed band
trailers and excessive nesting are rejected. Untouched original SHA-256 values
are verified first. All 16 actual Windows SDK/CUDA normalized samples and all
480 framed band pairs are byte-identical. This is Windows reference integrity,
not a Mac encoder parity result.

## NAS acquisition and corrected payload scope (2026-10-15)

The provided data was found at
`smb://TRUENAS/DeDub/HyperCine/golden/windows/fs3/2026-10-14/tier-a`.
The shell's directory access stalled; Finder successfully copied the ZIP and
two manifest files to `/private/tmp/hypercine-nas-tier-a`. No NAS files were
modified. The archive has the exact expected SHA-256 and extracts 280 files,
276,444,160 bytes. All 280 lengths/SHA-256 values match the pinned Git manifest.
The transferred manifest is one byte longer (CRLF versus LF at EOF); bytes are
otherwise identical and parsed JSON is identical. Both file hashes are recorded in `validation/macos_windows_nas.json`;
the versioned reference manifest is unchanged.

**Correction to the initial draft:** Windows `*_payload_sha256` hashes complete
framed band blocks produced by `BuildLowpassPayloadPtr` /
`BuildHighpassPayloadPtr`, including headers and trailers. The initial Mac
probe hashed only entropy bytes, so its payload-hash comparison was not a valid
like-for-like test. Coefficient-FNV comparisons were independent and remain
valid. The updated diagnostic extracts actual framed blocks, validates all 960
individual SDK/CUDA band hashes, and compares Mac entropy both directly and
inside an explicitly marked diagnostic wrapper made from the reference band
header/trailer with a recomputed subband length. This wrapper is **not** a Mac
encoder-produced SDK-compatible sample or a production framing implementation.
Without actual reference headers payload comparison stays PENDING.

For frames 0/3/6/13, the first LL coordinate is (0,0) in Y L3:

| Frame | SDK signed coefficient | Mac signed coefficient | SDK sample entropy byte offset |
|---|---:|---:|---:|
| 0 FlatBlack | 1888 | 13936 | 788 |
| 3 HorizontalGradient | 4832 | 13936 | 788 |
| 6 DeterministicNoise | 7744 | 13936 | 788 |
| 13 HardEdges | 6096 | 13936 | 788 |

Source inspection identifies a concrete frontend difference to isolate next:
Windows `BgraToYuvKernel` reads row `height-1-row`, uses SIMD-style per-product
multiply-high rounding and picks chroma from the even pixel; the current Q2
frontend reads row `row`, rounds after summation and averages chroma across the
pair. The isolated SDK replay below implements those documented arithmetic differences;
the legacy Q2 frontend/quantization remains unchanged. Windows converted/filter
dumps are still needed to measure each earlier intermediate stage directly.

Optional binary-backed CTest registration:

```sh
cmake -S . -B build \
  -DHYPERCINE_REFERENCE_CORPUS_ROOT=/private/tmp/hypercine-nas-tier-a/extracted
ctest --test-dir build -C Release --output-on-failure
python3 tests/validate_windows_reference.py \
  --probe build/hypercine_reference_probe \
  --corpus-root /private/tmp/hypercine-nas-tier-a/extracted \
  --stage-dir /private/tmp/hypercine-nas-stages \
  --output docs/validation/macos_windows_bitexact.json
```

The Tier A file blocker is resolved. The 120-frame binary corpus, Windows
conversion/filter/HP-coefficient dumps, SDK-compatible profile/state evolution,
and genuine Mac full-sample parity remain open.

## Frozen-table SDK arithmetic replay

`hypercine_reference_probe diagnose-sdk-replay` is a separate diagnostic mode.
It implements the documented CG709 multiply-high, signed floor, saturating and
16-bit wrapping operations, bottom-up source row traversal, even-pixel chroma
selection, and default pre-emphasis midpoint (`q/2-1`) before reciprocal
quantization. Wavelet passes still use the existing safe scalar functions and
prescale [0,2,0]. No SDK headers, SDK build or SDK runtime are required. The
implementation uses explicit C++17 floors, not undefined negative shifts or
out-of-bounds SIMD accesses.

Using each frame's exported divisors, **all 16 frames match all 480 coefficient
FNV hashes**. All 480 Mac-produced entropy byte blocks also equal the actual
Windows bytes, and all 480 complete band SHA-256 values match after the explicit
reference-header diagnostic wrapping. All 48 actual LL coefficient planes
match signed value for signed value; exact HP coefficient bytes still require
Windows coefficient dumps, although every exported HP FNV matches. Results are
in `validation/macos_windows_sdk_replay.json`.

```sh
python3 tests/validate_windows_reference.py \
  --probe build/hypercine_reference_probe \
  --corpus-root /private/tmp/hypercine-nas-tier-a/extracted \
  --sdk-replay --frames 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 \
  --output docs/validation/macos_windows_sdk_replay.json
```

This mode lives in the diagnostic tool, not the public encoder. It does not
compute dynamic quant tables, evolve FSratelimiter/previous-GOP state, consume
scale[36] as an arbitrary transform profile, or create channel/frame/metadata
headers. Reusing a validated reference header to isolate entropy behavior is
not an independent SDK-compatible encoder or a full sample hash match. The
command therefore still exits 2 and the production gate remains BLOCKED.
Default `diagnose` continues to measure the original Q2 frontend independently.

The SDK arithmetic CTest covers flat, gradient, noise and hard-edge vectors,
checks the input SHA first, then all 30 coefficient hashes. With the optional
corpus root it additionally checks actual LL signed values and entropy bytes
and all 30 reframed payload hashes for each vector. Separate parser tests cover
bounds, raw index entries, immutable originals, whitelist normalization, corrupt
trailers/metadata, and explicit diagnostic reframing.

## Remaining implementation and release gates

1. Tier A original files are now verified. Obtain Windows intermediate dumps
   and the separate 120-frame binary sequence.
   Compare BGRA → conversion → horizontal/vertical stages → levels → quantized
   coefficients to locate the earliest differing signed value. Resolve the
   conversion/transform gap based on that evidence.
2. Promote the isolated frozen arithmetic replay into a **separate** tested
   core FS3 profile with a public stage interface, then add full quantization and FSratelimiter / previous GOP
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

## Validation recorded for the NAS follow-up

Release CTest: **19/19 PASS**. Debug ASan+UBSan CTest: **19/19 PASS**.
Both include all existing native FFmpeg decode checks, deterministic SDK replay
vectors, and the optional actual-corpus parser/byte comparisons. The input CTest
regenerates all 136 corpus inputs. Independently compared all **27/27** Q2 sample
packet SHA-256 values against `macos_variable_size_2026-10-15.json`; all remain
unchanged, including 128×128, 256×256 and 1920×1080.

Current checks and exact log/result hashes are recorded in
`validation/macos_windows_nas.json`. The earlier
`validation/macos_windows_checks.json` remains the historical initial-draft
record. Local tests and frozen SDK replay PASS do not release the dynamic
encoder/full sample/Metal production gates.
