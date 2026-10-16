<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# FFmpeg versus archived GoPro decoder outputs — issue #11, first audit

The four existing 128×128 packets reproduce **stable decoder differences**,
not decoder pixel identity. Flat is exact; the three other packets differ by
at most three unsigned p10 levels. This extends the existing
[Windows standalone oracle](WINDOWS_STANDALONE_ORACLE.md) with fresh thread-mode
replays, signed errors, spatial counts, first 32 differences **per plane**, and
verified extraction of the identical compressed packet. It does not repeat or
replace the Windows oracle exporter. No codec, upstream or CUDA code changes.

Audited baseline: `dc8e67f9f9ac50adb135dba948339543a834a613`.
The [machine-readable matrix](reference_corpus/decoder_matrix.json) contains
actual measurements, 30 parsed bands per packet (geometry, quantization, scale
where present, payload SHA256), packet hashes, SDK provenance, tool binary hashes
and the analysis script hash. There are no newly generated media fixtures.

## Inputs and comparison contract

The three synthetic Mac Q2 packets (`flat`, `gradient`, `canonical`) come from
`tests/fixtures/standalone/manifest.json`, encoder commit
`a0518c0f39649056116eba57547fea1b96457dcf`. They are compatible packets, not
GoPro/CUDA encoder-bit-identity references. The fourth packet is the actual
GoPro FilmScan3 control in `tests/fixtures/windows-oracle/control.cfhd`, SHA256
`2d9d57e3bd9334eca6e9a2e26b27df8a05555ee910bc673cdf0bd44a2671e411`.

The GoPro outputs are **archived Windows SDK execution**, run
[38627689864](https://github.com/louis-0hrs/HyperCine/actions/runs/38627689864),
commit `b905e5038d947c19d4759f992696678e89d4745c`, MSVC 19.51.36260.0.
The existing exporter used the pristine SDK, full resolution, explicit BT.709,
`TAG_CPU_MAX=1`, and disabled chroma filtering. For the three Mac packets it
reported one distinct output across five fresh SDK instances per format. The
control has one archived SDK decode. This audit runs **no SDK on Apple ARM64**
and makes no new claim about SDK repeated-instance determinism.

All fixture files are checked against both existing manifests before decoding.
A test-only AVI wraps one unchanged packet. FFmpeg stream-copy extraction through
its rawvideo muxer must reproduce exactly that packet before any pixel comparison.
This extraction uses `-c:v copy`; the muxer name does not imply decoded pixels.
FFprobe must report one CFHD video stream, 128×128, `yuv422p10le`. Each decode
must produce exactly 65,536 bytes: one complete frame, no missing/extra planes.

The common numerical space is top-down planar **Y, Cb, Cr**, little-endian
unsigned 10-bit samples in 16-bit containers, tight rows (256 bytes Y, 128 bytes
Cb/Cr), BT.709 studio range. Archived SDK YU64 was explicitly unpacked from
`Y0,Cr,Y1,Cb` and shifted right six bits by the existing Windows exporter.
That loses any lower six SDK bits; this is a **p10 comparison**, not a p16 parity
claim. No orientation search, clipping, pixel masks or best-match selection.

Three fresh FFmpeg processes per packet and mode:

* `bitexact_single`: input `-threads 1 -flags +bitexact`.
* `default_auto`: input `-threads 0`, ordinary decoder flags.
* `bitexact_two`: input `-threads 2 -flags +bitexact`.

Each process decodes all frames to explicit raw `yuv422p10le`; the exact byte
count also detects extra frames. Requested thread counts are recorded; this
small progressive workload does not prove that multiple workers did useful
work. FFmpeg/ffprobe identify as n8.0; the locally available validator was built
from `140fd653aed8cad774f991ba083e2d01e86420c7`. Binary SHA256 values in the
matrix identify the actual executables without exposing local build flags.

## Measured results

Native macOS arm64, 2026-10-16. **36/36 fresh FFmpeg decodes** completed;
all three repetitions and all three modes produced the same output hash per
packet. Each whole frame has 32,768 samples. Sign is always **FFmpeg minus SDK**.

| Packet | Differing samples | Exact samples | Max absolute error | Mean signed bias | Overall PSNR |
| --- | ---: | ---: | ---: | ---: | ---: |
| Flat Q2 | 0 | 32,768 | 0 | 0 | exact |
| Gradient Q2 | 16,633 | 16,135 | 3 | −0.477692 | 62.705486 dB |
| Canonical Q2 | 16,380 | 16,388 | 3 | −0.467865 | 62.842691 dB |
| GoPro control | 16,821 | 15,947 | 3 | −0.486328 | 62.751775 dB |

Overall PSNR aggregates squared errors across Y/Cb/Cr with peak 1023; it is
not an average of plane PSNRs. The JSON contains exact fraction, max error,
mean absolute error, RMSE, signed bias, error histogram, first 32 differing
coordinates/values, affected rows/columns and busiest rows/columns **per plane**.
Exact PSNR uses JSON `null` plus `psnr_infinite=true`, never a nonstandard Infinity.
The CLI does not turn a PSNR threshold into a parity PASS.

Most differences are in the interior: for Gradient Y, 7,914 interior versus
303 outer-border mismatches; Canonical Y, 7,965 versus 285; control Y, 7,880
versus 305. Chroma also differs throughout the image. These observations rule
out an exclusively outer-border effect **in this decoder comparison**. They
are separate from the known width-16 encoder coefficient issue #21.

The same immutable packet has different SDK and FFmpeg reconstructed p10 planes.
The codec encoder cannot explain a difference between those two decodes of that
same packet. Small errors with a negative bias are **consistent with differing
inverse-transform/output rounding**, but no exact rounding operation is proven.
Do not patch either decoder or subtract a guessed bias to produce a false match.
Stage-level signed inverse-wavelet evidence is the next localization gate.
The independent FFmpeg [CFHD decoder source](https://github.com/FFmpeg/FFmpeg/blob/140fd653aed8cad774f991ba083e2d01e86420c7/libavcodec/cfhd.c)
is the relevant entry point for that follow-up, alongside the frozen SDK inverse
transform and output-conversion paths. This report makes no source-derived
claim that one decoder is mathematically correct and the other incorrect.

RGB remains `PENDING_MATCHED_CHROMA_UPSAMPLING`: the existing oracle's SDK
unfiltered versus FFmpeg bicubic RGB conversion uses different pipelines.
Those diagnostic BGRA results must not become a decoder parity verdict here.
Likewise source-versus-decoded quality is a separate experiment; the existing
control's large source-chroma-border error is not masked by this small
SDK-versus-FFmpeg error.

## Reproduction and validation

With Python 3.9+, CMake 3.25+, FFmpeg and ffprobe on PATH:

```sh
python3 tests/compare_cfhd_decoders.py --ffmpeg ffmpeg --ffprobe ffprobe \
  --repeats 3 --output build/decoder_matrix.json
cmake -S tests/decoder_diff -B build-decoder-diff
ctest --test-dir build-decoder-diff -C Release --output-on-failure
```

The separate CTest project requires both external tools; missing tools fail
configuration. Absolute executable paths can be configured locally using
`HYPERCINE_FFMPEG_EXECUTABLE` and `HYPERCINE_FFPROBE_EXECUTABLE`. No paths are
stored in the public report. Temporary containers and pixels are removed.
Fixture failures and decoder failures stop with an error; numerical differences
remain measured results. Nonstable FFmpeg hashes write `UNSTABLE_OUTPUT` and
return failure. Matching hashes across modes refer to `bitexact_single` metrics
rather than duplicating the same arrays.

Validation performed:

* Differential CTest **2/2 PASS**, including six unittest cases: hand-calculated
  signed metrics/PSNR, exact images, border/interior and first-32 counts,
  little-/big-endian input, padded stride, vertical orientation, unsigned
  extremes, planar chroma ordering, malformed storage and fixture tampering.
* Complete configured SDK-free Release CTest **27/27 PASS**, including existing
  fixture, manifest, sequence and independent external-decode gates (76.85 s).
  Apple Clang 21.0.0, Python 3.9.6, CMake 3.31.5. Reproduce the main suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 3
ctest --test-dir build -C Release --output-on-failure
```

This is the first bounded #11 deliverable, not completion of the entire issue.
1080p/4K/8K and 120-frame **same-packet SDK decoded planes** are not present in
these fixtures and remain PENDING. Golden bitstream/encoder manifests alone
cannot supply missing decoded pixels. New SDK thread/dither experiments,
matched RGB, exact first divergent inverse operation, CUDA decoding, sanitizer
qualification, Premiere and Resolve are NOT RUN. No performance claim,
production promotion, automatic merge or issue closure.
