# HyperCine validation contract

**Decision:** [#382](https://github.com/louis-0hrs/HyperCine/issues/382),
2026-11-13. **Dispatch:** [#62](https://github.com/louis-0hrs/HyperCine/issues/62).
This is an engineering acceptance policy and implementation roadmap, not a new
codec qualification or an executable CI migration. Historical evidence remains
valid only for its measured scope.

## Product goals and dialects

Build a standards-correct VC-5 encoder **and** decoder; retain GoPro as an
engineering reference and preserve bidirectional CineForm compatibility.
HyperCine must read valid GoPro CFHD in its declared supported domain and emit
CFHD that correctly decodes in the corresponding GoPro and independent decoder
builds. Application, container and active-metadata behavior need their own tests.

Prefer VC-5 choices that also interoperate wherever the standard permits and
measure that interoperability. Do not assume that every legal VC-5 feature is
implemented by an older GoPro decoder. If wire syntax cannot be shared, provide
explicit CFHD-compatible and standardized VC-5 modes, not an undocumented hybrid.
Share arithmetic and resources where semantics agree; do not fork the codec per
resolution or GPU. These are target constraints, not currently available modes.

Full VC-5 essence coverage is the objective. First implement and qualify
ST 2073-1/-3/-4; retain -5/-6/-7 as explicit unfinished requirements. Record
editions, enabled features and unmet coverage. ST 2073-10 MXF wrapping is a
separate container milestone. A limited milestone must not be marketed as full
VC-5 support. The [SMPTE suite listing](https://www.smpte.org/standards/st2073-landing-page)
and [integrated Phase A audit](research/vc5/PHASE_A_REFERENCE_AUDIT.md) are starting
points, not substitutes for current evidence.

## Six independent acceptance classes

| Class | Required evidence | Not a substitute |
| :--- | :--- | :--- |
| `VC5_BITSTREAM_CONFORMANCE` | Applicable normative syntax/semantics and RP 2073-2 procedure, exact edition/parts and qualified reference build | GoPro encoder hash equality or a header-only inspector |
| `VC5_DECODER_CONFORMANCE` | Complete applicable official-vector inventory, prescribed output format and exact decoded-reference file equality | PSNR, visual similarity, or matching a related decoder |
| `CFHD_INTEROPERABILITY` | GoPro -> HyperCine and HyperCine -> GoPro/independent decoders, exact format/settings/versions and correct output | Successful open, FourCC, or an SDK success code alone |
| `PIXEL_AND_GENERATION_QUALITY` | First-generation quality and additional drift, native pixel domain and reviewed limits | A fresh encode of the original at every checkpoint |
| `INTERNAL_DETERMINISM_AND_SAFETY` | Own CPU/GPU arithmetic contract, state/session independence, resource bounds, fault/sanitizer/negative controls | Reproducing an SDK bug or a CPU fallback in a GPU-required test |
| `GOPRO_ENCODER_PARITY` | Targeted normalized/raw packet and state comparisons with a qualified, pinned SDK run | Universal proof of standard correctness or correct decoded pixels |

Universal GoPro encoder-byte identity is not the product objective. It remains
valuable for existing regression contracts and differential diagnosis. No current
test is removed, renamed, skipped or weakened by this document. Normative decoder
exactness and internal determinism remain strict.

For the standards lane, [RP 2073-2:2022, sections 9.1-9.4](https://pub.smpte.org/latest/rp2073-2/rp2073-2-2022.pdf)
distinguishes bitstream, decoder and metadata checks. The bitstream procedure
requires the specified parts in the reference build and runtime and completed
decoding without reported warnings/errors. Decoder tests cover every applicable
reference stream and compare the prescribed image-file lengths and bytes exactly.
Track Part 7 metadata separately when claimed. Independent structural and safety
checks complement, rather than replace, that procedure.

## Evidence and verdict rules

Each row binds source commit/tree, build configuration and executable identities,
wire dialect, standard edition/parts, input/output hashes, actual pixel layout,
precision/range/chroma policy, encoder/decoder settings, backend, platform and
reference provenance. Record return code, warnings, completed work, counts and
cleaned-log SHA-256. Do not recycle historical tests onto changed source.

`PASS` requires all requirements of the declared row. `FAIL` records a demonstrated
violation. `BLOCKED` records an unavailable prerequisite; `UNQUALIFIED` records
insufficient or unreliable evidence. `SKIP` is an execution outcome, not success.
`NOT_APPLICABLE` needs a reason tied to the declared scope; missing full-roadmap
coverage cannot be hidden with that label. Keep counts and verdicts separate.

No decoder success code alone is enough. Unresolved normative/reference conflicts,
unstable reference output or unqualified local reference patches remain visible.
The Phase A PrescaleShift interpretation finding must not be resolved by selecting
whichever reading makes the existing samples pass. External reference source,
tables and media stay outside HyperCine unless rights permit their inclusion;
record permitted access, exact revision and shared implementation ancestry.

## Corpus policy

Preserve all old inputs, SDK packets, state traces, decoded captures and indexes
without deletion, relocation, renaming, overwriting or blanket regeneration.
Keep private data as loose files under the already authorized local binding of
`HYPERCINE_GOLDEN_DIR`; never publish its value. A separate ledger classifies each
artifact's permissible use: identity verified, repeatability measured,
independently qualified expectation, known-bad diagnostic, or unqualified.

Hash agreement proves identity, not semantic correctness. A demonstrated SDK
decoder race invalidates affected pixel-oracle uses, not automatically every
encoded packet. Majority-vote decoding and a one-worker setting are not universal
correctness proofs. Preserve bad captures for diagnostics without comparing
production pixels against them as ground truth.

Historical compiler recovery is withdrawn. #321 records the remaining historical
limitations; #381 owns live compatibility. A bounded new SDK build/export is
justified by a named missing test, not by a wish to relabel old goldens. Use
pristine sources, pinned current tools, isolated staging and new identities;
never rewrite old records or claim retrospective attestation.

## Multigeneration quality

For fixed encoder/decoder configuration and defined state, run
`x[n+1] = D(E(x[n]))`. Execute every intervening generation. The experiment in
[#383](https://github.com/louis-0hrs/HyperCine/issues/383) starts CPU-first, using
synthetic fields, ramps, edges and seeded noise, then admitted real test cases.

Distinguish initial losslessness (`x[1] == x[0]`), no additional measured loss
(`x[n] == x[1]` for all tested n >= 2), later stabilization, and continuing drift
or cycles. Finite tests establish only their measured domain, not a mathematical
guarantee for all pictures. Bitstream-copy passthrough is a separate control.
GoPro's [WaveletDemo guidance](https://gopro.github.io/cineform-sdk/) motivates
small multigeneration loss; it is not a universal zero-loss standard requirement.

Run generations 1 through 50 with checkpoints 1, 2, 5, 10, 25 and 50. Record
intermediate hashes and compare active component samples to the original,
generation 1 and the predecessor: changed-sample count, maximum error, signed
bias, MSE/PSNR, clipping and alpha behavior. Preserve exact equality rather than
rounding a small error into 'lossless'. Predeclare and review thresholds, then
qualify on a separate held-out set.

First isolate native-domain arithmetic at fixed quantization/prescale/companding.
Then test adaptive quality and sequence state, and separately the RGB/YUV,
chroma-resampling, precision, metadata and color-management operations of a real
workflow. A BGRA8 loop is not proof of a native 10-/12-bit property. Reset sequence
state per whole generation according to the declared contract, not per frame
merely to improve results. Qualify decoder stability before interpreting drift.

Harness controls include identity, an idempotent quantizer, deliberately drifting
and cycling transforms, wrong channels/range, missing or unstable decoders, and
the accidental reuse of `x[0]`. Test the actual chained inputs, not just the final
score. Include HyperCine, qualified GoPro and mixed CFHD chains where available.

## Migration and executable milestones

| Owner / task | Next concrete result |
| :--- | :--- |
| Standards / [#303](https://github.com/louis-0hrs/HyperCine/issues/303) | Bounded independently authored dialect/header inspector and evidence schema, with positive, malformed-input and false-PASS controls; partial validator only |
| Compatibility / [#381](https://github.com/louis-0hrs/HyperCine/issues/381) | Bidirectional matched-format runner and use-qualified inventory of existing references |
| Architecture / [#259](https://github.com/louis-0hrs/HyperCine/issues/259) | Design-level dialect/part/interoperability capability distinctions without an unreviewed ABI rewrite |
| Quality / [#383](https://github.com/louis-0hrs/HyperCine/issues/383) | Small actual generation loop with verified measurement controls and explicit unavailable paths |
| Native profiles / [#46](https://github.com/louis-0hrs/HyperCine/issues/46) | Shared quality/state and real CPU/CUDA/Metal qualification; no duplicate profile mathematics |

A test-migration PR must inventory the old tests, identify each changed assertion,
show its replacement coverage and negative controls, and preserve all remaining
safety/native/privacy gates. Explain every reduction; no blanket parity deletion
or skip-as-pass. Existing narrow fixes #379/#380 and P7 #360 continue separately.

No scheduled CI churn, new bulk corpus or hardware sweep is required by this
policy. Start small local tests and use native devices only for the relevant
claims. Public reports contain safe relative IDs, versions, hashes and results;
no private corpus, original protected media, credentials or infrastructure details.
`upstream/` remains frozen. Merges, releases, tool permissions and CI budget gates
are unchanged. Issues and documentation do not start local agents by themselves.
