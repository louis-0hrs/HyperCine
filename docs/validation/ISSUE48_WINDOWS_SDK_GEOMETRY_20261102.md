<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under the MIT License or Apache License, Version 2.0.
See LICENSE-MIT and LICENSE-APACHE for details. -->

# Issue #48 Windows original-SDK geometry smoke — 2026-11-02

## Scope

This records step 1 of the bounded compatibility probe in
[`EXTREME_RESOLUTION_17K_FEASIBILITY.md`](../EXTREME_RESOLUTION_17K_FEASIBILITY.md).
It exercises the frozen GoPro SDK only: BGRA8 input, YUV422, FilmScan 3,
progressive single-frame samples, followed by full-resolution BGRA decode by
the same SDK. It does not test HyperCine encoding, FFmpeg, Adobe, Resolve, or
cross-decoder conformance.

The opt-in runner is `test_issue48_sdk_geometry_smoke`. Each of its twelve
geometry/pattern cases runs in a fresh child process with a 15-minute timeout.
Inputs are synthetic, seeded with `0x1337BEEF`, 32-byte aligned, and never
written to disk. The helper records input, SDK sample, and decoded-pixel
SHA-256, SDK status and embedded version, elapsed stage times, dimensions,
sample length, and peak process working set. It checks available physical
memory before allocating the largest frames. It makes no CUDA call.

## Environment and result

- Windows x64, build 26200; MSVC 19.44.35229.0; MSBuild 17.14.60.
- CMake 3.31.6; Visual Studio 2022 generator; Release; CUDA disabled.
- Frozen SDK source: `11574d0295771edccadd17af14af74a539f924c7`.
- Sample-reported SDK and encoder versions: 10.1.0 (`655616`).
- Build base: `a9893c42f9a986210f2886ef0344f6c4942af2ba`; upstream remained clean.
- SDK encode plus full-resolution SDK decode: **12/12 cases succeeded** with
  the requested dimensions and BGRA output format.
- Full Windows CPU/SDK CTest: **70 passed, 0 failed, 2 skipped** out of 72,
  139.27 seconds. The skipped tests were `standalone_sdk_decode_3_packets` and
  `ffmpeg_oracle_qualification`; no test failed.
- `ffmpeg` and `ffprobe` were unavailable on this host. The FFmpeg decode,
  output dimensions, and independent decoded-pixel hashes remain pending.

The 17K high-entropy sample was 494,177,000 bytes and the process peak working
set was 3,677,609,984 bytes. These measurements show that this SDK build can
encode and decode its own synthetic sample at 17K on this host. They do not
establish interoperability or standard conformance for that sample size.

## Per-case measurements

`input`, `sample`, and `decoded` are SHA-256 values. Times are milliseconds.
Sample hashes are observations from this run; SDK metadata may be volatile.

| Geometry | Pattern | Sample bytes | Encode ms | Decode ms | Peak working set bytes | Input SHA-256 | Sample SHA-256 | Decoded SHA-256 |
| --- | --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| 3840×2160 | flat | 604172 | 40.542 | 6.242 | 197414912 | `9672f87d10074b7555075eca0a72b0591a484549cf41324732e1342239549048` | `23a76feeb52d0236ff72cd68650bd7f742cc08a0776234a469b956b88de7f1e5` | `2d93ffec9e6b7e5a1a638fd6c125da72e72428c51536ce23c43e92020a4fbd09` |
| 3840×2160 | gradient | 2488860 | 47.587 | 10.177 | 199270400 | `cb90ccb0d047c38adc1d0bb2199bba9e91fbe111e4b3dc74a5b66e091f6df7f7` | `b9d40940f993d0d065a2e95bb20f012a24e1e8eacd9a02c3faf33ef5d745d17e` | `f420dd9ede1f1c26ae7ccee7e84c9cffa2f2886cd2f2f4a06a70e528f4637feb` |
| 3840×2160 | noise | 29103244 | 140.368 | 38.906 | 225918976 | `5338cbc0aed1938637751f32dd48254694048bbdf2ca7d927209670b15c4fc36` | `36ff5e37df310c06a1fdc06d841de25eec2a35f6da60b10b09f3ba219e9913fc` | `d0d799e1e322027756e87a6112c01c64f5a234cc686c7c8487d3c63045468f40` |
| 7680×4320 | flat | 2408120 | 159.838 | 20.736 | 759582720 | `89841eaeb11927f077c6cd9015963da36de0b8502789b3a709b17f6f3f883ccf` | `634a89f5ca5295902287ca05780dc1b503ce458f5d479dffecd2c23efc909e47` | `117904333c00c9adc099592f125587c94587c54a6dc8f5b3e904983b2164f948` |
| 7680×4320 | gradient | 9938588 | 180.786 | 35.672 | 767074304 | `08cd72a284c24e345cccb5692443cb24bed130be5380d6efd99dac4b0033f2f3` | `34bd8c719ed817470cbee4600c7d6189e9ee963231a15b2f746b6408814cfe56` | `4a75fa8f09979c1d33ef76962a7358e0a43a38c1bd289122bce740e17dbab392` |
| 7680×4320 | noise | 116399656 | 512.825 | 155.660 | 873463808 | `1f502ece3bb48b7df16769a14e2fcf9c57b1271c94ad15402b8b392b1783f1c3` | `e04feb1780f252754fda95755f2b0f7b13a3780a07ea477df41497876ae492b0` | `1735bbe2cf219bb80167c8ae35f543ede878b6db6781804702d11140abbf0633` |
| 12288×8040 | flat | 7165256 | 472.623 | 58.911 | 2241761280 | `a4166dd4a8fbda446462a8134bf0fc6c5bf720771f9c998557cb90abb0fcf912` | `d6db65420c586e3545378a8b04dcd3ea157122f0be7fa24b270206a8586faa5b` | `c56d946ba1039941295f6cfb174f0ccd538f70499f27e0145552b7698cea97f8` |
| 12288×8040 | gradient | 29468108 | 513.157 | 82.553 | 2263883776 | `4c29864775a07d3a3bc10da463d45a3c6adf13c9ac8b14f811629ca1cacc4ed4` | `77607b09db2b094704438f143dc45f72954fc5f6f1bfa41124091f271dae2076` | `09fd39dfdf9ee12dba3d836b231208e13c4ae0d8a6e3ef8b5659c258fcd8cf53` |
| 12288×8040 | noise | 346574340 | 1485.650 | 466.207 | 2581073920 | `7d6ec82bdc9dccc89548f65bffe662ebfd9e39ff63e2ddfff27d2633cd6225d1` | `9b029a540fba0d63ad95017a3fdc9e6088d28ece7e63557dbfa758733b3fa7ff` | `6acd99fd97efb071a6e51d0c6abb6ee308f09ead1ad3db1382e5a0c78d6e67b1` |
| 17520×8040 | flat | 10215720 | 674.326 | 93.881 | 3193999360 | `b815fb81d0ab366316980e8e9df42322ee0fe30c2e6ef67a0168154347dba3b0` | `03032be4fc595c7fc0a9cd5df2d427727be6b347014ccc054616abb49307459a` | `eda8609b5ffcde511c46134c25d4546ccfb9c8d16026ba92c1596f48f90e64ec` |
| 17520×8040 | gradient | 42121304 | 715.893 | 120.265 | 3225718784 | `c671399f4029ad755cc472d85b87313cbb3f21c2b4ead7d7628bf3d7e0649ced` | `3220f831f045696c06c7da3813f1cdba28ca48aa07d99b295599424ac302c7ee` | `edc855c6bdb61a221e2c88f276ab112f8732945c8c52cae6c2777e5fa2e551ea` |
| 17520×8040 | noise | 494177000 | 2142.060 | 636.512 | 3677609984 | `e49709a68d2df1074b2a9a39b7c66bbd248ee0df25d88752d9de2b8d80c6da6c` | `7f717e4ff6ce30a23e4655fe605a74364cfef8e06348e8274b83ff755debe783` | `c9a3a973335ab9d28a64efed0de80112cf1e911bbf1e501be1442d04c72a4c01` |

One repeated 3840×2160 flat case produced the same input and decoded-pixel
hashes but a different whole-sample hash. The difference was not byte-localized;
no sample-normalization claim is made.

## Storage and remaining gates

The private golden corpus is discovered through a local `HYPERCINE_GOLDEN_DIR`
binding. On a mobile machine, more than one SMB endpoint may reach the same
share, including a Tailscale route. Resolve and verify the active path locally;
do not save the endpoint in this repository or in public results.

No sample or source files were written by this smoke run. Next compatibility
gates are an independent FFmpeg decode at the original dimensions and a
qualified comparison against a HyperCine-produced sample where that geometry
is supported. Until those pass, these results are SDK self-roundtrip evidence
only; they do not establish SMPTE VC-5 bitstream or application compatibility.
