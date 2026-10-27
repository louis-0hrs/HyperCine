<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0. -->

# Windows/CUDA CFHD quality profiles

The Phase3A public configuration now accepts the six regular SDK profiles through the typed `EncodingQuality` enum: `Low`, `Medium`, `High`, `FilmScan1`, `FilmScan2`, and `FilmScan3`. The enum values match GoPro's SDK values 1–6. `KEYING` and `*_UNCOMPRESSED` are outside this quality API. Existing HyperCine callers retain FilmScan3 as the explicit default.

The selected profile is passed to `CFHD_PrepareToEncode` for the upstream oracle and CPU reference. CUDA continues to run HyperCine's own wavelet and entropy pipeline with the selected profile's quant/scale state. The reference context reads the initial `FSratelimiter` from the freshly prepared SDK encoder because that starting state varies by profile; it does not assume FilmScan3's value for every profile.

## Reproduce

Configure and run the Windows CUDA suite:

```powershell
cmake -S . -B build-issue24 -DCMAKE_BUILD_TYPE=Release -DHYPERCINE_ENABLE_CUDA=ON -DHYPERCINE_BUILD_UPSTREAM=ON -DHYPERCINE_BUILD_TESTS=ON
cmake --build build-issue24 --config Release -j 8
ctest --test-dir build-issue24 -C Release --output-on-failure
```

Export one 1080p, 16-frame corpus for a profile:

```powershell
build-issue24/tests/Release/hypercine_reference_export.exe --quality filmscan2 --sequence 1920 1080 16 --out-dir build-issue24/quality-corpus/filmscan2
build-issue24/tests/Release/hypercine_reference_export.exe --verify build-issue24/quality-corpus/filmscan2
```

Valid `--quality` values are `low`, `medium`, `high`, `filmscan1`, `filmscan2`, and `filmscan3`. Omitting the option keeps the historical FilmScan3 behavior. Each manifest stores both the profile string and SDK enum value, input SHA-256, raw and normalized SDK/CUDA samples, frame state, all 36 quant/scale values, metadata mask ranges, and all 30 coefficient/payload hashes. `--verify` checks every listed file's byte length and SHA-256.

## Windows/CUDA results on 2026-10-15

- MSVC 19.44.35229.0, CUDA Toolkit 12.8.61, CUDA runtime 12.8, driver 610.62, NVIDIA GeForce RTX 4090 Laptop GPU (compute capability 8.9).
- Full Release CTest: **18/18 passed**, including the six-profile 1080p matrix, the 120-frame FilmScan2 state sequence, the existing 120-frame FilmScan3 regression, and Windows GoPro decoder tests.
- Each of the six profiles matched upstream SDK == CPU == CUDA for **16/16 1920×1080 frames**. Flat, gradient, edge/detail, and seeded-noise patterns are included. FilmScan2's separate 120-frame run also passed; FilmScan2 and FilmScan3 have separately recorded state trajectories and exports.
- One actual 3840×2160 and one actual 7680×4320 frame passed per profile. These are sample checks, not broad high-resolution coverage.
- The GoPro decoder verified generated samples in the reference sequence path. CTest also passed its standalone Windows SDK fixture decoder checks. A separate FFmpeg decode of the newly generated profile samples was **not run** because FFmpeg is unavailable in this environment.
- The 128×128 upstream boundary was not used as a parity gate; see issue #21's documented SDK SIMD/OOB case.
- The existing checked-in FilmScan3 corpus was not modified. New local binary outputs are under ignored `build-issue24/quality-corpus/` and are summarized by manifest SHA-256 in [the quality matrix](validation/cuda_quality_matrix_2026-10-15.json).
- NAS archival was authorized by the user, but the destination was unreachable during the 2026-10-15 attempt (system error 53); no NAS files were written. This is a historical result, not a fresh connectivity check. The verified local corpora were retained under `build-issue24/quality-corpus/`. For a later transfer, configure `HYPERCINE_GOLDEN_DIR` privately as the complete destination directory; do not infer a host or append a machine-specific share hierarchy from this report. Authorization and retention are also recorded in the [quality matrix](validation/cuda_quality_matrix_2026-10-15.json).

The matrix JSON records each output manifest hash, resolution, frame count, rate-limiter states, artifact count, test outcome, and the archival status. The local build directory is machine-specific and is not part of Git.
