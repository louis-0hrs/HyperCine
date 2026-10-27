# EPYC VM guest-affinity results — issue #158

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

Status: **protocol executed as pre-registered; result INCONCLUSIVE / NO
MATERIAL AFFINITY EFFECT**. Every arm remained nondeterministic. The #120
release gate stays `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## What was measured

`tests/reference_decode/affinity_1080.py` ran the pre-registered derivative
protocol described in
[`EPYC_VM_AFFINITY_PROTOCOL.md`](EPYC_VM_AFFINITY_PROTOCOL.md) against the
immutable 1080p BGRA packet and source:

| field | value |
| --- | --- |
| packet SHA-256 | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` |
| source BGRA SHA-256 | `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1` |
| protocol script SHA-256 | `23c4f532c3c19a6c31244dea005be9e2929f19e0d3795b995744ae26d7b8ab48` |
| worker binary SHA-256 | `6b68434eba12b458dcbda8aa762662a2e62643f0a77680f359403b3887cf71f8` |
| source commit | `fadf99a47d40916536fde2fc655bb7fd2044c99c` |
| compiler / CRT | MSVC 19.44.35229.0, `/O2 /Ob2 /DNDEBUG`, dynamic `/MD` |
| CUDA | not linked |
| calls | 180 (4 arms x 45), 9 fresh processes per arm |
| child bound | 20 s; no timeout occurred |

The worker set and verified affinity **before** creating any SDK object. All
180 observations reported `CFHD_ERROR_OKAY`, valid 1920x1080 BGRA geometry,
pitch 7680, 8294400 output bytes, metadata Open/Init/Set status 0 and
`PROCESSING_ALL_ON` 65535.

## Affinity verification

| arm | requested | effective, all 45 calls | system mask |
| --- | --- | --- | --- |
| A | none (unrestricted) | `0xff` | `0xff` |
| B | `0x0f` (4 LP) | `0x0f` | `0xff` |
| C | `0x03` (2 LP) | `0x03` | `0xff` |
| D | `0x01` (1 LP) | `0x01` | `0xff` |

No affinity request was silently ignored and the system mask did not change
between arms.

## Guest and build configuration

| dimension | value |
| --- | --- |
| host type | QEMU virtual machine, hypervisor present |
| CPU | AMD EPYC 7601, guest sees 8 cores / 8 logical processors |
| processor groups / SMT | 1 group, 8 core masks `0x01`..`0x80`, no SMT sibling pairs |
| NUMA | 1 node, mask `0xff` |
| OS | Windows 11 Pro, build 26100 |
| worker threading | original SDK defaults, `_THREADED_DECODER=1`, `THREADS_IN_LAST_WAVELET=4` |

The host vCPU topology, physical-core mapping and hypervisor scheduler were
**not** changed.

## Results

`divergent calls` compares each call against its arm's modal digest;
`divergent processes` counts fresh processes containing more than one pixel
variant.

| arm | logical processors | calls | divergent calls | divergent processes | distinct hashes | PSNR range, dB | decode median / max, ms |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A unrestricted | 8/8 | 45 | 6 (13.3%) | 5/9 | 7 | 13.660250986112796 - 44.465986143258924 | 21.217 / 32.0898 |
| B | 4 | 45 | 4 (8.9%) | 2/9 | 5 | 14.531633802628793 - 44.465986143258924 | 20.6177 / 27.1961 |
| C | 2 | 45 | 3 (6.7%) | 2/9 | 4 | 14.388078808005728 - 44.465986143258924 | 26.7432 / 49.2024 |
| D | 1 | 45 | 10 (22.2%) | 5/9 | 11 | 11.831183588944674 - 44.465986143258924 | 41.7593 / 56.5015 |
| combined | - | 180 | 23 (12.8%) | 14/36 | 24 | 11.831183588944674 - 44.465986143258924 | - |

The modal reconstruction is
`c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0` in
**157/180** calls (87.2%) and is identical to the modal output reported by
#116 Handoff 5 and the #120 Windows arm. Decode timings are VM observations,
not a performance result; the 1-LP arm is slower because it has one logical
processor.

## Process clustering and first-difference semantics

Per-process divergent calls (hash different from the arm modal):

| arm | processes with divergence | distribution over processes 1-9 |
| --- | --- | --- |
| A | 5/9 | 0,1,1,0,1,0,1,2,0 |
| B | 2/9 | 1,3,0,0,0,0,0,0,0 |
| C | 2/9 | 0,2,1,0,0,0,0,0,0 |
| D | 5/9 | 2,1,0,0,2,4,1,0,0 |

Primary 3x5 phase contributed 12/60 divergent calls (20.0%) and the extension
6x5 phase 10/120 (8.3%). Arms and phases ran sequentially and processes are
the independent unit, so this difference is descriptive and is not a temporal
or warm-up claim.

First-difference counts show why both reference fields are required:

| arm | calls differing from process-first | calls differing from arm-first |
| --- | --- | --- |
| A | 8 | 6 |
| B | 8 | 44 |
| C | 5 | 3 |
| D | 14 | 44 |

In arms B and D the first output of the whole arm was itself a non-modal
variant, so nearly every later call differs from `arm_first` even when it
matches that process's first decode. The frozen #138 `first_difference`
semantics compare against the arm-first buffer; this report records both.

Divergent outputs were localized and large: changed-byte counts ranged from
53,079 to 4,184,803, maximum absolute deltas from 75 to 255, and PSNR from
11.831 to 34.583 dB. Examples of first divergence coordinates versus the same
process's first output include `(0, 706, B)` with reference 0 -> 20, `(1, 14,
G)`, `(0, 0, G)` with 0 -> 68, and `(4, 902, G)`. No divergent call returned a
non-zero API status.

## Decision

The pre-registered confirmatory trigger was **not met**: arm A differed from
B, C and D by 2, 3 and 4 divergent calls respectively (threshold: at least 5),
and every arm was nondeterministic in the same direction. No confirmatory
rerun was started.

Therefore:

- Affinity restriction alone did **not** remove the successful-but-corrupt
  output in any arm, including 2 and 4 logical processors.
- The 1-LP arm's higher point estimate (10/45) is consistent with the
  historical exploratory 1-CPU observation but was not reproduced at its
  historical 28/30 scale; that historical number used different worker
  source/protocol and is not protocol evidence.
- Process-level event rates (A 5/9, B 2/9, C 2/9, D 5/9) show clustering, not
  a clean monotonic affinity effect.
- The result is **INCONCLUSIVE / NO MATERIAL AFFINITY EFFECT**, not evidence
  that scheduling is irrelevant. Host CPU/topology, VM scheduling and other
  unrecorded runtime dimensions remain possible contributors; this is still
  not a Maxwell/CUDA defect claim.

A descriptive one-sided Fisher comparison of divergent calls (not a formal
inference because opens within a process are dependent) gives p ~ 0.37
(A vs B), 0.24 (A vs C) and 0.20 (D vs A).

## Limits

- One Windows VM host; no Ryzen control was rerun under this derivative
  binary and mask semantics.
- No hypervisor, vCPU topology, host affinity, driver, GPU, priority or OS
  changes were made.
- 45 calls per arm; zero divergences would not have established absence, and
  the observed rates do not support a precise effect size.
- No encoder-packet pair, 4K/8K, sanitizer, Adobe/Resolve, FFmpeg or
  performance qualification.
- Raw decoded buffers and unredacted local evidence remain off Git; the
  published JSON contains only hashes, statuses, metrics, sanitized host
  metadata and affinity/topology values.

## Reproduction

```powershell
cmake --build build --config Release --target reference_decode_worker --parallel 4
python -B tests/reference_decode/affinity_1080.py --worker build/tests/Release/reference_decode_worker.exe --packet <private-packet.cfhd> --source <private-source.bgra> --work build/issue158/primary --output build/issue158-primary.json --arms A,B,C,D --processes 9
```

The measured document is committed as
`docs/validation/reference_decode_epyc_vm_affinity_20261018.json`.
