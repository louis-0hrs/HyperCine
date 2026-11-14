# Issue 303: partial dialect/structure evidence

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.

This independently authored SDK-free Python 3 standard-library tool implements
only the first small slice of the [integrated Phase A audit](../research/vc5/PHASE_A_REFERENCE_AUDIT.md),
sections 4.1, 4.2 and 8.1. No external reference source, table or media was imported.
It is **not a conformance oracle**, decoder, entropy validator or CFHD validator.
A structurally accepted synthetic packet is deliberately not proof of real media.

## Portable invocation

Provide regular files and externally enforce a finite execution deadline:


timeout 10s python3 -B tools/vc5_inspect.py --input sample.bin --context context.json

The command above is POSIX; other callers must use their native process timeout
(e.g. Python subprocess.run(timeout=10)). The inspector starts no subprocess.
Network filesystems and OS-level stalled I/O still require the caller deadline.
Input is at most 4 MiB, context 16 KiB, parsed segments 65,536, JSON output
32 KiB. On POSIX, nonblocking open plus fstat rejects FIFOs/devices. Windows
regular-file and CLI behavior require native confirmation; POSIX FIFO behavior
is not claimed on Windows. The tests give every CLI child a five-second timeout.

Exit **1** means demonstrated syntax/claim/reference failure; **2** means
blocked/refused input or missing prerequisite; **3** means unqualified or
not applicable. This first slice never emits top-level PASS or exits zero.
Argument-parser usage errors produce fixed INSPECTION_REFUSED JSON and exit 2,
without echoing argv or private values. --help remains usable; long-option
abbreviations are disabled. Parser diagnostics are independent of argument size.
Syntax.status PASS
means only the bounded implemented structure checks succeeded. Never promote
that field, exit 3, missing coverage or a mock decoder's exit zero to acceptance.

## Accepted structure domain

Four-byte network-order start marker VC-5 selects the VC-5 grammar. Legacy
SAMPLE=9 framing is identified separately and remains NOT_APPLICABLE to VC-5
when the claim is identify or legacy-cfhd; an explicit vc5 claim fails. Other
markers are unknown, not guessed CFHD. Legacy payload structure is not checked.

The supported header subset requires ChannelCount, ImageWidth and ImageHeight;
checks listed Part 1 / Part 3 header tags, duplicate parameters, positive
geometry/precision, channels 1..4, precision at most 16, ImageFormat 1..4 and
channel/subband indices against preceding counts. These are **subset safety
constraints**, not an exhaustive normative legality predicate. PatternWidth
0x006A is an ordinary full 16-bit header tag and requires requested Part 3.
Optional tags use two's-complement negation. Small chunk lengths and 24-bit
large codeblock lengths are bounded against remaining input; reserved large
chunk families are refused. Payloads are opaque and skipped, not decoded.
After the first chunk this slice accepts only chunks; otherwise legal later
body state changes may therefore be refused as BODY_TAG_UNSUPPORTED. Refusal
indicates the supported parser subset, not necessarily a normative violation.
The syntax.status field is a supported-subset execution outcome, not a complete
normative verdict. BODY_TAG_UNSUPPORTED, HEADER_TAG and supported-domain
channel/precision/image-format range refusals are UNQUALIFIED, never promoted
to normative FAIL. Input/segment/output budgets and nonregular-file refusal
are BLOCKED. Demonstrated truncation, duplicate parameters, chunk bounds,
reserved chunk families, invalid state and explicit dialect-claim mismatches
remain FAIL. A known VC-5 marker remains dialect vc5 even when parsing refuses
a later field or resource limit. Unknown framing is not invented as VC-5.

Prescale 0x2800 records both Table B.1 [0,0,8] and reference layout [0,2,2].
Neither is selected as correct; both are within their stated ranges. Any
prescale record remains UNQUALIFIED. Errata/revision status is NOT_VERIFIED.

## Evidence schema hypercine.vc5-evidence/1

The context is a strict object with no additional fields:

- claim: vc5, legacy-cfhd or identify; acceptance_claim: encoder or decoder.
- parts, build_parts, runtime_parts: unique integer arrays from 1,3,4,5,6,7,10;
  requested parts must contain 1. Boolean integers are refused.
- source: commit and tree (40 lowercase hex), dirty (actual boolean),
  build_sha256 and executable_sha256 (64 lowercase hex).
- decoder: state (completed/not_run/unavailable), qualified (boolean),
  executable_sha256, configuration_sha256, output_sha256 (64 hex or null),
  version_id (1..64 safe identifier characters or null), warnings/errors
  (integers 0..1000000), exit_code (integer -255..255 or null), pixel_format
  (unknown/rg48/b64a/byr4/nv12/yuv422p10le), materials_available,
  inventory_complete and pixels_equal (actual booleans). Completed state
  requires all identities, exit and known format.

The fixture factory in tests/test_vc5_inspect.py shows a full synthetic context;
its repeated hashes and mock completed decoder are not actual reference evidence.
Source and external decoder values are **caller assertions**, not attestations.
The inspector records its own file SHA-256 and the actual input bytes/hash.
It never runs that decoder, replays an inventory, verifies an output hash or
compares pixels; even a fully positive qualified mock stays UNQUALIFIED with
RECORDED_EXTERNAL_ASSERTION_NOT_REPLAYED. Reference warnings, errors, nonzero
exit and unequal asserted pixels yield FAIL. Missing enabled parts, reference
materials/inventory or completed execution yield BLOCKED. This is not a
qualified external-runner implementation.

Output includes one explicitly requested encoder/decoder acceptance class,
RP 2073-2:2022 section 9.2/9.3, source/build/executable identities, input identity,
decoder/output assertions, requested edition/part ledger, checks and reasons.
Ledger editions preserve Phase A's historical inventory: Part 1:2017,
3/4/5/6:2015, 7:2022, 10:2017. Every part is MISSING_COVERAGE, including unrequested
roadmap parts; Part 10 remains a separate wrapper milestone. Part 7 metadata
is separately MISSING_COVERAGE if requested. No fresh edition/errata audit is
implied. FAIL takes precedence over BLOCKED, then UNQUALIFIED and NOT_APPLICABLE.
Fixed reasons and constrained context fields prevent diagnostics echoing raw
input, paths or decoder messages; hashes do not prove semantic correctness.

## Tests and integration proposal

Run both commands normally and with python3 -O:

- python3 -B tests/test_vc5_inspect.py -v
- python3 -B tests/test_vc5_inspect_mutations.py -v

25 contract tests cover structure positives, malformed framing/header/chunks,
PatternWidth context, both prescale interpretations, dialect claims, parts,
reference false-PASS controls, strict types, budgets, privacy and finite CLI.
Review regressions include unsupported-body UNQUALIFIED, resource BLOCKED,
known-dialect retention, real CLI unknown/malformed/private/oversized arguments,
sanitized in-process parser behavior and working help. The 36 backward mutations
include blanket-FAIL promotion and restoring default argparse argument echo.
The mutation suite compiles individual mutants **in memory**, never changing
owner source; each mapped regression test must fail for its mutant. Baseline
runs must succeed separately; a mutation harness success means mutations caught.

The shared SDK-free CTest registration includes all four normal and optimized
unit and mutation tests with finite timeouts. The strict standalone inventory
requires each entry; removal must fail the gate. Native Windows/macOS,
qualified references, complete normative semantics/payloads and actual decoded
pixel equality remain separate open gates. No existing gate is replaced.
