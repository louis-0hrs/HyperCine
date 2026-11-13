# Issue 383: bounded FFmpeg CFHD multigeneration control

2026-11-13. **CONTROL ONLY**, not HyperCine, GoPro SDK or normative VC-5
qualification. Implements a first executable measurement slice of #383/#382.
No existing codec, CMake, CI, private corpus or frozen upstream is changed.

## Actual experiment

FFmpeg `7.1.5-0+deb13u1`, Linux x86-64, same FFmpeg executable for encode/decode.
Executable SHA-256:
`e8a8d46f5225f3062cec7c07fb145d58ae73c603cb740dcd5bad34bfb54e455a`.
Runner SHA-256:
`43a0d82a7454f5cfdb67c40393bbd2f1b4de3017400bb4535a18a85fd49e7b12`.
Unit test SHA-256:
`e5c2e587900e5150334a0e2cc27f378c3046c0497ceecc23f9b2e90001ac70c2`.

Four synthetic 128x64 planar YUV422p10le inputs: flat, boundary-crossing ramp,
checkerboard and seeded noise (`0x1337BEEF`). Input and output descriptors are
both TV range / BT.709 / left chroma position. No RGB conversion or chroma
resampling is requested; `+yuv422p10le` disables automatic format conversion.
This does not instrument internal codec arithmetic or prove the absence of all
internal rounding. FFmpeg quality option `film3`, one requested thread, new
single-frame encoder and decoder processes for every step. A preset name is not
proof of fixed actual quantization; quant/prescale/state remain **NOT_MEASURED**.

Every case executed all generations 1..50, genuinely feeding the immediately
previous decoded raw frame into the next encoder. **200 complete encode/decode
steps.** Every output and next input hash was checked for chain continuity.
Five fresh-process decodes of each identical first-generation AVI agreed
(20/20 decoded outputs). Persistent-session, cross-host and independent pixel
correctness were NOT tested. One requested thread is not a general oracle fix.

## Results

PSNR uses peak 1023, not TV-range width. Max error is in native 10-bit sample
units. AVI hashes/byte lengths in the full receipts are container identities,
not isolated elementary-packet hashes. No volatile fields were normalized.

| Pattern | Y PSNR vs original, gen 1 | Y PSNR vs original, gen 50 | Max absolute difference gen 50 vs gen 1: Y / U / V | Stable after gen 1? |
| --- | ---: | ---: | --- | --- |
| Flat | Infinity | Infinity | 0 / 0 / 0 | Yes, all measured generations; initially lossless too |
| Ramp | 59.3240 dB | 25.4335 dB | 137 / 126 / 106 | No |
| Checkerboard | 48.6594 dB | 26.3241 dB | 49 / 0 / 0 | No |
| Noise | 51.9100 dB | 24.8851 dB | 109 / 163 / 99 | No |

These results falsify zero additional loss for these particular **FFmpeg**
pipelines. They do not falsify a normative requirement, diagnose a GoPro defect,
measure HyperCine quality, or establish the source of drift. The same preset
name across generations does not isolate adaptive behavior, quantization,
companding, prescale, clipping or transform rounding. Next investigate those
separately, and compare actual GoPro/HyperCine paths at matched domains and
first-generation quality/bitrate. Do not copy FFmpeg pixels as new golden truth.

## Measurement controls actually run

Eight unit tests passed both normally and with Python `-O`: identity, lossy
idempotent quantizer, chained drift, cycle-not-fixed-point, signed error metrics,
invalid output length/range, channel swap and insufficient-generation/bounds.
Three independent mutations were detected: reusing the original input instead
of the predecessor, fabricating finite perfect PSNR, and checking only final
rather than all intermediate equality. Original source bytes restored and
SHA-256 verified before retaining the real receipts.

Normal test log SHA-256:
`60f683c7fc7c5db6fcacd6f5bcb6b014b903e14be96b9f5cd4f24a97c0e3f810`.
Optimized test log:
`d15a578fa45dcb3c9fe60de730a4aa8b28bb304a4b9f86165a2bdd2a91104a17`.
Mutation log hashes, in the order above:
`d952f619c9001fccddb3b99e0734deb86347e928920ed370bb8f3820d8de600d`,
`530df7ecbc957d3d9c4d8b942ca5ebe04fa730050a31d04301def3afadfdf79a`,
`63d26f67995e7cf6d1d2a4ce1dcc46fea25ecbb268e453b907bc06a6c36282bf`.

Full per-generation JSON receipt SHA-256 (retained outside the Git tree):
- flat: `a5d353a099fb7bb111472e63edeb2a86ce5796007ef710f4e96a7fd456921f48`
- ramp: `911baa4b349b13c805238c6606c50e14757db427f4cc248f7ef931850d86f332`
- checker: `59f9aa2763c4b5ec315091446431defc35d9dc6eb42f9af5fc9575b7dc7c37d9`
- noise: `dc312696a6b0d28bef90c977dce6ec6ae0540d98333b67e1303cebffbbafb1be`

## Reproduce

Run from the repository root with a trusted local FFmpeg. Bind the path and its
independently verified executable digest locally; the recorded digest above
identifies this experiment, not every installation. Use a new output path.

```sh
python -B -m unittest discover -s tests -p test_generation_control.py -v
python -O -B -m unittest discover -s tests -p test_generation_control.py -v
python -B tools/measure_cfhd_generations.py \
  --ffmpeg "$HYPERCINE_FFMPEG" \
  --ffmpeg-sha256 "$HYPERCINE_FFMPEG_SHA256" \
  --case ramp --output issue383-ramp-new.json
```

Repeat the last invocation separately with `flat`, `checker` and `noise`, each
with a new output filename. The runner is deliberately bounded to small
synthetic frames, 50 generations, 8-second individual process timeouts and
bounded FFmpeg output. It is for a **trusted local executable**, not a sandbox
for arbitrary hostile binaries/process trees. It has no core codec dependency.
No CTest registration or mandatory CI migration is included in this first slice.

## Failed attempts and limits

The first real invocation omitted explicit INPUT color descriptors while
prohibiting implicit conversion. FFmpeg correctly refused filter negotiation;
no result from that attempt is counted. Matching native input/output descriptors
fixed the setup without enabling automatic conversion. A combined four-case
invocation exceeded the execution environment's total call timeout; no partial
result was claimed. The retained results are four separate completed runs.

This runner does not yet measure actual quant/prescale tables, persistent sequence
state, isolated elementary packet bytes, alpha/Bayer/RGB modes, GPU behavior,
application interoperability, linked-library hashes, peak memory or timing.
Fresh-process repeatability is not an independent pixel oracle. No full
repository CTest/build or hardware suite was run: local GitHub DNS prevented a
complete checkout; this standalone source and its tests were executed locally.
The independent reviewer must verify this limited scope before integration.
