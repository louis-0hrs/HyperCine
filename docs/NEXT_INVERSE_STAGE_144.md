<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Next inverse unit: wavelet 4 to temporal lowpass

**Design for independent review, 2026-10-21; implementation not authorized by
this document.** Issue [#144](https://github.com/louis-0hrs/HyperCine/issues/144).
Base: main `1db7c1ebf82f3dca0a058b2a765682bbd25fcedf`, after #201/#203 and
post-merge native/hosted qualification. The completed wavelet5-to-wavelet4
research unit stays unchanged. This proposal selects its next spatial consumer,
not the temporal inverse itself or a full packet decoder.

## Selection and existing references

Select original FIELDPLUS **wavelet4 bands0/1/2/3 -> wavelet2 band0**, the
level-2 temporal lowpass. For each of two packets and three SDK channels, use:

- LL input: Phase-A `stages/chC_wavelet4_ll.i16le`, the prior stage's original
  output, captured as a resident original-SDK buffer after full inverse.
- LH/HL/HH inputs: Phase-B `pre_chC_band4.i16le`, `pre_chC_band5.i16le`,
  `pre_chC_band6.i16le`. Logical bands4/5/6 map to wavelet4 physical bands1/2/3;
  capture is after entropy completion and before inverse submission.
- Expected output: Phase-A `stages/chC_temporal_low.i16le`, wavelet2 band0.

[The selection manifest](validation/NEXT_INVERSE_STAGE_CONTRACT_144.json) pins
four reference-index blobs, seven frozen source blobs and six cases with **30
artifact bindings**. Every case binds to one original packet hash and length.
The indices already integrated by #190 are sufficient for the design: no new
export, encoder call, original-file move or expected-output regeneration.

| Packet | SDK channels | Input per band | Expected temporal LL |
| --- | --- | --- | --- |
| 1920 x 1080 | ch0 Y | 480 x 270 | 960 x 540 |
| 1920 x 1080 | ch1 V, ch2 U | 240 x 270 | 480 x 540 |
| 3840 x 2160 | ch0 Y | 960 x 540 | 1920 x 1080 |
| 3840 x 2160 | ch1 V, ch2 U | 480 x 540 | 960 x 1080 |

All planes are compact top-down signed16 little-endian in SDK **Y,V,U** order.
Input stage level3/prescale2 and output level2/prescale0 are different stage
identities. Inputs are already dequantized (`_DEQUANTIZE_IN_FSM=1`); their
recorded quantization is provenance, not a multiplier to apply again. Both
frames share this temporal-lowpass input; do not duplicate the six cases into
12 supposedly independent frame cases. No p10 clipping, bias change, chroma
swap, YU64 packing or final pixel comparison belongs to this unit.

The Phase-A output is a resident-buffer observation, not a new pre-temporal
capture. Source review must confirm its lifetime and absence of later mutation;
public metadata alone cannot prove this. Before implementation acceptance the
reference owner must confirm that the indexed buffers represent the selected
primitive, with the archived compiler/ISA path. Prior wavelet5 primitive
qualification does not automatically qualify this different primitive.

## Active source path and arithmetic differences

Frozen SDK baseline `11574d0295771edccadd17af14af74a539f924c7`, upstream tree
`837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`; links below refer to that unchanged
source in this branch.

1. [decoder.c:13060](../upstream/Codec/decoder.c#L13060): for FIELDPLUS index4,
   `lowpass_index = index - 2`, so destination is wavelet2, band0. Width/height
   double, level decreases by one. `inverse_prescale` is true at precision 10.
2. [wavelet.c:5750](../upstream/Codec/wavelet.c#L5750): active scale > 1 branch
   calls `InvertSpatialQuantDescale16s` with `descale=2`. It does not select
   `InvertSpatialQuantOverflowProtected16s` or `InvertSpatialQuant1x16s`.
3. [spatial.c:22414](../upstream/Codec/spatial.c#L22414): vertical synthesis
   produces signed16 saturated intermediate low/high rows. The active
   `XMMOPT` interior uses **32-bit SSE2 arithmetic** and signed pack saturation
   (`#if 1`, lines22648–22825), rather than the inactive signed16 alternative.
4. [InvertHorizontalStrip16s.c:1700](../upstream/Codec/InvertHorizontalStrip16s.c#L1700):
   the Pentium-4 `InvertHorizontalStripDescale16s` branch reconstructs and
   restores scaling. Its operations differ from the scale0 horizontal primitive.

Define F(v,k)=floor(v/2^k), S(v)=clamp(v,-32768,32767), and W(v)=signed low16
bits. Implement these explicitly with wide arithmetic in any future C++17
unit; never copy original negative signed shifts or out-of-range short casts.
The source uses rounding +4 for either sign (`filter.h:96–98,173`).

**Vertical:** use wide border convolutions (11,-4,1)/(5,4,-1), then +4, F by3,
highpass correction and F by1, followed by S. Interior lowpass pair at row i:
`S(F(L[i]+F(L[i-1]-L[i+1]+4,3)+H[i],1))` and
`S(F(L[i]+F(-L[i-1]+L[i+1]+4,3)-H[i],1))`.
Apply separately to (band0,band2) and (band1,band3).

Crucially, the active `XMMOPT` path advances `lowlow` once more before the
bottom border (`spatial.c:23051–23055`). Its LL bottom anchor is **h-1**, with
neighbors h-2,h-3; LH also uses h-1,h-2,h-3. Do not carry forward the completed
scale0 unit's LL h-2 endpoint quirk. A build without `XMMOPT` follows another
pointer path and requires separate qualification. Initial research geometry
should require width multiple 8, height>=4 and only the maxima in the table;
smaller generic profiles remain outside this first original-fixture claim.

**Horizontal, pinned Pentium-4 aligned fast path:** at width n (multiple8),
`post=8*floor((n-1)/8)=n-8`. For x=1..post, model saturated signed16 subtraction,
+4, F by3, saturated center addition and saturated highpass addition/subtraction.
There is **no horizontal divide by2**. Restore scale with S(2*v). The stored
remainder at x=post also follows this saturated doubling.

At x0 the wide border filter plus/minus H is inserted as low16 bits before
saturated doubling: S(2*W(v)); it must not become S(2*v). The scalar tail
x=post+1..n-2 uses **wide int accumulators**, not the old unit's short compound
assignment wrap; after filter and highpass correction store S(2*v). The right
border likewise uses wide border arithmetic then S(2*v). The active vector
stores unconditionally double using `_mm_adds_epi16(out,out)`; that is suitable
for this descale2 selection and must not be generalized to descale0 by guessing.

These are source-derived rules awaiting independent review, not newly measured
original arithmetic. An implementation review must inspect actual lane order,
first/last SSE blocks and archived branch selection. Useful independent
counterexamples for future vectors: horizontal tail Lprev=0,Lcenter=32767,
Lnext=0,H=32767 yields +32767 after wide correction and saturated doubling;
reusing the scale0 short-tail wrap would yield a different sign. Nonconstant
LL/LH bottom-row ramps must distinguish h-1 from h-2.

## Ownership, storage and acceptance contract

After review and explicit research-scope approval, propose a separate private
`src/cpu/decode_spatial_descale.{h,cpp}` unit and corresponding standalone test
file/replay helper. Reuse the existing plane-view descriptors and graph infrastructure; do not
widen `InverseWavelet5To4` or its size limits. Existing `OwnedSignedPlane`
rejects dimensions over 960, so it cannot own the 4K case's 1920x1080 output.
Propose a stage-local move-only RAII owner using the same C++17 aligned
allocation/deletion pattern with checked dimensions up to 1920x1080 and a
4MiB extent cap. The maximum compact output is 4,147,200 bytes with
32-byte-aligned rows. Keep the existing owner's 960 limit and the old
function's 2MiB view guard intact; review the new owner separately.
Account separately for four inputs, output and four aligned intermediate rows
per concurrent callback; all allocation arithmetic is checked before writes.
No SDK headers or third-party dependency enters the core.

Validate four distinct aligned input allocations in physical band order,
channel/wavelet/band/level/prescale identity, matching generation, extents and
exclusive nonaliasing output. Reject every invalid request before output writes.
The owning harness retains all allocations through graph drain. It requires
all four producer publications before the inverse callback; completion publishes
only a fully written matching-generation output. Cancellation or exception can
leave partial unpublished bytes. Do not introduce a second scheduler, public
API or real GraphProfile; current graph tests remain Synthetic infrastructure.

Acceptance sequence for a separately approved implementation:

1. Independently review this contract and confirm archived original Descale
   compiler/ISA path plus selected resident-output lifetime. Resolve disagreement
   before implementation; do not treat silence as acceptance.
2. Create independent synthetic vectors for borders, wide vertical vs saturated
   horizontal operations, scale restoration, x0 wrap, wide tail, negative
   rounding, saturation and both bottom anchors. Include padded strides and
   allocation/alias/extent/identity rejection with unchanged-output guards.
3. Authenticate the **new selected 30 artifacts** and sidecars read-only through
   `HYPERCINE_GOLDEN_DIR`, then execute six exact signed output comparisons.
   Missing or changed bytes fail a requested replay; no fabricated equality or
   SKIP-to-PASS. Recheck originals afterwards. Do not repeat the old stage's
   six-pair sweep or regenerate references.
4. Exercise existing synthetic graph callbacks at workers 1/2/4/16, producer
   failure, held producer, cancellation and stale generation. First test the
   new unit with original LL input; only afterwards compare composition with
   the prior unit, retaining an intermediate wavelet4 equality check.
5. Full native CTest plus scoped ASan/UBSan and same-head Windows/Linux checks.
   These qualify one arithmetic unit; GOP1, temporal inversion, pixels, full
   decoder safety, public profile and #13/#16 production gates remain separate.

## Evidence for this design change

This proposal changes only this document and its selection manifest. Source,
reference indices, corpus, scheduler, public API and prior arithmetic remain
unchanged. The metadata audit checks all 30 bindings against pinned index blobs,
same packet, logical/physical identity, scale, compact lengths and doubled
output dimensions. **Private bytes, SDK execution and numeric replay NOT_RUN**.
The post-main 42/42 result recorded in #144 predates this documentation change;
new pre-commit full-suite evidence is recorded in the PR handoff. No performance
claim or acceleration milestone is made.

Reproduce the public metadata/source audit from this checkout (standard-library
Python only; the pinned base Git object must be available):

```sh
python3 - <<'PY_AUDIT'
import hashlib,json,subprocess
from pathlib import Path
p=json.loads(Path('docs/validation/NEXT_INVERSE_STAGE_CONTRACT_144.json').read_text())
def require(ok,label):
 if not ok: raise SystemExit('Binding mismatch: '+label)
def blob(path):return subprocess.check_output(['git','show',p['base_sha']+':'+path])
indices={}
for entry in p['reference_indices']+p['source_fingerprints']:
 data=blob(entry['id'])
 require(len(data)==entry['byte_length'],'blob length')
 require(hashlib.sha256(data).hexdigest()==entry['sha256'],'blob hash')
 if entry in p['reference_indices']:indices[entry['id']]=json.loads(data)
require(len(p['cases'])==6,'case count');require(not p['implementation_authorized'],'design gate')
for case in p['cases']:
 for item in case['inputs']+[case['expected']]:
  idx=indices[item['index_id']];inv=idx.get('original_stages',idx.get('artifacts'))
  orig=next(x for x in inv if x['id']==item['id'])
  require(all(orig[k]==v for k,v in item.items() if k!='index_id'),'artifact identity')
  require(item['byte_length']==2*item['stage']['width']*item['stage']['height'],'compact size')
  packet=idx.get('input_packet')
  if packet is None:
   a=next(x for x in inv if x['id']=='original_sdk.cfhd');packet={k:a[k] for k in ('sha256','byte_length')}
  require(packet==case['packet'],'packet identity')
 bands=case['inputs'];out=case['expected']['stage'];c=bands[0]['stage']['channel']
 for i,item in enumerate(bands):
  s=item['stage']
  require((s['channel'],s['wavelet'],s['band'],s['level'],s['prescale'])==(c,4,i,3,2),'input stage')
  require(all(s[k]==bands[0]['stage'][k] for k in ('width','height')),'input geometry')
  if i:require(s['logical_band']==i+3 and s['frame_association']=='both','entropy identity')
 require((out['channel'],out['wavelet'],out['band'],out['level'],out['prescale'])==(c,2,0,2,0),'output stage')
 require(all(out[k]==2*bands[0]['stage'][k] for k in ('width','height')),'2x geometry')
require(len(indices)==4,'index count')
print('PASS: six next-stage cases, 30 artifact bindings, four index blobs, seven source blobs; private bytes and original numeric replay NOT_RUN')
PY_AUDIT
```

This audit deliberately does not open private paths or qualify arithmetic.
