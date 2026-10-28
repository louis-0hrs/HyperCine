<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# S3: typed spatial task attachment (#292 / #195)

Status: bounded synthetic attachment, ready for source review. No parser,
entropy decoder, temporal inverse, GPU backend, frame-decoder API or release
qualification is added. The scalar `InverseWavelet5To4` arithmetic is unchanged.

## The rule carried forward from the SDK investigation

The [all-channel #160 experiment](https://github.com/louis-0hrs/HyperCine/issues/160#issuecomment-6472901842)
confirmed a missing lowpass publication dependency on its measured packet in
FULL/HALF. It does not qualify every original SDK path. The owned scheduler
therefore expresses dependencies as four explicit edges, not a comparison of
started/valid masks, and never parks a spatial callback waiting for its producer:

```text
(generation, channel, wavelet5, band0) --+
(generation, channel, wavelet5, band1) --+--> wavelet4 band0, same channel
(generation, channel, wavelet5, band2) --+
(generation, channel, wavelet5, band3) --+
```

Each predecessor must complete with an owned publication before the callback
is made Ready. Under the existing graph mutex, publication installs the copy
and geometry before the release-store of the version. Admission observes that
version and identity under the mutex/acquire path. Completion releases a
dependent only after all predecessor counts reach zero. A missing or rejected
publication fails the generation; a callback return by itself is not success.
These mechanisms derive from #195's S2/T1/T2/T4/T5 contracts; this change does
not alter the frozen original SDK or the frozen #197 evidence branch.

## Implementation

The private SDK-free `hypercine_decode_spatial_attachment` library exposes
`MakeSpatialLowpassTask` in `src/core/decode_s3_attachment.*`, which declares exactly
four wavelet5 bands of the selected Y/V/U channel, fixed spatial scale/frame,
and one wavelet4 lowpass result. It admits inputs exclusively through
`TaskContext::input_plane`, including generation, version, sample encoding,
channel, wavelet, band, transform, frame, geometry and graph-owned extent.
An untyped published token is insufficient.

The input domain remains width 16..480, multiple of 8, height 4..270.
The callback requires doubled output dimensions. The old implementation
applied input limits to output as well, incorrectly refusing large qualified
outputs. The output-role validator now permits upper bounds 960x540, retaining
the existing lower bounds for legacy small synthetic graph records; the actual
consumer enforces exact doubling. Pitch and data alignment remain 32 bytes,
with a 2 MiB per-plane ceiling. Input admission is not widened.

The graph's existing 26 MiB ceiling covers every owned publication, typed or
untyped; rejected publications store no version, wake no waiter, charge no
bytes and release no dependent. A tighter generation budget remains effective.
The graph owns immutable copies until it drains/dies; the caller may destroy
its source after publication. A PlaneView now exposes its nonzero version and
explicit signed-16 little-endian encoding; empty views contain neither.

C++17 lifetime safety is explicit: the graph's unsigned-char copies are not
aliased as int16 arrays. After typed admission, visible samples are copied into
four bounded, aligned `OwnedSignedPlane` scratch arrays with actual int16
object lifetimes. Input padding is not read by the primitive. The private
output is published only after Complete. This scratch and the primitive's
workspace are additional to the graph-owned publication budget; this is not
a total process-memory limit or an optimization claim. Big-endian native
execution is rejected by the factory rather than misinterpreting LE samples.

Cancellation/error outranks later admission or publication. A canceled or
incomplete result is discarded. The inherited write-once first error survives
the callback's later error, and every started worker joins before run returns.
The owner must still serialize frame generations and retain the graph through
the drain; no cross-generation cache or async completion is introduced.

## Validation

Configure the complete SDK-free project slice:

```powershell
cmake -S . -B build -DHYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY=ON -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build --config Release --parallel 6
ctest --test-dir build -C Release --output-on-failure
python tests/decode_task_graph/mutate_attachment_292.py build/mutants-new
```

The attachment tests run at 1/2/4/16 workers with process-level 30-second
watchdogs. They compare every visible result coefficient against the existing
fixed synthetic expected vectors for all three channels, plus independent
analytic DC at 120x135, 240x135, 480x270 and 240x270 input extents. Distinct
channel DC values detect Y/V/U permutation. Original SDK decoding is not an
oracle here, and the #120 pixel-reference gate is not relaxed.

Required negative cases exercise crossed band edges, V/U identity, untyped
input, short-span refusal followed by a valid retry, stale generation,
output-band mismatch and cancellation before admission. A wrong edge must not
reach even the output-check callback, so a later pixel-comparison failure
cannot hide a successful invalid admission. Existing T4 cases supply budget,
waiter/Ready/version and abandoned-copy checks; existing #195/T1/T2/T5 gates
retain their constructed first-error and stop-before-publication witnesses.
The producer list deliberately queues LL after the three highpasses and checks
that the spatial consumer is still Pending immediately before LL publication,
including the one-worker case. This requires no sleeping/blocking worker.

Native MSVC Release and MSVC AddressSanitizer each pass all 29 registered
tests in this project slice at `eedbe519`. The ASan run needs the installed MSVC
ASan runtime on the local process PATH; the initial missing-runtime launch
errors are not counted as executions.

After the reconciliation with main (`11e17a24`) at head `0199aa61`, the same
SDK-free slice was qualified on Linux x86_64 / GCC 14.2.0. All three
configurations register 30 tests here, not 29, because main added
`rca160_public_evidence` to `tests/decode_task_graph/CMakeLists.txt`; the four
`decoder_attachment_292_workers_{1,2,4,16}` entries are registered and executed
in each of them.

| Configuration | Registered | Result |
| --- | --- | --- |
| Linux x64 GCC Release | 30 | 30/30 pass, attachment 4/4 |
| Linux x64 GCC ASan+UBSan (`-fno-sanitize-recover=undefined`) | 30 | 30/30 pass, attachment 4/4 |
| Linux x64 GCC TSan (`-O1`, `halt_on_error=0`) | 30 | 30/30 pass, attachment 4/4, zero ThreadSanitizer warnings |

This is an independent Linux graph-only result, not a re-run of the Windows
figures above: it was produced on a different host with a different toolchain,
and the Windows numbers remain the owner's. macOS arm64 is still not measured
from this host; the hosted workflows are cited by job URL only, and a cancelled
job is not a passing one.

The mutation driver builds four private copies, removing identity, channel-only, generation-budget
or short-extent guards. Each must exit nonzero and print the first BROKEN
predicate. Compilation failure or an exit without that witness is not accepted
as a killed mutant. Source/binary hashes and measured outcomes are in
`docs/validation/scheduler_s3_292_windows.json`.

## Remaining work

This attaches one qualified spatial research stage to a synthetic graph. It
does not attach arbitrary CFHD packets or establish whole-frame decode
correctness. Further inverse stages require their own explicit input/output
contracts and independent numeric fixtures; parser, entropy, temporal, color,
format coverage and backend qualification remain separate work. No exhaustive
interleaving, universal liveness, GPU or original-SDK repair claim follows from
these 29 tests. No protected merge or release is authorized by this result.
