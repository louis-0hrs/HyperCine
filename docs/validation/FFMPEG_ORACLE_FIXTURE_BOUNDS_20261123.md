# FFmpeg oracle fixture-input bounds — scoped Draft (#381, 2026-11-23)

This change closes the follow-up source finding posted on [#381](https://github.com/louis-0hrs/HyperCine/issues/381) for HEAD `f1bce0d0f0fc6172a4f9500397cb2af1b859a325` (`tests/qualify_ffmpeg_oracle.py` blob `7b02cb2f9e82c744d37bac575a933a54cdec8c0a`). It bounds **fixture input** only. It does not revisit the now-merged process/privacy contract of #410/#411, and it does not qualify SDK, GoPro, SMPTE VC-5 or GPU outputs.

Base: protected `main` `ee6deae9eaffe85e272349742a8893bf9c2282c8` (the #411 squash-merge).

## Finding

`load_bound()` resolved a fixture path and called `Path(...).read_bytes()` *before* comparing the observed length with `manifest.json`, and `validate_manifest()` rejected only negative declared sizes. `--fixtures` accepts an arbitrary directory, so an oversized sparse entry or an arbitrarily large declared `bytes` value turned into an unbounded Python allocation before the existing size/hash rejection. That is an input-bounding defect on a caller-supplied path, not a regression in the bounded FFmpeg stdout/stderr/Job-Object contract.

## Enforced contract

Every one of the twelve admitted names now carries a finite budget, derived from the fixed 128x128 geometry rather than from the directory being inspected:

| Category | Names | Budget | Rationale |
|---|---|---|---|
| `.bgra` | 3 | 65,536 B | 128*128 pixels at 4 bytes (BGRA 8:8:8:8) — exact |
| `.input-p10`, `.p10` | 6 | 65,536 B | 128*128 pixels at 4 bytes (yuv422p10le, 16-bit samples) — exact |
| `.cfhd` | 3 | 262,144 B | one intra frame of that plane, 4x the plain budget for an incompressible frame; largest committed packet is 25,604 B |

- A declaration above its category budget is rejected **while the manifest is validated**, before any fixture byte is requested, and again in `load_bound()` so a hand-built mapping cannot bypass the loader.
- The read is bounded to `declared_bytes + 1`, so a file longer than its declaration is detected instead of silently truncated; the exact length and SHA-256 checks are unchanged.
- Only regular files are accepted. The check runs before `open()` (a FIFO would otherwise block) and again on the opened handle. A directory, device or FIFO behind an admitted name fails closed.
- Diagnostics stay fixed and privacy-safe: they name only an allowlisted fixture name and never echo the `--fixtures` path, file contents or a traceback.

Unchanged: the manifest schema and its provenance fields, all twelve fixture names, committed fixture bytes and hashes, `0/1/77` semantics, the three packet/reference comparisons, the cross-packet and mutation controls, process budgets, and executable admission. No CTest is added or renamed; the standalone registry stays at **169**.

## Local evidence

Host: Debian 13 x86_64, kernel 6.12, Python 3.13.5, 48 logical CPUs, FFmpeg `7.1.5-0+deb13u1` (the measured Debian pair).

Change: head `e37e62ec9accae7bb6e3f268202c43083d181e0c`, tree `a3c8fe44cd07f8a8d627e395d1d7749dfe95f933` — `tests/qualify_ffmpeg_oracle.py` blob `3665a160e239a31bfd38efda367ceaa17342d85c`, `tests/test_ffmpeg_oracle_qualification.py` blob `137c92e2d3fb8dbb29b0c2f05bcc6abb6bbd1014` (+258/−7).

- `python3 -B tests/test_ffmpeg_oracle_qualification.py`: **35 tests, OK** (19.430 s), no skips.
- `python3 -B -O tests/test_ffmpeg_oracle_qualification.py`: **35 tests, OK** (46.833 s), no skips.
- Eight of those are new (`BoundedFixtureInputTest`): budgets and geometry, all twelve committed fixtures still load at declared length and hash, oversized declarations rejected before any read, `load_bound()` guard, directory and FIFO entries fail closed, sparse-oversized bounding, and the CLI arms below.
- Standalone configure in a clean build tree, then `ctest -R ffmpeg_oracle`: `ffmpeg_oracle_process_contract` **Passed 18.58 s**, `ffmpeg_oracle_process_contract_optimized` **Passed 47.31 s**, 2/2. `ctest -N` still reports **169 tests**.
- Real measured pair, `python3 -B tests/qualify_ffmpeg_oracle.py`: `state=QUALIFIED decoder_build=7.1.5-0+deb13u1 packets=3 repeats=5`, `executable_identity=measured`, all three `distinct=1 reference_match=True`, controls `cross_packet=REJECTED` and `mutated_packet=PIXELS_DIFFER`. Packet, reference and output hashes are the pinned ones; the bounded reads change no byte that reaches the decoder.
- CLI arms (`--fixtures` at a sentinel-named temporary directory), normal and `-O`: an oversized numeric declaration exits 1 with `declaration exceeds its byte budget`, and a manifest-valid 512 MiB sparse packet exits 1 with `size does not match manifest`. Neither prints the sentinel, the temporary path, a traceback or `state=QUALIFIED`.
- `python3 ci/privacy_guard.py --staged` (before commit) and `--commit HEAD`: **passed**.

Measured allocation for the sparse case (`tracemalloc` peak while a 512 MiB sparse `flat.cfhd` sits behind a 3,444-byte declaration): **8,616 bytes**.

## Causal mutant

In an isolated copy (`/tmp`, no push, originals untouched) the read line was restored to the pre-change `(Path(fixtures) / name).read_bytes()`:

- `BoundedFixtureInputTest`: **3 of 8 fail** — sparse peak **536,876,059 bytes** against the 262,144-byte ceiling, the FIFO arm reports *"a FIFO fixture blocked the read"*, and the directory arm leaks an `IsADirectoryError` traceback.
- Restored copy: **8 tests OK** in both `python -B` and `python -B -O`.

## Not claimed

The full 169-test standalone matrix was not built and run for this change; only the two registered CTests that execute this file were, plus the direct and CTest-driven Python suites. No Windows, MSVC, macOS, ASan/UBSan, CUDA or GPU receipt exists for this head, no SDK/GoPro/SMPTE qualification is implied, and no merge authorization is claimed. The change is a review-derived hardening of fixture input and nothing else.
