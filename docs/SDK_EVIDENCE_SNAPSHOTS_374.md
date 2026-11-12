<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under the MIT License or Apache License, Version 2.0. -->
<!-- See LICENSE-MIT and LICENSE-APACHE for details. -->

# Bounded Windows SDK evidence snapshots — issue #374

Tracking: [#374](https://github.com/louis-0hrs/HyperCine/issues/374), continuing
the [fundamental audit #364](https://github.com/louis-0hrs/HyperCine/issues/364).
The parent is published [Draft #373](https://github.com/louis-0hrs/HyperCine/pull/373),
commit `d44528aa49f650c50276710319067f8a7d6381f3`, tree
`268e503fd7111a3f8877663821d427e30c7c5e3b`.

## Problem and repair contract

`tools/verify_windows_sdk_corpus.py` previously verified a transfer index and
its artifacts, then reopened the paths to parse evidence. A same-length packet
replacement after `verify_index()` could reach `checked_frame()` without the
replacement bytes being checked against the accepted SHA-256. The index itself,
manifests, traces and report index digest also had separate verification and
consumption reads. A successful earlier hash therefore did not establish the
integrity of the bytes used for later observations.

The repair binds the index hash and parser to one bounded byte snapshot. Every
manifest, trace and packet consumed later is read within its indexed byte length
and checked against its indexed SHA-256 before interpretation. Hash-only
inventory and provenance reads also have a fixed budget. A file that changes
between inventory verification and consumption is rejected, or an already read
verified snapshot is used. This is an integrity contract for each observation;
it is not a transaction locking the entire corpus against concurrent writers.

Relative corpus reads traverse pinned directory handles. Symlink components
are rejected; a path replaced after opening remains tied to the original
directory handle or is rejected, without reading the replacement. Inventory
enumeration is incremental and permits only indexed files and the directory
prefixes needed to reach them. An unexpected directory, including an empty
unindexed directory, is rejected. This is deliberately stricter than merely
comparing a recursive list of regular files after reading them. Repeated names
are rejected too, so directory churn cannot extend an accepted enumeration past
the finite indexed shape.

## Explicit tool limits

| Evidence | Maximum accepted bytes |
| --- | ---: |
| Transfer index | 8 MiB |
| Manifest or trace consumed as metadata | 4 MiB |
| Parsed packet | 128 MiB |
| Any inventory or provenance artifact hashed by this tool | 128 MiB |

An indexed size is additionally an exact length. Reads use at most 64 KiB per
request and at most one overflow-probe byte beyond the admitted extent. The
probe never reaches a parser. Short reads accumulate; early EOF and growth
beyond the admitted extent fail. Generic zero-length artifacts remain supported
only with their exact zero-length index pin. These limits are verifier policy,
not codec format maxima or a claim that every packet within the limit is valid.

Relative paths contain at most 32 portable ASCII components, each at most 255
characters. Components must satisfy the shared no-traversal/no-reserved-name
rules. Size fields contain at most 20 ASCII decimal digits before conversion;
the byte cap still applies to the resulting integer.

## Public behavior and report ownership

`verify_index(root, count=None, digest=None)` still returns the indexed mapping.
`checked_frame(root, rows, frame, packet_key)` still returns the accepted bytes
and parsed packet. `parse_packet()` retains its packet interpretation contract,
including its existing importer in `tests/observe_ffmpeg_1080.py`. The verifier
uses the shared filesystem primitives introduced by #373; it does not change
them or the core codec.

The command line retains a generic failure diagnostic that does not echo local
corpus paths. Report publication uses a pinned parent, excludes the corpus by
both path and physical ancestry, and commits complete bytes without replacing
an existing destination. A report concurrently created by another process
survives. Incomplete private staging is retained on failure; the verifier does
not delete an unowned path while attempting cleanup.

The parent is reopened after verification and its current identity and ancestry
are checked again. Moving the same directory into the corpus and leaving a
symlink at its previous location therefore fails this check. These are bounded
path-substitution checks, not an operating-system sandbox against a process
with the same user's authority to rearrange arbitrary containing directories
between checks. The shared filesystem primitive retains that explicit boundary.

## Validation

The final source-bound execution receipt is
[SDK_EVIDENCE_SNAPSHOTS_374_20261112.json](validation/SDK_EVIDENCE_SNAPSHOTS_374_20261112.json).
Environment: Linux x86_64, GCC 14.2.0, Python 3.12.14, CMake 3.31.6 and
FFmpeg/ffprobe 7.1.5-0+deb13u1. Native GPU backends were disabled.

| Check | Final result |
| --- | --- |
| Existing and extended verifier tests | 23/23 in each of normal and optimized Python |
| Independent snapshot and report regression tests | 25/25 in each Python mode, zero skips |
| CI inventory guard tests | 35/35 in each Python mode |
| Complete strict standalone CTest | 139/139 PASS; zero failure, error or skip; 375.28 seconds |
| Complete SDK-enabled CTest | 68 PASS and one explicit oracle skip out of 69; zero failure/error; 82.46 seconds |
| CTest registration delta from #373 | Exactly two added in each build mode; none removed |

The SDK skip is `ffmpeg_oracle_qualification`: the installed decoder build is
outside that gate's measured set of n8.0 and 9.0.2. CTest itself returns zero,
but the source-bound qualification summary correctly retains a failed strict
no-skip verdict. This remains an open decoder-reference qualification gate.

Both complete runs used unchanged copies of all 59 code, build and test files
that differ from protected main. The receipt records their SHA-256 values and
the six source files changed in this follow-up. The parser AST and shared
filesystem helper remain unchanged. The reported durations are concurrent local
qualification measurements, not performance benchmarks.

The original #373 verifier was also checked in a disposable copy. A valid
synthetic 30-band packet passes its positive control. Nine selected regression
assertions fail on the old implementation at the intended packet, index,
manifest, trace or report boundary; one further 20,001-byte metadata fixture
produces an actual uncaught `RecursionError`. None of these controls fail because
the old source lacks a newly introduced API. The receipt records the initial
insufficient nesting probes and the final discriminating fixture.

Independent review found and resolved two additional cases before the full
runs: repeated directory entries and relocation of the same report-parent inode
into the corpus. The final source review found no remaining blocker in this
bounded filesystem, snapshot and publication scope.

Portable focused commands:

```sh
python -B tests/test_windows_sdk_corpus.py
python -B -O tests/test_windows_sdk_corpus.py
python -B tests/test_windows_sdk_snapshots.py
python -B -O tests/test_windows_sdk_snapshots.py
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

The shared CMake registration includes `windows_sdk_snapshot_contract` and
`windows_sdk_snapshot_optimized`. Both, and the existing corpus contract, are
mandatory in the strict standalone inventory; dropping a registration must
fail the gate. Tests use small synthetic data and the already public fixtures.
They do not require or mutate a private corpus.

## Remaining work and qualification limits

The P6 replay gates still need their own correction. In
`tools/p6_sdk_gap_gate.py` and `tools/p6_windows_sdk_gate.py`, the encoder receives
original input paths after verification. A complete repair must copy verified
input bytes into an exclusively owned staging directory and pass those copies
to the child process. Reference manifests, traces, packets and transfer indexes
must likewise be interpreted from hash-bound bounded snapshots. Merely bounding
a later `read_bytes()` would leave the child-process handoff unprotected.

Other reader work listed in the parent audit remains open. This patch does not
import the separate #361/#370 lineage or resolve its historical publication
finding. It does not establish native Windows filesystem behavior, original-SDK
capture provenance, decoder qualification, CUDA/Metal/Vulkan support, codec
conformance or release readiness. Frozen `upstream/` remains byte-identical.
