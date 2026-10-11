# Decode Phase 3B-0 — CineForm capability baseline

Phase 3B-0 maps what this CineForm SDK emits, what HyperCine already accepts, and where each profile stops. It does not implement a new production profile.

Branch `decode-phase3b0` starts at `9eb9e3d` (`docs(decoder): evaluate conservative Auto production policy`). Production decoder sources, the encoder, and `upstream/` were not modified. `main` stays at `5143589`.

## 1. Purpose

The production decoder is a progressive 10-bit YUV 4:2:2 decoder. Before any interlaced, RGB, or precision work, this phase records the bitstream each encoder request actually produces and compares that bitstream with the production parser, the upstream CPU decoder, and the existing GPU entropy path.

Requested encoder mode and emitted profile are separate facts. A combination that this SDK does not emit is recorded as not emitted. No fixture was manufactured for it.

## 2. Phase 3A baseline

Phase 3A through 3A-F remain the progressive record. Those reports were not rewritten.

The production contract, unchanged in this phase:

- `ParseCineFormSampleValidated` accepts 30 bands, 3 channels, precision 10, transform type 0, one frame, and the progressive sample flag.
- Geometry accepted by that parser is 64..7680 by 64..4320, width divisible by 16, height divisible by 8.
- `QueueDecode` rejects a payload above 32 MiB with `SampleTooLarge`.
- `Decoder::Capabilities()` leaves `interlaced_output` false.
- Device output is `DeviceBGRA8` or `PlanarYUV422Signed16`. Planes are signed 16-bit with effective precision 10. Device plane order is Y, U, V.
- Phase 3A-E signed-color repair and the Phase 3A-F Auto policy are untouched.
- The existing difference-coded interlaced test already showed 30/30 coefficient agreement and `UnsupportedEncodingMode` on output. This phase repeats that recipe.

Focused regression after this phase, Release:

| Test | Result | Time |
| --- | ---: | ---: |
| `test_decode_phase3ae` | Passed | 0.52 s |
| `test_decoder` | Passed | 20.64 s |
| `test_decode_phase3b0` | Passed | 2.48 s |

`test_decoder` consumes `tests/decode_phase0_golden.txt`. Full sanitizer suites were not run. No production `.cpp` changed.

## 3. Terminology

Use these names. "12-bit CineForm" is not a profile.

| Term | Meaning |
| --- | --- |
| Input precision | Bit depth of the buffer passed to `CFHD_EncodeSample`. |
| Coded precision | `CODEC_TAG_PRECISION` and `codec.precision`. The wavelet domain's nominal range. |
| Internal reconstruction | Signed 16-bit samples after the inverse wavelet, before output packing. |
| Output precision | The decoded container: BGRA8, YU64 uint16, RG48, and so on. |
| API encoded format | `CFHD_EncodedFormat` passed to `CFHD_PrepareToEncode`. |
| Internal encoded format | `ENCODED_FORMAT` stored in tag 84 and `codec.encoded_format`. |
| Progressive GOP-1 | Transform type spatial, `SAMPLE_FLAGS_PROGRESSIVE` present, one frame. |
| Interlaced GOP-1 | Transform type still spatial. The progressive sample flag is omitted. Subband 8 is difference-coded. |
| Fieldplus GOP-2 | Transform type `FIELDPLUS`, two frames, `codec.num_subbands` 17. |
| Sampling layout | YUV 4:2:2, RGB 4:4:4, RGBA 4:4:4:4, or Bayer. Read it from channel count and per-channel band sizes. |

States used below: PASS, PARTIAL, UNSUPPORTED, NOT IMPLEMENTED, NOT TESTED, UNKNOWN. Confidence is High, Medium, or Low.

## 4. Upstream profile definitions

Two encoded-format enums must stay apart.

`CFHD_EncodedFormat` in `upstream/Common/CFHDTypes.h`:

| Name | Value |
| --- | ---: |
| YUV_422 | 0 |
| RGB_444 | 1 |
| RGBA_4444 | 2 |
| BAYER | 3 |
| YUVA_4444 | 4, commented not implemented |

`ENCODED_FORMAT` in `upstream/Codec/codec.h`. Tag 84 uses these numbers:

| Name | Value |
| --- | ---: |
| UNKNOWN | 0 |
| YUV_422 | 1 |
| BAYER | 2 |
| RGB_444 | 3 |
| RGBA_4444 | 4 |
| YUVA_4444 | 5 |

`CFHD_GetSampleInfo` for `CFHD_SAMPLE_ENCODED_FORMAT` returned the API numbering on this build: 0 for YUV, 1 for RGB, 2 for RGBA. `info_channels` returned 1 even for RGBA. The channel count that describes the profile is `codec.num_channels`.

Transform types in `upstream/Codec/wavelet.h`:

| Name | Value | In `TRANSFORM_TYPE_LAST` |
| --- | ---: | --- |
| SPATIAL | 0 | yes |
| FIELD | 1 | yes |
| FIELDPLUS | 2 | yes, this is the last implemented value |
| FRAME | 3 | no |
| INTERLACED | 4 | no |

Fieldplus constants are 6 wavelets, 3 spatial levels, 17 subbands. A GOP length of 1 still takes the spatial path. The interlaced encode flag does not switch the transform type to `INTERLACED`.

Frame formats in `upstream/Codec/frame.h`: none 0, gray 1, YUV 2, RGB 3, RGBA 4. The generated samples in this survey did not carry `CODEC_TAG_FRAME_FORMAT`. Sampling is visible in the band sizes instead.

Precision enum: 8, 10, and 12. The default constant is 8. The encoder overrides it. YUY2 and UYVY set coded precision 10. RGB, RGBA, and Bayer paths set coded precision 12. Encoding those pixels as YUV 4:2:2 sets coded precision 10. No probed request emitted coded precision 8, and no YUV request emitted coded precision 12.

Encoding flags that matter here, from `CFHDTypes.h`: `YUV_INTERLACED` is bit 0 and is documented YUV 4:2:2 only. `YUV_2FRAME_GOP` is bit 1, also YUV only. `YUV_601` is bit 2. `RGB_STUDIO` is bit 8 and applies to RGB 4:4:4.

## 5. Bitstream fields

Tags are big-endian signed 16-bit pairs. A negative tag is the optional form of the same tag id. The research walker records tags below 128 and walks subband chunks (`tag & 0xff00 == 0x2000`). The full table is `benchmarks/results/decode_phase3b0_upstream_symbols.json`.

Fields that decide a profile:

| Tag | Name | What this SDK did |
| ---: | --- | --- |
| 10 | transform type | 0 for GOP-1, including interlaced. 2 for GOP-2. |
| 11 | frame count | 1 or 2. |
| 12 | channel count | 3 for YUV and RGB. 4 for RGBA and Bayer. |
| 15 | spatial levels | 2 on GOP-1 samples, 3 on fieldplus. |
| 20, 21 | width, height | Encoded dimensions. Bayer tags are half the requested frame. |
| 22 | frame format | Absent on every generated and AVI sample in this survey. |
| 62 | channel | Selects the channel that owns the following subbands. |
| 68 | sample flags | Bit 0 is `SAMPLE_FLAGS_PROGRESSIVE`. `PutVideoSampleFlags` writes the tag only when the flags are non-zero, so an interlaced sample omits it. |
| 70 | precision | 10 or 12 in this survey. |
| 72 | band coding flags | Low 4 bits are the codebook. Bit 4 is difference coding. |
| 74 | peak level | Present when a subband carries a Peak table. |
| 84 | encoded format | Internal enum. Absent on the GOP-2 samples; `codec.encoded_format` was still YUV 4:2:2. |
| 0x2000 | subband size | One chunk per entropy payload. |
| 0x4001 | peak table | Chunk of peak entries. |

`codec.c` defines `CODEC_COEFFICIENT_START_CODE` as `0x0F0F`. That marker starts a lowpass payload. Highpass payloads start with `CODEC_TAG_BAND_HEADER`.

## 6. Encoder probe methodology

`tests/test_decode_phase3b0.cpp` drives the unchanged upstream encoder. Seed is `0x1337BEEF`. Each attempt records the request, the tags, `codec` fields after `CFHD_DecodeSample`, the production parser result, and a hash of the upstream decoded buffer. Coefficient hashes cover the tight wavelet samples, with padding excluded.

The probe skips inputs that this SDK cannot convert safely:

- RG30, R210, DPX0, AR10, and AB10, because `FAST_RG30` skips `ConvertRGBA64ToFrame16s` and the planes would be uninitialized.
- RG64, NV12, YV12, and `CFHD_ENCODED_FORMAT_YUVA_4444`, because the encoder's default color-format case opens a modal dialog.
- RGB24 requested as RGBA, and BGRA requested as Bayer, faulted inside the encoder (`0xC0000005`) on every quality. Those requests produced no sample.

297 attempts ran. 274 returned a sample. 23 did not. Seven distinct emitted profiles survived upstream decode. One Filmscan-1 sample of each profile, at or under 256 KiB, is in `tests/fixtures/phase3b0/`.

The 1920×1080 interlaced reconfirm uses `GenerateTestVectorYUY2(PATTERN_PSEUDO_RANDOM_NOISE, 1920, 1080)`, the same generator as `tests/test_decoder.cpp` `Difference()`. An earlier probe-local noise pattern at that size faulted inside the encode/decode call and was replaced with this generator.

## 7. Generated profile inventory

Every successful decode in the survey landed in one of these profiles. Confidence is High: each row is a decoded sample, not an encoder request.

| Profile key | Coded precision | Channels | Transform | Frames | Subband chunks | Sampling evidence |
| --- | ---: | ---: | --- | ---: | ---: | --- |
| YUV422 spatial progressive | 10 | 3 | spatial 0 | 1 | 30 | Chroma lowpass is half the luma width. |
| YUV422 spatial interlaced | 10 | 3 | spatial 0 | 1 | 30 | Same sizes. Progressive flag absent. Subband 8 difference-coded. |
| YUV422 fieldplus progressive | 10 | 3 | fieldplus 2 | 2 | 54 | `codec.num_subbands` is 17. Three extra walker chunks use subband index 255. |
| YUV422 fieldplus interlaced | 10 | 3 | fieldplus 2 | 2 | 54 | Same, plus difference coding on subbands 12 and 15. Progressive flag absent. |
| RGB444 spatial progressive | 12 | 3 | spatial 0 | 1 | 30 | All three channels have full luma band sizes. |
| RGBA4444 spatial progressive | 12 | 4 | spatial 0 | 1 | 40 | Four full-resolution channels. |
| Bayer spatial progressive | 12 | 4 | spatial 0 | 1 | 40 | BYR4 request 128×128. Tags say 64×64. Upstream decode returns 128×128. |

GOP-1 YUV, RGB, and RGBA all use three wavelet levels and 10 subbands per channel. Fieldplus is the only mode whose `codec.num_wavelets` is 6 and whose `codec.num_subbands` is 17.

`CFHD_ENCODING_FLAGS_YUV_601` did not create a new profile. It stayed progressive 10-bit YUV 4:2:2. `CFHD_ENCODING_FLAGS_RGB_STUDIO` stayed RGB444 precision 12 and set `CODEC_TAG_ENCODED_COLORSPACE` to 4. The same RGB request without that flag omitted the colorspace tag. Putting `YUV_INTERLACED` on an RGB request left `SAMPLE_FLAGS_PROGRESSIVE` set. It did not emit interlaced RGB.

## 8. External fixture inventory

| Path | Frames | First sample | Upstream | Production parser |
| --- | ---: | --- | --- | --- |
| `upstream/Example/videos/CFHDsample422.AVI` | 24 | 703,168 bytes, 1920×1080 | Decoded to BGRA | PASS. Precision 10, 3 channels, 30 bands, progressive, internal encoded format 1. |
| `upstream/Example/videos/CFHDsample444.AVI` | 24 | 890,036 bytes, 1920×1080 | Decoded to BGRA | `InvalidSample`: precision/geometry contract. Precision 12, 3 channels, 30 bands, internal encoded format 3. All three channels are full width. |

Both are upstream SDK fixtures. Neither carries tag 22. The 444 file is RGB444 coded precision 12, the same profile the encoder emitted from RG24 and BGRA.

## 9. Master capability matrix

Parse is the production strict parser. Entropy and coefficient columns refer to the existing GPU path. "Upstream output" is `CFHD_DecodeSample`. Production output is `Decoder::QueueDecode`.

| Encoded profile | Precision | Temporal | Parse | Entropy | Coeff exact | Spatial inverse | Field inverse | Upstream output | Production output | Vector |
| --- | ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| YUV422 progressive | 8 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| YUV422 progressive | 10 | GOP-1 | PASS | PASS | PASS | PASS | n/a | PASS | PASS | fixture + Phase 3A corpus |
| YUV422 progressive | 12 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| YUV422 interlaced | 8 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| YUV422 interlaced | 10 | GOP-1 | UNSUPPORTED | PASS | PASS | PARTIAL | NOT IMPLEMENTED | PASS | UNSUPPORTED | 64×64 fixture; 1080 recipe |
| YUV422 interlaced | 12 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| RGB444 progressive | 8 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| RGB444 progressive | 10 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| RGB444 progressive | 12 | GOP-1 | UNSUPPORTED | NOT TESTED | NOT TESTED | NOT IMPLEMENTED | n/a | PASS | UNSUPPORTED | 64×64 fixture; AVI 444 |
| RGBA4444 progressive | 8 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| RGBA4444 progressive | 10 | GOP-1 | NOT EMITTED | — | — | — | — | — | — | none |
| RGBA4444 progressive | 12 | GOP-1 | UNSUPPORTED | NOT TESTED | NOT TESTED | NOT IMPLEMENTED | n/a | PASS | UNSUPPORTED | 64×64 fixture |

Additional emitted modes, outside the 12-row minimum:

| Profile | Parse | Entropy | Upstream output | Production output |
| --- | --- | --- | --- | --- |
| YUV422 fieldplus progressive, precision 10, 2 frames | UNSUPPORTED (`unsupported temporal/field transform`) | NOT TESTED | PASS | UNSUPPORTED |
| YUV422 fieldplus interlaced, precision 10, 2 frames | UNSUPPORTED (same status) | NOT TESTED | PASS | UNSUPPORTED |
| Bayer precision 12, 4 channels, tags 64×64 | UNSUPPORTED (`InvalidSample`, 40 bands) | NOT TESTED | PASS, display 128×128 | UNSUPPORTED |

Of 100 progressive 10-bit YUV GOP-1 samples, 97 passed the production parser. The other three are legal CineForm frames below the HyperCine geometry floor: 48×48, 64×40, and 32×32. Upstream decoded all three.

## 10. Parser support matrix

`ParseCineFormSample`, the non-strict walker, still returns false unless it collects exactly 30 bands. It cannot describe RGBA, Bayer, or fieldplus.

`ParseCineFormSampleValidated` adds the production gates. Initial progressive state is false in strict mode, so a missing sample-flags tag is interlaced and returns `UnsupportedEncodingMode` with "unsupported interlaced output reconstruction". Transform type other than 0, or frame count other than 1, returns the same error with "unsupported temporal/field transform". A present frame-format tag other than 2 returns `InvalidSample` with "unsupported frame format"; that tag did not appear in this survey. After the 30-band check, precision other than 10 or geometry outside 64..7680 × 64..4320 with the divisibility rules returns `InvalidSample` with "unsupported or inconsistent frame geometry/precision".

Observed production results:

| Sample class | Status | Error string |
| --- | --- | --- |
| YUV422 progressive 10, in-range geometry | PASS | |
| YUV422 progressive 10, 48×48, 64×40, 32×32 | `InvalidSample` | unsupported or inconsistent frame geometry/precision |
| YUV422 interlaced 10 | `UnsupportedEncodingMode` | unsupported interlaced output reconstruction |
| YUV422 fieldplus | `UnsupportedEncodingMode` | unsupported temporal/field transform |
| RGB444 precision 12, 30 bands | `InvalidSample` | unsupported or inconsistent frame geometry/precision |
| RGBA4444 and Bayer, 40 bands | `InvalidSample` | expected 30 bands for YUV422 progressive frame, but parsed 40 |
| AVI 422 first frame | PASS | |
| AVI 444 first frame | `InvalidSample` | precision/geometry contract |

No sample that met the written YUV422 contract was rejected. The rejections match the gates.

## 11. Entropy and coefficient matrix

The GPU entropy replay was run on the 1920×1080 interlaced Filmscan-3 reconfirm, the sample that already has a production test. Legacy `Phase2EHostStreaming` and `Phase2FCHybridFullFrame` both finished. They were compared with `ExtractBandsFromNative` across 4,147,200 coefficients. Mismatches: 0. Maximum absolute error: 0.

That path is the same 30-band, 3-channel, precision-10 layout as progressive YUV. Difference coding and one Peak level are inside those bands. The production parser rejects the sample before `QueueDecode` would reach that replay.

GPU entropy was not run for RGB444, RGBA4444, Bayer, or fieldplus. Those streams are not 30-band YUV layouts. Calling the current descriptor builder on them would be a new decoder, which this phase does not add. Their coefficient status is NOT TESTED. Upstream did decode them, so the coefficient bytes exist in the SDK.

## 12. Inverse and output matrix

Progressive 10-bit YUV 4:2:2 has a spatial inverse and color conversion to BGRA8 and signed planar YUV. That path is the Phase 3A production decoder.

Interlaced GOP-1 still has the spatial subbands. The missing stage is the field reconstruction in `TransformInverseFrameToYUV` (`upstream/Codec/decoder.c`). The GPU inverse kernels implement the spatial path only. Upstream `CFHD_DecodeSample` produced BGRA for every interlaced sample in the survey, including the 1080 reconfirm (`output_hash` `41a4e58b09555e3e`).

RGB444 and RGBA4444 upstream-decode to BGRA. HyperCine has no RGB inverse and no RGBA output. Bayer upstream-decodes; HyperCine has no demosaic.

Production `QueueDecode` on the 1080 interlaced sample returns `UnsupportedEncodingMode` from both the queue and the wait. Wait state is `Failed` (4). No device output is published.

## 13. Precision findings

Three different numbers travel with one frame.

Input precision is the source buffer. An 8-bit YUY2 or BGRA buffer is a normal request. The encoder then sets coded precision itself: 10 for every YUV 4:2:2 sample in this survey, 12 for every RGB444, RGBA4444, and Bayer sample. `CODEC_PRECISION_DEFAULT` is 8, and this build did not emit it.

Internal reconstruction is signed 16-bit. The active code for 10-bit YUV is 0..1023, and the inverse can step one or more codes past that. Phase 3A-F recorded plane samples 1024..1063. Output precision is whatever container the caller requested from the SDK. HyperCine's production containers are 8-bit BGRA and signed 16-bit planar with effective precision 10.

A 12-bit coded RGB frame is not a 12-bit YUV frame. Asking for RGB and asking for YUV from the same pixels produced different coded precisions.

## 14. Channel and subband ordering

YUV 4:2:2, measured on a 64×64 chroma-split frame (Y=128, U=16, V=235) and confirmed by `ConvertPlanarYUVToYU64`:

| Wavelet channel | Contents | Lowpass mean on the split |
| ---: | --- | --- |
| 0 | Y | about 8216 |
| 1 | V | about 15064 |
| 2 | U | about 1048 |

`frame.h` names channel 1 U and channel 2 V. The encoder and the YU64 packer do the opposite: planar channel 1 is the V source and planar channel 2 is the U source. Device planar output remains Y, U, V, so device plane 1 matches wavelet channel 2 and device plane 2 matches wavelet channel 1.

GOP-1 subbands, per channel, from the band signature: index 0 is the level-2 lowpass, then three highpass bands at that level, then three at level 1, then three at level 0. That is 10 subbands. The production level table is `{2,2,2,2,1,1,1,0,0,0}`. Chroma widths are half the luma widths at every level. Level-1 highpass uses prescale 2.

RGB and RGBA, from `ConvertRGBAtoRGB48` and `ConvertRGBAtoRGBA64` for a BGRA buffer:

| Wavelet channel | Stored component |
| ---: | --- |
| 0 | G |
| 1 | R |
| 2 | B |
| 3 | A, RGBA only |

The store is `channel0 = G`, `channel1 = R`, `channel2 = B`. The RG24 converter reads the three bytes as B, G, R and then applies the same rotation, which is why a buffer whose first byte is 255 shows up in channel 2. RGB band sizes match across channels. There is no 4:2:2 width reduction.

Fieldplus GOP-2 stores six wavelets per channel. `codec.num_subbands` is 17. The research walker counts 54 chunks per sample because each channel also contributes a chunk reported at subband index 255.

## 15. Difference coding

`SetCodingFlags` in `encoder.c` is the source of the flags. This build has difference coding enabled: the real samples contain the bit.

| Mode | Subbands with the difference bit | Codebook |
| --- | --- | --- |
| Progressive GOP-1, all probed qualities and patterns | none | 0 on the lowpass, 1 on highpass |
| Interlaced GOP-1 | subband 8 on each channel | 2 on that subband |
| Progressive fieldplus GOP-2 | none on the six gradient qualities | |
| Interlaced fieldplus GOP-2 | subbands 12 and 15 on each channel | 2 |

The 1920×1080 Filmscan-3 interlaced noise sample has difference coding on subbands 0:8, 1:8, and 2:8. GPU coefficients for that sample matched the CPU bands exactly, so the current entropy replay already accepts this difference-coded layout.

## 16. Interlaced and temporal reconstruction

GOP-1 interlaced is not transform type `INTERLACED`. The sequence is:

1. `CFHD_ENCODING_FLAGS_YUV_INTERLACED` clears the encoder's progressive flag. GOP length stays 1, so the transform type written in tag 10 stays spatial.
2. The forward path is `TransformForwardInterlacedIntraFrameRecursiveYUV`. It pairs rows and filters them with `FilterInterlacedRecursiveStrip`.
3. `PutVideoSampleFlags` writes nothing, because the flags word is zero. Strict HyperCine parsing treats the missing bit as interlaced.
4. `SetCodingFlags` marks subband 8.
5. On decode, `codec->progressive` is false. Full-resolution YUV inverse goes to `TransformInverseFrameToYUV`. That inverts the horizontal bands, then combines fields. `field_pitch` is `2 * output_pitch`, and the loop runs `frame_height / 2` rows. Precision 10 then packs YUV.

The GPU inverse stops after the spatial step. The field combine is the unimplemented stage.

Fieldplus GOP-2 is a different architecture. Tag 10 is 2, tag 11 is 2, and there are six wavelets. Progressive fieldplus still carries the progressive sample flag. Interlaced fieldplus omits it and difference-codes subbands 12 and 15. Production rejects both with "unsupported temporal/field transform" before band layout is considered. This phase does not specify a temporal inverse implementation.

## 17. RGB and RGBA findings

Every RGB444 sample in the survey is coded precision 12, 3 channels, 10 subbands, full-resolution chroma, progressive. Sources that produced it: RG24, BGRA, RG48, and B64A requested as RGB, plus the SDK's `CFHDsample444.AVI`.

Every RGBA4444 sample is coded precision 12, 4 channels, 10 subbands, full-resolution chroma, progressive. Sources: BGRA and B64A requested as RGBA. RGB24 requested as RGBA faulted in the encoder and emitted nothing. `CFHD_ENCODED_FORMAT_YUVA_4444` was not encoded; the API comment says it is not implemented, and the probe skipped it to avoid the encoder dialog.

8-bit and 10-bit coded RGB and RGBA were not emitted. Input 8-bit BGRA is still a real request, and it becomes coded precision 12.

Upstream decodes both profiles to BGRA. HyperCine rejects RGB on the precision/geometry gate and RGBA on the 40-band gate. There is no production planar-RGB, BGRA-from-RGB444, or RGBA16 path. Those output formats are separate from supporting the encoded profile.

## 18. Alpha findings

Alpha is wavelet channel 3. It is full resolution, with its own 10 subbands. It is not a copy of an RGB channel. `ConvertRGBAtoRGBA64` has an alpha range adjustment in the converter comments; the keying sample (`CFHD_ENCODING_QUALITY_KEYING` on BGRA RGBA, alpha ramp) stayed in the same RGBA4444 precision-12 profile, with quality low tag 5 and quality high tag 9632. Those quality-tag bits are the encoder's format-toggle field, not a second coded precision.

RGBA is the only emitted alpha mode. YUVA was not emitted.

## 19. YU64 convention

```
YU64 canonical packing: shift the reconstructed sample by (16 - precision), then saturate to uint16.
A 10-bit sample of 1024 becomes 65535. Saturate-to-1023-then-shift produces 65472.
Evidence: ConvertPlanarYUVToYU64, upstream/Codec/convert.c lines 14141-14144,
dispatched from ConvertYUVStripPlanarToV210 for DECODED_FORMAT_YU64 at line 16143.
Word order is little-endian Y0, V, Y1, U. V is planar channel 1. U is planar channel 2.
YR16 uses the other convention and saturates before the shift. The two paths differ.

HyperCine current behavior: the production API does not emit packed YU64.
tests/decode_phase1_support.h ConvertPlanarToYU64 saturates to (1 << precision) - 1
and then shifts, so 1024 becomes 65472. The comment on that helper says upstream
saturates to the active precision first. That comment does not match ConvertPlanarYUVToYU64.
The helper was not changed in this phase.

Upstream CPU reference: Phase 3A-F measured CFHD_DecodeSample YU64 words of 65535
on 14 frames whose planes held 1024..1063. The helper word on those same planes was 65472.

This phase's edges fixture (YU64 input, 128×64, Filmscan 3) did not reproduce 65535.
CFHD_DecodeSample wrote 16,384 words, maximum word 65472, and 7,616 words equal to 65472.
The research inverse saw 768 samples at 1024. Every packed word matched
saturate-then-shift of that research inverse, and 15,616 words also matched
shift-then-saturate because those samples were inside 0..1023.
The fixture shows a delivered maximum of 65472. It does not show the packer
receiving 1024. The source function and the Phase 3A-F measurement remain the
description of the overshoot branch.

Recommended future test oracle: the bytes from CFHD_DecodeSample into YU64.
```

## 20. Peak-Escape observations

The Phase 3A-RC progressive search was not repeated.

One new sample carried peak levels. `yuy2_yuv_intl_64_filmscan3_noise` has peak level 1000 on subband 8 of all three channels. The 1920×1080 Filmscan-3 interlaced reconfirm has peak level 1000 on channel 0 subband 8 only. The other two difference-coded subbands on that frame have the difference bit and codebook 2 without a peak level.

No RGB, RGBA, Bayer, or progressive sample in this survey carried a peak table. Peak tables are real on some interlaced Filmscan-3 noise, and they are sparse.

## 21. Geometry restrictions

CineForm's GOP-1 spatial rule comes from `IsFrameTransformable`. The chroma divisor is 8, which is frame width divisible by 16 and height divisible by 8. The encoder emitted and the upstream decoder accepted 48×48, 64×40, and 32×32.

HyperCine adds its own floor and ceiling in `Decoder::Initialize` and again in the strict parser: 64..7680 by 64..4320, with the same divisibility. Those three small frames fail the HyperCine check and pass upstream. Bayer tags are the half-resolution size (64×64 from a 128×128 BYR4 request). The upstream decoder returns the full display size, 128×128.

## 22. 32 MiB limit

`Decoder::QueueDecode` rejects `size > 32u * 1024 * 1024` with `SampleTooLarge` (`src/core/decoder.cpp`). The research GPU index uses the same ceiling. This is HyperCine policy. CineForm samples can be larger. Phase 3A-D already saw an 8K Filmscan-3 noise sample above the ceiling. This phase does not raise it.

The largest sample encoded here is the 1920×1080 interlaced Filmscan-3 reconfirm at 4,655,384 bytes. It is under the limit. The limit was not what rejected it. The parser rejected it for interlaced reconstruction.

## 23. Reusable GPU components

Design evidence only. Nothing here was retargeted.

| Component | YUV422 interlaced GOP-1, precision 10 | RGB444 / RGBA4444 precision 12 | Fieldplus GOP-2 | Bayer |
| --- | --- | --- | --- | --- |
| Entropy replay | Reusable. Already exact on the 1080 sample, including difference coding. | Needs parameterization for precision, and for 4 channels on RGBA. | Needs parameterization for 17 subbands and 6 wavelets. | Needs parameterization for 4 channels and half-resolution tags. |
| Inverse kernels | Spatial kernels cover the horizontal step. The field combine needs a new kernel. | Needs a new full-resolution RGB inverse. | Needs a new temporal inverse. | Needs a new reconstruction. |
| Color conversion | Reusable once the field stage has produced planar YUV. | Needs an RGB or RGBA converter. Current converters are YUV 4:2:2 only. | Reusable only after a YUV temporal inverse. | Needs a demosaic, which is a different output. |
| Slot buffers | Reusable at 3 channels and 30 bands. | RGBA needs a fourth channel. | Needs storage for the extra wavelets. | Needs four half-resolution channels. |
| Dependency graph | The 30-band graph stands. The new edge is the field combine after the spatial inverse. | New graph. | New graph. | New graph. |
| Checkpoint machinery | Reusable on the 30-band interlaced sample. | Needs parameterization. | Needs parameterization. | Needs parameterization. |

## 24. API and capability-reporting gaps

`DecoderCapabilities` cannot express the matrix:

- `interlaced_output` exists and `Capabilities()` never sets it.
- Nothing distinguishes RGB444 from RGBA4444, or either of them from YUV.
- Nothing reports the coded-precision set {10} or {12}.
- Output formats are only `DeviceBGRA8` and `PlanarYUV422Signed16`.

No API change is proposed for implementation inside 3B-0. A later phase that enables interlaced GOP-1 can set `interlaced_output` once the field inverse matches upstream. RGB, precision, and alpha need their own fields when those profiles are actually decoded.

## 25. Confidence

High:

- The seven emitted profiles and the twelve-row matrix. The not-emitted rows are absent from 274 samples and match the encoder's precision assignments.
- Production parser gates, because each rejected class returned the expected status string.
- Interlaced GOP-1 coefficient agreement on the official 1080 noise recipe.
- Channel order for YUV, from the chroma-split means and from `ConvertPlanarYUVToYU64`.
- RGB/RGBA channel store order, from `ConvertRGBAtoRGB48` and `ConvertRGBAtoRGBA64`.
- The YU64 source function and the Phase 3A-F 65535 measurement.

Medium:

- The edges-fixture YU64 bytes topping out at 65472. The measurement is exact. Which earlier stage limited the sample to 1023 was not isolated.
- Peak tables. They appeared on two Filmscan-3 interlaced noise samples and nowhere else in this survey. A different picture could emit more.
- Bayer display scaling. Tags are half resolution and the upstream decoder returned the requested display size. The demosaic itself was not inspected.

Low:

- None of the profile classifications rely on a low-confidence guess. GPU entropy for RGB, RGBA, Bayer, and fieldplus is NOT TESTED, which is a coverage gap, not a weak measurement.

## 26. Test-vector inventory

Committed fixtures, one per emitted profile, Filmscan 1, seed `0x1337BEEF`, gradient unless noted. Hashes are FNV-1a of the sample bytes.

| File | Bytes | Sample hash | Provenance |
| --- | ---: | --- | --- |
| `YUV422-p10-ch3-spatial-prog-f1-bands30-fmt-1.cfhd` | 3284 | `22e6e32f60daa579` | YUY2 64×64 progressive Filmscan 1 |
| `YUV422-p10-ch3-spatial-field-f1-bands30-fmt-1.cfhd` | 3364 | `7aa6e151c0620073` | YUY2 64×64 interlaced Filmscan 1 |
| `YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd` | 6132 | `09cdbfd3ce79cae9` | YUY2 64×64 GOP-2 Filmscan 1, two frames |
| `YUV422-p10-ch3-fieldplus-field-f2-bands54-fmt-1.cfhd` | 6296 | `766c61a035dea429` | YUY2 64×64 interlaced GOP-2 Filmscan 1 |
| `RGB444-p12-ch3-spatial-prog-f1-bands30-fmt-1.cfhd` | 3388 | `75d4c52c5a99849f` | RG24 64×64 RGB444 Filmscan 1 |
| `RGBA4444-p12-ch4-spatial-prog-f1-bands40-fmt-1.cfhd` | 4284 | `e69ae613acc6ef37` | BGRA 64×64 RGBA Filmscan 1 |
| `BAYER-p12-ch4-spatial-prog-f1-bands40-fmt-1.cfhd` | 9044 | `e513e61938beca3f` | BYR4 request 128×128, tags 64×64, Filmscan 1 |

The `fmt-1` suffix means tag 22 was absent. It is not a frame-format value of -1 inside the SDK.

The 1080 interlaced reference is not committed. It is 4,655,384 bytes. Recipe: `GenerateTestVectorYUY2(PATTERN_PSEUDO_RANDOM_NOISE, 1920, 1080)`, YUY2, `CFHD_ENCODING_FLAGS_YUV_INTERLACED`, Filmscan 3, one frame. Source hash `2d12a276f3594b09`. Sample hash `356ad5c204104b7d`. Upstream BGRA hash `41a4e58b09555e3e`. Coefficient hash `3455cbb3a354825f`.

External references: the two AVI files above. Upstream decode is the output oracle. HyperCine GPU output is not an oracle for a profile HyperCine does not reconstruct.

## 27. Prioritization

The profiles, in the order they should be considered:

1. Interlaced 10-bit YUV 4:2:2, GOP-1. Coefficients already match. The missing piece is one field inverse. Fixtures and an upstream oracle exist.
2. Other YUV precisions. Coded 8 and coded 12 were not emitted. There is nothing to implement until an encoder emits them.
3. RGB444 precision 12. Real, including the SDK's 444 AVI. It needs a new inverse and a new color path. Entropy was not validated.
4. RGBA4444 precision 12. Same, plus channel 3 and alpha range behavior.
5. Fieldplus GOP-2. A different transform, 17 subbands, and a temporal inverse. Interlaced GOP-2 adds difference coding on subbands 12 and 15.
6. Bayer precision 12. Four channels, half-resolution tags, upstream display scaling. Out of scope for a YUV decoder phase.

## 28. Phase 3B-1 recommendation

**Phase 3B-1 — 10-bit YUV422 Field-Aware Interlaced Reconstruction**

Scope: GOP-1 interlaced YUV 4:2:2 at coded precision 10. Reconstruct fields with the upstream sequence in `TransformInverseFrameToYUV`, then use the existing YUV color conversion. Accept the omitted progressive flag as this profile. Keep difference coding and Peak tables on subband 8 working, which the entropy replay already does.

Out of scope: fieldplus, GOP-2, RGB, RGBA, Bayer, coded precision 8 or 12, alpha, the 32 MiB ceiling, YU64 helper changes, and the Auto classifier.

Entry criteria, all present:

| Gate | Evidence |
| --- | --- |
| Fixture | 64×64 interlaced Filmscan-1 file above. 1080 recipe and hashes in section 26. |
| CPU coefficient reference | `ExtractBandsFromNative`, 30 bands. |
| GPU coefficient check | Legacy and hybrid, 4,147,200 samples, max abs error 0. |
| Upstream reconstruction | `CFHD_DecodeSample` BGRA hash `41a4e58b09555e3e` on the 1080 sample. |
| Band order | 10 subbands × 3 channels. Y, V, U as in section 14. Difference on subband 8, codebook 2. |
| Field sequence | Section 16. Transform type stays 0. Flag is absent. Field pitch is twice the output pitch. |
| Production behavior today | `QueueDecode` and `WaitForDecode` return `UnsupportedEncodingMode`. No output is published. |
| Acceptance for 3B-1 | The 1080 sample decodes. Coefficients stay exact. BGRA and signed planar match the upstream decoder. The 64×64 interlaced fixture matches upstream. Progressive Phase 3A tests stay exact. GOP-2 and RGB remain rejected. |

Fixtures are ready. Acceptance gates are ready. Phase 3B-1 can start from this baseline.
