<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Five macOS contract follow-ups

Audited source: `e87ef0258ba37f5c78f1557c59d9c7331df5521c` (current main).
Date: 2026-10-17. Native ARM64, macOS 26.5.2, AppleClang 21.0.0,
CMake 3.31.5, Python 3.9.6. This extends #134 with new negative tests and
arithmetic coverage; it does not repeat its qualification claims.

| Issue | New completed subtask | Remaining dependency |
| --- | --- | --- |
| [#44](https://github.com/louis-0hrs/HyperCine/issues/44) | Three accepted geometry probes, thirteen invalid/budget geometry cases, six malformed mutable Plane descriptors, invalid prescale and preserved original storage. Invalid frame dimensions are checked without frame allocation. | Existing main limits remain binding. P4 API acceptance and original SDK 4K packet parity remain separate. |
| [#45](https://github.com/louis-0hrs/HyperCine/issues/45) | Source-backed resident 42-buffer model for 1080p/4K/8K/12K/17K, padded input and overlapping workspace replacement. No allocation or shader change. | P4 acceptance, measured Metal limits, RAM budget, shader/padding correctness and real 4K GPU execution. |
| [#46](https://github.com/louis-0hrs/HyperCine/issues/46) | Six 120-call shared QualityState sequences compared isolated versus interleaved/reordered, injecting invalid profile calls between valid calls. All semantic fields, quant/scale and limiter/overbitrate state must match. | Synthetic completed-byte trajectories test isolation; no new SDK oracle comparison or CPU/Metal profile adapter. P4/P5 gates unchanged. |
| [#47](https://github.com/louis-0hrs/HyperCine/issues/47) | Fault-inject existing P7.A oracle: each plane's unsigned p10 range, byte swap, every short-frame length, extra frame, changed/missing verified report fields, absent executable, timeout and subprocess failure. Valid control must pass. | SDK pixel parity and native decoder remain pending; #13/#16 safety/mainline gates unchanged. |
| [#70](https://github.com/louis-0hrs/HyperCine/issues/70) | Independent int64 2D scalar oracle, prescale 0/2, saturation, six patterns, tails and poisoned input padding at ten widths and three heights. A deliberate rounding mutation is rejected. | No NEON code, hardware dispatch or speedup qualification. P4 acceptance still required. |

## New measured evidence

Independent contract project: **5/5 CTest PASS** in Release and **5/5 PASS**
with ASan/UBSan. Sanitizers instrument the C++ tests, not Python or Metal;
leak detection disabled. C++ outputs: geometry **21 checks**, quality **2,880**,
wavelet **1,698,720**. P7 negative suite contains **4 test methods**.

Wavelet coverage comprises 360 planes: widths 6, 8, 10, 14, 16, 18, 30, 34,
128 and 3840; heights 6, 10 and 18; prescale 0/2; zero, signed minimum,
signed maximum, checker, last-pixel impulse and seeded signed noise.
The 3840-wide probes are not full 3840x2160 frames. Horizontal/vertical
reference equations use widened quotient/remainder floor division and final
int16 clamping. Prescale lowpass rounds the original pair sum; highpass rounds
individual taps. Padding poisoning proves output independence for the tested
values; it does not prove absence of all logical padding reads.

Mutation control: compile a temporary copy of `standalone_frontend.cpp` with
prescale pair-sum rounding changed from +3 to +4; the new oracle exits 1 with
`independent 2D coefficient mismatch`. Production sources remain untouched.

P7 source is existing PR #83 head
`fd844fd058e0d79d4eabd39769e453c65ea78ba3`, baseline.py SHA-256
`d1fe9f8b6e6c913b3d506125032c14590da0fe9b47e78f65af82cef95db65492`.
Its report verification is tested with injected captures; this run does not
re-decode the public AVI or invent original SDK pixel comparisons.

Existing full native standalone Release suite: **31/31 CTest PASS**, no skips,
96.83 seconds, including privately configured real SDK Tier A/120-frame
sequence packet replay and required external FFmpeg decode. Existing CI Python
unit suites: **19/19 PASS**. Corpus locations and raw logs stay outside Git.

## Metal memory finding

See [metal-budget.json](metal-budget.json). These are calculated buffer payload
sizes, not device measurements or supported output geometries. The model mirrors
`Metal::Prepare`/`AllocateSurface`, with input + 3 color + 36 band + 2 scratch
buffers, including 16-byte plane-pitch padding. The emitted source SHA binds the
analysis to the inspected implementation; re-audit the equations if it changes.

| Geometry | One workspace bytes | Largest buffer bytes |
| --- | ---: | ---: |
| 1920x1080 | 31,622,400 | 8,294,400 |
| 3840x2160 | 126,489,600 | 33,177,600 |
| 7680x4320 | 505,958,400 | 132,710,400 |
| 12288x8040 | 1,506,631,680 | 395,182,080 |
| 17520x8040 | 2,148,545,280 | 563,443,200 |

`Prepare` allocates the new workspace before assigning `workspace_`, preserving
the old allocation on failure. Therefore a 1080p-to-4K transition requires
**158,112,000 buffer payload bytes concurrently**, rather than only 126,489,600;
a modeled 12K-to-17K transition requires **3,655,176,960**. The existing
`peak_buffer_bytes` update records only `next->bytes`, so it does not represent
this transient overlap. `peak_device_allocated_bytes` queries a separate device
allocation metric; neither is an end-to-end process/shared-RAM peak.

Host planes, 30 readbacks, entropy/sample storage and driver overhead are
excluded. No large buffers were allocated. All listed input index products fit
uint32, but that alone establishes neither buffer feasibility nor valid wavelet
geometry/CFHD output. Current main rejects 4K+ before preparation. P5 must query
`maxBufferLength` and available RAM and budget both workspaces before allocation.

## Reproduce

From this branch with CMake 3.25+, a C++17 compiler and Python 3.8+:

```sh
# Configure P7_AUDIT_ROOT privately to the existing PR #83 checkout.
cmake -S tests/macos_contracts -B build/contracts -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_P7_AUDIT_ROOT="$P7_AUDIT_ROOT"
cmake --build build/contracts --parallel 4
ctest --test-dir build/contracts -C Release --output-on-failure

cmake -S tests/macos_contracts -B build/contracts-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DHYPERCINE_P7_AUDIT_ROOT="$P7_AUDIT_ROOT" \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/contracts-asan --parallel 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/contracts-asan \
  -C Release --output-on-failure
```

Without the optional P7 checkout only four tests register; CMake explicitly
reports the missing registration. An invalid configured checkout fails configure.
No production target, existing CI inventory, GPU source or `upstream/` changes.
All five parent issues remain open for their implementation/acceptance gates.
