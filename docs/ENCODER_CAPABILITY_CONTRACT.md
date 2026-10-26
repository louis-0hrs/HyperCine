# Encoder capability contract

Issue #259, parts A and B. This is a design note plus a CPU-only prototype.
It is not a product encoder, not an original-SDK reference, and not a runtime rewrite.

The public `FilmScan3Encoder` constructors stay source-compatible. This contract
does not replace them. `upstream/` is not part of the change.

## Pipeline

FilmScan3, as implemented by the single SDK-free writer in
`src/core/filmscan3_encoder.cpp`, is:

```
BGRA8 input
  -> color conversion (Y, Cr, Cb; 4:2:2)
  -> 3-level 2-6 wavelet (prescale 0, then 2, then 0)
  -> scalar quantization
  -> highpass entropy
  -> one CFHD sample (tags, metadata, lowpass, levels, bands)
```

| Stage | What it is in this tree | Varies with |
| --- | --- | --- |
| Input storage | BGRA8, 16-byte aligned pointer, stride in bytes, bottom-up in the FilmScan3 converter | Geometry and caller pitch |
| Color | `ConvertFilmScan3Bgra` (FilmScan3 integer matrix). Legacy `ConvertBgra` is a different profile and is not this session | Profile. Execution may be CPU, Metal, or a future CUDA/Vulkan frontend |
| Wavelet | `Frontend::Transform`: three levels, prescale `0,2,0`. Band shape is `width>>level` and, for chroma, half width | Geometry sets the plane sizes. The arithmetic contract is shared. Which device runs it is the backend |
| Quantization | `ComputeFilmScan3Tables` + `QuantizeFilmScan3`, driven by `FilmScan3State::fs_rate_limiter` | Profile and session rate state. Host-side for every backend that exists today |
| Entropy | `PackHighpass` on the nine highpass bands per channel | Shared coder. Payload size depends on coefficients and geometry |
| CFHD pack | One `Writer`: channel index, dimension tags 20/21, metadata, raw lowpass, `0x2100` levels, `0x2000` bands, outer `0x2200` | One writer for the family. Dimension tags and lowpass extent follow geometry. Backends must not grow a second writer |

There is no per-resolution encoder fork. A geometry the validator accepts is still
not a qualified product mode.

## Which answers are which

**Profile-specific.** Codec family, quality profile, pixel format, progressive
GOP1, 10-bit YUV 4:2:2, the FilmScan3 color matrix, and the FilmScan3 quant
tables. Six other qualities, RGB444, GOP2, interlaced, YUY2, and RGB48 are
different contracts. This prototype reports them as valid but not implemented.
Legacy fixed-Q2 (`EncodeBgra`, `kMaxPixels = 2,097,152`) stays on its own path;
this contract does not raise that cap.

**Geometry-dependent.** Width, height, input stride, plane strides, wavelet
band extents, the pixel budget, the derived sample ceiling `8*pixels+16384`,
and the uncompressed lowpass byte count. Logical width and stored stride are
different numbers.

**Backend-specific.** Where color and the wavelet run, whether that backend is
compiled, device memory, GPU pitch or buffer limits, and fallback. Fallback is
off unless the request sets `allow_cpu_scalar_fallback`. Metal, CUDA, and
Vulkan have **no** qualification record in this revision, even if a caller
marks the backend present. Seeing a CUDA device does not select a CUDA encoder.
Quantization, entropy, and the sample writer stay shared host code.

**Must be bit-identical.** For one session config, one input, and the same
`FilmScan3State`, every backend that claims parity must emit the same sample
bytes. The pure functions `ComputeFilmScan3Tables`, `QuantizeFilmScan3`, and
`PackHighpass`, and the single writer, are the bit-exact seam. Timing, device
name, and coefficient hashes are not part of that seam. This prototype does
not itself re-prove Metal or CUDA parity.

**Session state.** Fixed at construction: codec, quality, pixel format,
geometry, stride contract, frame-rate rational, GOP, backend choice, fallback,
memory and sample budgets, diagnostic mode. Mutable across frames, and reset
by `FilmScan3Encoder::Reset` without changing geometry: `frame_index`,
`fs_rate_limiter`, `previous_sample_bytes`. The frontend object and its
geometry live for the session. Quant tables are recomputed from the limiter;
they are not a process-global switch.

**Diagnostic only.** `FilmScan3Frame::coefficient_fnv64`, host/Metal timing,
and logs. Those hashes are not written into the sample. `FilmScan3Tables` are
not diagnostic: the writer stores quant and scale in the sample. Requesting
hashes embedded in the sample is refused, because that would change bytes.
The current writer always fills `coefficient_fnv64` as a side result; this
prototype does not add a switch inside the writer.

Frame rate is session metadata. The current writer does not put the rational
into the sample; the no-metadata encode path still stamps timecode as 30 fps.
A non-30 positive rational is accepted as session data and is **not** a claim
that the bitstream carries it.

## Capability query

`CheckEncoderCapability(config, device)` is internal
(`src/core/encoder_session_config.h`). It is fail-closed and does not allocate
a frame, plane, wavelet scratch buffer, or sample. It calls
`ValidateFilmScan3Geometry` for FilmScan3 integer dimensions and maps that
result. It does not reimplement the 4096 envelope or the 8,294,400-pixel
budget.

The wavelet alignment rule (width multiple of 128, height multiple of 8,
width at least 128, height at least 24) is the shared 3-level 4:2:2 shape
from `standalone.h`. It is evaluated separately so a large aligned frame is
not reported as a bad modulus. The implementation cap remains the validator's.

Axes, kept separate:

| Axis | Meaning |
| --- | --- |
| Format mathematically invalid | Non-positive size, `uint64` product or stride overflow, bad rational, wavelet shape, or a bound input that cannot hold the frame |
| Valid but not implemented | Known FilmScan3-adjacent request this writer does not implement, including per-session CPU SIMD |
| Backend not present | The requested backend is not in the caller-supplied device. A observed CUDA card is not presence |
| Backend not qualified for this geometry | Outside the validator's dimension envelope, a GPU backend, or a CPU geometry with no evidence record. Includes 4096×2024, which is inside the pixel budget |
| Memory budget exceeded | Pixel budget, sample ceiling, sample floor below the raw lowpass, or the caller's CPU/GPU ceiling below the static planning ceiling |
| Reference missing | No original-SDK oracle is linked. Always true here. Blocks only if `require_reference_oracle` is set |
| Experimentally supported | CPU scalar, validator passed, geometry is on the evidence list, and no axis above blocks. Allocation may start. This is not `CODEC_PASS` and not a reference pass |

`CODEC_PASS` is only a full encode plus an independent FFmpeg decode that
reports the requested width and height. A passing
`ValidateFilmScan3Geometry` is not that.

The static planning ceiling is the published expression in
`docs/MACOS_GEOMETRY_P4.md` (`36*pixels + 1 MiB`, plus bound input storage).
It is not a measured RSS or a measured `maxBufferLength`.

### Evidence list

CPU scalar attempts are permitted only for 128×24, 256×256, 1280×720,
1920×1080, 2048×1080, and 3840×2160. 1920×1080 and 3840×2160 already have
CPU tests on `main`. The other four are experimental attempts for this
contract's test, not native GoPro qualification. 1280×720 and 2048×1080 do
not become qualified by passing the structural guard.

4096×2024 is 8,290,304 pixels, at or under `kMaxPlaneElements` (8,294,400),
and is not on the list. 4096×2160 fails the pixel budget. 7680×4320 is
wavelet-aligned and outside the qualified dimension envelope; the pixel budget
fails as well and is reported separately. The primary reason for 8K stays
"not qualified", matching the validator, which rejects the dimension before
the pixel check.

## What this revision does not do

- No change to the sample writer, quantizer, wavelet, or public constructors.
- No CUDA, Metal, or Vulkan runtime work. Draft #249 and #255 stay separate.
- No 8K, 12K, or 17K claim, and no silent crop, downscale, or tile codec.
- No second CFHD writer.
- Root `CMakeLists.txt` is not edited. The test target lives in
  `tests/encoder_capability/CMakeLists.txt` and is not in the default CTest
  inventory until that file is added from the standalone section.

## Host evidence from this change

Linux x86_64, FFmpeg 7.1.5-0+deb13u1 (`/usr/bin/ffmpeg`). One FlatBlack BGRA8
frame, seed `0x1337BEEF`, GOP1, two fresh CPU sessions compared before decode.
`CODEC_PASS` means the packet inspection above plus `ffprobe`
`yuv422p10le` and a decoded `rawvideo` extent of `width*height*4`. It is not
an original-SDK reference and not a bitrate law for other patterns.

| Geometry | Sample bytes | Stride | FFmpeg decoded bytes |
| --- | ---: | ---: | ---: |
| 128×24 | 3116 | 512 | 12288 |
| 256×256 | 7632 | 1024 | 262144 |
| 1280×720 | 102092 | 5120 | 3686400 |
| 1920×1080 | 185648 | 7680 | 8294400 |
| 2048×1080 | 195608 | 8192 | 8847360 |
| 3840×2160 | 636612 | 15360 | 33177600 |

1920×1080 matches the existing flat golden size of 185648 bytes. 4096×2024
was not encoded: the query refused it before allocation (`NO_CODEC_PASS`).
The 636612-byte 4K figure is this flat frame only; it is not the separate
6,156,376-byte synthetic sample cited by the 8K audit.
