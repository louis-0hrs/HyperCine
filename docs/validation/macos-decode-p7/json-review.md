<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# P7.A JSON evidence review

Parent PR #83 source: `fd844fd058e0d79d4eabd39769e453c65ea78ba3`.
Refs #47. Review date: 2026-10-17, native macOS ARM64.

The baseline verifier accepted `true` or `1.0` for integer schema 1, an
integer-valued float for an integer stream dimension, and duplicate JSON keys.
A list at the manifest root raised an uncaught TypeError, exposing a traceback
instead of the intended sanitized failure. These were reproduced by invoking
the real entry point with a mocked capture; no decoder result was fabricated.

The reader now rejects duplicate keys at every nesting depth, nonstandard
NaN/Infinity constants and non-object roots. Verified components require equal
JSON types recursively, while object key order remains irrelevant. Tool JSON
uses the same parser. Type/Unicode errors use the existing generic failure
message. Only the five existing verified components are binding; FFmpeg version
provenance may still differ as documented by the parent.

Regression before fix: five methods, 10 failed subcases and six errors.
After fix: P7 audit CTest 3/3 PASS, including the unchanged real public 24-packet /
24-frame FFmpeg baseline. FFmpeg n8.0, Python 3.9.6; no original SDK decode,
new decoder implementation, GPU, or safety-gate acceptance is claimed.
Full standalone Release regression with required external FFmpeg: 28/28 CTest
PASS (79.35 s), no skips; no private SDK corpus configured.

Reproduce from the repository root with FFmpeg/ffprobe configured privately:

```sh
cmake -S tests/macos_decode_portability -B build/p7 \
  -DFFMPEG_EXECUTABLE="$FFMPEG" -DFFPROBE_EXECUTABLE="$FFPROBE"
ctest --test-dir build/p7 -C Release --output-on-failure
```

P7.B remains blocked by #13/#16 and matched original SDK output.
