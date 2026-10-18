# EPYC VM guest-affinity protocol — issue #158

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

Status: **pre-registered derivative protocol, frozen before the four measurement
arms**. This document is test evidence planning only. It does not change the
production decoder, `upstream/`, or the #138 owner branch.

Before freezing, one discarded harness smoke (`--arms A,C --processes 4`)
exercised the script and worker plumbing, including its performance path. No
smoke observation, hash or classification is carried into the protocol arms or
the published evidence.

## Purpose and lineage

Issue #158 asks whether guest process affinity and vCPU scheduling change the
original GoPro decoder's successful-but-corrupt 1080p BGRA output on the
M4/EPYC Windows VM. The frozen baseline is #116 Handoff 5: 4/45 divergent
calls, five pixel digests, 45/45 `CFHD_ERROR_OKAY`.

This protocol is a **versioned derivative** of the #138 protocol:

| field | value |
| --- | --- |
| derivative base | `97c34500324b550da5f6444fe9d28c8729dea8d2` |
| parent protocol | `matched-sha-1080-bgra-full-20261018` |
| derivative protocol | `epyc-vm-affinity-1080-bgra-20261018-v1` |
| protocol script | `tests/reference_decode/affinity_1080.py` |
| script SHA-256 | `23c4f532c3c19a6c31244dea005be9e2929f19e0d3795b995744ae26d7b8ab48` |
| worker source | `tests/reference_decode/worker.cpp` |
| worker source SHA-256 | `a43b73d1063bfbf49dcd9ed28dbef939a2df73e6d16d1ec96678253c0140938b` |
| worker binary SHA-256 | `6b68434eba12b458dcbda8aa762662a2e62643f0a77680f359403b3887cf71f8` |
| self-test | `tests/reference_decode/test_affinity_protocol.py` (5 cases, portable) |
| compiler / flags | MSVC 19.44.35229.0, `/O2 /Ob2 /DNDEBUG` |
| CRT | dynamic `/MD` |
| CUDA | not linked (`cuda_linked: false`) |
| upstream threading | original defaults: `_THREADED_DECODER=1`, `THREADS_IN_LAST_WAVELET=4` |

The worker gains one optional argument, an affinity mask, applied and verified
**before any encoder, decoder or metadata object is created**. The existing
five-argument form is byte-for-byte unchanged in behaviour and remains the
unrestricted arm.

## Immutable input and decode configuration

| field | fixed value |
| --- | --- |
| packet SHA-256 / size | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` / 900396 B |
| source BGRA SHA-256 | `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1` |
| geometry | 1920x1080, pitch 7680, 8294400 output bytes |
| requested output | BGRA, `CFHD_DECODED_RESOLUTION_FULL`, `CFHD_DECODING_FLAGS_NONE` |
| active metadata | `TAG_PROCESS_PATH = PROCESSING_ALL_ON`, flags 65535 |
| decoder threading | original SDK defaults, no override |

The script rejects any packet/source hash mismatch before the first worker
process starts.

## Arms

The guest exposes eight logical processors in one processor group and one NUMA
node; no SMT sibling pairs are reported. Arms use the lowest-numbered active
logical processors of the guest **system affinity mask**, computed at runtime:

| arm | requested affinity | intent |
| --- | --- | --- |
| A | none | unrestricted control; effective mask must equal the full system mask |
| B | lowest 4 active LPs | reduce scheduling breadth |
| C | lowest 2 active LPs | stronger restriction |
| D | lowest 1 active LP | optional secondary arm; earlier exploratory 1-CPU data (28/30) is historical and is not reused as protocol evidence |

Every worker call reports `affinity_requested`, `affinity_requested_mask`,
`affinity_effective_mask` and `affinity_system_mask`. The protocol declares an
arm **INVALID** if the requested and effective masks differ, if the system mask
changes between calls or arms, or if the launcher itself is already restricted.

## Counts, order and bounds

- Per arm: **3 fresh processes x 5 opens** (primary) plus a pre-registered
  **6 fresh processes x 5 opens** extension, 45 calls total.
- Primary phase order: **A, B, C, D**.
- Extension phase order: **D, C, B, A**.
- Each process is independent and bounded by a **20 s** child timeout.
- No soak, no repeated arm after failure, no host/VM reconfiguration.
- Decode timings are VM observations, not a Ryzen-vs-VM benchmark.

## Per-call reporting and first-difference semantics

Each call records the decoder status, geometry, metadata statuses, affinity
masks, `decode_ns`, pixel and RGB SHA-256, PSNR versus the fixed source,
`versus_process_first` and `versus_arm_first` metrics, and a first-difference
record for both references.

The two first-difference references are deliberately distinct:

- `versus_process_first` compares with the first output of the **same fresh
  process**.
- `versus_arm_first` compares with the first output of the **whole arm**, which
  is the `first_difference` semantics used by the frozen #138 report.

Processes are the independent unit for event rate; the five opens inside one
process are dependent. The report therefore gives per-call, per-process and
per-arm counts and clustering, not a formal Bernoulli significance claim.

## Classification and confirmatory trigger

- Any timeout, affinity failure, decode error, metadata failure or invalid
  geometry => arm `INVALID`.
- More than one pixel digest in an arm =>
  `REFERENCE_DECODER_NONDETERMINISTIC`.
- Uniform output without an encoder packet pair => `UNRESOLVED`.
- A confirmatory rerun is triggered when arm A and a reduced arm differ in
  stability direction, or by at least 5 divergent calls out of 45. The rerun
  uses fresh processes, the same 45-call-per-arm size, the reverse order
  (extension before primary) and a separate output file. It is not an adaptive
  extension of the first sample.

A null affinity result is reported as **NO_EFFECT / INCONCLUSIVE**, not as
evidence that scheduling is irrelevant.

## Host and topology record

Recorded read-only at the start of the run and repeated per call where
available: CPU model, cores/logical processors, hypervisor presence, OS
caption/version/build, processor groups, active logical processors, core masks,
SMT sibling masks, NUMA node masks, launcher and system affinity masks, process
priority class, and page/allocation granularity. No host name, user name,
domain, serial number, credential or local path is recorded.

Known unknowns are published as such, including unavailable physical-core or
NUMA mappings if discovery fails.

## Stop and escalation

Stop an arm on timeout, invalid geometry or hashes, affinity verification
failure, or evidence of conflicting VM CPU load. Publish `BLOCKED` or
`INCONCLUSIVE` rather than a fabricated pass. No hypervisor, vCPU topology,
driver, GPU, priority, service or OS changes are made. The #120 release gate
remains `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## Reproduction

Inputs are supplied privately and must match the hashes above; the work
directory must be fresh.

```powershell
cmake --build build --config Release --target reference_decode_worker --parallel 4
python -B tests/reference_decode/affinity_1080.py --worker build/tests/Release/reference_decode_worker.exe --packet <private-packet.cfhd> --source <private-source.bgra> --work build/issue158/primary --output build/issue158-primary.json
python -B tests/reference_decode/test_affinity_protocol.py
```

Exit 0 means the protocol ran and evidence was written; 2 means invalid
evidence or a bound was exceeded. Divergence does not make the run invalid.
