# CPU-affinity and SMT-topology A/B/C/D result — #157

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

**Status: REVIEW. Outcome `NO_EFFECT_DETECTED` in this sample; release blocked.**
Draft PR: [#164](https://github.com/louis-0hrs/HyperCine/pull/164). Cross-links:
[#116](https://github.com/louis-0hrs/HyperCine/issues/116),
[#161](https://github.com/louis-0hrs/HyperCine/issues/161),
[#158](https://github.com/louis-0hrs/HyperCine/issues/158).

## Run identity

| field | value |
| --- | --- |
| issue | [#157](https://github.com/louis-0hrs/HyperCine/issues/157) |
| protocol | `topology-affinity-ab-1080-bgra-full-20261018`, version 1 |
| pre-registration commit | `a16237536a9602cbb6f42d49c5139766facb842d` |
| frozen #138 base | `97c34500324b550da5f6444fe9d28c8729dea8d2` |
| A/B/C manifest SHA-256 | `40aa5da85dbd0c28b8e936e003e7665fbdf801859530a2f29bc9d448f53c047b` |
| D manifest SHA-256 | `f4e8f09ebc1b2906f3aeac050b08ff29a3b9fd2f7f46488a1e8a8ed56bb85883` |
| worker binary SHA-256 | `246b71c16a1b3cb442e946f9c0cbdad38b8957e0b92fe07994f357ffff85241e` |
| launcher binary SHA-256 | `717ccf82909ec1df5c56869475143c577704771af44d6ac1697c981e7cad829b` |
| protocol script SHA-256 | `62508d3c2198e96204d9dcdf0e8f7ee41e5afbbf6b615c78b5f59b2a8dcfd0ad` |

The worker is the frozen #138 Windows binary, reused byte-for-byte. The
launcher is the only new executable. The #138 worker source, script and report
are unchanged, and `git status --porcelain upstream/` is empty.

## Host and topology

| field | observed value |
| --- | --- |
| CPU | AMD Ryzen 9 7940HS w/ Radeon 780M Graphics |
| physical cores / logical processors | 8 / 16 |
| processor groups | 1 |
| system affinity mask | `0xffff` |
| core masks | `0x3`, `0xc`, `0x30`, `0xc0`, `0x300`, `0xc00`, `0x3000`, `0xc000` |
| SMT | two siblings on every physical core |
| topology source | `GetLogicalProcessorInformation` |
| OS / compiler / CRT | Windows 11, MSVC 19.44.35229.0, `/O2 /Ob2 /DNDEBUG`, dynamic `/MD` |
| CUDA / GPU | not linked (`cuda_linked: false`) |

The launcher cross-checks the `RelationProcessorCore` coverage against the
system affinity mask and falls back to the legacy topology API when the Ex
coverage is incomplete. The recorded source identifies the accepted map; the
eight masks partition `0xffff` exactly. Multi-group hosts are rejected rather
than guessed.

## Protocol

Arms were placed before any SDK decoder creation: the launcher creates the
worker suspended, applies `SetProcessAffinityMask`, reads back the effective
mask with `GetProcessAffinityMask`, and refuses to resume on any mismatch. A
failed placement is INVALID, never an unrestricted run.

| arm | placement | requested mask | processes | calls |
| --- | --- | --- | --- | --- |
| A | unrestricted 16 LP | `0xffff` | 9 | 45 |
| B | one SMT sibling from each of 8 cores | `0x5555` | 9 | 45 |
| C | both SMT siblings from 4 cores | `0x00ff` | 9 | 45 |
| D | one sibling from each of 4 cores (post-review optional) | `0x0055` | 9 | 45 |

A/B/C used the pre-registered balanced order
`ABC BCA CAB CBA ACB BAC ABC BCA CAB`; D was added only after reviewing A-C and
has its own manifest. Each arm contains 3 primary processes x 5 opens and a
predefined 6-process x 5-open extension. Four unrestricted baseline probes
(1 process x 5 opens) ran across the A/B/C sequence; two ran across D.

## Observations

| arm | status | distinct pixel hashes | distinct RGB hashes | divergent calls | per-call rate | divergent processes | per-process rate | PSNR, dB | decode median / max, ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A | STABLE | 1 | 1 | 0/45 | 0.0 | 0/9 | 0.0 | 44.465986143258924 | 2.9162 / 4.4479 |
| B | STABLE | 1 | 1 | 0/45 | 0.0 | 0/9 | 0.0 | 44.465986143258924 | 3.1778 / 4.7844 |
| C | STABLE | 1 | 1 | 0/45 | 0.0 | 0/9 | 0.0 | 44.465986143258924 | 3.3847 / 3.8716 |
| D | STABLE | 1 | 1 | 0/45 | 0.0 | 0/9 | 0.0 | 44.465986143258924 | 3.8949 / 4.9571 |
| baseline A-C | STABLE | 1 | — | 0/20 | 0.0 | — | — | — | — |
| baseline D | STABLE | 1 | — | 0/10 | 0.0 | — | — | — | — |

Every one of the 180 arm calls and 30 baseline calls returned
`CFHD_ERROR_OKAY`, valid 1920x1080 geometry, pitch 7680 and 8294400 output
bytes. Every arm and both baseline groups produced the same pixel SHA-256:

```
c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0
```

The BGR-only hash was also identical in all four arms:

```
843b93ba3c3c770d89932658ce0f311ff7a385f3da0f97795fc938a2c70aa8b5
```

The packet SHA-256 was `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52`
and the source BGRA SHA-256 was
`45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1`
in every call. No call produced a differing coordinate; both
`first_difference_vs_arm_first` and `first_difference_vs_process_first` are
null throughout.

## Outcome

`NO_EFFECT_DETECTED` for this packet and host: limiting the process to one
sibling per physical core, to both siblings of four cores, or to four logical
processors did not produce decoder-output divergence, and all arms agreed with
the unrestricted baseline on the exact modal output.

This is a bounded negative observation, not proof of absence:

- 45 calls per arm across 9 fresh processes; calls within one process are
  dependent, so the per-process sample is only 9 events per arm.
- One immutable 1080p BGRA FilmScan3 packet, one frozen worker lineage and one
  host. No significance test or causal claim is made.
- The EPYC VM's 4/45 divergence and the #138 lowpass-dependency evidence are
  not explained by this result. Affinity and SMT placement remain possible
  factors on other hosts, but this host did not reproduce them.
- No encoder-packet pair was measured; `encoder_packets_equal` stays `null`.
- Release gate remains `UNRESOLVED`; `release_blocked` is true.

## Verification

The new branch passed the full local suite:

```
ctest --test-dir build -C Release --output-on-failure
```

13/13 tests passed. The affinity launcher is an `EXCLUDE_FROM_ALL` test target
and links no codec or GPU target.

## Reproduction

The pinned inputs must match the hashes above; the work directory must be
fresh. The worker path is the frozen #138 binary.

```powershell
cmake --build build --config Release --target reference_decode_affinity_launcher --parallel 4
python -B tests/reference_decode/topology_ab.py `
  --worker <frozen #138 worker> `
  --launcher build/tests/Release/reference_decode_affinity_launcher.exe `
  --packet <pinned packet> --source <pinned source> `
  --work build/topology-157-abc --output build/topology-157-abc.json `
  --arms A,B,C --build build
python -B tests/reference_decode/topology_ab.py `
  --worker <frozen #138 worker> `
  --launcher build/tests/Release/reference_decode_affinity_launcher.exe `
  --packet <pinned packet> --source <pinned source> `
  --work build/topology-157-d --output build/topology-157-d.json `
  --arms D --build build
```

Sanitized evidence:

- `reference_decode_topology_ab_20261018.json` — A/B/C calls, summaries,
  baseline probes and both manifests' hashes.
- `reference_decode_topology_ab_manifest_20261018.json` — A/B/C manifest.
- `reference_decode_topology_ab_d_20261018.json` — D calls and summary.
- `reference_decode_topology_ab_d_manifest_20261018.json` — D manifest.

Raw pixel buffers and per-run stdout/stderr stay local.

## Next action

Report this exact-head result to #157, #116 and the #161 campaign. The sibling
#158 EPYC-VM lane should use the same mask semantics and record whether its
guest exposes a discoverable physical sibling map. A stable result here does
not unblock #120 or the release gate.
