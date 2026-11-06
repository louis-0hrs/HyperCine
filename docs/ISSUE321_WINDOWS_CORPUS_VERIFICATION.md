# Issue #321 — independent Windows SDK corpus verification

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

**2026-10-30 follow-up:** the original Windows library and both capture
executables have now been recovered, hash-verified, privately archived and
replayed on native Windows. See [retained-build provenance](validation/ISSUE321_RETAINED_PROVENANCE_20261030.md).
The historical snapshot and its original limitations below remain dated evidence.

Snapshot: 2026-10-29; repository base `9909bfccf220ec914a3c1ebf3e57fc9d95696f99`.
This is read-only corpus integrity and bounded packet observation, **not** a
VC-5 conformance, decoder, GPU or historical-build certification. No exports,
raw assets, corpus edits or upstream edits were made.

## Reproduce

Bind `HYPERCINE_GOLDEN_DIR` locally to the shared corpus root (not its
`golden/` subdirectory). Use an existing clean checkout with SDK history.
The only output is a new report outside that root; an existing output is refused.

~~~sh
python3 -B tools/verify_windows_sdk_corpus.py --output issue321-observations.json
python3 -B tests/test_windows_sdk_corpus.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHYPERCINE_BUILD_UPSTREAM=ON -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build -j4
ctest --test-dir build -C Release --output-on-failure
~~~

The script imports no corpus code and never calls an exporter. It pins each
index hash/count, requires an exact indexed inventory, rejects missing files,
symlinks, traversal, duplicate rows, size/hash changes and unsupported framing.
It exits nonzero before writing a success report on any such failure. A PASS
means the stated checks passed, not that the limitations below disappeared.
The generated JSON contains every indexed file's verified byte count/SHA-256,
every observed Windows frame's quant/scale, and exact per-frame differences.
Do not commit unsanitized local build logs or JUnit output.

## Complete transfer readback

All entries include inputs, packets, manifests and provenance source files.
The transfer index cannot hash itself; its own digest is independently pinned
in the verifier. Extra files are rejected. Counts exclude the four index files.

| Corpus | Files size + SHA-256 OK | Sum of indexed bytes | Index SHA-256 |
|---|---:|---:|---|
| Windows A | 606/606 | 1,520,292,514 | `62c6f4c9297d760f1f14bde73ad15f62ca254371fa8b05f55c7ddf68e1d2e0bc` |
| Windows B | 177/177 | 390,885,680 | `72e7c314e0e28a9831d1e9270045642b28ade24aec9363f550c825a6a6fa5857` |
| Windows C | 35/35 | 59,385,771 | `a718243eb1f0d9efdd1c8fb030720eff7d2321d20d24fad5860b26bb8f91a4e5` |
| Linux SDK gaps | 776/776 | 2,024,958,339 | `dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb` |

**Windows: 818/818. Combined: 1594/1594**, plus four verified index digests.
The comparison corpus resides under the historical Windows quality namespace;
its Linux attribution is documented capture provenance, not inferred from its
storage location. The original 1760-entry baseline is outside this check.

## Order: packet evidence versus input assertions

Independently walk the sized sample/level/subband and metadata chunks. Entropy
payload is skipped by its declared boundary, never searched for apparent tags.
All 646 Windows packets (A 480, B 145, C 21) have the expected 30 ordered
channel/subband pairs, profile tag -80, frame-number tag -69 = index+1 and
metadata UFRM = index. All 480 compared Linux packets have matching witnesses.
Manifest quant/scale is checked against tags 53/54 read from actual packets;
previous-packet lengths are recomputed from the preceding file.

Input order is checked as paths **and read-back file hashes**, not merely labels:

- A: indices 0–119 map to input frames 0000–0119 for each of four profiles.
- B ramp: noise input 0006 twice, amplitudes 100,95,...,5 each twice, flat 0000
  once (43). Low calibration: noise twice; amplitudes 14,13,12,11 each three
  times; flat twice (16).
- C: noise twice; amplitude low=13 / medium=15 / high=10 three times; flat twice.

A packet does not contain a cryptographic hash of its source pixels. Thus
packet sequence numbers prove stored packet order; manifest/hash agreement
proves the declared input sequence, **not** historical input-to-packet causality.
A controlled replay would be needed for that stronger claim.

## Framerate and state: explicitly different evidence classes

**Actual playback framerate is NOT_MEASURED.** No FRMR record was found in the
parsed metadata of the 1126 inspected packets. UFRM and frame-number counters
are ordinal witnesses, not clocks. DATE/TIME/TIMC are capture metadata, not a
measurement of playback cadence. There is no timed container in this check.

Source inspection is checkable: `upstream/EncoderSDK/SampleEncoder.h` initializes
`m_frameRate(0.0)`; `SampleEncoder.cpp` passes it into the encoder;
`upstream/Codec/quantize.c:SetTransformQuantization` falls back to 30 when the
rate is outside (10,120). Supplied capture sources do not set the frame rate.
This supports a **30-fps source-fallback calculation**, not proof that the
absent historical Windows binary used those sources or that playback is 30 fps.

**overbitrate, FSratelimiter and lastgopbitcount are not packet fields in this
observed dialect.** They are independently read from each indexed raw C trace,
then compared to its manifest. The supplied capture source reads them from
`CSampleEncoder::m_encoder` after EncodeSample/GetSampleData. This source
inspection supports the stated mechanism, but does not authenticate execution
of that source. A/B have no such state traces; do not infer their internal state
from quant tags. The verifier does not relabel trace values as packet reads.

C trace observations (all FSratelimiter values are 0):

| Profile | Packet bytes at indices 0,1,2,3,4,5,6 | overbitrate 0..6 | lastgopbitcount 0..6 |
|---|---|---|---|
| Low | 1655372,1655372,440980,440980,440980,163452,163452 | 0,2,3,2,1,1,0 | 13242976,13242976,3527840,3527840,3527840,1307616,1307616 |
| Medium | 2117644,1834008,520400,520400,520400,165024,167256 | 0,2,3,2,1,1,0 | 16941152,14672064,4163200,4163200,4163200,1320192,1338048 |
| High | 2712844,2336132,525552,525552,525552,168408,170824 | 0,2,3,2,1,1,0 | 21702752,18689056,4204416,4204416,4204416,1347264,1366592 |

All 21 lastgopbitcount values equal **actual packet bytes × 8**.
At indices 3,4,5 the preceding actual packet yields:

| Profile | Previous bytes | Bytes × 8 × 30 (bit/s) | Source limit | Percentage |
|---|---:|---:|---:|---:|
| Low | 440980 | 105835200 | 110000000 | 96.213818% |
| Medium | 520400 | 124896000 | 130000000 | 96.073846% |
| High | 525552 | 126132480 | 150000000 | 84.088320% |

The trace enters at frame 1, rises at 2, descends 3→2→1 at frames 3/4,
**holds at 1 at frame 5**, and leaves at 6 after a smaller real packet. Calling
both frames 3/4 a constant-state hold would be inaccurate: they are in the
80–100% band but the ladder descends. These percentages are conditional on the
source-fallback 30-fps scenario, not measured framerate.

## Windows versus Linux: precise 120-frame result

| Profile | Same input bytes/order | Same packet size | Same all-band quant/scale | Same normalized packet | Raw-identical packet |
|---|---:|---:|---:|---:|---:|
| Low | 120/120 | 120/120 | 120/120 | 120/120 | 0/120 |
| Medium | 120/120 | 120/120 | 120/120 | 120/120 | 0/120 |
| High | 120/120 | 120/120 | 120/120 | 120/120 | 0/120 |
| FilmScan1 | 120/120 | 120/120 | 120/120 | 120/120 | 0/120 |

For **each profile and every frame index 0–119**, no codec payload, quant_53,
scale_54 or packet-size difference exists. First differing codec frame: **none**.
Normalization masks only parsed GUID/DATE/TIME/TIMC payloads with checked record
types/lengths, never UFRM, quant, scale or entropy. The generated JSON records
per-index volatile field names and value digests without publishing raw IDs or
capture clock values. Precisely **GUID, TIME and TIMC differ at all 480
profile/frame pairs**; DATE, UFRM and FREE do not differ. No other byte
differences remain after that bounded mask. Raw comparisons must not be
described as bit-identical.

The 78/120 Medium/High result discussed in #322 was a different comparison:
HyperCine candidate versus Linux original SDK. It is **not** a Windows-original
versus Linux-original divergence. These 480 normalized original-SDK equalities
remove that particular cross-platform packet difference as an explanation,
but do not prove the root cause or qualify #325's implementation.

## Provenance: what can actually be checked

Verified from local immutable source/data:

- Five `frozen_source_sha256` bindings in the Linux manifest match current
  `upstream/EncoderSDK/{CFHDEncoder.cpp,SampleEncoder.cpp}` and
  `upstream/Codec/{encoder.c,quantize.c,config.h}` (exact digests in JSON).
- Repository SDK tree `837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8` equals the tree
  of SDK commit `11574d0295771edccadd17af14af74a539f924c7` and the Windows
  manifests' declared trees. This binds available source, not historical binary.
- All supplied provenance source/CMake/package/generator bytes match their
  transfer entries. Linux capture source matches declared SHA-256
  `70633459c0243e2ddeebb59d94b06632ef117892d382fe0d9dd1149d6ac6e1e7`.
- Capture source uses SDK API calls and a single encoder instance per invocation;
  state source reads rather than injects the three reported fields.

**Not independently established by the supplied corpus:**

1. Actual Windows compiler identity/version, build time, OS/host and executed
   flags (MSVC 19.44.35229 / Release x64 are manifest claims).
2. Static archive bytes for declared hash `65ba13a8f7d26f5c94cc18520a6e7ce1e4ca5b35ae084d65b5b16b022b7fc84f`;
   archive absent; no build log or source-to-library reproduction in this run.
3. Capture executable bytes for A/B hash `6b1b88ddbfc45f56390e77e63ac4bcbb496723b86af292ac788a1531f6924d53`
   or C hash `6e218944b0a5724c3da4e3482a9efe11fdfd3447a204b81d4456b03847e7106a`;
   binaries absent, hence source-to-executable and library-to-executable binding
   are not independently verifiable. CMake merely names a pre-existing archive.
4. Which frozen source, flags and ABI produced that historical archive, whether
   it was pristine at build time, and whether the documented commands were run.
5. Historical trace authenticity/no-state-injection in the executed binary;
   source supports it, but checksums alone do not prove origin or execution.
6. Original input generator seed and original acquisition process. Derived input
   generator source is hashed, but generation is not executed by this verifier.
7. Actual playback cadence / explicit frame-rate configuration.
8. Linux `capture_sha256` executable and `generator_sha256`: referenced artifacts
   absent here. The reconstructed-source executable reproduction assertion is
   not rerun. Original source-transfer index assertion is not verified here.

## Validation and maintainer gates

Local Linux x86-64, GCC 14.2, CMake 3.31.6, Python 3.13.5; CPU only.
16 synthetic regression tests cover missing index/file, size corruption,
same-size hash corruption, pinned-index corruption, duplicate/malformed/unindexed
entries, traversal/symlinks, count mismatch, malformed/truncated framing,
packet order/profile mutation, and manifest/input hash mismatch.
Full configured Release CTest: **56 registered, 55 passed, 0 failed, 1 skipped**,
67.02 seconds; exit 0 agrees with JUnit. The skipped
`ffmpeg_oracle_qualification` is not an external-decoder PASS.
`windows_sdk_corpus_contract` passes all 16 synthetic checks.
Sanitized computed snapshot: [JSON](evidence/issue321-windows-corpus.json).
No native Windows execution or GPU result is claimed.

Maintainer next actions:

- Review script framing/mask assumptions and reproduce against the pinned data.
- Accept integrity evidence separately from unproven build/cadence provenance.
- If stronger build provenance is required, supply archived binaries/library and
  build transcript or a controlled minimal reproducible build, **not bulk exports**.
- #322/#325 consumers must qualify candidate implementation on their exact head;
  keep #321 open until that independent consumer review. No merge is authorized.
