<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Strict reference-decoder evidence JSON — #120

Child of PR #127, parent 41322c17d491bf2591ace7807683ac757c2ce058.
Python's default JSON loader silently kept the last duplicate key. An observation
containing decoder_status=7 followed by decoder_status=0 could therefore become
PASS. The same ambiguity affected encoder_packets_equal.

The CLI now rejects duplicate keys at every object depth, NaN/Infinity constants
and finite-syntax numbers that overflow Python's float representation. Invalid
input returns exit 2 with a generic INVALID_EVIDENCE report and release_blocked;
raw evidence and local paths are never echoed. Existing packet/quality failure
precedence and release gates remain binding. The classifier still consumes
observations rather than authenticating or running a decoder.

Regression evidence: before the correction the added CLI method had five failing
subcases. After correction all 15 methods pass, including nested duplicate status,
duplicate encoder parity, three non-standard constants and exponent overflow.
Full native SDK-free Release CTest: 29/29 PASS, zero skips, required independent
FFmpeg decode. Native Windows immutable-packet measurement and second-decoder
triangulation remain pending; #120 is not completely resolved.

```sh
python3 tests/reference_decode/test_contract.py
ctest --test-dir build -C Release --output-on-failure
```

Native test environment: macOS ARM64, AppleClang 21, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Full Release configuration uses HYPERCINE_BUILD_STANDALONE=ON,
HYPERCINE_BUILD_UPSTREAM=OFF, HYPERCINE_ENABLE_CUDA=OFF,
HYPERCINE_BUILD_BENCHMARKS=OFF and HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON.
Supply FFmpeg/ffprobe through PATH or the existing CMake executable cache options.
No protected-main merge or release; frozen upstream remains unchanged.
