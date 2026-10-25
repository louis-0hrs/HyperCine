# Capability audit: 4K, 8K, 12K and 17K (issues #48, #34)

Status: analysis only. No large frame was allocated, no bitstream for 8K or
above was produced, and no resolution above 4K is claimed as supported.

Every number below is either measured on this host or read from the cited
source line. Nothing here is inferred from a resolution name.

## What was actually run

```
Linux x86_64, NVIDIA A2 (compute 8.6), Release, -DHYPERCINE_BUILD_STANDALONE=ON

hypercine_encode_filmscan3 --geometry 3840x2160   input 31 MiB BGRA
  rc=0  frame_0000.cfhd = 6,156,376 bytes

hypercine_encode_filmscan3 --geometry 7680x4320   input 126 MiB BGRA
  rc=1  "FilmScan3 geometry requires width %128, height %8,
         width [128,4096], height [24,4096]; 8K unqualified"

hypercine_encode_filmscan3 --geometry 4104x2160
  rc=1  same message
```

8K is rejected before a single byte of packet is written. The refusal comes
from geometry validation, not from memory exhaustion and not from the CineForm
chunk limit.

## Two independent limits, in the order they actually fire

### 1. Geometry validation (fires first, at build time of the frame)

`src/cpu/filmscan3_frontend.cpp:9-14`

```cpp
if (g.width < 128 || g.width > kMaxDimension || g.width % 128 ||
    g.height < 24 || g.height > kMaxDimension || g.height % 8)
    throw std::invalid_argument("... width [128,4096], height [24,4096]; 8K unqualified");
if (g.width > kMaxPlaneElements / g.height)
    throw std::length_error("FilmScan3 exceeds 8294400-pixel CPU session budget");
```

with `include/hypercine/standalone.h:31,34`

```cpp
inline constexpr int kMaxDimension = 4096;
inline constexpr std::size_t kMaxPlaneElements = 3840u * 2160u;   // 8,294,400
```

`kMaxPlaneElements` is exactly 3840x2160, so the CPU session budget is *tuned
to 4K UHD and nothing larger*. A width of 4104 already fails the modulo-128
requirement and the 4096 cap; 7680 fails both.

### 2. CineForm sub-band chunk length (would fire second, if 1 were lifted)

`src/core/standalone_encoder.cpp:104-108`

```cpp
const auto words=(bytes.size()-at-4)/4;
if (words>0xFFFFFF) throw std::length_error("sample chunk too large");
Patch32(at,(uint32_t(-int(tag | (words>>16)))<<16) | (words&65535));
```

`End()` is called once per lowpass and once per high-frequency band
(`standalone_encoder.cpp:182-184`), so the 24-bit word count applies **per
band**, not per frame. Ceiling: `0xFFFFFF` words = 67,108,860 bytes = 64 MiB
per band.

## Table

| Resolution | Dimensions | Pixels | Verdict | First blocker | Evidence |
|---|---|---|---|---|---|
| UHD 4K | 3840x2160 | 8,294,400 | **SUPPORTED** | none | encoded, 6,156,376 B packet, rc=0 |
| UHD 8K | 7680x4320 | 33,177,600 | **FORMAT_BLOCKED** | geometry guard: width>4096 and pixels>8,294,400 | measured rc=1, message quoted above |
| URSA 12K | 12288x8040 | 98,795,520 | **FORMAT_BLOCKED** | same geometry guard | code, not measured |
| URSA 17K | 17520x8040 | 140,860,800 | **FORMAT_BLOCKED** | same geometry guard | code, not measured |

None of the three larger resolutions reached the 64 MiB band limit. Reporting
that limit as the blocker for 8K would be wrong: the geometry guard refuses
first.

## Is the 64 MiB band limit reachable at all?

Only after lift 1. Rough per-band payload at 16-bit samples, ignoring padding:

| Resolution | total pixels | ÷16 (LL + highpass bands) | per band, 2 B/sample |
|---|---|---|---|
| 4K | 8.3 M | 0.52 M | ~1.0 MiB |
| 8K | 33.2 M | 2.07 M | ~4.1 MiB |
| 12K | 98.8 M | 6.17 M | ~12.3 MiB |
| 17K | 140.9 M | 8.80 M | ~17.6 MiB |

Even at 17K a single band stays far below 64 MiB, so the chunk field is **not**
the binding constraint for these four geometries. The binding constraint is the
4096 dimension cap plus the 4K-sized session budget. Anyone planning 8K work
should lift `kMaxDimension`/`kMaxPlaneElements` deliberately and re-measure
memory, not chase the chunk format.

Correction to an earlier draft of this audit: that draft listed 8K as
SUPPORTED and named the 24-bit sub-band field as the blocker for 12K/17K. Both
claims were unsupported by measurement and are withdrawn here.

## Memory, if the guards were lifted

Per-frame working set at 2 bytes/sample for a full pyramid, plus the BGRA input:

| Resolution | BGRA input | full pyramid (est.) |
|---|---|---|
| 4K | 31.6 MiB | ~50 MiB |
| 8K | 126.6 MiB | ~200 MiB |
| 12K | 376.9 MiB | ~600 MiB |
| 17K | 537.3 MiB | ~850 MiB |

Estimates are arithmetic, not measurements. Band-wise processing would remove
the full-pyramid term; the input term stays.

## Not done, deliberately

- No 8K/12K/17K frame was allocated. The A2 has 15,356 MiB total, and the point
  of this audit is to find the first blocker without provoking an OOM.
- No CineForm bitstream above 4K was produced, so no compatibility claim is
  made. A resolution is not "supported" because its dimensions are representable
  in the format description.
- Streaming/tiled workflow is not designed here. Any such design must keep the
  global bitstream unchanged; that is a separate proposal.

## Recommended next action

Treat 8K as a deliberate capability change, not a bug fix: raise
`kMaxDimension` and the session budget behind an explicit opt-in, then re-run
the 4K regression set to prove the smaller path is unchanged. Do not advertise
8K before a real packet exists and an independent decoder has read it.
