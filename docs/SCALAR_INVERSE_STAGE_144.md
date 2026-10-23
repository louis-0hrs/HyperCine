<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# First scalar inverse stage: source and oracle contract

**Architecture proposal requiring #16/#47 owner review, 2026-10-20.**
This resolves the concrete stage/base/files/math choices requested in
[#144](https://github.com/louis-0hrs/HyperCine/issues/144). It does not grant
Milestone 2 implementation approval. The existing
[synthetic graph](DECODE_TASK_GRAPH_147.md) remains unchanged.

Propose one private coefficient-to-lowpass arithmetic unit: original FIELDPLUS
**wavelet 5 bands 0/1/2/3 -> wavelet 4 band 0**, before temporal reconstruction.
Use the already archived original entropy-completed input bands and resident
output, rather than trying to infer a single spatial stage from final pixels.
This is explicitly a **GOP2-sourced unit fixture**. The earlier proposed first
GOP1 profile is not qualified by it. Owner acceptance must explicitly permit
this narrower arithmetic experiment; P7.B still needs its own GOP1 oracle and
#13/#16 acceptance. No bitstream, temporal, color or public decoder route is
proposed in this slice.

## Exact source, base and ownership

Proposed implementation base: current main
`bc37b5c388ebe561851becf6ebc417564f1c909b`.
Reference metadata: Draft [#190](https://github.com/louis-0hrs/HyperCine/pull/190)
`77acadbd497a490c034440709390807087147d64`, including integrated #191,
#196/#199 and #200's separately reconciled Windows evidence. No reference files
need regeneration. #190 remains a separate Draft awaiting owner acceptance.

[The machine-readable selection](validation/SCALAR_INVERSE_STAGE_PROPOSAL_144.json)
pins four reference-index Git blobs, nine source fingerprints and all 30
original input/output artifact bindings. Fingerprints use Git-blob bytes;
additional LF-normalized source hashes distinguish checkout line endings.
The frozen SDK tree is `837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`, baseline
`11574d0295771edccadd17af14af74a539f924c7`.

After separate approval, propose these new owned files only:

| Proposed file | Responsibility |
| --- | --- |
| `src/cpu/decode_spatial_scalar.{h,cpp}` | Private C++17 typed planes and this single arithmetic operation. No SDK headers. |
| `tests/test_decode_spatial_scalar.cpp` | Deterministic borders, signed extremes, allocation/storage rejection and producer/publication tests. |
| `tests/validate_decode_spatial_scalar.py` | Optional read-only replay of the exact private original artifacts; missing bytes fail the requested gate. |
| Root `CMakeLists.txt`, `ci/run_standalone.py` | Separately reviewed SDK-free target, mandatory synthetic cases and conditional real-fixture inventory. |

Reuse `src/core/decode_task_graph.{h,cpp}` without changing scheduler behavior.
Keep graph tests explicitly infrastructure-only: current `GraphProfile` accepts
only `Synthetic`. A test harness may schedule supplied coefficients as synthetic
work; that does not create a supported real-codec profile. A product profile
requires another reviewed graph/decoder integration. Existing phase-stack,
CUDA, Metal, encoder and P7 audit owner branches are outside this change surface.

## Selected original pair, not an entire GOP2 decoder

For each geometry/channel, four Phase-B files `pre_chC_band0.i16le` through
`pre_chC_band3.i16le` belong to wavelet 5, level 4, prescale 0. Their capture is
`DecodeSampleSubband_after_entropy_before_inverse_submission`. Match them to
Phase-A `stages/chC_wavelet4_ll.i16le`, captured after original reconstruction.
All five artifacts bind to the same original packet. The output stage is
wavelet 4, band 0, level 3. Source inspection of `ReconstructWaveletBand` selects
`lowpass_index = index - 1` for index 5 and publishes this exact buffer.
Later spatial reconstruction reads this LL; it is not the final signed plane.

| Packet | SDK component | Four input planes | One expected output plane |
| --- | --- | --- | --- |
| 1920 x 1080 | ch0 Y | 240 x 135 | 480 x 270 |
| 1920 x 1080 | ch1 V / ch2 U | 120 x 135 each | 240 x 270 each |
| 3840 x 2160 | ch0 Y | 480 x 270 | 960 x 540 |
| 3840 x 2160 | ch1 V / ch2 U | 240 x 270 each | 480 x 540 each |

Preserve SDK channel identity **Y,V,U**. Each file contains compact, top-down
signed int16 little-endian rows: byte length = 2 x width x height. Do not hash
aligned padding. Decode little-endian bytes explicitly into native signed
values, then copy into independently owned aligned planes. Inputs are already
entropy-completed/dequantized: `_DEQUANTIZE_IN_FSM=1` in `codec.h` makes the
inverse read them directly. Preserve recorded quantization as provenance; never
multiply it a second time. Likewise the output descriptor's prescale 2 belongs
to the next stage; do not apply it to this scale-0 inverse.

The pair is a source-derived **eligibility proposal**, not a new execution of
the original SDK or a scalar equality result. Original one-CPU stability,
observer neutrality and private readback are Windows owner evidence at their
recorded SHAs. This run inspected public metadata/source only. Its configured
`HYPERCINE_GOLDEN_DIR` was unreachable; no private-file hash, value range or
numeric replay was observed. Reuse existing loose artifacts once reachable;
no export/copy/archive is needed to implement a read-only replay.

## Arithmetic contract that a portable implementation must preserve

The actual call chain at index 5 is `ReconstructWaveletBand` ->
`TransformInverseSpatialQuantLowpass(scale=0, inverse_prescale=true)` ->
**`InvertSpatialQuantOverflowProtected16s`**, because input level is at least 4.
It is not the lower-level `InvertSpatialQuant16s` or the final signed-V210 path.
The private observer changes `decoder.c` and `wavelet.c` call sites only; the
spatial/horizontal units and these operators are frozen original code.

Define `F(v,k) = floor(v / 2^k)`, `S(v) = clamp(v,-32768,32767)` and
`W(v) = ((v + 32768) mod 65536) - 32768`, with nonnegative modulo.
The future C++17 implementation must implement F and W explicitly using wide
arithmetic. Signed negative right shift and out-of-range signed narrowing are
implementation-defined in C++17; do not rely on them. All convolution sums
fit int32: absolute weighted sum at a border is at most 16 x 32768, before
adding 4; no signed overflow or negative left shift is necessary.

Vertical synthesis first processes the pair (band0,band2), then (band1,band3),
with the first member providing L and the second H. For index i along height n:

| Position | E before correction | O before correction |
| --- | --- | --- |
| i = 0 | F(11 L0 - 4 L1 + L2 + 4, 3) | F(5 L0 + 4 L1 - L2 + 4, 3) |
| 0 < i < n-1 | Li + F(L[i-1] - L[i+1] + 4, 3) | Li + F(-L[i-1] + L[i+1] + 4, 3) |
| i = n-1 | F(5 Li + 4 L[i-1] - L[i-2] + 4, 3) | F(11 Li - 4 L[i-1] + L[i-2] + 4, 3) |

Store vertical even = S(F(E + Hi,1)) and odd = S(F(O - Hi,1)).
There is no intermediate 16-bit wrap in this overflow-protected vertical loop;
the disabled MMX block is not the selected path. Rounding is always +4,
including negative sums. At the final row, the table applies directly only to
(band1,band3): its three-row LH ring reaches n-1. The original scalar loop
leaves the LL pointer at n-2, so for (band0,band2) substitute L[n-2], L[n-3],
L[n-4] in the final-row filter while retaining H[n-1]. See spatial.c:21766-21883.
This source-pointer asymmetry corrects the original symmetric proposal after
actual original replay exposed its 1080p U lower-pair mismatch. Require n >= 4
to avoid a negative LL row; the selected heights are 135/270.
The resulting even/odd lowpass and highpass rows feed `InvertHorizontalStrip16s`.

**Horizontal arithmetic is position-dependent in the pinned x86 original.**
`config.h` defaults select `_PROCESSOR_PENTIUM_4=1`, `_PROCESSOR_GENERIC=0`,
`_XMMOPT=1`; the horizontal unit forces `_UNALIGNED=0` and defaults `_FASTLOOP=1`.
This is the source model to verify against archived build provenance before
implementation signoff. Do not infer a universal wide-accumulator scalar filter.
For input row width n (120/240/480), post = 8 x floor((n-1)/8) = n-8:

- x=0: use the left-border E/O formulas above along width; after F(E+H,1)
  and F(O-H,1), SSE insertion retains the low 16 bits, i.e. **W**, not S.
- x=1..post: emulate each signed saturating SSE add/subtract. Compute
  `e = F(S(S(L[x-1]-L[x+1])+4),3)`, then
  `even = F(S(S(e+L[x])+H[x]),1)`.
  Compute `o = F(S(S(L[x+1]-L[x-1])+4),3)`, then
  `odd = F(S(S(o+L[x])-H[x]),1)`.
- x=post+1..n-2: the source uses `short` compound assignments. Apply **W after
  each assignment**, including the subtraction and +4, then F by 3, W when
  adding the center, W when adding/subtracting H, and finally F by 1.
- x=n-1: use the right-border wide E/O formulas and **S** after the final
  divide by 2. Do not replace asymmetric border filters with replicated edges.

Emit each horizontal pair in even/odd order; vertical pairs produce rows 2y,
2y+1. Saturation/wrap is part of compatibility at these boundaries. Retain
signed output; do not clip to p10, offset chroma or pack YU64. The original
16-bit tail narrowing is an x86/MSVC behavior to model explicitly, not a reason
to make a new ARM implementation depend on implementation-defined casts.

Independent integer counterexamples, also recorded in the JSON (Lprev,L,Lnext,H):

| Input | Wide interior with final S | SSE interior | Short tail |
| --- | --- | --- | --- |
| -32768, 0, 32767, 0 | -4096,4096 | -2048,2047 | 0,0 |
| 0,32767,0,32767 | 32767,0 | 16383,0 | -1,0 |

For left-border L0=32767,L1=-32768,L2=32767,H0=32767, the wide pair before
storage is 49151,-16384; first SSE insertion yields -16385,-16384.
These are arithmetic thought experiments computed with explicit integer
operators, not original-SDK runtime observations or evidence that the pinned
real planes reach these extrema. Before choosing a cleaner numerical model,
obtain a separate owner decision and qualify its differences; do not silently
call it bit-exact across signed16 input.

## Typed storage, publication and failure boundary

Propose a private move-only RAII `OwnedSignedPlane` with 32-byte base alignment,
stride `round_up(2*width,32)`, checked allocation extent and zero padding.
`SpatialStageInputs` has four const plane views in original band order, explicit
stage kind, geometry and generation/producer identity. Output is a separate
exclusive plane with doubled dimensions. Reject wrong counts, aliases, precision,
channel/stage mapping, unsupported sizes, short extents, bad pitches, arithmetic
overflow and generation mismatches before any output write. No public ABI is
introduced by these names; header details remain for implementation review.

Preallocate input/output and four intermediate rows per active callback using
RAII, outside scheduler locks. Readonly input lifetime and exclusive output
ownership extend through graph drain. Test producers fill each owned band;
the inverse callback depends on all four and polls cancellation between bounded
row pairs. Publish only after the entire output succeeds. Consumers check the
matching completed generation, not a started flag. A canceled/throwing callback
must leave output unpublished and must not free any storage until all workers
join. Existing graph cancellation is cooperative, with no hard thread kill;
use a bounded CTest process watchdog for a deliberately stuck test. Missing real
fixtures are a failing requested oracle gate, never an absent successful test.

## Decision and acceptance requested

The concrete review choice is: approve this GOP2-sourced **spatial arithmetic
unit only**, with the pinned base/files/pair and position-specific compatibility
rules, or retain the original GOP1-first policy and require a separately
qualified GOP1 pair. This document proposes the former to use existing evidence;
it does not assert the choice has been approved. Reference-owner signoff on
pair provenance/build selection and #16/#47 stage ownership remain required.
No production implementation, decoder profile or phase merge follows from silence.

After approval, implement a direct scalar call and qualify six real pair cases
(three channels, two geometries) with exact output bytes, plus tiny deterministic
signed/border/saturation/tail vectors derived independently of that code.
Required test failures include every missing input, held producer, allocation,
bad extent/alignment, wrong stage/GOP, cancel, cross-generation reuse and corrupt
fixture. Exercise the existing graph with 1/2/4/16 workers; drain before reuse.
Run native ARM64 full CTest and scoped ASan/UBSan, then Windows/Linux portability
at the same source head. Initial arithmetic equality is not packet decoding,
PSNR, GOP1/GOP2 product support, or acceleration; performance remains unmeasured.

## Validation of this proposal

This document, its JSON selection and the required upstream-quirk note in
`docs/UPSTREAM.md` are changed. Native macOS 26.5.2
ARM64, AppleClang 21, CMake 3.31.5, Python 3.9.6 with optimization and external
FFmpeg n8.0: SDK-free Release full CTest **32/32 PASS**, zero skips, **73.87 s**.
Full pre-follow-up-commit CTest also passed **32/32**, zero skips, **71.07 s**.
This is unchanged-main infrastructure/encoder coverage, not scalar decoder
execution. The 34-test reference branch inventory belongs to #190 and is not
silently attributed to this main-based documentation branch.

Public Git-blob audit matched all six pairs/30 artifact bindings, same packet,
2x dimensions, signed row lengths, input wavelet/level/prescale, four index hashes
and nine source fingerprints. No private corpus, SDK/GPU, sanitizer or benchmark
was executed. New architecture is REVIEW_REQUIRED; parent #144/#47 stay open.

Reproduce the unchanged full suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
PYTHONOPTIMIZE=1 ctest --test-dir build -C Release --output-on-failure
```

Reproduce the public selection audit from this branch, with the pinned reference
commit available locally (`git fetch origin codex/issue189-sdk-goldens` if needed).
This reads Git objects only; successful metadata binding is not a raw-byte pass:

```sh
python3 - <<'PY'
import hashlib, json, subprocess
from pathlib import Path
p = json.loads(Path('docs/validation/SCALAR_INVERSE_STAGE_PROPOSAL_144.json').read_text())
def blob(rev, path):
    return subprocess.check_output(['git', 'show', rev + ':' + path])
def require(ok):
    if not ok:
        raise SystemExit('Proposal binding mismatch')
indices = {}
for entry in p['reference_indices'] + p['source_fingerprints']:
    rev = p['reference_sha'] if entry in p['reference_indices'] else p['base_sha']
    data = blob(rev, entry['id'])
    require(len(data) == entry['byte_length'])
    require(hashlib.sha256(data).hexdigest() == entry['sha256'])
    if 'sha256_lf' in entry:
        require(hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest() == entry['sha256_lf'])
    else:
        indices[entry['id']] = json.loads(data)
for case in p['cases']:
    for item in case['inputs'] + [case['expected']]:
        index = indices[item['index_id']]
        inventory = index.get('original_stages', index.get('artifacts'))
        original = next(x for x in inventory if x['id'] == item['id'])
        require(all(original[k] == v for k, v in item.items() if k != 'index_id'))
        require(item['byte_length'] == item['stage']['pitch'] * item['stage']['height'])
        packet = index.get('input_packet')
        if packet is None:
            a = next(x for x in inventory if x['id'] == 'original_sdk.cfhd')
            packet = {k: a[k] for k in ('sha256', 'byte_length')}
        require(packet == case['packet'])
    source, target = case['inputs'][0]['stage'], case['expected']['stage']
    require(all(target[k] == 2 * source[k] for k in ('width', 'height')))
require(len(p['cases']) == 6)
print('Six public pairs / 30 artifact bindings match; private replay NOT RUN.')
PY
```
