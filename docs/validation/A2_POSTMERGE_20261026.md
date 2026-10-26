# A2 Post-Merge Qualification — 2026-10-26

Re-run of the CUDA frontend on the current `main`, because the #249 evidence was
produced **before** that PR was merged. Nothing here is new development; this only
answers whether main still does what #249 claimed.

- **`main` at measurement**: `81d8a94` (`feat(cuda): add experimental SDK-free FilmScan3 A2 reference frontend and parity gates (#249)`)
- **Worktree**: post-merge branch from `main`, `-DCMAKE_CUDA_ARCHITECTURES=86`
- **Build**: `BUILD_RC=0`, 0 compiler errors, 69 tests registered

## Machine, as reported by the test itself

```
GPU: NVIDIA A2 compute 8.6
Device native architecture: sm_86
CUDA runtime/driver: 12040/12040
```

Driver 550.163.01, CUDA toolkit 12.4.131, 15,356 MiB total.

## Registered CUDA suite

Six CUDA tests, all passed:

| Test | Time |
|---|---|
| `cuda_filmscan3_complete_packet` | 17:55 |
| `cuda_filmscan3_negative_skip_kernel` | 17:55 |
| `cuda_filmscan3_negative_altered_output` | 17:55 |
| `cuda_filmscan3_negative_pre_quantized_stage` | 17:55 |
| `cuda_filmscan3_ffmpeg_decode` | 17:56 |
| `cuda_filmscan3_output_dir_negative` | 17:56 |

Whole run: `0` failing tests. The suite exercises 1920x1080 by default, so the
larger geometries below were driven directly through
`hypercine_cuda_frontend_compare`.

## Geometries, measured on this main

### 1920x1080

```
WAVELET_STAGES: PASS (all 30 logical coefficient bands byte-equal before quantization)
COEFFICIENT_HASHES: PASS (30/30)
FULL_PACKET: PASS (3833564 bytes byte-equal)
PASS: complete 1920x1080 FilmScan3 CPU/A2 CUDA sample
```

### 3840x2160

```
COEFFICIENT_HASHES: PASS (30/30)
FULL_PACKET: PASS (15325284 bytes byte-equal)
PASS: complete 3840x2160 FilmScan3 CPU/A2 CUDA sample
```

`15,325,284` bytes matches the #249 figure exactly. The pre-merge claim survives the
merge.

### 16-frame session at 1920x1080

```
FRAME 0: bytes=3833564 limiter=4  index=0  PASS
FRAME 1: bytes=3543324 limiter=9  index=1  PASS
FRAME 2: bytes=3195852 limiter=14 index=2  PASS
FRAME 3: bytes=3112788 limiter=17 index=3  PASS
FRAME 4: bytes=3115152 limiter=20 index=4  PASS
...
FRAME 15: bytes=3113708 limiter=20 index=15 PASS
MULTIFRAME: PASS (16 ordered frames, 51045800 bytes total, rate state and frame counter identical)
PASS: complete 1920x1080 x 16 frames FilmScan3 CPU/A2 CUDA sample
```

The limiter walks 4 → 9 → 14 → 17 → 20 and stays there; 16 distinct byte counts;
frame indices contiguous 0..15. `51,045,800` bytes total matches #249.

## Independent decode

```
FFMPEG_DECODE: PASS CPU and CUDA packets decoded independently;
               yuv_sha256=6fbc6b590d385077276ebecf112c1f37acf1a745e86e528ce9333d0cd80f7017
```

CPU and CUDA packets were decoded separately by FFmpeg, not compared as bytes only.

## Negative controls

These are the tests that must fail. Each printed `FAIL` and the harness treated the
failure as success, because a silently-passing negative control is worthless.

| Control | Observation | Verdict |
|---|---|---|
| `skip-kernel` | `cpu=2583 gpu=23130` at wavelet L3-LL, CPU size 3833564 vs GPU 10876988 | fired (0x5A poison preserved) |
| `alter-output` | `cpu=-546 gpu=-16930`, hash `0x8e72ee4c...` vs `0x4cf14849...` | fired |
| `alter-pre-quantized-stage` | coefficient hashes and full packet still **PASS** (30/30), but the pre-quantized alteration was detected | fired |
| `output-dir-negative` | 31 checks: pre-existing files, subdirectories, symlinks survive repeated runs unchanged | PASS |

The third is the interesting one: it shows the guard catches an alteration that does
*nothing* to the final packet. A check that only looked at output bytes would miss it.

## Measurement caveat — read this before quoting the numbers

During the 4K and 16-frame runs, the A2 was **not** idle. A co-tenant
`llama-server` held **9,612 MiB** of 15,356 MiB (`qwen3.5:9b`, 8,487 MiB VRAM). The
CUDA runs passed anyway, at roughly 5.7 GB free.

So these results show the frontend works under memory pressure, but they are **not**
clean-room numbers and must not be used as a performance baseline. Timing figures in
particular would be meaningless here; none are quoted above for that reason.

## What this does and does not establish

**Established:** main's CUDA frontend produces byte-identical packets to the CPU path
at 720p-class through 4K on real SM86 hardware, keeps rate-state and frame counters
correct across a 16-frame session, rejects three kinds of deliberate corruption, and
yields packets that an independent FFmpeg pass decodes identically.

**Not established:** no Metal or Vulkan cross-check here; no claim that the CUDA path
is a qualified backend (it remains an experimental internal frontend); no performance
characterisation, because the GPU was shared. The original #195 SDK races are
untouched and still open.
