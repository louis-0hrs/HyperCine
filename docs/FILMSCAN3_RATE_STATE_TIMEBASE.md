<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# FilmScan3 rate state, timebase, and the 65,535-frame session

Design note for issue #259. This change does **not** retarget the FilmScan3
writer. `src/core/filmscan3_encoder.cpp` and `include/hypercine/filmscan3.h`
stay byte-for-byte as on `origin/main` (`260eab0`). Existing 30 fps packets
therefore stay byte-identical; `tests/test_filmscan3.cpp` locks the
deterministic 1920×1080 FlatBlack frame 0 that the API test already sizes at
185,648 bytes.

Measured on this host, Release, unchanged writer, `sha256sum`:

`d75e6af7980558e42a520e8ce3bd3513626a854c3210da1c8b78240f4d75d6df`

That digest is an observation of the current writer, not a new bitstream.

## A. Where 30 fps is hard-coded

The only FilmScan3 synthesis site is the metadata-less overload:

```text
src/core/filmscan3_encoder.cpp:113
const unsigned f=state_.frame_index,parts[]={f/(30*60*60)%24,f/(30*60)%60,f/30%60,f%30};
```

`grep` of `src include tests docs` finds that `30*60*60` expression only there.
The colon bytes are not written by the loop; they remain the default
`FilmScan3Metadata::timecode` separators (`include/hypercine/filmscan3.h:31`).

The other overload (`filmscan3_encoder.cpp:117`) copies the caller
`FilmScan3Metadata` and does not recompute TIMC. `Metadata`
(`filmscan3_encoder.cpp:68-73`) writes five records and nothing else:

| Record | Source | Width |
| --- | --- | --- |
| GUID, DATE, TIME, TIMC | caller bytes, or the defaults above | 16 / 10 / 8 / 11 |
| UFRM `'L'` | `state_.frame_index` as 32-bit little-endian | 4 |

Separately, the sample header writes `Tag(-69, state_.frame_index+1)`
(`filmscan3_encoder.cpp:136`). Tag 69 is `CODEC_TAG_FRAME_NUMBER`
(`upstream/Codec/codec.h:312`), "sequence number of the frame in the
bitstream". The SDK comment at `upstream/Codec/codec.c:967` calls that field
useful for debugging. It is not the metadata unique-frame counter.

So three fields move together in this writer, but they are not one wire type:

1. **TIMC** — 11-character clock. Default path assumes exactly 30 non-drop
   frames per second. Caller path is an opaque 11 bytes.
2. **Tag 69** — 16-bit header value `frame_index+1` (1…65535).
3. **UFRM** — 32-bit little-endian `frame_index` (0…65534 on a full session).

A Linux Release probe of the unchanged writer, 128×24, showed frame 0
`00:00:00:00` / UFRM 0 / tag 69 = 1, frame 29 `00:00:00:29` / 29 / 30, and
frame 30 `00:00:01:00` / 30 / 31. The same probe found one GUID, TIME, TIMC
and UFRM in the 1080p golden, and **zero** `TIMB` or `TIMD` fourccs. Replacing
only the timecode digits (`01:02:03:04`) changed four payload bytes; tag 69
and UFRM stayed 1 and 0. Tag `-80` (`CODEC_TAG_QUALITY_L`) was present once,
value 6.

### SDK references that this writer does not implement

Read from unchanged `upstream/` (not modified here):

- `TAG_TIMECODE_BASE` (`TIMB`) is one byte, "common 24, 25, 30, 50, 60"
  (`upstream/Common/CFHDMetadataTags.h:205`). FilmScan3 does not write it.
- `TAG_TIMECODE_DROP` (`TIMD`) is one byte, "1=on, 0=off. Only value for
  30/60Hz base" (`CFHDMetadataTags.h:206`). A search of `upstream/EncoderSDK/*.cpp`
  and `upstream/Codec/*.c` found the tag definition and no drop-frame frame
  skip. This note does not invent one.
- When TIMC is absent, `CSampleEncoder` sets the base to **24** and stamps
  wall-clock `HH:MM:SS:00` (`upstream/EncoderSDK/SampleEncoder.cpp:805-814`).
  The same default is in `EncoderPool.cpp:491`. HyperCine's default is 30,
  not 24. Those are different contracts; do not "correct" the golden path to 24.
- Auto-increment of an existing timecode runs only when the parsed frame
  number equals the previous one **and** `m_last_timecode_base <= 30`
  (`SampleEncoder.cpp:865`, `EncoderPool.cpp:553`). Integer 50 and 60 are
  named on `TIMB` but are outside that increment. No rational
  `60000/1001` base appears in that path.
- `TAG_UNIQUE_FRAMENUM` is a separate 32-bit counter
  (`CFHDMetadataTags.h:239`, `MetadataWriter.cpp` `AddFrameNumber`). The SDK
  bitstream `frame_number` (`encoder->frame_number`, incremented at
  `upstream/Codec/encoder.c:7515` before the GOP1 header) is not that counter.
  HyperCine currently derives both tag 69 and UFRM from one session
  `frame_index`. Splitting them would change bytes and is not done here.
- `CalculateQualityState` already takes a rational frame rate
  (`include/hypercine/quality_state.h`). `docs/QUALITY_STATE_SDKFREE.md`
  records that the corpus is 30 fps, that other positive rationals are
  arithmetic tests rather than SDK rate-control parity, and that the SDK
  substitutes 30 fps outside `(10,120)`. The FilmScan3 writer does not call
  that calculator. Its quantizer is the local `NextLimiter` /
  `ComputeFilmScan3Tables` path, and the quality tag stays 6.

`FilmScan3Metadata` is a usable **transition** for applications that already
know the timecode string: the writer will not overwrite TIMC, and it will not
change tag 69 or UFRM because of that string. It is not a timebase. It has no
numerator, denominator, `TIMB`, or `TIMD`, and the no-argument `Encode` keeps
synthesizing 30 fps.

## Proposed rational contract (not implemented)

Do not change the no-metadata overload. Add a later, explicit opt-in that
existing call sites cannot hit.

```text
struct FilmScan3Timebase { int numerator; int denominator; }; // both > 0
```

Suggested shape, after review, as a new overload or a trailing optional — not
a silent field change on the golden path:

1. **Frozen default.** No timebase argument means today's expression at
   `filmscan3_encoder.cpp:113`, including the `% 24` hour wrap. Golden bytes
   stay the oracle.
2. **Caller TIMC wins.** If the caller passes `FilmScan3Metadata`, copy those
   11 bytes and ignore the timebase for the string. Tag 69 and UFRM still
   follow the session counter only.
3. **Integer non-drop display, opt-in only.** For `denominator == 1` and
   `numerator` in `{24,25,30}` the same integer split as the current 30 fps
   code, with `30` replaced by `numerator`, is the proposed display. `30/1`
   must match the frozen default on every frame index. `24` is the SDK's
   *missing-metadata* base, but it must not become HyperCine's default.
4. **50.** Two digits can hold frame `00..49`, so a non-drop split is
   representable. It is **not** SDK auto-increment (`base <= 30` excludes it).
   A future opt-in may define the integer split only as a HyperCine display,
   labeled as not SDK-identical. Do not switch it on from `TIMB` inference.
5. **`60000/1001`.** No proven FilmScan3 or SampleEncoder mapping. Fail
   closed. Do not approximate 59.94, do not count in a 60-frame field, and
   do not implement SMPTE drop-frame. `TIMD` stays unwritten until an
   original algorithm and an oracle exist.
6. **Still no `TIMB`/`TIMD` in this profile** until a decoder/oracle says the
   bytes are required. Adding either record changes every packet.

Future tests, not encoder tests in this change: display vectors for 24, 25,
30 and an explicitly non-SDK 50, plus a fail-closed `60000/1001`. The only
rate executed against the writer here is the existing 30 fps path.

## B. The 65,535-frame stop

`Tag` rejects values above 65535 (`filmscan3_encoder.cpp:40`). The header
stores `frame_index+1`, and `Encode` throws `"FilmScan3 frame counter exhausted"`
when `frame_index >= 65535` (`filmscan3_encoder.cpp:118`) **before**
transform, quantize, or any state update. Indices 0..65534 are 65,535
successful samples. The next call does not increment.

That matches the wire width of a codec tag, not a 32-bit UFRM limit:

- `CODEC_TAG_SIZE` is 16 and `CODEC_TAG_MASK` is `0xFFFF`
  (`upstream/Codec/codec.h:182-183`).
- `PutTagPairOptional` writes `value & CODEC_TAG_MASK`
  (`upstream/Codec/bitstream.c:1260`). The debug build asserts that the value
  already fits (`bitstream.c:1252-1254`); a non-debug SDK build truncates.
- UFRM is a 32-bit `'L'` (`CFHDMetadataTags.h:239`). The session stops while
  UFRM is still 65534.

This stop is therefore a **fail-closed session contract on top of a 16-bit
header field**. It is stricter than the SDK's release-build mask. It is not
evidence that a CFHD file cannot contain more than 65,535 samples under some
other writer.

### Why this change does not wrap or auto-reset

`encoder->frame_number` in the SDK is incremented with no cap at the GOP1
site (`encoder.c:7515`). Release `PutTagPairOptional` would then store the
low 16 bits, so the value after 65535 becomes 0. The legacy sample-header
parser continues while `header->frame_number == 0`
(`upstream/Codec/decoder.c:2249`, after initializing it to 0 at
`decoder.c:2187`). A wrapped 0 is indistinguishable from "tag not seen yet"
in that loop. `DecodedFrameNumber` (`decoder.c:17607`) later returns
`codec->frame_number`. That is enough to refuse silent wrap. It is not a
proof that every decoder path treats a repeated non-zero tag 69 as a new
session.

`FilmScan3Encoder::Reset` (`include/hypercine/filmscan3.h:64`) already starts
a new counter, limiter 4, and `previous_sample_bytes == 0`. Calling it after
the stop produces a new first packet. That is an explicit API, not a
container rollover. This change does not call it automatically. Whether a
decoder accepts a mid-file restart of tag 69 at 1 and UFRM at 0 is an open
question; no external decoder was run on a 65,536-frame sequence here.

Rate state is independent of the counter width. `NextLimiter` only looks at
`fs_rate_limiter` and `previous_sample_bytes`
(`filmscan3_encoder.cpp:86-94`). On the measured 128×24 constant-128 session
the limiter had reached the existing clamp of 20 by sample 65,535, and the
rejected next call left index 65535, limiter 20, and `previous_sample_bytes`
3116 untouched. Metadata of the last *successful* sample is a normal frame:
UFRM 65534, tag 69 = 65535, TIMC still on the 30 fps split. The failure
itself writes nothing.

Local Release timing for those 65,535 tiny frames was 9.80 s. The test locks
the count and the unchanged state, not the wall time.

## C. What the new tests actually force

`tests/test_filmscan3.cpp` (existing `filmscan3_api` target; root
`CMakeLists.txt` untouched):

| Case | What is proven |
| --- | --- |
| 1080p FlatBlack frame 0 SHA-256 and size 185648 | golden bytes of the unchanged writer |
| SHA-256("abc") self-check | the hasher matches the known test vector |
| tag 69, tag 80 = 6, TIMC, UFRM; no second quality tag | header identity; the writer has no second quality profile |
| frames 29 and 30 at 128×24 | the `/30` split is the one in the writer, not 24 or 25 |
| TIMC-only and GUID-only overrides | timecode/GUID bytes do not move tag 69 or UFRM |
| `sample_capacity_bytes == size-1` and a 4-byte budget | `CFHD capacity exceeded`, state not committed |
| injected frontend: `bad_alloc` during transform, then a band `width = 1` after a real transform | state not committed; the second throw is `invalid entropy plane` **after** `QuantizeFilmScan3` (`filmscan3_encoder.cpp:154`) and inside `PackHighpass` (`filmscan3_encoder.cpp:158`); state commits only at line 175 |
| `PackHighpass` on a width-1 plane, and on an 8×8 plane with capacity 0 | public entropy failure without a session |
| `Reset` after the budget failure | first packet matches a fresh encoder; geometry unchanged |
| two 128×24 encoders | frame counts and a `Reset` on one do not move the other |
| 65,535 successful 128×24 samples, then `length_error` | no wrap, rate-state fields unchanged |

No test writes a drop-frame timecode or a non-30 synthesis into a packet.

## Open questions

1. May tag 69 repeat a non-zero value after an explicit `Reset` inside one
   file, or must a long program open a new encoder instance per 65,535
   samples? Not decided. No automatic rollover until a decoder oracle says so.
2. Should UFRM be allowed to keep counting past 65535 while tag 69 stops?
   The SDK counters are separate. Changing that here would change packets.
   Left coupled and capped.
3. Is there a drop-frame implementation outside `EncoderSDK/*.cpp` and
   `Codec/*.c`? This audit did not find one. `TIMD` is not implemented.
4. Are 50, 60, and `60000/1001` display rules defined anywhere for FilmScan3
   packets? Not in this writer. `quality_state` rationals are not that oracle.
5. External-decoder behavior for a full 65,535-frame FilmScan3 session was
   not run (no long golden, no ffmpeg gate in this change).

## Integration

Keep this as a draft beside #259. Do not merge a timebase into the writer
until 24 and 25 have an explicit opt-in and a byte oracle that the default
30 fps path did not move, and until 50 / `60000/1001` fail closed. The
65,535 stop should stay a thrown session limit; `Reset` remains the only
restart. A later PR can add the opt-in overload without editing root
`CMakeLists.txt` if it lands in the existing `filmscan3_api` test.
