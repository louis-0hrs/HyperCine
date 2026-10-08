<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under the MIT License or Apache License, Version 2.0.
See LICENSE-MIT and LICENSE-APACHE for details. -->

# Decode Phase 2F-B: parallel intra-band FSM research

Research date: 2026-10-08. Branch: `decode-phase2fb`.
Base: `773669cbbcccda8aedd5c5b968f7649df0fa5de3`, final Phase 2F-A HEAD.
Implementation commit: bc60373c1d9653c5901b94150d3efb853518c2ae.
This report and its artifacts are recorded by the subsequent documentation commit.

**Outcome A: the B/F hybrid passes the isolated critical-band gate.**
For 4K noise band 6, the median of three run means is **2.635 ms**, with
**851,072 bytes (0.812 MiB) scratch**, exact coefficients and exact restarts.
Band 7 is **2.818 ms** and **852,432 bytes**. These are resident isolated-band
measurements, including CPU correction and metadata transfers. They do not
establish full-frame throughput or justify changing decoder defaults.

## 1. Problem, scope and preserved paths

The dependency is both FSM state and variable coefficient position at every
compressed nibble. A byte boundary does not imply a valid restart. Phase 2F-A
moved this recurrence to one GPU lane and took about 2.25 seconds per dense
critical band. It remains a negative reference, not an optimization target.

Phase 2D remains the throughput reference, Phase 2E the latency/reference
implementation, and Phase 2F-A the callable negative research reference.
No production source in `src/` or `include/`, encoder, upstream, wavelet,
color, DeviceOutput, slot scheduling, public defaults or formats was changed.
No full-frame integration, push, merge, release or tag operation was performed.

All eight prior decoder documents were read before implementation. New build
targets are conditional standalone research targets. The research library is
linked into the prototype and focused test, never into the production library.
The independent experimental entry point is `Phase2FB::RunGpu`; it is not
a public decoder mode.

## 2. Reproduction and frozen fixtures

Build the CUDA-enabled Release tree with benchmarks enabled, then run from
the repository root:

```powershell
cmake --build build --config Release --target prototype_decode_phase2fb test_decode_phase2fb
.\build\benchmarks\Release\prototype_decode_phase2fb.exe > benchmarks/results/decode_phase2fb_research.json
.\build\benchmarks\Release\prototype_decode_phase2fb.exe --all-state-critical > benchmarks/results/decode_phase2fb_all_state_critical.json
python benchmarks/analyze_decode_phase2fb.py
ctest --test-dir build -C Release --output-on-failure
```

`--quick` restricts the experiment to 4K noise with two timed repeats.
`--sanitizer-corpus` runs all selected fixtures at 256 bytes, excluding the
rejected all-state and serial F-A baselines. Normal measurements use one
warm-up and five timed trials for every B/F band/chunk pair.

Fixtures use the existing deterministic Phase 0 generator (seed
`0x1337BEEF`), aligned input, and the frozen golden sample manifest.
Regenerated sample hashes and lengths must match before extraction. No giant
compressed samples are duplicated. Raw JSON records payload offset/length,
geometry, codebook, quantization, difference flag, input/sample/payload hashes,
reference checkpoints, serial trace hash and coefficient comparisons.
The reference coefficients can be regenerated from pristine upstream native
band extraction. All 13 selected real bands use codebook 1; graph analysis
also covers codebooks 0 and 2, without claiming real-band coverage for those.

The primary fixture is 3840x2160 noise, channel 0, level 0, orientation 1,
band index 6: offset 4,154,520; length 5,022,296 bytes; coefficient geometry
1920x1080; quantizer 4; difference coding false. Sample hash
`6b12d20527760fc6`, payload hash `4e0354132183700d`, exact trace hash
`2f9c57a7978e1840`. Band 7 is the slightly larger companion noise L0 band.
Thus both the named critical band and largest neighboring dense band are tested.

## 3. Entropy statistics and reachability

| Frame | Pattern | Band | Bytes | Emitted values | FSM zero gaps | Mean gap | Max gap | Observed states |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1920x1080 | flat | 6 | 2636 | 0 | 1620 | 320 | 320 | 19 |
| 1920x1080 | gradient | 6 | 71352 | 59940 | 122445 | 3.744 | 12 | 14 |
| 1920x1080 | noise | 6 | 1255524 | 516560 | 1834 | 1.003 | 2 | 400 |
| 1920x1080 | noise | 7 | 1256952 | 516459 | 1934 | 1.004 | 2 | 402 |
| 1920x1080 | texture | 5 | 421204 | 129600 | 0 | 0 | 0 | 15 |
| 1920x1080 | edges | 6 | 117052 | 31320 | 53737 | 9.064 | 62 | 29 |
| 3840x2160 | flat | 6 | 10536 | 0 | 6480 | 320 | 320 | 19 |
| 3840x2160 | gradient | 6 | 206016 | 124200 | 314280 | 6.203 | 23 | 15 |
| 3840x2160 | noise | 6 | 5022296 | 2066343 | 7237 | 1.003 | 2 | 406 |
| 3840x2160 | noise | 7 | 5030940 | 2068438 | 5156 | 1.001 | 2 | 404 |
| 3840x2160 | texture | 5 | 1684804 | 518400 | 0 | 0 | 0 | 15 |
| 3840x2160 | edges | 6 | 477100 | 127440 | 220867 | 8.811 | 62 | 29 |
| 7680x4320 | flat | 6 | 42124 | 0 | 25920 | 320 | 320 | 19 |

Nibbles equal twice payload bytes; marker indices are zero-based. The raw
artifact contains exact marker position and reference checkpoint count for
every band. Primary band 6: 10,044,592 payload nibbles, marker 10,044,588,
64 Phase 2E checkpoints, final coefficient position 2,073,600. Band 7:
10,061,880 nibbles, marker 10,061,878, 64 checkpoints.

"Emitted values" counts FSM value emissions, not original Huffman codewords.
"Zero runs" counts nonzero pre-skip/post-skip zero gaps from the decoded FSM
trace, not reconstructed original zero-run codeword boundaries. This distinction
matters for gradient/edges where a codeword can span several transitions.

Full state/transition frequency arrays, 256-byte boundary states, nibble
alignment and previous transition classes are in
`decode_phase2fb_fsm_stats.json`. Transition indices are state*16+nibble;
previous classes encode emission count +4 if pre-skip +8 if post-skip exceeds
emission count. Selected fixed-byte boundaries have even nibble alignment.
The serial reference and synthetic tests also exercise odd nibble restarts.

For primary noise, the most frequent incoming states are
0:517631, 1:516965, 3:514007, 4:510541, 7:271942, 6:238041,
8:235496, 13:231945, 18:229229, 11:165013.
Band 6 visits 406/518 states and band 7 visits 404/518.
Low corpus frequency is **never** used to prune a mathematically possible state.

## 4. FSM graph and output-aware equivalence

| Codebook | States | Reachable from root | SCC sizes | Unique rows | Output-aware classes | Unique entries / total |
|---|---:|---:|---|---:|---:|---:|
| 0 | 136 | 136 | 136 | 136 | 136 | 2143 / 2176 |
| 1 | 518 | 518 | 518 | 518 | 518 | 8277 / 8288 |
| 2 | 518 | 518 | 518 | 518 | 518 | 8277 / 8288 |

Reachability follows nonterminal edges. Every relevant state is in one
strongly connected component; there are no absorbing nonterminal states.
Each codebook has 15 marker edges. Termination is modeled as absorbing in
summary composition, rather than claiming that the original graph has an
absorbing terminal state. In/out-degree arrays are saved for every state.

Partition refinement includes values, skips, terminal behavior and successor
class. It finds no global output-aware state merges. Whole transition rows
are distinct and entry redundancy is small. This excludes a simple global
quotient-state reduction; it does not exclude every possible local dictionary
or arbitrary repeated-subgraph compression.

## 5. Candidate A: all-state and hierarchical summaries

A bounded summary stores outgoing state (16 bits), coefficient advance
(32 bits), and terminal offset/continuation (16 bits): 8 bytes. Coarse
1 KiB chunks make all-state leaf storage fit below the 32 MiB ceiling.

A real full-grid leaf launch was measured on both critical bands, three
times, with up to 64 uniformly spaced chunks checked for **all 518 incoming
states** against CPU scans. Band 6 summary times are 14.049, 13.767, 13.746 ms,
with 20,396,768 bytes scratch (19.452 MiB). Band 7 times are
13.755, 13.762, 13.758 ms, with 20,434,064 bytes.

These are **GPU leaf-summary times only**. No hierarchical propagation or
coefficient replay was implemented for A. Sampled summary exactness is not
complete coefficient exactness. A is deferred because its leaf construction
already costs substantially more time/memory than complete B/F. It did not
demonstrate a >30 ms preprocessing kill or impossibility of a better hierarchy.

Early 64-chunk pilots and their linear projections remain in raw results.
Those projections greatly overestimate a full parallel grid and must not be
presented as measured full-band latency. Full-grid measurements supersede them.

At fanout 16 and 1 KiB leaves, storage models for 2/3/4 levels are:

| Levels | Maps per level | Summary-map bytes |
|---|---|---:|
| 2 | 4905, 307 | 21,598,528 |
| 3 | 4905, 307, 20 | 21,681,408 |
| 4 | 4905, 307, 20, 2 | 21,689,696 |

These are retained-map storage models, excluding fixed tables/checkpoints.
Higher-level GPU composition was killed before implementation; there is no
measured best hierarchy depth or measured scan-work reduction. More retained
levels add storage and do not remove the measured leaf construction cost.

## 6. Composition proof and its limits

For fixed nibble intervals, let T_x(s) contain successor state, additive
coefficient advance and either continuation or the first marker offset.
Composition first evaluates T_x(s). If terminal, it returns that terminal
result unchanged. Otherwise it evaluates T_y at the returned state, adds
advances, and shifts any terminal offset by length(x).

This is evaluation of the same deterministic transducer on concatenated input,
so T_z composed with (T_y composed with T_x) equals
(T_z composed with T_y) composed with T_x. Termination remains absorbing;
position additions associate. The FSM state contains sign/value continuation;
absolute nibble coordinates select identical input and preserve alignment.
No per-row difference accumulator is omitted because difference coding is
a separate post-pass.

The packed research `Compose` helper is bounded: terminal offsets must fit
below the reserved 65535 continuation sentinel and advances must fit uint32.
The synthetic associativity test compares both parenthesizations and direct
scans of three small intervals. A whole-band hierarchy would require a wider
terminal coordinate or a richer interval representation. No whole-band
associativity claim is made for overflowing packed offsets.

## 7. Candidate B/F: predicted summaries and exact correction

1. GPU threads independently scan every fixed-size chunk from predicted root
   state 0. They record advance, successor and terminal; they emit no output.
2. CPU walks chunk boundaries in order, knowing the exact incoming state
   and position from the preceding resolved chunk. It advances the actual
   and predicted machine on the same input until their states meet at the
   same next nibble.
3. Once they meet, future transitions are identical. If actual prefix advance
   is a, predicted prefix advance b, and root-summary advance R, exact chunk
   advance is **a + R - b**. The root tail supplies exact successor/marker.
4. If the predicted path terminated before meeting, its tail is unusable.
   The CPU scans the entire remaining actual chunk. If there is no meeting,
   the entire chunk is scanned exactly. Actual termination stops discovery.
5. Exact checkpoints are copied to GPU. One thread per chunk replays the
   verified recurrence into disjoint coefficient intervals. Errors are
   checked before the function returns output.

Induction over chunks proves exact incoming state **and** coefficient position.
Equality of state at a different nibble would not prove anything and is never
used. Prediction is an internal work-saving device, never a guessed restart.
This breaks most of the long dense-band CPU recurrence through parallel
summary construction, while retaining a bounded serial correction stage.

At 256 bytes, band 6 corrects 216,238/10,044,592 nibbles (2.15%); maximum
correction 123 nibbles; no complete chunks. Band 7 corrects 243,012 nibbles
(2.42%); maximum 124; no complete chunks. Worst-case periodic input may
require every chunk to be scanned fully. Work remains O(payload nibbles)
for fixed chunk size, not guaranteed sublinear serial work.

B and F are the same implementation: coarse-to-fine discovery realized as a
CPU/GPU hybrid. They are not two independent winning algorithms.

## 8. Candidate C: selective state expansion

The mathematically safe initial over-approximation is all 518 reachable
states. Controlled suffix experiments evaluate every such state at up to
64 evenly spaced boundaries, for windows of 2/4/8/16/32/64 nibbles. They
measure local images, not globally feasible predecessor states.

For 64-nibble noise windows, mean outgoing state+terminal width is 1.922
(band 6) and 1.906 (band 7), maximum 3. Adding coefficient advance retains
14.875 and 14.625 distinct result classes. Earlier emitted prefixes may still
differ even where future state has merged. Thus "two outgoing states" is
not "two exact incoming candidates" or complete output equivalence.

There is real convergence, so a claim that observed candidates simply blow
up to 518 would be false. Nevertheless no complete economical neighboring
constraint solver, output-prefix reconstruction or latency measurement was
derived. C is a structural analysis, not an implemented exact decoder;
latency and scratch are **unmeasured**, not zero. It is deferred in favor
of the proven cheaper B/F route, not declared mathematically impossible.

## 9. Candidates D/E and alternatives

D recursively needs exact midpoint state and position. No independent
midpoint oracle was established; a prefix scan or full-state composed map
reintroduces the costs already measured. D has no decoder or timing result.
This is an unresolved algorithmic obstacle, not an impossibility proof.

E investigates state IDs, common-output grouping and shared dictionaries.
State IDs require 10 bits for 518 states; global output-aware equivalence
provides no merges. Local outgoing convergence can compact *results*, but
prefix advances/values remain distinct and construction work still exists.
A dictionary implementation and its complete latency were not measured.
B avoids that matrix by computing one root map and verifying only the needed
correction; this is the additional viable idea.

Persistent scheduling, CUDA dynamic parallelism, CPU SIMD optimization and
profiler installation were not needed for this gate. No claim of measured
benefit from these unimplemented alternatives is made.

## 10. Timing method and the idle rebenchmark

After the user's report of competing GPU processes, all complete sweeps were
rerun three times on 2026-10-08, serially, with no project tests or sanitizers
running concurrently. Starting telemetry reported 0% utilization and 0 MiB
GPU memory. Before each run, the compute-app list showed the Codex process.
GPU utilization, memory utilization, memory, clock, temperature and power
were sampled every 200 ms in `decode_phase2fb_idle_gpu_samples.csv`.
Run timestamps and process snapshots are in `decode_phase2fb_idle_run_context.json`.

This is evidence of idle starting conditions, not a guarantee against every
transient WDDM/graphics workload. Clock changes and run variation remain
visible. Five timed trials follow each warm-up. Tables use the median of
the three **run means**, not a median of 15 individual trials. Independently
aggregated component medians need not sum exactly to the total median.

Initial payload/table H2D and allocations occur before the timer; final
diagnostic coefficient D2H and upstream/reference comparison occur after it.
The band is assumed resident, as in the isolated E replay comparison.
Included: GPU root summaries and summary D2H, CPU exact correction,
checkpoint H2D, zero initialization, GPU replay, optional difference post-pass,
and final error-flag D2H validation. CPU uses a steady clock; GPU spans use
CUDA events. Replay-ready wall time ends after checkpoint H2D.
GPU state resolution is absent: exact boundary resolution is on the CPU.

| Component, 256 bytes | Band 6 ms | Band 7 ms |
|---|---:|---:|
| GPU summaries + summary D2H | 0.589 | 0.662 |
| CPU exact resolution | 1.239 | 1.213 |
| Checkpoint H2D | 0.082 | 0.077 |
| Replay-ready wall time | 1.939 | 1.981 |
| GPU zero/replay/difference | 0.671 | 0.814 |
| Total including error validation | 2.635 | 2.818 |

Band 6 run means: 3.544, 2.470, 2.635 ms; worst individual trial 3.930 ms.
Band 7: 2.818, 2.237, 2.901 ms; worst trial 4.176 ms.
The exceptional <10 ms target is passed for these isolated fixtures.

## 11. Chunk sweep and corpus results

| Chunk bytes | Band 6 scratch MiB | Band 6 total ms | Band 7 total ms | Band 6 full corrections |
| --- | --- | --- | --- | --- |
| 16 | 12.037 | 13.631 | 14.259 | 17752 |
| 32 | 6.05 | 7.371 | 7.899 | 520 |
| 64 | 3.057 | 4.099 | 4.514 | 0 |
| 128 | 1.56 | 2.458 | 2.568 | 0 |
| 256 | 0.812 | 2.635 | 2.818 | 0 |
| 512 | 0.437 | 3.004 | 3.309 | 0 |
| 1024 | 0.25 | 4.688 | 5.883 | 0 |

128 bytes is the fastest critical median in these reruns. 256 bytes is a
practical near-fast point with about half the scratch (0.812 versus 1.560 MiB).
Neither is a proven global optimum. Smaller chunks incur more CPU boundary
and transfer work; larger chunks lengthen per-thread GPU scans.

| Frame width | Pattern | Band | 256-byte total ms | Scratch MiB | Full corrections |
| --- | --- | --- | --- | --- | --- |
| 1920 | flat | 6 | 0.354 | 0.064 | 0 |
| 1920 | gradient | 6 | 0.434 | 0.074 | 0 |
| 1920 | noise | 6 | 0.812 | 0.25 | 0 |
| 1920 | noise | 7 | 0.882 | 0.251 | 0 |
| 1920 | texture | 5 | 2.419 | 0.126 | 1392 |
| 1920 | edges | 6 | 0.437 | 0.081 | 0 |
| 3840 | flat | 6 | 0.434 | 0.065 | 0 |
| 3840 | gradient | 6 | 0.476 | 0.094 | 0 |
| 3840 | noise | 6 | 2.635 | 0.812 | 0 |
| 3840 | noise | 7 | 2.818 | 0.813 | 0 |
| 3840 | texture | 5 | 8.225 | 0.314 | 5568 |
| 3840 | edges | 6 | 0.464 | 0.134 | 0 |
| 7680 | flat | 6 | 0.927 | 0.07 | 0 |

All entries have max coefficient error 0. At 4K texture/256, 5,568 chunks
require complete correction and CPU resolution takes 7.648 ms of 8.225 ms
total. This is a counterexample to universal cheap local synchronization.
Sparse flat bands can favor smaller chunks. Selection must therefore remain
experimental and workload dependent.

## 12. Four-baseline comparison

| Resident isolated noise band | Band 6 median ms | Band 7 median ms |
|---|---:|---:|
| E CPU exact index | 23.460 | 27.492 |
| E existing GPU replay, zero/difference included | 36.737 | 40.724 |
| F-A serial GPU index alone | 2229.038 | 2246.711 |
| B/F replay-ready, 256 bytes | 1.939 | 1.981 |
| B/F complete, 256 bytes | 2.635 | 2.818 |

E GPU replay run means range 36.717-76.760 ms (band 6) and
39.451-53.310 ms (band 7), despite idle starting conditions. The raw runs
preserve this variation. These new timings supersede the earlier contaminated
run, while historical Phase 2E/2F-A documentation remains unchanged.
Do not claim a precise universal speedup ratio from these variable spans.
The candidate clearly beats even the smallest measured E component here.
F-A remains multi-second and is not rescued.

## 13. Explicit scratch accounting

For N chunks and S states, conservative B/F scratch is
**N*(2*8 + 2*12) + S*16*8 + 8 bytes**. Both host and device metadata are
counted; fixed table includes the active device codebook. Immutable resident
payload, existing host codebook and final coefficient storage are input/output,
excluded from scratch. The later diagnostic CPU coefficient copy is excluded
from algorithm scratch and timing.

| Item, band 6 / 256 bytes | Bytes |
|---|---:|
| 19,619 device summaries | 156,952 |
| Host summary mirror | 156,952 |
| Device exact checkpoints | 235,428 |
| Host exact checkpoints | 235,428 |
| Device FSM table, 518*16*8 | 66,304 |
| Host/device error scalars | 8 |
| Total | 851,072 |

There are no candidate arrays, separate scan/prefix buffers, shared-memory
replay buffers or coefficient shadow scratch. Output is zeroed once and
written after checkpoints are verified. At most 27 equally large active
bands would cost approximately 21.9 MiB of this metadata (simple replication,
not a measured full-frame peak). Table sharing could reduce this further.
At 16 bytes the critical sweep is still below 16 MiB. B/F satisfies the
preferred 8 MiB, target 16 MiB and hard 32 MiB limits. A meets only the hard
ceiling and exceeds the target.

## 14. Exactness, quantization and difference coding

Every replay is compared coefficient-by-coefficient to pristine upstream
selected-band output, not accepted by PSNR or +/-1 tolerance. Every resolved
checkpoint is checked against the serial nibble traversal for state and
position; marker, final state and final position must match. Deterministic
trace hashes cover values, skip/position and successor sequence. The corpus
has no difference-coded selected band; a synthetic row-wise difference-coded
case checks the exact post-pass. Real difference-coded-band coverage remains
for a later stage.

The existing FSM table supplies companded values; replay applies the original
quantizer with tested int16 narrowing behavior. Difference coding is an
independent exact per-row post-pass. No optimization of these transforms was
attempted. No new encoder bitstream or NLE/FFmpeg compatibility claim is made.

## 15. Malformed-input model

Descriptors validate pointer, payload bounds (<=32 MiB), states (1..518),
initial state, geometry, quantizer, transitions, nonoverlapping emission and
coefficient capacity. Scratch is checked before allocation. Chunk loops are
bounded to <=2048 nibbles and correction loops to the same interval.
Missing marker/truncation causes rejection; predicted premature termination
never establishes a restart. Exact termination and continuation are distinct;
invalid input is represented by an exception/error flag, not a valid summary.

Position math uses wider accumulation with explicit bounds; replay checks
every coefficient store and next checkpoint's state/position. The reference
capacity+8 advancement guard does not permit stores past capacity.
Tests cover invalid state/codebook extent, first/odd marker, missing marker,
false predicted marker, nonconverging paths, impossible advance, integer
overflow, chunk size, summary count, scratch overflow and overlapping stores.
Output returns only after error validation. These are research-level bounds;
the prototype is not promoted as a general hostile-bitstream production parser.

## 16. Validation and sanitizers

Full CUDA CTest: **28/28 passed**, 954.83 s. CPU-only build: **5/5 passed**,
6.82 s. The final focused executable reports **39 deterministic exactness/
bounds cases**, max error 0. The full suite ran before the later bounded
all-state verification/CLI diagnostic change; final focused checks, the actual
three-repeat all-state launches and all sanitizer runs cover the final
research version. Production code did not change between these checks.

Compute Sanitizer memcheck, initcheck, racecheck and synccheck each ran on
the focused tests and all 13 real B/F bands at 256 bytes: **8/8 commands
exit 0, zero error/hazard summaries**, memcheck zero leaked bytes.
Raw logs and `decode_phase2fb_sanitizers.json` are committed.
No sanitizer performance number is used for ordinary timing.

## 17. GPU execution analysis

Hardware is **RTX 4090 Laptop GPU**, 76 SMs, CC 8.9; not a desktop 4090.
CUDA runtime API 12080, driver API 13030. Root summary/replay kernels use
16/22 registers per thread, zero static shared and local bytes. CUDA occupancy
API allows 12 blocks of 128 threads/SM, theoretical 100% thread occupancy.
This is an occupancy limit, not measured active-warps/utilization.

Nibble table dependencies are serial within each chunk, parallel across
chunks. Data-dependent marker exits and transitions can diverge. No measured
branch-divergence or achieved bandwidth counter is claimed; telemetry and
kernel timing provide evidence without mandatory profiler installation.
Periodic texture shows that convergence behavior, not just occupancy,
determines the hybrid CPU cost.

## 18. Codec regression benchmark

`benchmark_codec --json` was rerun and preserved in
`reference_windows_msvc_phase2fb.json`; the original baseline is unchanged.
The validation JSON stores complete FPS, MP/s, avg/min/max latency and peak
memory for baseline/current.

| Case | Mode | FPS | MP/s | Avg ms | Min ms | Max ms | Peak bytes | FPS vs baseline % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1080p | encode | 86.4869 | 179.339 | 11.2673 | 10.4977 | 12.9872 | 97034240 | 11.31 |
| 1080p | decode | 470.723 | 976.091 | 2.12412 | 1.889 | 2.7124 | 97034240 | 19.43 |
| 4K_UHD | encode | 23.7466 | 196.964 | 41.0786 | 38.9039 | 43.215 | 240472064 | 8.74 |
| 4K_UHD | decode | 129.318 | 1072.61 | 7.73256 | 7.0879 | 9.3713 | 240472064 | 3.2 |
| 8K_UHD | encode | 5.83058 | 193.445 | 168.3 | 148.02 | 191.947 | 643821568 | 36.15 |
| 8K_UHD | decode | 27.6418 | 917.088 | 36.1767 | 33.5849 | 38.4985 | 643821568 | -1.15 |

8K decode FPS differs by -1.15%, with current latency spread
33.585-38.499 ms versus baseline 32.803-38.242 ms. This small variation
is below the run's spread; production source and compressed sizes are
unchanged. It is not attributed to a research optimization. Research timing
must not be confused with production codec throughput.

## 19. Decision matrix

| Candidate | Exact | Critical band 6 ms | Scratch MiB | Complexity / scaling | Continue? |
|---|---|---:|---:|---|---|
| A all-state/hierarchy | Sampled summaries; no coefficient decoder | 13.767 leaf only | 19.452 | O(518*nibbles) leaf work; hierarchy unimplemented | Defer |
| B coarse-to-fine | Yes, max error 0 | 2.635 complete | 0.812 | O(nibbles) parallel work; bounded serial corrections | Yes |
| C selective constraints | Structural analysis only | Unmeasured | Unmeasured | Exact global constraint/prefix solver unresolved | Defer |
| D recursive midpoint | No prototype | Unmeasured | Unmeasured | Midpoint oracle unresolved | Defer |
| E compact dictionaries | Structural analysis only | Unmeasured | Unmeasured | Output-aware global quotient gives no reduction | Defer |
| F CPU/GPU hybrid | Same exact B implementation | 2.635 complete | 0.812 | CPU 1.239 ms here; periodic worst case remains | Yes |

Three fundamentally different approaches were investigated: all-state
hierarchical maps, verified root-tail correction, and selective neighbor-state
constraints; D/E provide additional structural investigations. Only B/F is
a complete surviving coefficient prototype. Rejected/deferred designs retain
their measurements and limits instead of manufacturing comparable timings.

## 20. Recommendation and integration gate

A separate Phase 2F-C experimental stage is warranted. It should evaluate
all 27 bands, one complete 4K noise frame, allocation/transfer reuse,
difference-coded real bands, multi-slot contention, broad malformed inputs
and fallback/selection behavior. Preserve D/E/F-A throughout. The isolated
gate is satisfied, but no full-frame prototype or default change is made in
this report; the user requested presentation before integration.

The stable D/E implementations remain the final production architecture
today. They are not proven to be the permanently best architecture. B/F's
strong isolated result warrants controlled integration research; the texture
counterexample and WDDM variation prevent extrapolating universal speedup.

## 21. Artifact map and 45-point completion report

Raw reruns: `decode_phase2fb_research.json`,
`decode_phase2fb_idle_repeat2.json`, `decode_phase2fb_idle_repeat3.json`.
Aggregates: `decode_phase2fb_chunk_sweep.json`,
`decode_phase2fb_fsm_stats.json`, `decode_phase2fb_candidate_matrix.json`.
A full-grid leaf pilot: `decode_phase2fb_all_state_critical.json`.
Validation: `decode_phase2fb_validation.json`, CTest/sanitizer logs.
Source SHA256, base and upstream tree: `decode_phase2fb_provenance.json`.
Upstream tree retained: `837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`.
Main retained: `51435890ba5853e87e8cf1ddc7cc747954bd9e80`.
Final status is clean after the local research/documentation commits.

| # | Requested completion item | Result / evidence |
|---:|---|---|
| 1 | Branch | decode-phase2fb |
| 2 | Base commit | 773669cbbcccda8aedd5c5b968f7649df0fa5de3 |
| 3 | Commits created | bc60373c1d9653c5901b94150d3efb853518c2ae implementation; subsequent report/artifact commit |
| 4 | Files changed | Two research CMake additions; five C++/CUDA/header files; see exact list below; analysis script, this report and results |
| 5 | Encoder unchanged | No production source changes |
| 6 | Upstream unchanged | Clean status and retained tree above |
| 7 | D/E/F-A preserved | Original callable paths unchanged |
| 8 | Critical fixture | 4K noise band 6, 5,022,296 bytes; companion band 7 |
| 9 | Theoretical FSM states | 518 relevant; codebook 0 has 136 |
| 10 | Reachability | 518 reachable; observed 406/404 in noise bands |
| 11 | Histogram | Full arrays in FSM stats; top states in section 3 |
| 12 | Transition redundancy | 518 unique rows/classes, 8277/8288 unique entries |
| 13 | Zero runs | Band 6: 7237 FSM gaps, mean 1.003, max 2 |
| 14 | A design | Coarse all-state leaves, hierarchical storage model |
| 15 | A exactness | Sampled maps exact; no complete coefficient reconstruction |
| 16 | A latency | 13.767 ms leaf summaries only; total unmeasured |
| 17 | A scratch | 20,396,768 bytes / 19.452 MiB |
| 18 | B design | Parallel root summaries, exact local correction, replay |
| 19 | B exactness | State, position, marker and coefficients exact |
| 20 | B latency | 2.635 ms total at 256 bytes |
| 21 | B scratch | 851,072 bytes |
| 22 | C design | All-state safe over-approximation and suffix images |
| 23 | C exactness | Structural analysis, no complete decoder |
| 24 | C latency | Unmeasured |
| 25 | C scratch | Unmeasured |
| 26 | Hybrid result | B/F identical implementation; 1.239 ms CPU |
| 27 | Best chunk | 128 bytes fastest critical median; 256 near-fast and smaller |
| 28 | Best hierarchy | None measured; 2/3/4-level storage only |
| 29 | Candidate width | 518 initial; 64-nibble image ~1.92, advance classes ~14.88 |
| 30 | CPU preprocessing | 1.239 ms correction |
| 31 | GPU preprocessing | 0.589 ms summary + D2H |
| 32 | Replay | 0.671 ms zero/replay/difference |
| 33 | Combined band | 2.635 ms, worst timed trial 3.930 ms |
| 34 | E host comparison | 23.460 ms median |
| 35 | E replay comparison | 36.737 ms median, variable range disclosed |
| 36 | F-A comparison | 2229.038 ms serial index |
| 37 | Scratch compliance | B/F preferred budget passed; A only hard ceiling |
| 38 | Malformed inputs | 39 focused bounded/exact tests, rejection cases included |
| 39 | Sanitizers | Four tools, focused + corpus, zero errors/hazards |
| 40 | Matrix | Section 19 and candidate JSON |
| 41 | Winner | B/F, outcome A for isolated research gate |
| 42 | Integration | Separate experiment recommended; defaults unchanged |
| 43 | Full-frame prototype | Justified next, deferred until results are presented |
| 44 | Phase 2F-C | Warranted for controlled experimental evaluation |
| 45 | Stable D/E final | Current production architecture retained |

Implementation file list: `benchmarks/CMakeLists.txt`,
`tests/CMakeLists.txt`, `benchmarks/prototype_decode_phase2fb.cpp`,
`benchmarks/decode_phase2fb_kernels.cu`, `benchmarks/decode_phase2fb_gpu.h`,
`tests/decode_phase2fb_support.h`, `tests/test_decode_phase2fb.cpp`,
`benchmarks/analyze_decode_phase2fb.py`. The documentation commit adds
this file and the machine-readable/log artifacts listed above.
