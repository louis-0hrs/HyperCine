<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# RCA-160 Windows cross-host band-0 confirmation — issue #158

Status: **BOUNDED_CROSS_HOST_CONFIRMATION_PASS / LIMITED_INDUCED_POSITIVE /
RELEASE_GATE_UNCHANGED**. On the EPYC 7601 Windows guest, a test-only strict
band-0 admission gate removed the corruption seen in the gate-off arms on the
pinned HALF/BGRA packet. This confirms the *bounded intervention* on a second
host. It is not a production decoder patch, a general original-SDK repair, or a
release qualification. The #120 gate stays
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## What was measured

[`band0_1080.py`](../../tests/reference_decode/band0_1080.py) ran the
pre-registered protocol
`rca160-windows-cross-host-band0-20261019-v1` with one decode per fresh
process, arms executed sequentially in the fixed order O → B → A′.

| field | value |
| --- | --- |
| packet SHA-256 | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` |
| packet bytes | 900396 |
| source BGRA SHA-256 | `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1` |
| source bytes | 8294400 |
| protocol SHA-256 | `c6a52b610e0c55a2b8b10d0984df840913ac68b85ce3ae7fd7fbca15fdfb4c25` |
| instrumented worker SHA-256 | `e889fbd198f1b06d3615b2e712c12231097aa8950ed0721fa9a977773f2edfa6` |
| gate applier SHA-256 | `712fa0a81a939eb33e2a39ac2d29b38f79e7f47b123a8bbf2d1df0b7526b9220` |
| probe header SHA-256 | `88031889d7e35d9dc355c492ab311a6d64bd44237628514c4db5ea85a23157c5` |
| patched private `decoder.c` SHA-256 | `ebf031082905e1b4cfb5818fe23df98b4c804675a06d2834cbfd62a3ca363f09` |
| compiler / CRT | MSVC 19.44.35229.0, `/O2 /Ob2 /DNDEBUG`, dynamic `/MD` |
| CUDA | not linked |
| calls | 360 (3 arms × 120), 360 fresh processes |
| child bound | 60 s; no child timeout |
| output domain | 960×540 BGRA, pitch 3840, 2,073,600 written bytes, zero row padding |
| digest domain | full SHA-256 over every written byte; geometry and padding recorded per call |

The worker reported `CFHD_ERROR_OKAY`, `metadata_open/init/cpu/set_status = 0`,
`PROCESSING_ALL_ON` 65535, `TAG_CPU_MAX=4`, HALF resolution and the expected
BGRA geometry on all 360 calls. A route probe on every output conversion
reported `CopyLowpass16sToBuffer`, `encode_format=1` (YUV422), so the exercised
path is the non-RGB444 lowpass conversion used by the Linux lane, not an
IPPFast/progressive route.

## Transferred gate

The frozen repository tree was not modified. The sanitized applier
[`apply_rca160_gate.py`](../../tests/reference_decode/apply_rca160_gate.py)
patches a private out-of-repo copy using the two spatial lowpass branches of
`ReconstructWaveletBand` as anchors; the `index == 3` highpass branch is left
untouched. Gate-off executes the original `BANDS_ALL_VALID` predicate only.
Gate-on additionally requires

```c
(wavelet->band_valid_flags & 1u) != 0 && wavelet->band_valid_flags == wavelet->band_started_flags
```

with a bounded wait of 50,000,000 spins or 5,000 ms, after which the original
`CODEC_ERROR_BAD_FRAME` return is taken. Transform math, the frame writer and
scheduling are unchanged. Readiness is observed on the same `IMAGE` pointer; a
`BAND_PUBLISH band=0` transition from invalid to valid must fall inside the wait
window.

The probe also records `band_started`/`band_publish` transitions and the output
route. It never writes to codec state.

## Results

| arm | gate | calls | valid | distinct outputs | modal | corrupt |
| --- | --- | --- | --- | --- | --- | --- |
| O | off (original) | 120 | 120 | 5 | 116 | **4** |
| B | on (strict band 0) | 120 | 120 | **1** | **120** | **0** |
| A′ | off (same binary) | 120 | 120 | 2 | 119 | **1** |

The modal output was
`38d1c1498d3a990a141b622b8758a49fd1b634aa8042ef402d219fc9ee68c686`
in all three arms, and its 16-hex prefix matches the Linux modal
`38d1c1498d3a990a…` reported in #160.

Corrupt calls and premature entries:

| arm | corrupt calls | each corrupt call's premature entries |
| --- | --- | --- |
| O | 94, 103, 108, 112 | exactly 1 |
| A′ | 37 | 2 |
| B | none | 60 waits, all consumed before the transform |

Premature entries (band 0 absent at gate entry) were *necessary but not
sufficient*: O had 31 such calls and 4 corrupt outputs; A′ had 42 and 1. This is
the same race-window shape reported by #160, not a deterministic switch.

## Coverage

| check | result |
| --- | --- |
| gate entries per call | 9/9 in every O, B and A′ call (3 channels × one index-1 and two index-2 sites) |
| B real waits | 60 |
| B waits matched to a band-0 publication on the same `IMAGE` | **60/60** |
| B wait channels | ch0/index1 55, ch2/index1 5 |
| B entry flags | `valid=0xE, started=0xE` in all 60 |
| B post-gate flags | `valid=0xF, started=0xF` in all 60 |
| B gate timeouts | 0 |
| B wait duration | 0 ms 42, 1 ms 10, 2 ms 6, 3 ms 2; spins 1–56,320 |

No call waited where band 0 was already valid. The Windows guest exercised the
same vacuous-gate window as Linux, but on ch0 and ch2 rather than the Linux
lane's ch1-only waits.

## Cross-host comparison

| observation | Linux #160 | EPYC Windows #158 |
| --- | --- | --- |
| packet / condition | same 900396 B, HALF/BGRA | same |
| O corrupt | 25/120 | 4/120 |
| B corrupt | 0/120 | 0/120 |
| A′ corrupt | 30/120 | 1/120 |
| real waits | 22/120 calls, ch1/index1 | 60 waits in 57/120 calls, ch0/index1 and ch2/index1 |
| modal prefix | `38d1c1498d3a990a` | `38d1c1498d3a990a` |

The zero in B and nonzero in both gate-off arms meet the pre-registered rule on
this host. The induced-positive control is **weaker** here than on Linux
(1/120 versus 30/120), so the confirmation should be read as bounded evidence
that the same admission window exists and is removable on Windows, not as an
equally strong causal effect size. Cross-host rates are not claimed to be
comparable; scheduling, CPU count and build differ.

## What this does not establish

- No general original-SDK repair. The frozen upstream defect and the release
  gate are unchanged.
- No claim that every corruption state is caused by this gate, or that the
  gate is safe for production. It is test-only and was not benchmarked.
- The gate deliberately changes timing. Unchanged scheduling policy does not
  imply unchanged interleaving.
- A plain polling loop on ordinary flag fields is a diagnostic, not a portable
  synchronization/publication contract. The writer publishes under the entropy
  worker lock; the gate reads without it, like the original consumer.
- One packet, one resolution, one output format, one Windows guest. No GPU,
  affinity grid, macOS or Linux rerun is claimed here.

## Reproduction

Relative to an out-of-repo copy of the source tree and the loose corpus bound
through `HYPERCINE_GOLDEN_DIR`:

```bash
python tests/reference_decode/apply_rca160_gate.py --root <private-sdk-copy>
cmake -S <private-sdk-copy> -B <private-sdk-copy>/build -G "Visual Studio 17 2022" -A x64 \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_UPSTREAM=ON
cmake --build <private-sdk-copy>/build --config Release --target reference_decode_worker
python tests/reference_decode/band0_1080.py \
  --worker <private-sdk-copy>/build/tests/Release/reference_decode_worker.exe \
  --packet "$HYPERCINE_GOLDEN_DIR/decoder-rca/2026-10-18/matched-sha-1080-bgra-full-20261018/sample_1080p_filmscan3_gop1.cfhd" \
  --source "$HYPERCINE_GOLDEN_DIR/decoder-rca/2026-10-18/matched-sha-1080-bgra-full-20261018/source_1080p_bgra.raw" \
  --out <local-output> --json <local-json> --calls 120
```

## Verification performed

- Full native Release build: PASS.
- `ctest --test-dir build-pristine -C Release --output-on-failure`: **14/14 PASS**.
- `tests/reference_decode/test_affinity_protocol.py`: **5/5 PASS**.
- `tests/reference_decode/test_band0_protocol.py`: **14/14 PASS** (parser, coverage matching and fail-closed verdict rules).
- Protocol preflight hashes: PASS.
- `git status --porcelain upstream/`: empty before and after.
- Sanitized per-call ledger:
  [`rca160_windows_cross_host_band0_2026-10-19.json`](rca160_windows_cross_host_band0_2026-10-19.json).
