// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE2A_GPU_H
#define HYPERCINE_PHASE2A_GPU_H

#include "hypercine/gpu_pyramid.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// input_kind: 1 host BGRA, 2 device BGRA.
// transport: 0 full int16 frame copy, 1 int16 band stream, 2 compact band stream.
enum {
    HYPERCINE_INPUT_HOST_BGRA = 1,
    HYPERCINE_INPUT_DEVICE_BGRA = 2,
    HYPERCINE_XPORT_FULL_INT16 = 0,
    HYPERCINE_XPORT_INT16_BAND = 1,
    HYPERCINE_XPORT_COMPACT_BAND = 2,
    HYPERCINE_XPORT_DEVICE = 3,
    HYPERCINE_XPORT_DEVICE_LOWPASS_D2H = 4
};

typedef struct HyperCineReadyBand {
    int channel;
    int level;
    int band;
    int width;
    int height;
    int elem_bytes;
    int value_min;
    int value_max;
    int fits_int8;
    const void* data;
    size_t bytes;
    float copy_ms;
} HyperCineReadyBand;

typedef struct HyperCineStreamTiming {
    float h2d_ms;
    float color_ms;
    float compute_ms;
    float range_ms;
    float pack_ms;
    float d2h_ms;
    float overlap_compute_d2h_ms;
    size_t h2d_bytes;
    size_t d2h_bytes;
    int int8_bands;
    int int16_bands;
    int band_count;
    int error;
} HyperCineStreamTiming;

typedef struct HyperCineTransferSample {
    char host_kind[32];
    char direction[24];
    size_t bytes;
    float ms;
    float gbps;
} HyperCineTransferSample;

typedef struct HyperCineTransferStats {
    char host_kind[32];
    char direction[24];
    size_t bytes;
    int runs;
    float mean_ms;
    float median_ms;
    float p95_ms;
    float mean_gbps;
    float median_gbps;
    float p95_gbps;
} HyperCineTransferStats;

typedef struct HyperCineDeviceLink {
    char name[256];
    int async_engines;
    int can_map_host;
    int concurrent_kernels;
    int gpu_overlap;
    int l2_bytes;
    int mem_bus_bits;
    int mem_clock_khz;
    int pci_bus;
    int pci_device;
    int unified_addressing;
    int driver_version;
    int runtime_version;
    int sm_count;
    int compute_major;
    int compute_minor;
    size_t total_vram_bytes;
} HyperCineDeviceLink;

HyperCineGpuSlot* HyperCine_GpuSlotCreateForPhase2C(int max_width, int max_height, int alloc_device_bgra);

int HyperCine_GpuSlotBegin(HyperCineGpuSlot* slot, int input_kind, int transport,
                           const uint8_t* host_or_device_bgra, int pitch, int width, int height,
                           int color_space, const int widths[3], const int heights[3],
                           const int prescale[3], const int quant[36]);

int HyperCine_GpuSlotKick(HyperCineGpuSlot* slot);
int HyperCine_GpuSlotKickStaged(HyperCineGpuSlot* slot, void* wait_frontend_event, void* wait_wavelet_event, int gpu_pack_lowpass);
int HyperCine_GpuSlotPollBand(HyperCineGpuSlot* slot, HyperCineReadyBand* band);
int HyperCine_GpuSlotGpuIdle(HyperCineGpuSlot* slot);
int HyperCine_GpuSlotWaveletIdle(HyperCineGpuSlot* slot);
int HyperCine_GpuSlotFrontendIdle(HyperCineGpuSlot* slot);
int HyperCine_GpuSlotReadTiming(HyperCineGpuSlot* slot, HyperCineStreamTiming* timing);

void* HyperCine_GpuSlotLevelEvent(const HyperCineGpuSlot* slot, int channel, int level);
void* HyperCine_GpuSlotWaveletEvent(const HyperCineGpuSlot* slot);
void* HyperCine_GpuSlotFrontendEvent(const HyperCineGpuSlot* slot);
void* HyperCine_GpuSlotCopyStream(const HyperCineGpuSlot* slot);
const int16_t* HyperCine_GpuSlotBandDevicePtr(const HyperCineGpuSlot* slot, int channel, int level, int band);
const int16_t* HyperCine_GpuSlotBandHostPtr(const HyperCineGpuSlot* slot, int channel, int level, int band);

int HyperCine_GpuSlotUploadBgra(HyperCineGpuSlot* slot, const uint8_t* host, int pitch,
                                int width, int height, const void** device_out);

int HyperCine_GpuSlotDownloadInt16(HyperCineGpuSlot* slot, int index, int16_t* dst, int capacity,
                                   int* width, int* height, int* channel, int* level, int* band);

typedef struct HyperCineDeviceBand {
    int channel;
    int level;
    int band;
    int width;
    int height;
    const int16_t* device;
} HyperCineDeviceBand;

int HyperCine_GpuSlotDeviceBandCount(const HyperCineGpuSlot* slot);
int HyperCine_GpuSlotDeviceBand(const HyperCineGpuSlot* slot, int index, HyperCineDeviceBand* band);

int HyperCine_QueryDeviceLink(HyperCineDeviceLink* info);
int HyperCine_BenchmarkTransfers(HyperCineTransferSample* out, int capacity);
int HyperCine_BenchmarkTransfersCanonical(HyperCineTransferStats* out, int capacity, int runs);
int HyperCine_RegisterHost(void* ptr, size_t bytes);
int HyperCine_UnregisterHost(void* ptr);
size_t HyperCine_QueryUsedVramBytes();

#ifdef __cplusplus
}
#endif

#endif
