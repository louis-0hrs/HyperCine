<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Five macOS validation fixes: independent qualification

Date: 2026-10-18. Base: main `e094682`.
This deliverable reviews five pending fixes at pinned source commits and adds
independent adversarial tests. It does not integrate their branches or clear
their parent feature gates. Existing owner worktrees were not modified.

| Issue | Reviewed PR / exact source | Independent checks | Remaining gate |
| --- | --- | --- | --- |
| [#23](https://github.com/louis-0hrs/HyperCine/issues/23) | [#151](https://github.com/louis-0hrs/HyperCine/pull/151), `6e6fe42734f8f2dd9900aa89ce7a1ff8e4448b4c` | Trace fields, bool/float/string aliases, table/band cardinality, altered coefficient digest and missing/extra frames | Actual private SDK packet corpus and unquantized intermediates were not supplied; no new original-packet parity claim |
| [#44](https://github.com/louis-0hrs/HyperCine/issues/44) | [#152](https://github.com/louis-0hrs/HyperCine/pull/152), `070359edee1ba748614bcb821139120e4f7312ec` | Invalid p10 values on both sides, storage lengths, known finite/infinite PSNR and rejection below 35 dB | P4 API acceptance and original SDK 4K packet parity remain open |
| [#45](https://github.com/louis-0hrs/HyperCine/issues/45) | [#148](https://github.com/louis-0hrs/HyperCine/pull/148), `89ddd8a221d487852dd50ffe8ca54bcb406f07a6` | Corrupt padding estimate under Python `-O`; independent 1080p resident-byte algebra and 42-buffer inventory | Memory estimates only; no Metal device allocation, 4K GPU execution or P5 start |
| [#46](https://github.com/louis-0hrs/HyperCine/issues/46) | [#149](https://github.com/louis-0hrs/HyperCine/pull/149), `a1d6e77f0be39603df9c0171760beb95250be3fe` | Real CMake rejects four incompatible requests using `TRUE`; tests-OFF yields zero registered tests | P6 full-packet profile adapter still depends on accepted P4/P5 interfaces |
| [#47](https://github.com/louis-0hrs/HyperCine/issues/47) | [#150](https://github.com/louis-0hrs/HyperCine/pull/150), `77b8715fd702043bc04863c9bbf975540aa95cf2` | Exact geometry types, byte extents, p10 range and independent complete/plane SHA-256 checks | P7.B requires decoder safety/mainline and matched original SDK pixels |

No new blocking finding in these five bounded validation fixes. Review their
parent stacks in order; this report grants no merge or production approval.

## Native execution

Native Apple ARM64, macOS 26.5.2, AppleClang 21.0.0, CMake 3.31.5,
Python 3.9.6, independent FFmpeg n8.0. All tests use `PYTHONOPTIMIZE=1`;
the new independent probes also explicitly invoke Python `-O`.

Full CTest runs, without skips:

| Exact source | Configuration | Result |
| --- | --- | --- |
| #151 | SDK-free Release, required external FFmpeg decode | 30/30 PASS |
| #152 | P4 SDK-free Release, required external FFmpeg decode | 31/31 PASS |
| #148 | Standalone contract project, with optional P7 negative tests enabled against #150 | 6/6 PASS |
| #149 | Shared quality-state-only Release | 3/3 PASS |
| #150 | P7.A standalone audit with actual public fixture decode | 3/3 PASS |
| This branch | Independent five-issue probe project | 5/5 PASS |

Total: **78/78 registered CTest cases passed across six projects**. The suites
overlap; this is not 78 distinct codec algorithms. #151/#152 include the
16/120-frame manifest gates, not private binary-corpus tests. #152 also executes
19 real 4K independent FFmpeg decodes and verifies sixteen ordered sequence
frame hashes. Minimum observed p10 PSNR: **52.32134949615484 dB**. #150 replays
24 compressed packets and 24 complete decoded public-fixture frames.

These are Release runs. No new sanitizer, Windows, CUDA, Vulkan or Metal GPU
execution is claimed. Two CPU suites overlapped; elapsed times are validation
durations, not performance measurements. Raw tool logs, generated media,
machine paths and CMake caches remain outside Git.

The new probes also fail against three immediate pre-fix parents:

| Fix | Parent | Observed failure |
| --- | --- | --- |
| #148 | `59da896391137d50ba147df617c763e40f51ddfb` | Damaged padding estimate accepted under `-O` |
| #149 | `e87ef0258ba37f5c78f1557c59d9c7331df5521c` | Four incompatible configurations accepted; tests-OFF still registers tests |
| #150 | `8f547c6719f4ec7a529764d0b0525b5ee9f9afcf` | Invalid geometry types raise incidental TypeError instead of the checked ValueError contract |

For #151/#152 the extracted validation functions are new APIs, so absence of
those functions on their parents is not presented as a reproduced acceptance
bug. Positive and adversarial tests were run against the actual fixed heads.

## Reproduce the independent probes

Privately configure the five `HYPERCINE_REVIEW_*_ROOT` variables to directories
containing the exact tracked source snapshots listed above. For example,
extract `git archive <exact-commit>` into the corresponding directory. No
private corpus or GPU is required for these five tests. A missing source root
is a configuration failure; no issue is silently skipped.

```sh
cmake -S tests/macos_pr_review -B build/macos-pr-review \
  -DHYPERCINE_REVIEW_23_ROOT="$HYPERCINE_REVIEW_23_ROOT" \
  -DHYPERCINE_REVIEW_44_ROOT="$HYPERCINE_REVIEW_44_ROOT" \
  -DHYPERCINE_REVIEW_45_ROOT="$HYPERCINE_REVIEW_45_ROOT" \
  -DHYPERCINE_REVIEW_46_ROOT="$HYPERCINE_REVIEW_46_ROOT" \
  -DHYPERCINE_REVIEW_47_ROOT="$HYPERCINE_REVIEW_47_ROOT"
ctest --test-dir build/macos-pr-review -C Release --output-on-failure
```

For full #151/#152 suites, configure each respective snapshot with standalone
ON, upstream/CUDA/Metal/benchmarks OFF, tests ON, Release, arm64 and
`HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON`. Supply existing FFmpeg/ffprobe through
PATH or their CMake cache variables, build, then run the full CTest suite with
`PYTHONOPTIMIZE=1`. #148 uses `tests/macos_contracts` and
`HYPERCINE_P7_AUDIT_ROOT`; #149 uses quality-state-only ON; #150 uses
`tests/macos_decode_portability` with existing FFmpeg/ffprobe. These commands
preserve the original feature/reference boundaries in the table above.

Next action for each issue: review the respective focused fix for ordered
integration. Keep all five parent issues open for their remaining gates.
