# Safety repair continuation — 12 November 2026

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

Tracking: [#364](https://github.com/louis-0hrs/HyperCine/issues/364),
[#367](https://github.com/louis-0hrs/HyperCine/issues/367),
[#371](https://github.com/louis-0hrs/HyperCine/issues/371), and
[#372](https://github.com/louis-0hrs/HyperCine/issues/372).

This local candidate continues the fundamental safety audit. It combines the
reviewed CPU bounds, pinned frontend geometry and frozen-reference protections
from #369 with the output, reader, ownership and SDK wrapper repairs below.
Focused branches remain available separately. This is engineering evidence for
the stated source and environment; native backend and product release gates
remain separate.

## Repairs and resulting contracts

### A07: preserve outputs and bound reads while data arrives

The original output fix refuses an occupied destination, pins the input and
parent directory, writes inside an exclusively owned staging directory and
publishes the complete result without replacement. P6 runs use a new work
directory and preserve earlier reports and corpora. The real 4K validator now
accepts the lowercase quality name produced by the CLI.

The continuation also fixes the gap between checking a file's size and reading
it: the P6 reader now enforces its byte budget during each read. Legitimate short
reads are accumulated. Growth beyond the budget after `stat`, premature end of
a pinned file and an oversized decimal size field have explicit rejection paths.
A single overflow byte can be observed to reject excess data, but it is never
delivered to a parser or copied into an accepted payload. Hashing and parsing use
the same accepted bytes.

The limits are tool acceptance budgets: 33,177,600 bytes for a BGRA input,
128 MiB for a packet, 8 MiB for the transfer index and 4 MiB each for a manifest
or trace. They do not define the maximum representable codec format. Existing
`require_pinned` and `load_transfer_index` callers retain their interface.

See [output ownership](OUTPUT_OWNERSHIP_367.md) and the
[bounded-read receipt](validation/P6_BOUNDED_READS_20261112.json). The additional
bounded reader does not automatically repair hash-then-reopen patterns in other
validation tools; those are listed below.

### A09: one owner for each encoder session

`FilmScan3Encoder` and the internal `EncoderSession` are now noncopyable and
explicitly movable. Moves transfer the frontend resource, rate and quality state,
profile lock, diagnostic identity, hashing policy and backend selection together.
The source becomes inactive and cannot encode or acquire an injected frontend.
An independently created live session can subsequently be moved into it.

This is a deliberate C++ source and object-layout change. Consumers must rebuild
and replace implicit copies with `std::move` or separately constructed encoders.
Moving an encoder is not a concurrency mechanism. Concurrent operations on one
object and intentional sharing of a mutable internal frontend remain unsupported.

The combined regression exercises moves while a bound frontend's live geometry
has drifted. The destination must retain its original geometry snapshot, reject
the inconsistent operation before transformation without consuming state, and
continue with the correct packet after the original geometry is restored. This
covers the interaction between #365 geometry binding and #371 ownership.
The older default-session Reset regression now uses an independently constructed
encoder instead of an implicit copy. It still establishes the lazy geometry
binding through an actual first encode and checks that Reset preserves it.

See [encoder ownership and migration](ENCODER_SESSION_OWNERSHIP.md).

### A08: validate SDK entropy metadata before indexing

The external-band entry points now require the actual channel-array extent.
Shared checks reject invalid channel, level, band and subband indices, malformed
prepared pyramids, incompatible band relationships, codebook selections and
table extents before the relevant array access or SDK call. The supported
relationship is an already prepared spatial GOP1 pyramid; other layouts require
separate qualification.

The Refresh path has its own three-channel YUV contract before conversion.
Prepared spatial counts must fit the physical arrays and agree with the
encoder. The regression checks actual plane, wavelet, input and scratch bytes,
so a conversion performed before rejection is observable even when the function
eventually returns failure.

The host wrappers build as `hypercine_entropy_cpu` and can therefore be tested
against the real frozen SDK without CUDA. Existing CMake consumers of
`hypercine_wavelet` receive this dependency transitively. Consumers manually
linking static archives must include the new host archive and its dependencies.
External callers must pass the new explicit channel extent.

The caller still owns the validity, actual allocation, lifetime and synchronization
of raw pointers and coefficient buffers. Structural checks cannot establish
those properties from a pointer alone. The frozen SDK is unchanged.

See [SDK entropy bounds and reproduction](SDK_ENTROPY_BOUNDS_ISSUE372.md).

## Source provenance and verification

The candidate starts from #369 at
`5b1724c3097da12ca655000c688e2251278bd9aa`.
The reviewed focused output branch ends at
`1aeb76190049d75ff2853450e02efaf150f095c2`; the ownership branch ends at
`a170e9451859040a9462ecc247900cf9b12cd277`.
The focused SDK commit is `a423f9196f55f40dadc987a116c0a1c12d725489`.
The final source-bound integration receipt records the source fingerprints and
both complete available CTest inventories.

| Final combined run | Result |
| --- | --- |
| Strict SDK-free Linux CTest, real FFmpeg/ffprobe required | **137/137 passed**, zero failed/error/skipped; 387.97 seconds. |
| Complete available Linux SDK CTest | **66 passed, zero failed/error, one explicit skip**, out of 67; 64.32 seconds. |
| SDK external-band and Refresh controls inside that inventory | **133 negative controls and 69 positive comparisons passed** against the actual SDK. |
| Source consistency | All **56** changed implementation/build/test fingerprints agree before and after both final runs. |

The one SDK skip is `ffmpeg_oracle_qualification`: this environment's FFmpeg
7.1.5 is outside that gate's measured-build list. Its allowlist and return code
remain unchanged. The complete SDK inventory has no failing tests, but **full
reference-oracle acceptance remains blocked**. Two initial nested-configure
failures were resolved by passing the genuine local uuid development prefix to
child CMake processes and then repeating the full SDK suite without source edits.

[Combined source-bound evidence](validation/SAFETY_CONTINUATION_20261112.json)
records the exact inputs, separate attempts and qualification boundaries.

Historical focused receipts describe their own source versions. Their counts
overlap with the combined runs and must not be added as independent coverage.
Runtime measurements reflect concurrent local qualification, not a performance
benchmark. Linux importer tests do not constitute a native Windows run, and a
CPU frontend does not qualify native Metal, CUDA or Vulkan behavior.

## Remaining work with concrete acceptance criteria

| Finding | Next implementation | Required evidence |
| --- | --- | --- |
| A10 — CUDA device identity | Carry the selected device identity through capability probing, factory construction, allocation, execution, diagnostics and cleanup. Cover `HyperCine_GetCUDADeviceInfo`, `phase2a_transfer.cu` and existing device-zero test assumptions. Specify whether cross-thread moves are supported and restore the caller's device when required. | A real CUDA toolkit build, device switches and cleanup on native hardware; two distinct devices when available. Replacing a literal device zero with one `cudaGetDevice` call alone does not establish lifecycle binding. |
| Reference decoder qualification | Run the unchanged `ffmpeg_oracle_qualification` with a measured decoder build and its committed pinned-output and mutation checks. The current gate recognizes n8.0 and 9.0.2; this environment supplies 7.1.5. | A real gate pass with the qualified tool. The current explicit skip remains a blocked acceptance, even though ordinary SDK CTest returns zero. Successful standalone decode tests answer a different, narrower interoperability question. |
| A12 — oracle reader limits and provenance | Resolve #361/#370's historical privacy finding before publishing that lineage. Apply bounded streaming with the existing limits to oracle logs, manifests and retained fixtures, and bind hash and parsing to the same bytes. | Growth-after-stat, short reads, early EOF and exact-limit-plus-one cases in normal and optimized Python; actual fixture/log replay after the change. The earlier #370 execution is historical evidence, not qualification of a future fix. |
| Related validator snapshot gaps | Review `p6_sdk_gap_gate.py::measure`, `verify_windows_sdk_corpus.py::verify_index`, `verify_issue46_group_a.py`, `validate_quality_state_corpus.py` and `sdk_gap_corpus.py` where verification is followed by a fresh read. Reuse an accepted snapshot or pinned handle with an explicit budget. | Mutate the file between verification and consumption; require consistent rejection or use of the original verified snapshot. The bounded implementation of `require_pinned` alone does not bind a caller's later read. |
| Related unbounded readers | Apply justified budgets to `observe_ffmpeg_1080.py::bound`, `validate_rgb444_rg48.py::checked_artifact`, `reference_decode/contract.py`, `test_upstream_off_contract.py::_archive_references`, `validate_filmscan3_geometry.py` and `validate_filmscan3.py::compare_run`. | Small deterministic short-read and growth controls; no allocation-exhaustion experiment is required. Distinguish external evidence from data produced by the test itself. |
| A11/A13/A14 — CI policy and maintenance | Qualify stable aggregate checks, scoped leak checking, explicit permissions/timeouts and pinned action revisions before changing branch enforcement. | Review event routing and required checks against the existing hosted-compute budget and owner-controlled Mac routing. |
| A15 — product acceptance | Complete long-session and timebase tests, original-SDK provenance, backend qualification and SMPTE VC-5 conformance work. | Actual named platform/corpus/hardware evidence; public synthetic samples and successful Linux CTest do not replace these gates. |

## Publication and native qualification boundary

This candidate does not include the #361/#370 code or ancestry. The historical
privacy finding remains open on that separate lineage; no history rewrite or
scanner bypass was used to clear it. The unchanged frozen upstream tree is
`837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`.

An earlier automatic approval review rejected publication of the output-safety
branch because it did not find explicit authorization for the exact public
payload. The local continuation is prepared for review; no alternative transport
is used to bypass that decision. Main merge, release, branch rules and paid CI
dispatch are outside this local repair delivery.
