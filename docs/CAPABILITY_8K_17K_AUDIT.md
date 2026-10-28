# FilmScan3 resolution and encoded-sample capability audit — 26 October 2026

Related: #48, #34, #259. **Source/measurement audit, not a 8K/12K/17K codec qualification.**

## Evidence and precise limits

On Linux x86_64 with NVIDIA A2 present, the *SDK-free CPU* `hypercine_encode_filmscan3` CLI was exercised using deterministic synthetic BGRA:

| Request | Observed result | Scope |
| --- | --- | --- |
| 3840×2160 (UHD 4K) | Encode succeeded; CFHD output 6,156,376 bytes | One synthetic CPU-encoded sample; native CPU/Metal packet parity and separate independent 4K FFmpeg decode were established in merged #248 |
| 7680×4320 (UHD 8K) | Historical trial rejected before output under the former [128,4096] dimension guard | On current main (#266), the 16-bit shape field permits these axes, but the 8,294,400-pixel **implementation budget** rejects 8K; neither verdict proves a CFHD format limit |
| 4104×2160 | Rejected by validation | Width is not a multiple of 128; the historical 4096 dimension bound no longer applies to FilmScan3 |

No complete >4K sample was created. No >4K independent FFmpeg/GoPro SDK decode was run. The A2's presence in the host does **not** make the CPU CLI trial a CUDA encode.

### 1. Current implementation gates: wavelet shape and separate pixel budget

`src/cpu/filmscan3_frontend.cpp::ValidateFilmScan3Geometry`, based on `include/hypercine/standalone.h`:

- FilmScan3 width: **128–65535**, divisible by **128**.
- FilmScan3 height: **24–65535**, divisible by **8**.
- These upper bounds reflect the 16-bit wire dimension field, not a claim that every such geometry is supported by the encoder.
- `width * height <= 8,294,400` pixels (`kMaxPlaneElements = 3840*2160`).
- `sample_capacity_bytes` can only reduce the default geometric bound, not enlarge it.

The axes are subject to a separate 8,294,400-pixel storage budget; none of this proves a CineForm format limit or support for large frames. The current validator rejects 4096×2160 (8,847,360 pixels) **because of the pixel budget**, not an axis limit. 4096×2024 (8,290,304 pixels) passes the basic shape and budget arithmetic but is **not** independently qualified as a full packet. URSA 17K's 17520-wide geometry is not divisible by 128 and also exceeds the pixel budget. 8K/12K and other large geometries need new output and decoder evidence rather than a constant change.

The legacy fixed-Q2 `EncodeBgra` path has a separate `kMaxPixels = 2,097,152` budget; do not silently conflate that path with the newer geometry-bearing `FilmScan3Encoder` session.

### 2. Second possible gate: the **enclosing sample** chunk

Use the actual FilmScan3 writer: `src/core/filmscan3_encoder.cpp::Writer::End`, not the older `standalone_encoder.cpp` as a proxy. The FilmScan3 encoder opens one outer sample chunk with `s.Begin(0x2200)` and closes it with `s.End(sample,0x2200)`. The writer rejects a chunk if its payload word count exceeds `0xFFFFFF` words:

`0xFFFFFF * 4 = 67,108,860` bytes, **approximately 64 MiB per enclosing payload**.

There are nested level (`0x2100`) and band (`0x2000`) chunks subject to their own checks, but a collection of individually legal bands can still exceed the **outer sample** ceiling. The encoded size also contains framing and metadata. Do not estimate compatibility by inspecting just one band's size.

**This is a limitation of the CURRENT FilmScan3 writer.** Whether the original GoPro SDK supports another legal large-sample representation, and whether FFmpeg/other decoders understand it, is **unverified**. Do not call this a proven global CineForm specification limit.

### 3. Conditional size illustration: one synthetic 4K sample is not a universal bitrate

The one measured 4K packet gives `6,156,376 / 8,294,400 ≈ 0.742 bytes/pixel`. Applying **exactly that same ratio** to other resolutions would give:

| Geometry | Pixels | Hypothetical CFHD sample | Test / interpretation |
| --- | ---: | ---: | --- |
| 3840×2160 | 8,294,400 | 6,156,376 bytes, **measured on one pattern** | Own SDK-free CPU encoder; 4K Metal parity separately proven |
| 7680×4320 | 33,177,600 | ≈23.5 MiB | **Not encoded**; might be smaller or larger with actual material |
| 12288×8040 | 98,795,520 | ≈69.9 MiB | **Not encoded**; that *hypothetical* ratio would exceed the current writer cap, but another content/state might not |
| 17520×8040 | 140,860,800 | ≈99.7 MiB | **Not encoded**; same conditional warning, plus width-alignment and memory issues |

This table is a sensitivity illustration, **not** evidence that 8K fits or that 12K/17K must overflow. Input entropy, quantization/profile, limiter state, metadata and temporal content all change sample size. A sufficiently incompressible 8K frame might breach the outer cap; a sufficiently compressible 12K frame might not. No universal codec conclusion follows from one synthetic 4K sample.

### 4. Third gate: host/device resource and index budgets

Before changing geometry validation, assess independently:

- Checked `size_t/uint64_t` calculations for width, pitch, buffer bytes, component and pyramid totals; each plane's allocated stride is distinct from logical width.
- Metal: `device.maxBufferLength`, unified memory availability, shader 32-bit index multiplication, peak transient workspace, upload/readback costs, error rollback and multi-session budgets.
- CUDA: native capability and runtime availability, peak VRAM and per-wavelet temporary allocations, row pitches, synchronisation, and real CPU/CUDA package parity (Draft #249 currently proves a 1080p reference path only).
- The **total sample** writer budget and verified independent decoder behaviour, not just dimension tags.

BGRA8 input alone requires approximately 31.6 MiB at UHD 4K, 126.6 MiB at UHD 8K, 376.9 MiB at 12288×8040 and 537.3 MiB at 17520×8040. Wavelet planes and scratch add substantial further memory. These input numbers are arithmetic estimates, not measured GPU occupancy.

## Honest capability matrix as of this audit

| Geometry | Current implementation | Native output/decoder evidence | Format-size status |
| --- | --- | --- | --- |
| UHD 4K 3840×2160 | **EXPERIMENTAL_SDK_FREE_CPU_AND_METAL_TESTED** | CPU sample measured here; complete CPU/Metal packet parity and independent FFmpeg 4K p10 decoder gate from #248; original GoPro 4K encoder parity **REFERENCE_PENDING** | Measured samples within current writer bounds; other input patterns still require bounded tests |
| UHD 8K 7680×4320 | **CURRENT_IMPLEMENTATION_PIXEL_BUDGET_BLOCKED** | Historical rejection; no full sample or decode | Current sample cap may or may not bind for any specific content; **UNMEASURED** |
| URSA Cine 12K 12288×8040 | **CURRENT_IMPLEMENTATION_PIXEL_BUDGET_BLOCKED** | No sample or decode | Conditional >64MiB risk; **UNMEASURED / REFERENCE_PENDING**, not a proven intrinsic format prohibition |
| URSA Cine 17K 17520×8040 | **CURRENT_IMPLEMENTATION_SHAPE_AND_PIXEL_BUDGET_BLOCKED**; fails width%128 | No sample or decode | Conditional >64MiB risk; **UNMEASURED / REFERENCE_PENDING**, not a proven intrinsic format prohibition |

Other valid intermediate geometries are **structurally parameterizable**, not automatically native codec-qualified. Actual CPU/Metal/CUDA packet and FFmpeg results must be tracked per geometry.

## Corrections recorded explicitly

1. The initial draft incorrectly marked 8K supported. It is actually rejected by the current implementation.
2. The next draft computed only per-band 24-bit limits using the older writer, overlooking FilmScan3's **outer sample** limit.
3. The following draft correctly found the outer limit but **incorrectly promoted one synthetic 4K bytes-per-pixel extrapolation into categorical 12K/17K format rejection and categorical 8K acceptance**. Those claims are withdrawn. The sample-size table is now explicitly hypothetical, not a test verdict.

## Next steps (separate PRs; no unbounded full-frame experiment)

1. #259: connect the **existing internal fail-closed capability prototype** to actual per-session geometry/backend/budget selection with positive and negative tests. Preserve existing FilmScan3/legacy packet hashes.
2. #48: inspect actual original GoPro SDK supported sample container and independent decoder semantics; determine whether legal large-sample representation exists. Keep `upstream/` unchanged.
3. Only after explicit resource and format gates: bounded **opt-in** 8K encoder prototype, real output and FFmpeg/GoPro decoder evidence, then per-backend GPU qualification.
4. 12K/17K format/allocator feasibility only after 8K and original wire-contract research. Do not silently crop/downscale or pack independent tiles as if they were one native full-resolution CFHD sample.
