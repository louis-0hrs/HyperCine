<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Wavelet dependency scheduling — issue #144 design proposal

Status: DESIGN REVIEW REQUIRED, 2026-10-18. No production change or qualification.
Scope: milestone 1 of [#144](https://github.com/louis-0hrs/HyperCine/issues/144).
Prefer correct decoding and necessary waits over latency. Never publish an image
that consumed incomplete coefficients, even if its hash repeats.

## Decision and evidence boundary

Propose option C: a HyperCine-owned task graph and portable CPU execution baseline,
integrated with the existing decoder stack only after its owners approve the
boundary. Reject public-SDK-call locking as a sufficient repair. Retain the CUDA
path's explicit stream/event dependencies; this proposal does not establish that
the CUDA decoder has the original SDK's defect. Do not start a competing macOS
decoder or replace the frozen phase stack.

The [#138 report at 9d5f654](https://github.com/louis-0hrs/HyperCine/blob/9d5f65437561e6c1b868c61fc4f6ed073d58ba38/docs/validation/LOWPASS_DEPENDENCY_AB.md)
provides diagnostic evidence for two specific immutable packets. Its experimental
queue intervention is not a production implementation or an independent pixel
oracle. The original #116 failing M4 packet is unavailable; no claim about that
packet's root cause or 1080p remediation follows. No measurements were replayed
for this design. No safety or performance result transfers between source heads.

## Inspected branches and collision boundaries

Issue bodies and recent coordination comments for #62, #13, #16, #2, #47 and #38,
plus live metadata for PRs #12, #90, #83 and #138, were read on 2026-10-18.
The source inventory below uses committed refs, not dirty working files.
Branch observations are a snapshot, not an implementation ownership grant.

| Lane / pinned head | Actual implementation / platform | Ownership boundary |
| --- | --- | --- |
| `main`, `e87ef0258ba37f5c78f1557c59d9c7331df5521c` | Encoder, standalone CPU/Metal encoder, optional original SDK. No `decoder.h`, `hypercine_decoder`, owned inverse decoder or decode scheduler. | Protected main. Metal encode support does not imply Metal decode. |
| #16 phase stack: `decode-phase3af` `9eb9e3d`, `decode-phase3b0` `7296f6f`, `decode-phase3b1` `f1d0fe4`, `decode-phase3b2r` `f433a74`, #12 `decode-phase3b2a` `1d78fc183ea28c4268345521e83fb2bca3226381` | CUDA decoder facade and research pipelines; progressive YUV422 p10 GOP1, interlaced GOP1, progressive FIELDPLUS GOP2. CPU-only builds expose stubs, not a portable full decoder. | #16 integration HOLD; #2 promotion and #13 safety remain separate gates. No modification/rebase of phase branches. |
| #13 / #90 `decode-phase3b2a-issue13-windows-asan`, `8e3561a8747c5e91e9a2e7c7d2921b1272f2a877` | Windows x64 host ASan and actual CUDA instrumentation evidence; upstream temporal oracle overread remains material. | Safety owner; bounded racecheck/synccheck is not unlimited coverage. |
| #38 / #52 `codex/linux-decoder-sanitizer-p0`, `1c34eabeda584ac88ab0624d60d3828600bbf080` | Linux x64 CUDA portability/sanitizer parent; child #80's 43/43 is not a full green parent result. | Shared A2 allocation contention; do not launch duplicate GPU suites or treat isolated retests as full acceptance. |
| #47 / #83 `codex/macos-decoder-compat-audit-p7`, `fd844fd058e0d79d4eabd39769e453c65ea78ba3` | ARM64 readiness docs and external FFmpeg baseline, no native owned decoder. Follow-ups #136/#141 audit JSON robustness. | P7.B blocked by #13/#16 and matched SDK output. |
| #120 / #138 `codex/issue120-windows-evidence`, `9d5f65437561e6c1b868c61fc4f6ed073d58ba38` | Isolated Windows x64 diagnostic and retained immutable-packet evidence. | Diagnosis owner; do not alter or import its private ABI/recursive-lock intervention. |

This docs-only lane is `codex/issue144-wavelet-design`, based on the pinned main.
Before implementation, repeat the live branch/claim check and obtain the #16,
#13/#38 and #47 owners' agreement on files and base SHA. No claim approval has
been inferred from an unassigned issue or lack of a reply.

At #12's committed head, responsibility is divided as follows:

* `src/core/decoder.cpp`: public ticket ownership, slots, request routing,
  lifecycle and error surface; no-CUDA initialization returns BackendUnavailable.
* `src/core/decode_parser.cpp`, `decode_fieldplus_parser.cpp`: supported-profile
  validation and coefficient descriptors. FIELDPLUS control records are not bands.
* `src/core/decode_thread_pool.cpp`: bounded persistent host workers, batch IDs,
  atomic task claiming, first-exception propagation and full batch quiescence.
  Its condition-variable waits and joins have no deadline; it is not yet the
  proposed cancelable dependency scheduler.
* `src/cuda/decode_phase2e_pipeline.cu`: per-slot compressed staging, pinned/device
  planes, band state, channel streams, events and inverse sequencing.
  `CheckAdvanceInverseDag` enqueues level 2 -> 1 -> 0 on a channel stream, waiting
  for LL and highpass events; `GpuQueued`/`l2_enqueued` mean submitted, not complete.
  Color waits for all channel events. FIELDPLUS uses one main stream for entropy,
  w3, w5/w4, temporal inverse, selected base inverse, color and ready publication.
* `src/cpu/upstream_wrapper.cpp` wraps a forward spatial transform, not decoder
  worker scheduling. Original SDK decoding elsewhere calls the public CFHD API.

## Three concrete implementation options

| Option | Concrete boundary | Correctness / cost / decision |
| --- | --- | --- |
| A. External SDK calls | RAII CFHD handle; serialize Prepare/Decode/Close per handle; validate packets before entry. | `upstream/Common/CFHDDecoder.h` exports opaque handles and whole-sample calls, output formats and metadata, but no band completion callback, worker dependency graph or cancellation primitive. Locking outside DecodeSample leaves internal workers racing. A serial SDK build could be investigated separately, but is not proven by these APIs or #138. Reject as the ordering guarantee; output hash repetition alone cannot validate it. |
| B. Private build overlay | Copy pinned SDK sources to a private build tree; patch entropy/reconstruction queue there; separate target and symbol namespace from pristine SDK. | Technically feasible without editing on-disk `upstream/`, but still a fork of third-party scheduling. Must own every selection/publication site, partial-mask mode, cancellation and platform lock mapping, prevent duplicate SDK symbols and requalify all modes. Recursive Windows locking cannot be assumed on Linux. High continuing maintenance/ABI/safety cost; do not select for product runtime. Could remain an explicitly isolated research alternative. |
| C. Owned scheduling and CPU baseline | Private C++17 graph in `src/core/`, portable scalar decode stages in `src/cpu/`, explicit CUDA adapter after contract qualification. | Own dependencies, memory and failure propagation end to end; no dependence on private CSampleDecoder layout. Largest initial effort: existing GPU kernels do not supply a full scalar decoder. Choose as proposed long-term boundary, reuse reviewed parsers/stage contracts after #16 agreement, and keep unsupported routes closed until each stage is independently qualified. No delivery/performance claim. |

## Dependency graph and supported routing

For each frame/slot generation, validate the complete supported topology and
allocation bounds before scheduling. A node key includes session, slot, generation,
channel, transform kind, level, field/frame index and output band. Every input
edge names an actual buffer version and producer; band-start flags are never
completion. Missing/duplicate producers, cycles, dimensions/precision mismatches
and invalid levels reject the packet before execution. A highpass-only node has
only its declared inputs; never globally require a `0x0f` mask.

For spatial GOP1, number levels from finest 0 to coarsest L-1:

```text
parse + validate -> entropy/dequantize LL[L-1] and HP[channel,level,1..3]
LL[L-1] + HP[L-1,1..3] -> inverse[L-1] -> LL[L-2]
LL[l]   + HP[l,1..3]   -> inverse[l]   -> LL[l-1] ... -> channel plane
all required channel planes -> output conversion -> error check -> publish frame
```

Entropy completion includes dequantization, difference coding and peak restoration
where required. A band is not ready at VLC submission. Interlaced GOP1 requires
field-specific geometry/inverse row mapping and final field combine; never let a
field consume the other field's scratch. Preserve the validated profile's mapping.

For progressive FIELDPLUS, preserve #12's logical 17 bands/channel, 54 physical
records / 51 coefficient bands and three index-255 controls. Decode w3's four
inputs (including raw16 temporal-high LL) -> temporal-high plane. Decode w5 ->
w4 lowpass, then w4 -> temporal-low plane. Both planes -> temporal inverse for
the requested frame index -> selected w0/w1 base inverse with its own highpasses
-> channel plane -> conversion/error check. Share no frame0/frame1 cache in the
first implementation. Graph construction must preserve signed math, biases and
frame-specific band selection; task ordering does not establish their correctness.

| Variant | Existing #12 route | Proposed first owned CPU route |
| --- | --- | --- |
| Progressive YUV422 p10 GOP1, three spatial levels | CUDA path, separate promotion gates | First qualification target only; UNIMPLEMENTED until proven |
| Interlaced YUV422 p10 GOP1 | CUDA field-aware inverse | UNIMPLEMENTED initially; explicit unsupported error |
| Progressive YUV422 p10 FIELDPLUS GOP2 | Fixed CUDA Hybrid route, frame_index 0/1 | DAG described above; UNIMPLEMENTED until independent temporal qualification |
| Interlaced FIELDPLUS, other level counts, RGB/RGBA/Bayer | Research descriptors are not product support | UNIMPLEMENTED; no fallback to an unsafe SDK decode |

The graph representation permits L levels, but a route must whitelist a qualified
topology rather than accepting an arbitrary level count. RGB/Bayer future graphs
must declare channel transforms, mosaic/demosaic and alpha dependencies explicitly;
they cannot inherit YUV channel count, chroma geometry or output math.

## State, synchronization and failure contract

Start with a mutex-protected ready queue and node state; no lock-free requirement.
States: Pending -> Ready -> Running -> Completed, with Failed/Canceled terminal
alternatives. Queued/scheduled is never Completed. A unique Pending->Ready and
Ready->Running transition under the graph mutex prevents duplicate execution.
Only nodes whose predecessors completed successfully enter the CPU ready queue.

```text
worker: lock; take Ready node for current generation; mark Running; unlock
        execute bounded stage using exclusively owned output buffers
        lock; validate generation and cancellation; preserve first failure
        on success: mark Completed; decrement each successor's remaining count
                    enqueue successor only when all predecessors succeeded
        on failure: fail frame; cancel non-running successors; notify coordinator
        unlock; notify ready workers / frame waiter
```

Producer writes precede its mutex unlock; the consumer's subsequent lock supplies
the happens-before relation. If a later optimization uses atomics, publication
requires a release store after all writes and acquire load before any consumer
read, plus a separately proven duplicate/queue/lifetime protocol. Relaxed flags
alone are insufficient. Never hold the queue lock while running kernels or
waiting for other work. Workers skip unresolved work; no worker waits for a
producer occupying the same finite pool. Empty ready queue with no running task
and unfinished nodes is an immediate invalid-dependency failure.

GPU adapter: a recorded event plus matching generation is an enqueued dependency
token, not host completion. Stream waits must precede dependent kernels; event
record/wait/launch errors fail the frame. Do not wait on an unrecorded event or
reuse it for another generation. Host publication waits for terminal event AND
checked device/host errors. CPU-produced input requires a complete upload/event;
GPU-produced CPU input requires completed download/event. C++ atomics do not
replace CUDA device synchronization.

Use per-frame deadline and cooperative cancellation checks inside bounded CPU
loops and between GPU submissions. On timeout, reject output immediately, cancel
unstarted nodes and drain running tasks before releasing their storage. C++17
cannot safely kill a stuck worker; a deadline on the existing ExecuteBatch wait
alone would cause use-after-free. Hard bounded termination of a noncooperative
SDK/driver requires a supervised process with kill/reap, or an unavailable backend
route; in-process product support remains blocked if bounded teardown cannot be
demonstrated. Never return timed-out buffers to the pool while workers/GPU use them.

Retain the first causal error (parse, missing input, allocation, stage, timeout,
cancel, device). Cancel dependents and never expose a successful image ticket.
Retire a slot only after all CPU workers, device work and registered output
consumers release it. Increment a checked generation before reuse; reject stale
completion/cancel/release messages and handle generation exhaustion by retiring
the session. Scratch is frame/channel-owned, with 16/32-byte aligned allocations
and pitches divisible by 16, checked size arithmetic and RAII lifetimes.

## Minimal future change surface and qualification

This milestone changes this document only. Proposed future files, not APIs that
already exist on main: `src/core/decode_task_graph.{h,cpp}` (private graph),
`src/cpu/decode_scalar.cpp` (reviewed bounded stages), and
`tests/test_decode_task_graph.cpp` (standalone deterministic harness).
Use C++17 standard library only; private CMake target with CMake 3.25+/CTest.
Keep public API changes in a separately reviewed #16-owned contract update.
Initially qualify the scheduler without SDK or CUDA. Do not merge #138's overlay,
private decoder access, recursive locks or diagnostic JSON into runtime targets.

After a separate technical go-ahead, implementation acceptance requires:

1. Deterministic synthetic delayed/failed producers, duplicate scheduling,
   highpass-only nodes, missing LL, cycles, allocation failures, cancellation,
   deadline and stale generation callbacks; worker counts 1/2/4/16, multiple
   slots/sessions, reverse release and cross-frame reuse. Verify producer writes,
   not only event timestamps. Assert finite pool progress and no premature publish.
2. Reuse the #138 immutable packet/source identities and bounded subprocess
   harness concepts as test-only fixtures. Resolve authorized packet bytes before
   tests; missing packet is BLOCKED_REFERENCE, never regeneration or PASS. Pin
   128 packet SHA `60ed81a3f164d585ae4d3dc0c4af4154a626cef12311a61b4d2dcca80be04b14`
   and 256 SHA `7655842a920a1d2f35f1cf568b21e99232004ce2a71e9ea5f4cf708da586f26e`.
   Verify strict byte identity before/after every decode, repeated RGB hashes,
   producer readiness, canonical source PSNR >35 dB and independent output checks.
   Stable wrong output, API error, packet mismatch or silently converted failure
   must fail. Keep encoder packet parity and decoder pixel parity separate.
3. Qualify scalar stages against sufficiently verified original SDK outputs at
   matched pixel format/range/matrix/pitch/orientation and independent coefficient
   references; the original nondeterministic decode cannot be the sole oracle.
   FFmpeg triangulation needs explicit format/range/matrix and precision limits.
   Corrupt/truncated packets, flags, all accepted GOPs and rejected variants are
   mandatory; each unimplemented route must return unsupported.
4. Native Windows x64; Linux x64 ASan/UBSan and TSAN for host graph where supported;
   CUDA memcheck/initcheck/racecheck/synccheck on actual launches after host gates,
   coordinated with #38's resource window. macOS ARM64 only once the owned target
   exists. Hosted portability tests without SDK must report original-oracle tests
   unavailable, never claim native SDK equivalence. No implicit SDK fallback.
5. Authentic #116 1080p packet A/B remains UNTESTED/UNAVAILABLE pending private
   authenticated handoff. Small 4K/8K preflight only after memory budget, bitstream
   and safety approval. Correctness precedes benchmark_codec comparison with
   `benchmarks/results/reference_windows_msvc.json`: FPS, MP/s, avg/min/max latency
   and peak memory, preserving unexplained regressions as blockers.

## Review decision required

Independent review must approve or amend option C, the first profile, owner/base
SHA and cancellation/teardown boundary before any production implementation.
Specifically resolve coexistence with #16/P7.B, scalar entropy/inverse ownership,
qualified oracle availability, and whether process isolation is required for
hard deadlines. #13 memory safety and #2 promotion remain independent HOLD gates.
This design does not close #144 or authorize a merge/release.

## Validation of this documentation change

Native Windows x64, MSVC 19.44.35229, pinned main base, CPU/SDK Release build:
`cmake -S . -B build -G "Visual Studio 17 2022" -A x64
-DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON
-DHYPERCINE_BUILD_BENCHMARKS=OFF`, followed by
`cmake --build build --config Release --parallel 4` and
`ctest --test-dir build -C Release --output-on-failure`.
All 10 registered tests passed, zero skips, 1.91 seconds. This verifies the
unchanged baseline configuration, not the proposed scheduler or decoder safety.
No CUDA workloads, #120 A/B replay or performance qualification was run.
Staged privacy scan and diff whitespace check passed; `upstream/` is unchanged.
Independent architecture review remains pending.
