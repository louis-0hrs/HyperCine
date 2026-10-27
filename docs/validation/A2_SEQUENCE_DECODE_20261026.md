<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# A2 sequence decode — the multi-frame claim had no independent decode behind it

Measured on the idle A2 on 2026-10-26. Read-only against codec sources: one
test-tool and validator change, described in section 5, and no change to the
encoder, headers or frozen `upstream/`.

Source baseline: protected `main` `44827e8` (#255 merge). Worktree
`HyperCine-a2rq`, detached, `-DCMAKE_CUDA_ARCHITECTURES=86`,
`-DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF`.

## 1. The gap

`docs/validation/A2_POSTMERGE_20261026.md` reports a 16-frame session and a
3-frame 4K run as evidence, and correctly warns:

> For reproducible evidence, attach the exact commands ... the 4K and 16-frame
> direct comparator invocations demonstrate full packet equality but this report
> does **not** include independent FFmpeg outputs for every one of those packets.

The reason is not an oversight in that report. It is structural. The registered
gate `cuda_filmscan3_ffmpeg_decode` calls the comparator without `--frames`, so
the comparator writes `cpu.cfhd`/`gpu.cfhd`/`trace.json`, and the validator decodes
exactly those two files. With `--frames N` the comparator instead writes
`cpu.fNNN.cfhd`/`gpu.fNNN.cfhd` and **no** `trace.json` at all, and the validator
had no `--frames` option:

```
validate_cuda_filmscan3.py: error: unrecognized arguments: --frames 3
```

So a multi-frame run could only ever be checked **in memory**, by the comparator's
own comparison. That is a real check, but it is the same process that produced the
packets. Bytes written to disk were never decoded. A sequence claim therefore
rested on the encoder agreeing with itself.

## 2. Why that is not a theoretical worry

The comparator prints `MULTIFRAME: PASS (3 ordered frames, ...)` on its own
in-memory equality. To test what that verdict is blind to, frame 1's *GPU artifact
file* was replaced by frame 0's packet while the in-memory comparison was left
untouched (mutation described in section 5). Result:

```
MULTIFRAME: PASS (3 ordered frames, 10572740 bytes total, rate state and frame counter identical)
FAIL: independent FFmpeg decodes of frame 1 differ at byte 0
```

The passing `MULTIFRAME` line is printed in the mutated build. Whatever the
sequence claim was worth, it was not worth an independent decode.

## 3. The change

- `tools/hypercine_cuda_frontend_compare.cpp`: in the `--frames > 1` branch, also
  write `trace.fNNN.json` per frame (same `WriteTrace` used for the single-frame
  case). No encoder path touched.
- `tests/validate_cuda_filmscan3.py`: add `--frames` (default 1). Above 1, it
  requires one `FRAME n:` line per frame, the `MULTIFRAME: PASS (N ordered frames`
  summary, 30 equal coefficient hashes **read from that frame's own trace file**,
  and then decodes `cpu.fNNN.cfhd` and `gpu.fNNN.cfhd` **separately per frame** and
  compares the decoded pixels.
- `CMakeLists.txt`: new `cuda_filmscan3_sequence_ffmpeg_decode`, same validator with
  `--frames 3`, labels `cuda;gpu-required;external-decode`, `SKIP_RETURN_CODE 77`.

The single-frame gate is unchanged in behaviour; its three assertions still run.

## 4. Results, measured on `44827e8`

Machine, from the tool's own output:

```
GPU: NVIDIA A2 compute 8.6
Device native architecture: sm_86
CUDA runtime/driver: 12040/12040
VRAM: 14804 MiB free / 14878 MiB total
```

Driver 550.163.01, toolkit 12.4.131. **The A2 was idle for every run below** —
`0 %` utilisation, `14 MiB` used, no compute process. This differs from the earlier
report, where a co-tenant model held 9.6 GiB.

### Registered CUDA suite

Seven registered CUDA tests, **7/7 PASS**, no SKIP77:

| Test | Result |
|---|---|
| `cuda_filmscan3_complete_packet` | Passed |
| `cuda_filmscan3_negative_skip_kernel` | Passed |
| `cuda_filmscan3_negative_altered_output` | Passed |
| `cuda_filmscan3_negative_pre_quantized_stage` | Passed |
| `cuda_filmscan3_ffmpeg_decode` | Passed |
| `cuda_filmscan3_sequence_ffmpeg_decode` (new) | Passed |
| `cuda_filmscan3_output_dir_negative` | Passed |

Full suite on this configuration: **79/79**, 0 failed, 138.83 s.

### Sequence runs with a real decode per frame

1920×1080, 3 frames:

```
FRAME 0: bytes=3833564 limiter=4  index=0 PASS
FRAME 1: bytes=3543324 limiter=9  index=1 PASS
FRAME 2: bytes=3195852 limiter=14 index=2 PASS
MULTIFRAME: PASS (3 ordered frames, 10572740 bytes total, rate state and frame counter identical)
FFMPEG_DECODE: PASS 3 ordered CPU and CUDA packets decoded independently;
  per_frame_yuv_sha256=6fbc6b590d385077276ebecf112c1f37acf1a745e86e528ce9333d0cd80f7017,
                     362fac935d0a293de598da05fd006e1b040ff1f6b64f00adafb702a05cc88f4d,
                     684c9739680b764be3b580415cc7f3ce616572d20607eac641c6dc31b4159d29
MULTIFRAME_TRACE: PASS (3x30 coefficient hashes from encoder traces)
```

3840×2160, 3 frames:

```
FRAME 0: bytes=15325284 limiter=4  index=0 PASS
FRAME 1: bytes=14146288 limiter=9  index=1 PASS
FRAME 2: bytes=12766764 limiter=14 index=2 PASS
MULTIFRAME: PASS (3 ordered frames, 42238336 bytes total, rate state and frame counter identical)
FFMPEG_DECODE: PASS 3 ordered CPU and CUDA packets decoded independently;
  per_frame_yuv_sha256=c67fd5f19db9d24fda8b2e991c8dc90428a2e332af0911473c14286b7f252bb2,
                     650bddfa518b001e80a71735602d5b95bbe060e013208d1690030ad7bd4c23af,
                     fd19d6aee697d465838b1be3e7aec765c48cdb178550e839663a5673e8c4655d
MULTIFRAME_TRACE: PASS (3x30 coefficient hashes from encoder traces)
```

Note the first 1080p digest here equals the single-frame digest in the earlier
report, which is the expected consistency: frame 0 is the same synthetic input in
both cases.

The 16-frame 1080p claim was checked the same way, frame by frame, decoded by
FFmpeg on both sides. All 16 decoded pairs were byte-identical, 0 decode failures.
That loop was driven directly rather than through a registered test, so it is
evidence, not a gate.

### Geometries the architecture note calls unproven

`docs/ARCHITECTURE_BOUNDARIES.md` states that 720p and 2048×1080 CUDA "were **not
established** by those specific recorded gates". Both now have a 3-frame session and
an independent decode per frame:

| Geometry | 3-frame packets | Independent decode |
|---|---|---|
| 1280×720 | 4,707,748 B total, all frames PASS | PASS, both sides decoded per frame |
| 2048×1080 | 11,274,552 B total, all frames PASS | PASS, both sides decoded per frame |

Single-frame independent-decode digests, for a per-geometry anchor:

| Geometry | Packet bytes | `yuv_sha256` |
|---|---|---|
| 128×24 | 8,756 | `d8e9cc464cf87e0074ea32e12454f98a64e76e3b611479772cd99ab77fb043c4` |
| 1280×720 | 1,707,164 | `27a02564d00b91b7b34c55ec7afff8940714f688fefd6898e7edfe8db191cfc8` |
| 1920×1080 | 3,833,564 | `6fbc6b590d385077276ebecf112c1f37acf1a745e86e528ce9333d0cd80f7017` |
| 2048×1080 | 4,089,468 | `b24ad68689ef9c17e1e7276a9507b26123577447e25501a1d1f8f3ea85332d6c` |
| 3840×2160 | 15,325,284 | `c67fd5f19db9d24fda8b2e991c8dc90428a2e332af0911473c14286b7f252bb2` |
| 4096×2024 | 15,316,828 | PASS (packet equal, 30/30 hashes) |

`4096×2024` is the largest geometry the current session budget admits
(8,290,304 px, under 8,294,400). It is included because the recorded matrix stops at
4K UHD and the geometry contract's real ceiling is elsewhere. `128×24` and `256×24`
are the smallest legal shapes; both produce complete packets that decode.

### Rejections still reject

| Geometry | exit | Reason string |
|---|---|---|
| 1920×1081 | 1 | width %128, height %8, width [128,4096], height [24,4096] |
| 4096×2160 | 1 | exceeds 8294400-pixel CPU session budget |
| 7680×4320 | 1 | width %128 ... 8K unqualified |
| 100×24 | 1 | width %128 ... 8K unqualified |
| `8K` (unparseable) | 1 | --geometry expects WIDTHxHEIGHT |

All five exit 1. The strings match the recorded matrix in
`docs/validation/windows-qualification-20261026/cuda-geometry.json` for the first
three.

## 5. Mutation control for the new arm

The question a new test has to answer is what it catches that the old one did not.
One line was changed in the artifact-writing branch: for frame 1 the GPU file on
disk is written from frame 0's packet, while the in-memory comparison is untouched.

```
MULTIFRAME: PASS (3 ordered frames, 10572740 bytes total, ...)
FAIL: independent FFmpeg decodes of frame 1 differ at byte 0
```

The test fails; the old gate would have passed it. Restored and re-run: PASS in
4.17 s. The mutation is a one-line edit, deliberately, because a mutant that also
breaks the in-memory comparison would not prove the decode arm does anything.

## 6. What this does and does not establish

**Established:** on an idle A2 at `44827e8`, the CUDA frontend produces packets that
are byte-identical to the CPU path at six geometries from 128×24 to 3840×2160,
keeps rate state and frame counters aligned across ordered 3-frame sessions at
720p, 1080p, 2K and 4K, and those packets decode identically under an independent
FFmpeg pass **frame by frame**. The registered suite now proves that rather than
assuming it.

**Not established:** no performance claim anywhere in this note — with an idle GPU
timings would now be meaningful, but none were taken, because the question here was
correctness. No 8K, no 12K, no 17K. No original GoPro packet parity: every input here
is the comparator's own synthetic pattern. The CUDA path is still an experimental
internal frontend reached through `FrontendAccess::Use`, not a public `backend`. The
`#195`/`#120` reference-decoder races are untouched and open. One host, one GPU, one
toolkit.
