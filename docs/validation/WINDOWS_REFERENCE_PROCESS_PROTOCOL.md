<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Bounded original-SDK fresh-process protocol

`windows_reference_process` is a Windows x64 test executable linked to the
unchanged original SDK. It accepts a pre-existing 1920x1080 CFHD sample and
requests full-resolution YU64 output. The Python runner requires its SHA-256
before any decoder starts. No sample is generated or modified by this protocol.

The plan is fixed before the first decode: three fresh processes per cell,
first TAG_CPU_MAX requests 1/2/4/8 at the fixed available process affinity,
then request 2 at selected affinities. With qualified single-group Windows
topology, those affinities are one logical processor, one logical processor
from each of two physical cores, and two SMT siblings from one physical core
(where available). The maximum is seven cells / 21 decodes. Missing physical
topology is explicit INCONCLUSIVE and does not establish an SMT comparison.
The masks and their meaning are retained in the preregistered plan.

Process affinity is set and checked before the decoder opens. TAG_CPU_MAX is
an SDK metadata request, not measured worker-count telemetry. Each record keeps
`requested_threads` separately from `observed_decoder_threads=null`. No SDK
internals are patched to force a requested worker configuration. Do not infer
worker-count causality from these requests alone.

Each observation includes input/executable/output SHA-256, CPU ISA availability
with OS register-state checks, requested and verified affinity, output format,
geometry/pitch, process exit, timeout and error stage/API code. The API query
CFHD_SAMPLE_SDK_VERSION returns the linked decoder SDK's packed version constants
(frozen SampleDecoder.cpp), recorded separately from the decoder-header label.
The frozen SDK baseline is recorded; the executable hash identifies the actual
linked probe build. Exact checkout/build options belong in the surrounding run
report. CPUID describes available ISA, not an observed internal SDK dispatch.

Run privately, using a newly named report path:

```powershell
python tests/validate_windows_reference_process.py --probe <probe.exe> `
  --sample <existing.cfhd> --input-sha256 <predeclared-sha256> --output <new-report.json>
```

Reports contain no source/output paths, raw process output or device identifiers.
Pixel buffers are temporary and removed after hashing.
The native probe exclusively creates its output and rejects any existing file,
including an input/output path collision, without overwriting it.
Errors/timeouts are FAIL; missing evidence is INCONCLUSIVE; differing hashes within a cell are
BLOCKED_NONDETERMINISTIC_REFERENCE. Stable per-cell but different cross-cell
outputs are BLOCKED_CONFIGURATION_DEPENDENT_REFERENCE. Repeated equal hashes
are REPEATABLE_OBSERVATION_ONLY, never proof of pixel correctness. There are no
retries, best-of-three selection, relaxed encoder gates or confirmed root-cause
claims. Packet/coefficient/structure/FFmpeg/SDK evidence must remain separate.
