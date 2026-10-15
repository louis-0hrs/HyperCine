# Windows/CUDA Reference Corpus

The exporter uses the existing Phase 3A sequence path, so GoPro's pristine `CFHD_EncodeSample` and HyperCine's CUDA encoder receive copies of the exact same deterministic BGRA frame bytes. It writes untouched input, SDK and CUDA samples, normalized sample copies, and `manifest.json` with raw/normalized SHA-256, quant/scale tables, 30 coefficient/payload hashes, and metadata mask offsets.

## Build and export (Windows, MSVC, CUDA)

```powershell
cmake -S . -B build-corpus -A x64 -DHYPERCINE_ENABLE_CUDA=ON -DHYPERCINE_BUILD_TESTS=ON
cmake --build build-corpus --config Release --target hypercine_reference_export
.\build-corpus\tests\Release\hypercine_reference_export.exe --tier-a --out-dir .\build-corpus\reference-corpus
.\build-corpus\tests\Release\hypercine_reference_export.exe --verify .\build-corpus\reference-corpus
.\build-corpus\tests\Release\hypercine_reference_export.exe --sequence 1920 1080 120 --out-dir .\build-corpus\sequence-1080 --omit-input-files
.\build-corpus\tests\Release\hypercine_reference_export.exe --verify .\build-corpus\sequence-1080
```

Tier A attempts the 11-pattern Phase 3A sequence at 128x128 (40 frames, covering the sequence's patterns) and exports 16 representative 1920x1080 frames (including noise, high-frequency, checker, edge, and photographic patterns). The 128x128 case is currently a measured mismatch and does not satisfy the SDK/CUDA equivalence gate; Canonical, Impulse, and explicit full-range color-bar fixtures are still pending. A 128x128 encode is retained, but the measured sequence did not match after metadata normalization (first differing byte 15); the run is marked `mismatch`, never as a pass. CUDA/SDK errors are recorded in the run manifest. `--verify` checks every listed file length and SHA-256 using Windows CNG.

## File and sample conventions

`input.bgra` is tightly packed top-down BGRA8 (`pitch = width * 4`). `sdk.cfhd` and `cuda.cfhd` are the untouched SDK/CUDA sample bytes. The separate `*.normalized.cfhd` files are copies where only the value bytes of explicitly parsed `TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, and `TAG_TIMECODE` fields are zeroed by the existing parser; offsets and lengths are recorded per frame. Original files are never modified. Each sample records all 30 subband dimensions, quant/scale values, FNV-64 coefficient hashes, and SHA-256 payload hashes.

The encoder profile is BGRA8 to YUV 4:2:2, FilmScan 3, 10-bit, intra GOP. Generator seed is fixed at `0x1337BEEF`; each frame records its pattern/index. Exact GPU, CUDA runtime/driver, source commit, compiler, byte lengths, and raw/normalized hashes are stored in the manifest.

`manifest.schema.json` defines schema version 1. The exporter does not upload data. Keep large generated corpus bundles outside Git; commit only explicitly selected small fixtures and the manifest. Historical hash-only claims in Phase 2C/3A docs are not treated as archived files. 4K/8K exports remain pending. Tier C was run at 1920x1080 for 120 frames: all 120 normalized samples and 3,600/3,600 subband coefficient/payload pairs matched. Its 480 raw/normalized CFHD files (784,605,872 bytes) and per-frame SHA-256 input index are in the ignored local `build-corpus/sequence-1080` folder; `tests/reference_corpus/phase3a_1080p_120_manifest.json` records the full run. Input BGRA files were omitted because the generator is deterministic. Recreate and verify the run with `--sequence`; `--omit-input-files` keeps the same input hashes without a second large copy of regenerable BGRA frames.
## Captured local bundle (2026-10-14)

The real Tier A run is indexed by `tests/reference_corpus/manifest.json`. Its 280 raw/normalized/input files are in the local ignored archive `build-corpus/reference-corpus-tier-a-20261014.zip` (86,987,753 bytes; SHA-256 `fdc9cd0e881db417e23423c8cd3f9f1e27161f2c946e40edfa2bcb8b56d58dcf`). The archive stays out of Git because it contains full raw frames. To restore and verify it, extract the ZIP so the `frames/` directory is at the restore root, copy the committed manifest there as `manifest.json`, then run the `--verify` command above. The separate `bundle.sha256` sidecar is in the local export directory.
## Verification performed

- `cmake --build build-corpus --config Release` completed on Windows/MSVC/CUDA 12.8.
- `ctest --test-dir build-corpus -C Release --output-on-failure`: **14/14 passed**.
- Tier A export: 40/40 frames wrote real files at 128x128 but normalized SDK/CUDA samples mismatch from byte 15 (1,040/1,200 bands match); that run is explicitly `mismatch`. At 1920x1080, all 16 frames and 480/480 bands match after metadata normalization.
- Tier C 120-frame run: 120/120 samples and 3,600/3,600 bands match. The exporter verified 280 Tier A files from the restored ZIP and 480 Tier C files from the local sequence folder against SHA-256 and byte length.