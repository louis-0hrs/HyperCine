# Bounded Metal packet capture — #381

One measured answer to the question the compatibility matrix could not answer:
what do the pixels of the committed 1920x1080 Metal FilmScan3 packet actually
decode to on this host, and do they agree with the macOS decode that was
recorded for the same bytes?

This is one packet, one frame, one decoder executable. It is not a
compatibility verdict, not a HyperCine decoder result, not GoPro product
qualification and not VC-5 conformance.

## Corrected measurement — 18 November 2026

```sh
python3 tools/metal_packet_capture.py \
  --ffmpeg /usr/bin/ffmpeg --ffprobe /usr/bin/ffprobe \
  --out docs/validation/metal-packet-capture-381-20261118.json
```

The tool wraps the raw packet in the already-reviewed single-frame AVI
container, decodes exactly one frame to `yuv422p10le`, and repeats that decode
in four independent processes (default, `-threads 1`, `-threads 4`,
`-filter_threads 1`) before it records anything.

The decoder options now precede `-i`; rawvideo output uses an independently
fixed `-threads:v 1`. FFmpeg applies codec options to the next input/output
file, as described in its [command documentation](https://ffmpeg.org/ffmpeg.html#Description).
Schema `/2` records the four portable command arrays and verifies their input
and output roles. The filter-thread option is global. These are requested
decoder configurations on **one frame**, not multiframe concurrency stress or
proof that four workers performed useful work.

| Field | Value |
| :-- | :-- |
| Packet | `tests/fixtures/macos-filmscan3/flat.cfhd`, 185648 bytes |
| Packet SHA-256 | `d75e6af7980558e42a520e8ce3bd3513626a854c3210da1c8b78240f4d75d6df` |
| Decoder | FFmpeg `7.1.5-0+deb13u1`, executable SHA-256 `e8a8d46f5225f3062cec7c07fb145d58ae73c603cb740dcd5bad34bfb54e455a` |
| Host | Linux x86_64 |
| Capture SHA-256 | `ab5fd41c064b943e53877d0ba6106614e1983d9c0d2701e255a632564b95f774` |
| Capture bytes / samples | 8294400 / 4147200 |
| Stability | 4 decodes, 1 distinct digest, byte-identical |
| macOS-recorded decode for the same frame | `ab5fd41c064b943e53877d0ba6106614e1983d9c0d2701e255a632564b95f774` |
| Agreement | exact |

The macOS digest is not re-derived here: it is read from the retained evidence
in `docs/validation/metal-filmscan3-ffmpeg.json` (frame 0, `decoded_sha256`).
The recorder refuses to publish a capture whose digest does not reproduce it.

### Historical evidence stays immutable

The retained `metal-packet-capture-381-20261117.json` is schema `/1`. Its runner
placed `-threads` after `-i`, so those options configured the rawvideo **output
encoder**, not the CFHD input decoder. Its four equal output digests remain
historical decoder-agreement evidence; they do not establish decoder-thread
configuration coverage. The verifier preserves this distinction and reports
`input_thread_options_bound=false` for the legacy record. Attaching new command
arrays to a legacy record is refused. The corrected `/2` capture independently
reproduced the same digest under all four corrected commands.

### Historical source attribution

The packet fixture's immutable `manifest.json` records
`base_commit=cf7a46dda868a4d65bd96eae71460f3a0258b32d`. That revision does not
contain four of the nine `sources[]` paths, and its `src/cpu/wavelet_cpu.cpp`
does not match the recorded hash. Only four source entries match that base.
All **nine** recorded source hashes match the later commit
`78acf9fa95d2e004542d01a35218e51668951288`. This reconciles the recorded file
identities; it does not retrospectively establish the original checkout,
compiler or build flags. The manifest, packet and retained macOS JSON are
unchanged, and no runtime result is inferred from source hashes alone.

## Pixel interpretation

Tightly packed planar 4:2:2, 10-bit code values in 16-bit little-endian storage,
4 bytes per pixel, no padding between planes:

| Plane | Width | Height | Stride (bytes) | Offset (bytes) |
| :-- | --: | --: | --: | --: |
| y | 1920 | 1080 | 3840 | 0 |
| u | 960 | 1080 | 1920 | 4147200 |
| v | 960 | 1080 | 1920 | 6220800 |

Range is studio (64–940); chroma siting is deliberately **not asserted**,
because this packet carries no independent siting evidence. Error metrics are
computed per sample in code values with peak 1023, never per byte.

## Negative controls actually executed

- **Changed capture.** The first sample is raised by one code value. The
  comparison must report exactly 1 mismatch and a maximum error of 1; a capture
  that cannot be distinguished from its neighbour is not a capture.
- **Forged metrics.** An identity record (0 mismatches, 0 maximum error) is
  submitted for those differing buffers. `require_metrics` recomputes the
  comparison and refuses the record, so an all-zero claim cannot be published
  over differing pixels.

Both are re-run by `tests/test_metal_packet_capture.py` in normal and `-O` mode,
and the committed evidence file is re-verified hash-only by
`tools/metal_packet_capture.py --verify` (registered as
`metal_packet_capture_evidence`).

The same guard suite checks actual command construction: decoder settings
must precede the input, output encoder settings remain separate, and timeout,
one-frame and capture-size bounds remain intact. It rejects command-role
tampering and a `compared.source` label that names any record other than the
retained macOS artifact. The historical `/1` record is also verified with its
explicitly narrower scope.

### Correction acceptance

On Linux x86_64, the corrected source passed all **165/165** registered
standalone CTests with zero failures or CTest skips. The capture guard script
passed **18/18** under normal Python and **18/18** under `python -O`.
An independent review separately removed the recorder agreement refusal,
verifier agreement refusal, input-thread option placement and comparison-source
binding: all four deliberate defects turned their intended tests RED in both
Python modes. Restored source re-passed the full guard script. An independent
live four-command capture reproduced the entire new JSON byte-for-byte.

These are local Linux and bounded measurement receipts. Updated-head Windows
and hosted sanitizer acceptance remain pending the separately reviewed CI
prerequisite #404 and subsequent final-tree checks; earlier #403 hosted results
are not transferred to this correction.

## What this still does not establish

The compatibility row `hypercine-metal-filmscan3-packets` stays
`NOT_OBSERVABLE`. A verdict needs a sample-level difference against a known
target, and neither side exists in Git:

- the encoder input for this frame (`input_sha256`
  `760ea8d69feb7f0d9bc12a2ebcc0d4ed037deea9c85391e289021b4ce4bd74b9`,
  8294400 bytes) is recorded only as a digest, and
- the macOS capture bytes are likewise recorded only as a digest
  (`ab5fd41c…`).

The recorded macOS PSNR of 75.5336 dB against that input is therefore retained
as previously-published evidence, not re-measured here. Committing either the
input planes or the macOS capture would turn this row into a real committed
comparison; until then this document records decoder agreement on exact packet
bytes and nothing wider.
