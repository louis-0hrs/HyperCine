# Fundamental safety follow-up — 12 November 2026

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

Tracking: [#364](https://github.com/louis-0hrs/HyperCine/issues/364).
Implementation: [#367](https://github.com/louis-0hrs/HyperCine/issues/367).
Baseline main: `dca0c0707a0d460fb8cb7ca7d198c0a7aa4be8e7`.

This record preserves the original #367 and #370 source-specific results below.
For the subsequent bounded-read, encoder-ownership and SDK-wrapper repairs and
their combined source, see the [safety continuation](SAFETY_CONTINUATION_20261112.md).
The historical repair plans in this record are superseded where that continuation
records an implementation and its acceptance evidence.

This continues the 11 November audit. It is a bounded engineering review with
actual Linux executions, not an exhaustive security certification. It preserves
the existing hardware/original-SDK and product acceptance boundaries.

## Completed work

### A07: output/evidence preservation

The two reproduced destructive paths have been replaced by exclusive run
ownership and complete-result publication. The CLI refuses existing output,
pins inputs, stages every file and publishes the complete directory without
replacement. The parity tool no longer removes an old `samples` directory or
reuses a caller-owned work tree. Reports retain explicit PASS/FAIL semantics
and are published without replacing earlier evidence.

[The output contract](OUTPUT_OWNERSHIP_367.md) documents caller migration,
native filesystem operations, remaining staging directories and platform
limits. New controls include prior-file/directory sentinels, aliases and
physical ancestry, invalid first input, late input truncation, actual partial
write failure, parent/stage replacement and concurrent process creation.
All existing trace-malformation assertions remain. Actual CLI/P6 execution
and a real 4K FFmpeg decode exercise the changed tools together.

The final main-based source passed **133/133 strict standalone CTests**, zero
failure/error/skip, with real FFmpeg/ffprobe required (195.45 seconds). All 19
implementation/test/build fingerprints were checked before and after the run.
The six new mandatory gates include 19 real-CLI cases, six native transaction
scenarios, 26 parity ownership cases in each Python mode, the actual two-frame
interoperability run and one actual 4K CLI decode. The real-CLI suite also passed
19/19 under Python `-O`. These overlapping counts must not be added as unique
tests. [Final source-bound receipt](validation/OUTPUT_OWNERSHIP_367_20261112.json).

### Additional 4K validator defect

`EncodingQualityName(FilmScan3)` emits lowercase `filmscan3`; the 4K validator
required `FilmScan3`. Mocked traces manufactured that incorrect spelling and
did not expose the failure of the real CLI path. The validator and mock
fixtures now use the actual contract, and a mandatory CPU 4K CLI/decode test
prevents the mismatch from remaining hidden behind a native Metal-only lane.
This is a validator correction, not new Metal qualification.

### Independent acceptance of integration PR #370

Exact head `5eb3ac9b8543ab155782a7e962a66cff6d984a02`, tree
`503fc5824c9937b763f69c842bcf2b2da5bc7433`, was independently checked without
modifying its source. Its only three changes from #369 exactly match the
reviewed #361 blobs; the frozen upstream subtree remains unchanged.

- Full strict Linux standalone CTest: **130/130 PASS**, zero failure/error/skip,
  external FFmpeg/ffprobe required; 220.18 seconds.
- Oracle contracts: **23/23 normal and 23/23 optimized**.
- Actual public retained-fixture and eight-record log FFmpeg/ffprobe replays
  passed. Synthetic adverse observation/child-error markers stayed out of
  reports and public process output.
- Independent double-read and overwrite mutations failed their intended
  regression assertions, with the checked-out source unchanged.
- Frozen-reference controls: **26/26 normal and 26/26 optimized**; workflow
  event matrix **48/48**; compute-policy tests **57/57**. The initial policy
  attempt rejected an untrusted writable workspace ancestor; only that small
  suite was rerun under an owner-private temporary parent. No shared
  permission changes or approval-state changes were made.

Full source binding, log hashes and limitations:
[INTEGRATION370_LINUX_20261112.json](validation/INTEGRATION370_LINUX_20261112.json).

**#370 remains blocked for publication/merge qualification.** The required
per-commit privacy scan of its 31 commits beyond main flags a historical
synthetic URL-rejection test marker in commit
`ec35f3c59ee11cfbb69a46657f97c4a57f597451`,
`tests/test_compare_sdk_oracle.py:214`. No live secret was identified. The
current file uses an accepted dummy placeholder, but that does not clear an
earlier added line. The unchanged scanner was not bypassed and no shared
history was rewritten. The finding and precise blocker were reported on #370.
The focused #367 branch starts from main and does not inherit this ancestry.
Resolve the lineage/privacy remediation explicitly before relying on this
candidate as merge-qualified; a final-tree-only scan is insufficient.

## Refined remaining repair plans

| Finding | Concrete next repair and acceptance |
| --- | --- |
| A08 — SDK array bounds | Add a trusted channel extent/view to both `EncodeOneExternalBand` in `src/cpu/entropy_parallel.cpp` **and** `MatchUpstreamHighpass` in `src/cpu/entropy_stages.cpp`. The latter indexes channels and band arrays and selects subband tables before reaching the former; fixing only one wrapper leaves another unchecked entry point. Validate channel, band, level, subband and lowpass/highpass relationships before any indexing or SDK call. Test negative/oversized indices on the actual supported SDK wrapper target. |
| A09 — copied GPU resources | Define move-only ownership or a true clone with fresh backend resources and independent diagnostic/session identities. Specify same-object concurrency and verify reset/failure isolation on native Metal/CUDA. An implicit shared-pointer copy still does not prove independent sessions. |
| A10 — CUDA device identity | Bind the selected current device through allocation, execution, reporting and destruction. Include frontend/probe code, `HyperCine_GetCUDADeviceInfo` in `src/cuda/wavelet_cuda_api.cu`, `phase2a_transfer.cu` and device-zero assumptions in native tests. Verify device switching, cross-thread policy, device-specific limits, cleanup and restoration using actual CUDA hardware; use two distinct devices when available. |
| A11/A13/A14 — CI enforcement and maintenance | Qualify stable aggregate build/test checks before branch-rule changes; separately qualify Linux leak checking, explicit permissions/timeouts and pinned action revisions. Preserve the existing hosted-compute budget and owner-controlled Mac routing. |
| A12 — oracle lineage and residual bounds | Keep #361/#370's actual Linux evidence distinct from hosted/native acceptance. Resolve the historical privacy finding before new publication of that lineage. The oracle reader checks file size before and after `read_bytes`; that is not an allocation cap against a file growing during the read. A future bounded-stream read should limit bytes while reading and bind hash/parser to the same snapshot, with a deliberate grow-after-stat negative. No allocation-exhaustion reproduction or exploitability claim was made here. |
| A15 — product gates | Keep long-session/timebase, original-SDK provenance, backend qualification and SMPTE VC-5 conformance requirements open. Successful public synthetic fixtures and Linux CTest do not substitute for those gates. |

The #367 source does not edit `src/`, public codec headers, frozen upstream,
workflow routing or branch rules. No main merge, release, paid CI dispatch,
private-corpus mutation or other agent's branch rewrite was performed.
