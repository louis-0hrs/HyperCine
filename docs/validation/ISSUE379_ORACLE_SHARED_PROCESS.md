# Oracle subprocess forward-port (#377 / #379)

Source base: `d8b6adc4b95809776b7d4f635d277405908db13d` (merged #395); no codec, SDK, upstream or
private corpus change. The historical #379 draft is not mergeable as-is.

The three FFmpeg/ffprobe calls in `tests/compare_sdk_oracle.py` now route
through the already integrated `tools/compat_process.py` instead of
maintaining an independent subprocess supervisor. The default `limit=None`
means **stdout discarded and drained to EOF**, with the shared supervisor's
finite stderr and deadline checks; it never means unlimited capture. Version
and ffprobe JSON captures use an explicit 64-KiB bound. Decode still writes
raw frame data to an exclusive, size-checked file with FFmpeg `-n`.

No launch paths, child stdout/stderr or tool exception messages enter the
public failure message. Cleanup/containment uncertainty is explicitly
separated from ordinary tool failure. The inherited-pipe, timeout,
overflow, invalid-budget, child-exit and suppressed-output regressions are
added to the existing Oracle unittest, including normal and optimized
execution via the repository's CTest registration. All pre-existing
registration/negative controls are retained.

**Required before non-Draft or main integration:** exact-head full
Windows/Linux/ASan 158-case standalone and Windows CPU reference CI;
real command/fixture tests and review of child/grandchild Windows lifecycle;
public privacy/hooks; mutation controls proving an ignored child exit
or truncated/held pipe cannot turn green; source/build/fixture SHA receipts.
A release or external GoPro/VC-5 interoperability verdict is **not**
established by this validation-only change. Both #259 and #46 remain open.
