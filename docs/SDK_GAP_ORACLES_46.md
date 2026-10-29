# Original SDK missing-coverage packet experiment (#46)

**Historical snapshot at #322 initial head `da58ed4`, before #325.**
The original failures and negative experiment below are intentionally preserved.
See [the forward refresh](SDK_GOLDENS_REFRESH_46.md) for separately measured
current-main and corrected-source results, independent Windows A/B/C gates,
and the explicit non-qualifying negative-ramp contract.

This experiment is evidence, not production qualification or certification.
The old Windows corpus is read-only. A separate dated corpus contains new original
SDK packets for Low/Medium/High/FilmScan1 at 1920x1080, 120 consecutive frames,
and a fixed 40-frame noise-amplitude ramp for Low/Medium/High.

## Original encoder, not the writer

The frozen SDK builds on Linux x64 with GCC 13.3.0 and libuuid 2.41.5:

```sh
cmake -S upstream -B sdk-build -DCMAKE_C_COMPILER=gcc-13 \
  -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TOOLS=OFF -DBUILD_SEPARATED=ON
cmake --build sdk-build --target CFHDEncoderStatic -j4
g++-13 -std=c++17 -O2 -Iupstream/Common tools/sdk_packet_capture.cpp \
  sdk-build/libCFHDEncoderStatic.a -luuid -lpthread -fopenmp -o sdk_packet_capture
python3 tools/sdk_gap_corpus.py --source "$HYPERCINE_QUALITY_ORACLE_ROOT" \
  --capture ./sdk_packet_capture --out "$HYPERCINE_SDK_GAP_CORPUS_ROOT"
```

The capture links only the original SDK archive. It calls CFHD_OpenEncoder,
CFHD_PrepareToEncode, CFHD_EncodeSample and CFHD_GetSampleData; it never invokes
HyperCine encoding or writes SDK state. The legacy CUDA/Windows comparison
exporter remains untouched: a GPU is not needed to capture an original packet.
Python hashlib provides portable SHA-256 without Windows bcrypt or a new core
dependency. Aligned input uses the platform allocator.

Inputs: verified source corpus archetypes, cyclic replay with an additional
frame identity stamp, not a claim of reproducing GeneratePhase3AFrame. The
separate fixed experiment is xorshift32 grayscale noise at amplitudes
128,112,96,80,64,56,48,40,32,24 (four frames each), seed 0x1337BEEF. No
post-hoc replacement sequence is substituted if hold coverage is negative.

## Fail closed

Set HYPERCINE_SDK_GAP_CORPUS_ROOT and HYPERCINE_SDK_GAP_INDEX_SHA256 to register
p6_sdk_gap_packets. The index is pinned before any corpus content is accepted;
all indexed files including manifests and capture traces are hashed. The
manifest must identify the frozen original SDK and exact 120/40-frame order.
Packet tags 34/53/54 are parsed independently from opaque entropy chunks;
intermediate lowpass slots absent from packets are null, not fabricated. Only
parsed GUID/DATE/TIME/TIMC values may be masked during full byte comparison.

The gate requires an original hold witness in all three relevant profiles in
addition to byte parity. A missing, stale or negative corpus is not PASS.
Mandatory SDK-free controls cover same-length packet mutation, truncated and
reordered traces, missing artifacts and stale manifest schema/profile/geometry.

## Measured hold experiment: negative result

All four 120-frame original SDK sessions and three 40-frame ramp sessions
completed: 480 sequence packets + 120 ramp packets. The fixed ramp did **not**
reach the requested 80%-100% hold band in any profile. No substitute sequence.

| Profile | Ceiling | Non-initial previous-sample bitrate range | In-band frames |
|---|---:|---:|---:|
| Low | 110,000,000 | 188,558,400–392,173,440 bps | 0/39 |
| Medium | 130,000,000 | 207,384,000–492,225,600 bps | 0/39 |
| High | 150,000,000 | 269,607,360–611,473,920 bps | 0/39 |

Complete per-frame trajectories and real parsed quant/scale tags:
[SDK_GAP_TRAJECTORY_46.json](SDK_GAP_TRAJECTORY_46.json). Bitrate is the preceding
SDK packet size × 8 × 30 (GOP1, SDK default rate); frame zero has no preceding
sample. No hidden state is inferred from this arithmetic.

Observed tag 53 luma level-1 triplets: Low stays 128/128/192; Medium changes
64/64/96 → 96/96/144; High changes 32/32/48 → 48/48/72. Tag 54 scale triplets
stay 2/2/1. These real packet changes are above the ceiling, **not hold evidence**.
Low base and midpoint tables coincide; equal packet tables cannot identify its
hidden hold state. The strict gate refuses that claim.

Transfer index pin: `dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb`.
Capture executable SHA256: `335c3eb3b22fcdbaec0ffdcb4f1da84ce61571192fa9caaf12618db4986b3e05`.
The capture ran before allocator portability cleanup. The retained exact source
snapshot rebuilds to the identical executable hash with file-prefix-map; it is
in the corpus. The committed portable allocator variant compiled separately.
No upstream source changed.

## Complete 120-frame writer parity

| Profile | Equal packets | Result | First differing offsets |
|---|---:|---|---|
| Low | 120/120 | PASS | none |
| Medium | 78/120 | FAIL | 9 or 10 |
| High | 78/120 | FAIL | 9 or 10 |
| FilmScan1 | 120/120 | PASS | none |

Full per-frame sizes, first 32 differing byte positions and differing counts:
[SDK_GAP_PARITY_46.json](SDK_GAP_PARITY_46.json). Only parsed variable metadata
values are masked. Both strict and sequence-only opt-in gates fail on the
observed Medium/High divergence. The strict gate also requires a real hold witness.
The 40-frame ramp itself has 40/40 byte parity for each profile; that is not hold
coverage. Every profile was actually run: no NOT_RUN rows.

Medium frame 17 is 165,116 SDK bytes vs 167,532 writer bytes; first normalized
difference offset 10, 38,217 differing common-prefix bytes plus 2,416 tail bytes.
Real SDK tag 53 level-1 luma remains 96/96/144, writer emits 64/64/96 after a
39,621,120 bps preceding sample. This is actual long-session state divergence,
not an oracle-generation failure. No codec fix is mixed into this capture task.
High independently measures 78/120 FAIL; frame 17 is 168,720 SDK bytes vs
171,148 writer bytes, first difference offset 10; actual SDK level-1 luma tags
48/48/72 vs writer 32/32/48. FilmScan1 is 120/120 byte-identical.

Remaining blocker: no original hold-band witness. One next action: review the
negative trajectory and design a separately authorized lower-amplitude capture
that enters then stays inside the measured ceiling band; do not relabel this
run as successful. Low packet-table observability also needs explicit review.

Native scope: Linux x64, GCC 13.3 original SDK, GCC 14.2 SDK-free writer, CPU
packets only. No Windows-node, Metal, CUDA, 8K or alternate-timebase claim.
The SDK-free baseline full Release CTest was 115/115 PASS before corpus opt-in;
the complete corpus-enabled Release run reports **116/118 PASS, 2 FAIL, 0
skipped**, 563.69 s. Both failures are substantive admission failures:
`p6_sdk_gap_packets` (missing hold + Medium/High divergence) and
`p6_sdk_gap_sequences` (Medium/High divergence). The new comparison-site
negative control is PASS. This is not a green suite or a support promotion.
The exact source loaded by the long-running strict gate preceded final
report-field and path-hardening changes; final sequence and unit/control runs
exercise the current code. No codec source changed during measurement.

Complete differing-position ranges are retained in the private diagnostic artifact
(half-open byte ranges, 42 Medium + 42 High frames), SHA256
`f25c4b55341e3db0c5738bc7951075ed706c09ad9c62506f7cc7e3025acd5131`.
New gate reports emit these exact ranges as well as first-offset summaries.
