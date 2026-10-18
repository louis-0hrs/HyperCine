# Phase 3B-0 — Linux host heap corruption / SIGSEGV: root cause (issue #13 / #38)

Scope: the one-time `test_decode_phase3b0` "heap corruption" seen once in a Windows
full-suite run (documented in `docs/DECODE_PHASE3B2A.md` and issue #13 as never
reproduced). The Linux A2 lane (issue #38) was asked to reproduce it under host
ASan/UBSan or document it honestly.

**Result: on Linux the failure is deterministic, not a flake — 20/20 in both the
Release and the ASan/UBSan build — and it is a crash inside the upstream encoder's
unsupported-format path, not a defect in the FIELDPLUS decoder.** Root cause below.

## Reproduction (deterministic)

| Build | Command | Result |
| --- | --- | --- |
| Release (NDEBUG) | `test_decode_phase3b0 <results> <fixtures> <root>` | SIGSEGV, exit 139, 20/20 |
| ASan/UBSan | same binary built with `-fsanitize=address,undefined`, Debug | exit 1 (ASan report), 20/20 |
| Full Linux CUDA CTest | `ctest --test-dir build-linux-cuda` | 42/43 pass, `test_decode_phase3b0` SegFault (0.20 s) |

The run dies while encoding the probe case `rg24_rgba_64_low_gradient`
(input `CFHD_PIXEL_FORMAT_RG24`, encoded `CFHD_ENCODED_FORMAT_RGBA_4444`), after
`CFHD_PrepareToEncode` has already returned success.

## Root cause

`CSampleEncoder::PrepareToEncode` (`upstream/EncoderSDK/SampleEncoder.cpp`) guards
unsupported input→encoded format conversions with `assert(0)`:

```
case CFHD_ENCODED_FORMAT_RGBA_4444:
    ...
    if (defaultformat != ENCODED_FORMAT_RGBA_4444) {
        if (Toggle444vs422EncodedFormat(...) == ENCODED_FORMAT_RGBA_4444) ...
        else if (Toggle4444vs444EncodedFormat(...) == ENCODED_FORMAT_RGBA_4444) ...
        else if (Toggle4444vs422EncodedFormat(...) == ENCODED_FORMAT_RGBA_4444) ...
        else
            assert(0);        // <-- SampleEncoder.cpp:260
    }
```

For `RG24 → RGBA_4444` **no** toggle produces the requested encoded format, so control
reaches `assert(0)`. Two build configurations then diverge:

- **Debug/assert-active** (upstream and tests built without `NDEBUG`): the assert fires
  and aborts cleanly — no memory corruption is seen.
- **Release/`NDEBUG`** (the configuration CI and every release build use): the assert is
  compiled out, `PrepareToEncode` keeps going in an inconsistent state, and the process
  SIGSEGVs during `CFHD_EncodeSample`.

That is why the defect is invisible to an assert-active sanitizer build yet fatal in
Release — and why it looked like a one-off heap corruption on Windows (WDDM/CRT heap
state made the same unchecked state surface as a heap-corruption abort rather than a
clean fault).

The probe's `Run()` only skips a fixed deny-list of formats
(`tests/test_decode_phase3b0.cpp`, the `RG30/R210/DPX0/AR10/AB10/RG64/NV12/YV12/YUVA`
list). `RG24 → RGBA_4444` is not on that list, so the probe drives an SDK conversion the
upstream encoder cannot perform.

## Which probe combinations are actually unsupported

Measured against the assert-active build (`Assertion '0' failed` at
`SampleEncoder.cpp:260` = unsupported):

| Input → Encoded | Verdict |
| --- | --- |
| YUY2 → RGB_444 | UNSUPPORTED (assert) |
| YUY2 → RGBA_4444 | UNSUPPORTED (assert) |
| RG24 → RGBA_4444 | UNSUPPORTED (assert) |
| RG48 → RGB_444 | UNSUPPORTED (assert) |
| RG48 → YUV_422 | UNSUPPORTED (assert) |
| B64A → RGBA_4444 / RGB_444 / YUV_422 | UNSUPPORTED (assert) |
| BYR4/BYR5 → BAYER | UNSUPPORTED (assert) |
| YUY2 → YUV_422, BGRA → RGB_444 / RGBA_4444 / YUV_422, RG24 → RGB_444 / YUV_422 | supported (encode completes) |

Each unsupported pair reaches the same compiled-out `assert(0)` and can crash a Release
build. The probe's `Axis::safe` flag and its deny-list do not cover these pairs.

## What this is *not*

- **Not** a FIELDPLUS / Phase 3B-2A decoder defect. The crash is in the upstream
  **encoder** path (`EncodeSample` → `TransformForwardSpatial`), reached only when the
  probe asks the SDK for a conversion it does not implement.
- **Not** proven to be the exact Windows incident. It is a concrete, reproducible
  Release-mode crash with the same signature (heap-corruption exit in
  `test_decode_phase3b0`); whether the single Windows observation was this case in
  particular cannot be proven retroactively. It explains the class.

## Note on the ASan `spatial.c:537` overflow

The standalone ASan trace for the supported `YUY2 → YUV_422` path also reports a
`heap-buffer-overflow` in `upstream/Codec/spatial.c:537` (`FilterHorizontalRow16s`,
reached from `spatial.c:10149`). That is **upstream reference code** (frozen by the
repository's rule 1) performing an aligned vector read that runs 4 bytes before a
64-byte-aligned region; it is an internal upstream SIMD over-read, not a HyperCine
defect, and is out of scope for this lane. It is recorded here only so the trace is not
mistaken for the RG24 crash.

## Suggested follow-up (maintainer decision)

1. `test_decode_phase3b0` should treat every unsupported input→encoded pair as
   skipped/recorded, the same way it already handles `RG30/R210/...`, so a Release build
   cannot crash on an SDK conversion the encoder does not implement. The
   unsupported-pair table above is the authoritative list.
2. Upstream `assert(0)` guards should not be the only guard for a user-reachable
   parameter combination in a Release library; a returned `CFHD_ERROR_*` would be safer.
   This touches `upstream/` and therefore requires a maintainer decision (rule 1).

## Evidence

- `benchmarks/results/decode_phase3b0_linux_heap_corruption.json`
- Reproducer: `tools/repro_phase3b0_convert.cpp` (investigation aid, not a CTest target)
- Sanitized raw logs: `benchmarks/results/decode_phase3b0_linux_heap_corruption_logs/`
  (ASan report for the first run, the repeated-run summary, and the full CTest log).
  Local paths are neutralised (`$REPO_ROOT`, `$HOME`, `$HOST`).
