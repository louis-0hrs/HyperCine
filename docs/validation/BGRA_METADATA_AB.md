# BGRA active-metadata A/B and retained-band observation (#120 / #138)

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

## Review addressed

The original YUY2 experiment did not reproduce Phase2B/C's requested BGRA format
or active processing metadata. This follow-up uses the actual
`hypercine::MakeFrame(... CanonicalPattern::Canonical)` generator, seed
`0x1337BEEF`, aligned BGRA8 input and original SDK FilmScan3/YUV422 encoding.
One packet is generated per geometry and held immutable for **both** A/B arms.
The control requests BGRA without calling any metadata API. The other arm uses
the same order as `src/core/phase2c_encoder.cpp:275`:
OpenDecoder, PrepareToDecode(BGRA), GetImagePitch, OpenMetadata,
InitSampleMetadata(ORIGINAL), SetActiveMetadata(TAG_PROCESS_PATH,
METADATATYPE_UINT32, PROCESSING_ALL_ON=65535), DecodeSample, CloseMetadata,
CloseDecoder. All buffers outlive close. Every metadata return status is checked
and recorded; any failure invalidates the observation, even with decode status 0.
This deliberately improves diagnostic validation without changing production code.

The packet is original-SDK encoded, rather than Phase2C GPU generated; this
matches the source pattern, source/decode formats and processing setup, **not**
all Phase2C encoder-state/packet-parity assumptions. Repeated encoder invocations
and CPU/GPU packet equality remain unmeasured (`encoder_packets_equal=null`).

## Measured BGRA A/B

[Per-call evidence](reference_decode_bgra_metadata_ab_20261017.json), diagnostic
source commit `d2c64e5dfec772dd0fca19c9146c8b74062f61d6`. Native Windows x64,
MSVC 19.44.35229 / CMake 3.31.6 / Python 3.10.9, original CPU SDK only. Four fresh
processes per arm, five decoder opens per process: **80 successful calls** total.
No GPU or overlapping CTest workload. Metadata OFF uses -1 status as an explicit
NOT_CALLED sentinel; its `processing_flags=0` is a reporting sentinel, **not**
proof of the decoder's effective internal flags. All three ON API statuses are
numeric zero in **40/40 calls**, with requested processing flags 65535.

| Geometry | Active metadata | Calls / processes | Distinct BGRA / RGB hashes | Internally divergent processes | BGR source PSNR min–max |
| --- | --- | --- | --- | --- | --- |
| 128x128 | OFF | 20 / 4 | 8 / 8 | 4/4 | 4.127–35.648 dB |
| 128x128 | ON | 20 / 4 | 7 / 7 | 4/4 | 8.121–35.648 dB |
| 1920x1080 | OFF | 20 / 4 | 1 / 1 | 0/4 | 44.465986 dB |
| 1920x1080 | ON | 20 / 4 | 1 / 1 | 0/4 | 44.465986 dB |

128px had max byte delta 255 in both arms, up to 45,828 / 48,280 changed bytes
relative to each arm's first call. Full hashes and RGB-only hashes both vary:
this is **not merely alpha-byte variation**. Requested/actual formats, pitch,
buffer size and first differing byte coordinate/channel are recorded per call.
1080p is identical across both arms, with common output SHA
`c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0`.
The PSNR computation matches Phase2C: ignore alpha, peak 255, evaluate both row
orientations and record both scores plus the selected orientation. The normal
BGRA output is bottom-up; no new orientation-selection rule changes a test gate.
Range and matrix are UNSPECIFIED; this does not qualify external color accuracy.

Raw immutable packet SHA-256:

* 128px: `60ed81a3f164d585ae4d3dc0c4af4154a626cef12311a61b4d2dcca80be04b14`
* 1080p: `7475f9e7c4dd02f50d58adc1e2a6e29c23c92792252736928b0d3f8b10d357e2`

**Interpretation:** setting processing metadata alone does not eliminate the
128px BGRA instability on this sample/setup. The measured 1080p setup was stable
in this bounded run; it neither reproduces nor disproves #116's intermittent
1080p result. No cross-device or all-profile assertion follows from either case.
Both 128 arms classify REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED. Both
1080 arms classify REFERENCE_EVIDENCE_INCOMPLETE because encoder-pair evidence is
absent, rather than falsely claiming encoder PASS. Release remains blocked.
FFmpeg BGRA comparison is explicitly UNAVAILABLE in this scoped A/B; the earlier
version-pinned YUY2 comparison and #11's separate matrix are preserved.

## Read-only retained-band observation

To narrow the first differing reconstruction boundary, a separate manual
`reference_decode_trace` executable is built only for MSVC x64, Release with
`_ALLOCATOR=1`, `WARPSTUFF=1`, matching the original static SDK. It is excluded
from the default build and does not link into production targets. It exposes
the matching `CSampleDecoder` header's private fields **only in this observer
translation unit**; no frozen file, decoder instruction, processing flag or
worker setting is changed. This is an internal ABI observer, not a public API.
Do not use it against a different SDK binary or a Debug/Release layout mismatch.

Immediately after successful DecodeSample, before teardown, it copies only
valid, allocated 16S bands using checked geometry and logical rows, excluding
pitch padding. Python verifies descriptors/offsets/sizes/non-overlap and computes
SHA-256 per retained band. Invalid/missing state is a measurement error, not a
skip. Raw coefficient buffers remain local. Snapshots may alter allocation and
scheduling for later calls; their repetition counts are **separate** from the
uninstrumented A/B matrix.

[Retained-band evidence](reference_decode_retained_bands_20261017.json), source
commit `31eba040e73295eeedfe81d8a2d715386dd20163`, uses the **same 128px packet
SHA above**, two processes/five opens per arm (20 total). Every call reports
**16 transform workers and 4 entropy workers**; `_THREADED_DECODER=1` is the
unchanged SDK default. The regular opaque-API A/B cannot query runtime counts
and reports them as null rather than inventing a number.

All **36 retained bands per call** were available. Within each arm, 34 band
identities have one SHA across all ten calls. Only wavelet index 0, lowpass
band 0 varies: channel 0 has two hashes in each arm; channel 1 has three OFF
and two ON. All wavelet indices 2 and 1 and all final-level highpass bands have
one hash. Final BGRA has four OFF / three ON distinct hashes, confirming the
observer still sees the instability. This narrows the **observed retained-state
difference** to reconstructed lowpass storage rather than only final RGB bytes.

The source path explains which boundary to investigate next:

* `SampleDecoder.cpp:1558`: call into original `::DecodeSample`.
* `decoder.c:12109`: threaded subband handling tests BANDS_ALL_STARTED.
* `image.h:230`: BANDS_ALL_VALID compares valid and started flags; the STARTED
  macro at :231 has different semantics. STARTED alone is not completion proof.
* `decoder.c:12984` ReconstructWaveletBand selects `lowpass_index=index-1` at
  :13000. Its non-allocation path **checks BANDS_ALL_VALID at :13032** before
  `TransformInverseSpatialQuantLowpass` at :13045. Therefore a source-only claim
  that every STARTED branch immediately reconstructs unfinished data would be
  too strong; the downstream validity guard must be included in the analysis.
* The threaded final output path ending at `decoder.c:16489` uses
  InvertHorizontalStrip16sYUVtoRGB (`InvertHorizontalStrip16s.c:17508`), distinct
  from the YUY2 `rand()`-rounding function. The output worker dispatcher at
  `decoder.c:27269` explicitly calls ThreadPoolWaitAllDone before returning.

The **first chronological inverse-stage mismatch remains UNOBSERVED**: these
are post-completion retained buffers, not before/after breakpoint captures.
Stable retained upstream bands do not prove they were stable at every earlier
instant; later writes, scratch lifetime and synchronization remain hypotheses.
The largest spatial-order differing retained identity is recorded precisely,
without relabeling it as a proven root-cause instruction. It is not sufficient
to blame YUY2 dither, change upstream or bypass the existing safety gate #13.

## Reproduction and validation

```powershell
cmake --build build --config Release --target reference_decode_worker --parallel 4
python -B tests/reference_decode/bgra_ab.py --worker build/tests/Release/reference_decode_worker.exe --work build/bgra-ab --output build/bgra-ab.json --processes 4
cmake --build build --config Release --target reference_decode_trace --parallel 4
python -B tests/reference_decode/trace_bands.py --worker build/tests/Release/reference_decode_trace.exe --packet build/bgra-ab/128/sample.cfhd --source build/bgra-ab/128/source.bgra --work build/bgra-trace --output build/bgra-trace.json
ctest --test-dir build -C Release --output-on-failure
```

Configure the same CPU-only build as the parent report first. Work directories
must be fresh. Both observation commands exit 1 for unresolved gates; 2 means
invalid measurement. No PASS/skip is manufactured from API success or missing
evidence. Source, packet, pixel, worker and band hashes are committed in the JSON.

Full native SDK-enabled CPU Release CTest **12/12 PASS**, zero skips; 15 parent
classifier methods and seven measurement methods (now including BGRA orientation,
alpha, metadata status/configuration and retained-storage validation). No
sanitizer, CUDA, 4K/8K or performance qualification is claimed. Existing packet
checks and quality thresholds remain binding. Frozen upstream is unchanged.

**Next action:** independently review the A/B and retained-state evidence, then
scope read-only before/after captures around the index-1 to index-0 lowpass
reconstruction to locate the first actual differing instruction/state. Keep
#120 and the release gate open; no automatic main merge or decoder fix.
