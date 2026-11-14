# Dialect-aware capability amendment (#259)

**Design proposal only; no public API or runtime change.** Builds on the integrated [#382 validation contract](../VALIDATION_CONTRACT.md). No replacement research audit, standards inspector or evidence schema; those belong to #303.

## Immutable request dimensions

| Dimension | Proposed meaning / refusal rule |
| --- | --- |
| Wire | Exactly legacy CFHD or standardized VC-5. Never inferred from quality name, FourCC or GPU. No automatic output dialect or wire fallback. |
| Standard | Explicit edition per requested ST 2073 part, features and applicable errata/material identity. CFHD carries no VC-5 assertion. Unknown editions are unsupported, never mapped silently to latest. RP 2073-2 is the qualification procedure, not codec Part 2. |
| Representation | Source layout/depth/pitch/orientation separately from encoded channels, precision, chroma/range, progressive/temporal/layer/stereo and metadata semantics. FilmScan3 is a quality/rate policy, not a normative VC-5 profile. |
| Geometry | Logical dimensions separate from physical stride/extents. Existing checked arithmetic and per-buffer/sample/device limits remain authoritative. No crop, scale or private tiling repair. |
| Interoperability | Exact target implementation/build/application version, direction and output domain. Multiple demanded targets are conjunctive; the proof models one at a time. ExperimentalOnly explicitly demands none. |
| Execution | Required/preferred backend, explicit CPU fallback policy, measured device limits, memory/sample budgets and session state/reset contract. Backend fallback reruns all admission checks without changing wire/features/geometry/target. |
| Qualification | Six independent acceptance classes in VALIDATION_CONTRACT, scoped to all fields above and immutable source/build/input/output identities. No caller-set qualified boolean can manufacture evidence. |

Part 3 image-format and Part 4 subsampled-colour applicability need the standards owner’s semantic mapping, not guessed quality names. Layers/sections/metadata require requested editions of Parts 5/6/7 respectively. The proof checks only that necessary dependency, not sufficiency or normative validity. Part 10 MXF wrapping is a separate wrapper request. Full -1/-3/-4 then -5/-6/-7 remains the roadmap, not current support.

## Admission composition

1. Pure intent validation rejects contradictory/unknown fields.
2. Resolve exact dialect/edition/features. Unsupported is a hard refusal before frontend/resource creation; never select CFHD for VC-5.
3. Reuse existing format, geometry, profile/backend and resource preflight, retaining all axes and requested/effective backend diagnostics. Existing CheckEncoderCapability remains authority; do not copy its planner.
4. Resolve required external evidence for exact target and request. Missing/unstable evidence is UNQUALIFIED, not PASS; an unavailable execution prerequisite may be BLOCKED in the ledger.
5. Only then create a session. Dynamic allocation/device failures still use existing typed semantics. Preflight does not guarantee memory.

A production result should retain requested wire, availability, separate admission and qualification verdicts, precise refusal axes and allocation permission. The isolated proof exposes only a primary refusal and may_plan, not the full diagnostic vector. ExperimentalPlan permits a hypothetical planning boundary, **not** actual codec allocation or any codec/interop/conformance PASS. #303 owns evidence schema; #381 owns compatibility evidence.

## Current capability versus target

Source/document assessment on main 72d6441154322b0065f2de3db7bbd8fb49e28274, not new runtime measurements:

| Row | Current scope | Proof result |
| --- | --- | --- |
| CFHD BGRA8 to progressive GOP1 YUV422 10-bit, scalar CPU, admitted geometry/profile | Existing shared writer and bounded experimental admission; six CPU qualities. Historical packet/decode receipts remain bound to measured source. | ExperimentalPlan only when supplied existing-preflight axes pass. |
| Same on Metal/CUDA | Separate device/backend/geometry evidence; internal session profile admission FilmScan3 only. | Existing admission decides; proof invents no GPU facts. |
| CFHD requiring GoPro, FFmpeg or application qualification | Matching direction/build/output evidence required, not byte identity or open success. | UNQUALIFIED: this proof contains no external evidence registry, including for FFmpeg. |
| VC-5 with explicit editions/features, optionally requiring GoPro | No standardized serializer introduced or qualified here; framing/PrescaleShift disagreement remains in Phase A audit. | Unsupported/WireUnavailable; requested wire unchanged. |
| Missing feature-part edition, or CFHD with VC-5 editions | Contradictory/incomplete intent, not backend failure. | InvalidRequest. |
| RGB(A)/Bayer/interlaced/temporal/stereo, 8K+, active rendering, MXF | Separate backlog or separately scoped paths. | No permission inferred from this FilmScan3 subset. |

## Meaning of one writer

One computation/resource contract where proven equal, **not one ambiguous wire syntax**. Backend frontends produce explicitly described coefficient/state views. Quantization, prescale and codeblock semantics are shared only where verified identical; common code must not conceal dialect-dependent interpretation. A dialect serializer owns framing/tags/packing; a dialect parser validates those before normalized shared reconstruction. Distinct CFHD/VC-5 serializers/parsers are legitimate; per-GPU/per-resolution writers are not. Serializer selection binds once to immutable session intent. None is implemented here.

Existing writer/constructors remain CFHD with historical contracts. The smallest later integration is an internal wrapper around current request/preflight, not new public class members or virtual methods. Later serializer work requires independent normative/interoperability tests. Resolve PrescaleShift through #303, not whichever interpretation reproduces SDK bytes. See [Phase A audit](../../research/vc5/PHASE_A_REFERENCE_AUDIT.md).

## Bounded standalone proof

Files: internal/contract_proofs/dialect_capability/contract.h and test_contract.cpp. Pure fixed-size C++17 preflight; no codec linkage, factories, frame allocation or reference calls. ExistingAdmission booleans are opaque test inputs, **not** trusted production caller claims. Real integration must derive them internally from existing preflight. Geometry validation here is only the FilmScan3 input subset, not universal VC-5 legality; physical extents/overflow/budgets remain existing preflight responsibilities.

Tests cover contradictions, unknown enums/features, necessary feature/part constraints, unknown edition, unsupported VC-5, actual source/coded format mismatch, geometry, backend/resources and three unqualified targets. Each refusal is exercised against a simulated factory counter. Explicit runtime checks survive NDEBUG.

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic internal/contract_proofs/dialect_capability/test_contract.cpp -o contract-test
./contract-test
c++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pedantic internal/contract_proofs/dialect_capability/test_contract.cpp -o contract-test
./contract-test
```

The shared SDK-free CMake registration adds `dialect_capability_proof` to both regular and standalone test inventories, before the optional Python lookup. The strict standalone runner requires it and rejects a missing or duplicate registration. Existing registrations are not removed or replaced. Run the registered proof with `ctest --test-dir build -C Release -R '^dialect_capability_proof$' --output-on-failure`; the full strict standalone gate uses `python3 ci/run_standalone.py --build-dir build --system Linux --machine x86_64` after the documented standalone configuration and build. Inventory counts are build-configuration dependent. This is a synthetic proof, not production integration; native GPU, SDK, official standards vectors and applications are not qualified. Publication/CI remains coordinator-owned.
