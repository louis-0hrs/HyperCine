# Capability audit: 4K, 8K, 12K and 17K (issues #48, #34)

Status: analysis only. No frame above 4K was allocated and no bitstream above 4K
exists. No resolution above 4K is claimed as supported.

This replaces an earlier version of this document that was wrong in two ways,
both corrected below and both called out in "Corrections" so the error is not
silently erased.

## What was actually run

Linux x86_64, NVIDIA A2 (compute 8.6), Release, `-DHYPERCINE_BUILD_STANDALONE=ON`:

```
hypercine_encode_filmscan3 --geometry 3840x2160   (31 MiB BGRA input)
  rc=0   frame_0000.cfhd = 6,156,376 bytes

hypercine_encode_filmscan3 --geometry 7680x4320   (126 MiB BGRA input)
  rc=1   "FilmScan3 geometry requires width %128, height %8,
          width [128,4096], height [24,4096]; 8K unqualified"

hypercine_encode_filmscan3 --geometry 4104x2160
  rc=1   same message
```

The 4K figure is the anchor for every estimate below: 6,156,376 bytes at
8,294,400 pixels is **0.742 bytes per pixel** for this pattern.

## Three limits, in the order they actually fire

### 1. Geometry validation (fires first, before a byte is written)

`src/cpu/filmscan3_frontend.cpp:9-14`, with `include/hypercine/standalone.h:31,34`:

```cpp
inline constexpr int kMaxDimension = 4096;
inline constexpr std::size_t kMaxPlaneElements = 3840u * 2160u;   // 8,294,400

if (g.width < 128 || g.width > kMaxDimension || g.width % 128 ||
    g.height < 24 || g.height > kMaxDimension || g.height % 8)
    throw std::invalid_argument("... width [128,4096], height [24,4096]; 8K unqualified");
if (g.width > kMaxPlaneElements / g.height)
    throw std::length_error("FilmScan3 exceeds 8294400-pixel CPU session budget");
```

The session budget is exactly 4K UHD. 8K, 12K and 17K all fail both the 4096
dimension cap and the pixel budget. This is the first blocker for every
resolution above 4K, and it is the blocker a developer will hit.

### 2. The outer sample chunk (the real format limit, and it does bind)

`src/core/filmscan3_encoder.cpp:138` opens one chunk that contains the entire
encoded sample, closed at `:170`:

```cpp
const auto sample=s.Begin(0x2200);      // whole sample starts here
...
s.Tag(24,0);s.End(sample,0x2200);       // whole sample ends here
```

and `End()` at `:52-56` caps any chunk at 24 bits of word count:

```cpp
const auto words=(bytes.size()-at-4)/4;
if(words>0xffffff)throw std::length_error("CFHD chunk overflow");
```

`0xFFFFFF` words = 67,108,860 bytes = **64 MiB for the complete encoded sample**.

Extrapolating the measured 0.742 bytes/pixel:

| resolution | pixels | sample (est.) | over 64 MiB? |
|---|---|---|---|
| 4K UHD 3840x2160 | 8.3 M | 5.9 MiB (measured) | no |
| 8K UHD 7680x4320 | 33.2 M | 23.5 MiB | no |
| URSA 12K 12288x8040 | 98.8 M | 69.9 MiB | **yes** |
| URSA 17K 17520x8040 | 140.9 M | 99.7 MiB | **yes** |

So the sample-chunk ceiling is a genuine format-level blocker at 12K and 17K,
and would be reached before any memory limit. 8K fits inside it comfortably.

The nested band chunks (`0x2000`) and level chunks (`0x2100`) are smaller than
the sample by construction and are not the binding constraint; each band at 17K
works out to roughly 17.6 MiB, well under the ceiling.

### 3. CPU session budget (see 1 — same source line)

Listed separately because lifting `kMaxDimension` alone would not be enough:
`kMaxPlaneElements` is an independent term and is tuned to exactly 4K UHD.

## Table

| Resolution | Dimensions | Pixels | Verdict | First blocker | Evidence |
|---|---|---|---|---|---|
| UHD 4K | 3840x2160 | 8,294,400 | **SUPPORTED** | none | encoded, 6,156,376 B, rc=0; CPU and Metal packet-equal |
| UHD 8K | 7680x4320 | 33,177,600 | **IMPLEMENTATION_LIMIT** | geometry guard (width > 4096, pixels > 8,294,400) | measured rc=1, message quoted above |
| URSA 12K | 12288x8040 | 98,795,520 | **IMPLEMENTATION_LIMIT**, and the format ceiling binds after that | geometry guard, then 64 MiB sample chunk (69.9 MiB est.) | code + extrapolation from the measured 4K sample |
| URSA 17K | 17520x8040 | 140,860,800 | **IMPLEMENTATION_LIMIT**, and the format ceiling binds after that | geometry guard, then 64 MiB sample chunk (99.7 MiB est.) | code + extrapolation |

"IMPLEMENTATION_LIMIT" means the current software refuses it; it does not mean
the CineForm format cannot represent it. For 12K/17K the format itself becomes
the second blocker, which is a different kind of problem and needs a different
decision.

## Corrections to the previous version of this document

1. **It called 8K SUPPORTED.** It is not. The encoder refuses it with
   `8K unqualified` before writing anything. The earlier draft drew that verdict
   without running the encode.
2. **It called the 24-bit chunk limit non-binding at 12K/17K.** That was
   computed per *band* only. The writer also opens one chunk around the whole
   sample (`filmscan3_encoder.cpp:138`, `Begin(0x2200)`), and that is what the
   64 MiB ceiling applies to. At the measured 0.742 bytes/pixel it binds at 12K
   and 17K.
3. The earlier per-band table (~1.0 to ~17.6 MiB) remains arithmetically correct
   and is kept above as context, but it is not the binding limit.

## Memory, if the guards were lifted

At 2 bytes/sample for a full pyramid, plus the BGRA input:

| Resolution | BGRA input | full pyramid (est.) |
|---|---|---|
| 4K | 31.6 MiB | ~50 MiB |
| 8K | 126.6 MiB | ~200 MiB |
| 12K | 376.9 MiB | ~600 MiB |
| 17K | 537.3 MiB | ~850 MiB |

Estimates, not measurements. Band-wise processing removes the pyramid term; the
input term stays.

## Not done, deliberately

- No frame above 4K was allocated. The A2 has 15,356 MiB and the point of this
  audit is to find the first blocker without provoking an OOM.
- No bitstream above 4K exists, so no format-compatibility claim is made. A
  resolution is not supported because its dimensions are representable.
- Streaming/tiled workflow is not designed here. Any such design must keep the
  global bitstream unchanged.

## Recommended next action

Treat 8K as a deliberate capability change: raise `kMaxDimension` and
`kMaxPlaneElements` behind an explicit opt-in, then re-run the 4K regression set
to prove the smaller path is unchanged. 8K stays inside the 64 MiB sample chunk,
so it is a code change and not a format problem.

12K and 17K are a different matter: after the geometry guard the sample chunk
ceiling binds. Do not plan those without a decision about the container format.
