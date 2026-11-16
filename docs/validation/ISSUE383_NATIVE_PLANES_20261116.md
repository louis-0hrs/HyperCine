# Issue 383: native planes-domain encode entry

2026-11-16. Bounded implementation slice of #383. Adds a native 10-bit
Y, Cr, Cb entry to the existing FilmScan3 session so the generation
measurement loop no longer has to re-enter through 8-bit BGRA. It does **not**
add a decoder, qualify HyperCine pixels, change the writer, or claim GoPro /
VC-5 conformance. No existing constructor, packet writer, rate law, corpus,
frozen `upstream/` or workflow was changed.

## The gap this closes

The 2026-11-14 #383 measurement recorded an exact blocker: `FilmScan3Encoder`
accepted only BGRA, and the declared inverse BGRA path carries an 8-bit/4:2:2
bottleneck by construction. That made a native-domain HyperCine loop
unexecutable, so codec additional loss could not be separated from the
interface. This change adds the planes entry that blocker named.

## Contract actually implemented

- `FilmScan3Encoder::Encode(const Planes&)` and
  `Encode(const Planes&, const FilmScan3Metadata&)`.
- `Planes` keeps its documented order Y, Cr, Cb. Y is `width x height`; both
  chroma planes are `width/2 x height` (YUV422). Samples are 10-bit codes in
  `[0,1024]`. 1024 is reachable through the existing CG709 conversion, so it is
  accepted; everything outside is refused instead of masked or clipped.
- The same session state, quality profile lock, tables, rate feedback,
  coefficient hashing and **single writer** are used. The BGRA and planes
  entries differ only in how the color planes are produced.
- The default `Frontend::TransformPlanes` refuses. A backend without a native
  planes path (currently Metal/CUDA) throws `logic_error` rather than
  reinterpreting BGRA storage; there is no silent fallback. CUDA/Metal
  planes-domain support remains open and is not claimed here.
- `tools/hypercine_encode_filmscan3 --input-yuv422p10le` reads a planar file in
  FFmpeg order (Y, then Cb, then Cr, 16-bit little-endian) and maps it to the
  encoder's Y, Cr, Cb order. `--reference-yuv` is refused for planar input
  because the input already is the canonical reference.

## Verification actually run

Linux x86-64, GCC 14.2.0, Release, standalone CPU-only
(`CUDA/METAL/VULKAN=OFF`, `BUILD_UPSTREAM=OFF`), base `main`
`d8b6adc` (tree `d8b6adc` + this patch).

- New `test_filmscan3_planes`: PASS. It proves byte-identical packets, tables,
  rate state and full coefficient hashes between the BGRA and planes entries on
  `128x24` (FilmScan3) and `256x40` (FilmScan1) across three chained frames; that
  the caller's planes are not mutated; that sample 1024 is accepted and 1025 /
  -1 refused; that wrong plane shape, truncated storage, unaligned stride and a
  foreign geometry are refused; that a backend without a native planes path
  refuses with the exact error; and that a failing native transform leaves the
  session state untouched.
- CLI end-to-end equivalence on the same synthetic frame: BGRA input with
  `--reference-yuv`, then planar re-entry through `--input-yuv422p10le`.
  `frame_0000.cfhd` and `trace.json` are byte-identical
  (SHA-256 `cfa0e86cf7d6ab12fab7b1fbddcac2e70f872634a169758c702a0a352beb2d8a`
  and `60e5cf75e880a5ae785a4d5f7401badf4d1fdb75b3c87556f278579872e20385`).
- Existing affected gates: `filmscan3_api`, `filmscan3_geometry`,
  `filmscan3_backend_selection`, `encoder_wire_request_normal|optimized`,
  `encoder_move_ownership`, `entropy_bitexact` all PASS before the full run.

## Still not established

No HyperCine decoder exists, so the actual `x[n+1] = D(E(x[n]))` loop is still
not executable end-to-end and no generation number is produced here. This slice
only removes the 8-bit input bottleneck. GoPro cross-chains, fixed quantizer
state, persistent-session effects, Metal/CUDA native planes and VC-5
qualification remain separate and unclaimed.
