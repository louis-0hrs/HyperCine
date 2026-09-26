// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_GPU_PYRAMID_H
#define HYPERCINE_GPU_PYRAMID_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HyperCineGpuSlot HyperCineGpuSlot;

typedef struct HyperCineGpuTiming {
    float h2d_ms;
    float compute_ms;
    float d2h_ms;
    size_t h2d_bytes;
    size_t d2h_bytes;
    int band_count;
} HyperCineGpuTiming;

typedef struct HyperCineColorTiming {
    float h2d_ms;
    float kernel_ms;
    float d2h_ms;
    float resident_kernel_ms;
    int mismatch_vs_reference;
} HyperCineColorTiming;

HyperCineGpuSlot* HyperCine_GpuSlotCreate(int max_width, int max_height);
void HyperCine_GpuSlotDestroy(HyperCineGpuSlot* slot);

// planes are tight int16 host pointers. quant is [channel][level][band] flattened
// as channel-major, then level 0..2, then LL,LH,HL,HH. prescale has 3 entries.
// async_launch queues work and returns. Sync fills timing and publishes band pointers.
int HyperCine_GpuSlotLaunch(HyperCineGpuSlot* slot,
                            const int16_t* const planes[3],
                            const int widths[3],
                            const int heights[3],
                            const int prescale[3],
                            const int quant[36],
                            int async_launch);

int HyperCine_GpuSlotSync(HyperCineGpuSlot* slot, HyperCineGpuTiming* timing);

int HyperCine_GpuSlotBandCount(const HyperCineGpuSlot* slot);
int HyperCine_GpuSlotBand(const HyperCineGpuSlot* slot, int index,
                          int* channel, int* level, int* band,
                          int* width, int* height, const int16_t** data);

int16_t* HyperCine_GpuSlotHostPlane(HyperCineGpuSlot* slot, int channel, int* capacity_elems);

// Exact integer port of ConvertRGB32to10bitYUVFrame's active 4:2:2 path.
// y/u/v are tight host planes. u and v are width/2. Natural Cb/Cr order
// (the encoder later swaps the U and V plane pointers).
int HyperCine_BgraToYuv10_CUDA(const uint8_t* bgra, int width, int height, int pitch,
                               int color_space,
                               int16_t* y, int16_t* u, int16_t* v,
                               HyperCineColorTiming* timing);

#ifdef __cplusplus
}
#endif

#endif
