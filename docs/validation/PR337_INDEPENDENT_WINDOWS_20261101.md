# PR #337: fresh-checkout native Windows execution

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Status: REVIEW. Executed 2026-11-01 on native Windows x64 with MSVC
19.44.35229.0. This is a new execution from a fresh detached checkout of
`0429eebe99cbe92551e960dc49d705f4003f1442`, with new builds and test logs;
the original author's 2026-10-30 receipts were not reused as execution evidence.
It does not establish a separate physical host or a separate human reviewer.

The sanitized [machine-readable receipt](../evidence/pr337-independent-windows-20261101.json)
records source SHA, the complete PR changed-file Git blob and working SHA-256
inventory, compiler and flags, configure/build/CTest exits, complete test
inventories, completed JUnit SHA-256 values, probe SHA-256 values and original
output observations. Private paths, binaries, build directories, JUnit and raw
logs remain outside the repository. No source, public header or upstream file
was changed. The subsequent evidence-only commit does not relabel its own SHA
as the executed source SHA.

| Configuration | Full configured CTest | Original outputs | Identity controls |
| --- | --- | --- | --- |
| MSVC Release | 32/32 PASS, 0 failures, 0 skips | 24/24 byte-exact | 48/48 rejected |
| MSVC ASan Release | 32/32 PASS, 0 failures, 0 skips | 24/24 byte-exact | 48/48 rejected |

Both configurations include `s3_original_stage_144` and
`s3_original_replay_protocol`. Each original replay authenticates all 30 original
payloads and 60 JSON/SHA sidecars before and after execution, at workers
1/2/4/16 across six original pairs. The reports confirm originals unchanged.
All configure/build/CTest exits are zero.

ASan retains `/DWIN32 /D_WINDOWS /EHsc`, appends `/fsanitize=address /Zi`,
and links with `/INCREMENTAL:NO /DEBUG`. The generated projects for the graph,
spatial attachment and replay executable contain the instrumentation flags.
`dumpbin /dependents` reports `clang_rt.asan_dynamic-x86_64.dll`. In a separate
startup check, `ASAN_OPTIONS=help=1:halt_on_error=1` causes the executable to
print the AddressSanitizer runtime's available flags, proving that runtime code
executed. That no-argument invocation intentionally exits 1 for invalid probe
usage; it is not a failed CTest. Full ASan tests use `halt_on_error=1`.

## Adversarial process-failure check

The existing eight-method protocol tests distinguish exit 1 with a forged
rejection witness and exit 2 without the witness. An additional native
non-decoder executable was compiled outside the repository for this run. It
prints and flushes the complete valid rejection witness, then terminates its
process with status `0xC0000005` rather than returning 2. The unchanged
`validate_spatial_attachment_144.execute` rejects it in all three modes:
normal, wrong-band and wrong-channel. Result: 3/3 rejections, zero false
positives. This injects a crash exit status; it does not perform an actual
invalid memory access. The receipt includes its full small source and
source/executable SHA-256 values for review and reproduction. No decoder
assertion, registration or failure classification was weakened.

## Portable reproduction and handoff

Follow the existing [original-S3 reproduction](ISSUE144_ORIGINAL_S3_GRAPH_20261030.md#reproduction)
from a fresh checkout of the pinned source above. Bind the existing private
corpus through `HYPERCINE_GOLDEN_DIR`, configure the explicit original root,
and use separate new Release and ASan build directories outside the Git tree.
Run the full configured CTest with `--output-junit` and
`--test-output-size-passed 200000`. Keep runtime dependencies available on the
test process PATH. The ASan flags and startup check above are required.
For the crash control, compile the receipt's source with native MSVC and call
the unchanged Python `execute` helper against analytic synthetic inputs in
each of the three modes; every call must raise `ValueError`.

The Windows execution portion of the latest #337 acceptance request is now
available for maintainer review. Next: the designated Linux integration owner
reconstructs the test-only change against current main, reports the resulting
source/blob inventory and four hosted workflow families, and evaluates evidence
carry-forward using unchanged executed-source blobs or reruns affected gates.
These old-head results are not native results for a future rebased head.

Keep #337 Draft, #144/#47 open, `GraphProfile::Synthetic`, and pixel/release HOLD.
This verifies one coefficient-stage transport path, not a decoded frame,
production decoder, SMPTE conformance, CUDA qualification or release.
No merge, tag or release is performed.
