# CPU-affinity and SMT-topology A/B/C protocol — #157

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

**Status: pre-registered before measurement; results are recorded separately.**

## Scope and ownership

This is a test-only derivative of the frozen #138 matched-SHA 1080p BGRA
protocol. It asks whether process affinity and physical-core/SMT placement
change the original GoPro decoder's output nondeterminism on the native
Windows Ryzen 9 7940HS host (8 physical cores / 16 logical processors).

It does **not** change `upstream/`, the production decoder, the #138 worker,
the #138 script, the #138 report, CUDA/GPU code, host settings, drivers or any
release gate. The frozen #138 source head is
`97c34500324b550da5f6444fe9d28c8729dea8d2`.

## Frozen input and decoder contract

| field | fixed value |
| --- | --- |
| packet SHA-256 | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` |
| source BGRA SHA-256 | `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1` |
| packet size / geometry | 900396 bytes, 1920x1080 |
| requested output | BGRA, `CFHD_DECODED_RESOLUTION_FULL`, `CFHD_DECODING_FLAGS_NONE` |
| active metadata | `TAG_PROCESS_PATH = PROCESSING_ALL_ON`, flags 65535 |
| decoder binary | frozen #138 worker, byte-for-byte reuse |
| decoder threading | original SDK defaults, no override |
| calls per process | 5 opens, fresh decoder each call |
| child bound | 20 s wall clock, process terminated and reaped on timeout |

The worker is the frozen #138 binary whose SHA is recorded in the manifest.
Reusing it avoids a compiler/path rebuild difference between the affinity arms.
The new launcher is the only executable introduced by this protocol.

## Process placement

The launcher creates the worker with `CreateProcessW(..., CREATE_SUSPENDED)` so
no worker code, and therefore no SDK decoder, runs before placement. It then:

1. applies `SetProcessAffinityMask` when a mask is requested;
2. reads back the effective process and system masks with
   `GetProcessAffinityMask`;
3. continues only if the effective mask equals the requested mask and is a
   subset of the system mask;
4. otherwise terminates the still-suspended child and reports the run INVALID.

The unrestricted arm applies no explicit mask but still verifies that the
effective mask equals the full single-group system mask. A restricted launcher
process is therefore reported as INVALID rather than silently measured as
unrestricted.

Topology is discovered in a separate launcher mode with
`GetLogicalProcessorInformationEx(RelationProcessorCore)` and cross-checked
against `GetLogicalProcessorInformation` when the Ex coverage does not
partition the system affinity mask. The recorded `topology_source` names the
API that produced the accepted map. Multi-processor-group hosts are rejected
rather than guessed.

## Arms and run order

| arm | requested placement | expected logical processors |
| --- | --- | --- |
| A | unrestricted single-group system mask | 16 |
| B | one SMT sibling from each physical core | 8 |
| C | both SMT siblings from four physical cores | 8 |
| D | optional one sibling from four physical cores, only if selected after A-C review | 4 |

B and C are computed from the discovered core masks. If the sibling map is
missing, a mask would not be exactly the required width, or the core count does
not match the arm, the arm is marked `BLOCKED_TOPOLOGY`; no mask is invented.

Each selected arm receives **3 fresh processes × 5 opens** (primary) and a
predefined **6 fresh processes × 5 opens** extension, 45 calls per arm. The
three-arm run order is a balanced Latin sequence:

```
ABC BCA CAB CBA ACB BAC ABC BCA CAB
```

Each arm occupies each position in the three-process cycle three times. Calls
within a process are dependent; the sequence balances slow host drift without
treating processes as independent Bernoulli trials.

A fresh unrestricted 1-process × 5-open baseline probe runs before the first
process and after every nine process runs, so baseline stability is observed
between the interleaved arm blocks.

## Evidence and first-difference semantics

Every call records decoder API status, output validity, geometry/pitch, decode
time, wall time, requested and effective affinity, pixel and BGR SHA-256, PSNR
against the fixed source, and the first differing coordinate. Both of these
reference fields are recorded separately:

- `first_difference_vs_arm_first`: against the first call of the whole arm;
- `first_difference_vs_process_first`: against the first call of the same
  process.

`arm_first_process`, `arm_first_ordinal` and `arm_first_run_position` identify
the arm reference. The #138 `first_difference` field is not reused with an
ambiguous meaning.

Per-arm reporting includes per-call divergence from the arm modal hash and a
per-process event rate (processes with more than one pixel hash), because
within-process calls are not independent. No significance test is claimed
without its independence assumptions.

## Classification

- affinity not applied, timeout, decoder error, invalid geometry or missing
  output => arm `INVALID`; a baseline probe with the same failures makes the
  run inconclusive;
- more than one pixel hash within an arm => `NONDETERMINISTIC`;
- one stable hash per arm with the same modal hash => `NO_EFFECT_DETECTED` in
  this sample only;
- one stable hash per arm but different modal hashes =>
  `OBSERVED_AFFINITY_DEPENDENT_OUTPUT`;
- nondeterministic and stable arms together =>
  `OBSERVED_AFFINITY_ASSOCIATED_DIVERGENCE`, an association rather than proof
  of causation.

A zero-divergence result in 45 calls per arm does not establish absence of the
bug. `release_blocked` stays true and the release gate stays `UNRESOLVED`.

## Artifacts

- `tests/reference_decode/affinity_launcher.cpp` — test-only launcher/topology
  probe; links no codec or GPU target.
- `tests/reference_decode/topology_ab.py` — pre-registered protocol and
  classifier.
- `build/.../manifest.json` — generated before the first decoder call, with
  script/launcher/worker/source hashes, topology, masks and run order.
- sanitized versioned JSON and Markdown evidence under `docs/validation/`; raw
  buffers and per-run logs stay local.

## Reproduction

Build the launcher and reuse the frozen worker:

```powershell
cmake --build build --config Release --target reference_decode_affinity_launcher --parallel 4
python -B tests/reference_decode/topology_ab.py `
  --worker <frozen #138 worker> `
  --launcher build/tests/Release/reference_decode_affinity_launcher.exe `
  --packet <pinned packet> --source <pinned source> `
  --work build/topology-157 --output build/topology-157.json --build build
```

The packet and source SHAs are verified before a work directory is created.
Exit 1 means evidence was collected but the observed outcome is not
`NO_EFFECT_DETECTED`; exit 2 means invalid evidence or protocol failure.

## Limits

- One native Windows host, one 1080p BGRA FilmScan3 packet and one frozen
  worker lineage. No cross-host attribution.
- CPU-only; no CUDA, GPU, sanitizer, 4K/8K, Adobe/Resolve or second-decoder
  qualification.
- No encoder-packet pair; `encoder_packets_equal` stays `null`.
- Affinity is applied at process level only; per-thread placement inside the
  SDK is not controlled.
