# Mandatory Metal 4K decode/parity gate (#45, #23)

## Defect and correction

The registration named `filmscan3_metal_4k_ffmpeg_gate` previously launched
`hypercine_encode_filmscan3`, the CPU tool. It neither required a Metal device
nor decoded output from the audited Metal tool. A successful historical run of
that registration is CPU decode evidence, not GPU decode evidence. Existing
native complete-packet equality results remain separate valid evidence.

The registration now launches `hypercine_encode_filmscan3_metal` with
`--require-metal --frames 16` and a separate CPU encoder. The existing audited
Metal frontend compares actual color and three-level wavelet buffers, including
padding, against the scalar frontend. Its completion report must cover all
16 frames and 144 channel-level audits; a device-name line alone is rejected.
The Metal tool throws when no Metal device is available. No skip/fallback is
permitted in this gate, and it has `metal;gpu-required;external-decode` labels.

Both independent encoder sessions process the same ordered 16-frame sequence
without Reset. Four deterministic 3840x2160 fixtures (flat, ramp, edges, detail)
cycle four times. No manifest tables, reference headers, or golden bytes are
supplied to either encoder. Every frame requires:

- full unmasked CFHD packet byte equality, with first differing byte reported;
- exact frame index, limiter, previous-sample size and current-sample size;
- exact 36-entry quant/scale tables and all 30 coefficient hashes;
- strict trace types/cardinality/order, rejecting duplicate JSON keys;
- separate FFmpeg decode of **both** encoded outputs, with 3840x2160 geometry,
  yuv422p10le format and 33,177,600 decoded bytes per frame.

The previous negative control merely compared a decoded size with arithmetic
for other dimensions. Portable tests now invoke the actual decode validator
with wrong geometry/format/storage, mutate packet/trace evidence, refuse CPU
substitution, and execute the actual CMake registration in a minimal project.
The required standalone CI inventory now includes both native 4K gates and
the portable contract suite. Deleting a gate cannot silently preserve green CI.

## Measured validation (2026-10-28)

Exact tested code: `ad67d0050815cd1b2060671544bf8ef65c8e9174`. Native Mac source blobs were independently matched to this commit; upstream stayed unchanged. [Machine-readable results](metal-4k-decode-results.json) preserve the source fingerprints and evidence boundaries.

| Execution | Result |
| --- | --- |
| Linux x86_64 GCC 14.2 full Release CTest | 106/106 PASS, zero skips |
| Native M2 Pro ARM64 full Metal Release CTest | 114/114 PASS, zero skips; eight GPU-required tests |
| Native M2 Pro Debug ASan/UBSan, Metal API+GPU validation | Targeted 4K encoder, portable contract, new 16-frame decode/parity gate: 3/3 PASS, zero skips |
| Native Windows x64 optimized Python | New contracts 13/13 and CI helpers 24/24 PASS |
| Native Windows x64 4K CPU smoke | One actual 3840x2160 yuv422p10le decode, 33,177,600 bytes: PASS |
| Native Windows x64 complete CTest | INCOMPLETE: gateway node invoke timeout at 1,200 seconds; no completed JUnit, no full-suite PASS claim or blind rerun |

The native M2 Pro corrected gate took 27.61 seconds in Release and decoded 16 CPU plus 16 Metal packets after exact packet/state/table/band comparisons. Debug sanitizer scope was three tests (521.76 seconds), not the full Debug suite. Metal API and GPU validation were confirmed enabled in actual test output. Leak detection was disabled; no leak-qualification claim. External Python and FFmpeg were not instrumented. Four actual CMake-registration mutations were rejected. No new performance benchmark was run.

## Resource scope

Four input files use 132,710,400 bytes total. Encoders run sequentially, with
only one process at a time, and decoded files are overwritten per frame, not
accumulated as 32 raw 4K outputs. CTest runs this gate serially. Native audit
snapshots add memory and are not a production performance measurement.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_ENABLE_METAL=ON \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build -C Release -R filmscan3_metal_4k_ffmpeg_gate -V
python3 -O tests/test_filmscan3_4k_gate.py
python3 -O ci/test_standalone_gate.py
```

Use a native Apple Metal device for the first four commands. The Python
contract suite runs on Linux/Windows/macOS without a GPU and does not qualify
Metal. CPU-only full CTest and explicitly labeled CPU/CPU replay also do not
qualify Metal. This change does not establish original GoPro-SDK 4K parity or
the still-missing original signed intermediate-stage oracle from #23, does
not qualify 8K+, and does not close either parent issue.
