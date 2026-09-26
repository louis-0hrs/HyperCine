// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE2B_H
#define HYPERCINE_PHASE2B_H

#include "hypercine/entropy_gpu.h"
#include "hypercine/entropy_stage.h"
#include "hypercine/phase2a.h"

#include <stdint.h>
#include <vector>

struct encoder;
struct transform;

namespace hypercine {

struct Phase2BConfig {
    Phase2Input input = Phase2Input::HostBgra;
    int frames = 4;
    int warmup = 1;
    bool verify = false;
    GpuEntropyDispatch dispatch = GpuEntropyDispatch::OneStream;
    int streams = 1;
};

struct Phase2BRun {
    bool success = false;
    bool byte_match = false;
    bool verified = false;
    int coeff_mismatches = 0;
    int coeff_compared = 0;
    int frames = 0;
    double wall_ms = 0;
    double sustained_fps = 0;
    double mean_latency_ms = 0;
    double mean_color_ms = 0;
    double mean_h2d_ms = 0;
    double mean_gpu_ms = 0;
    double mean_entropy_ms = 0;
    double mean_assembly_ms = 0;
    double cpu_seconds = 0;
    double cpu_cores = 0;
    size_t h2d_bytes = 0;
    size_t d2h_compressed_bytes = 0;
    size_t d2h_lowpass_bytes = 0;
    size_t d2h_bytes = 0;
    size_t encoded_bytes = 0;
    double bits_per_coeff = 0;
    int streams = 1;
    int scan_occupancy_blocks = 0;
    EntropyTiming entropy;
    std::string error;
    std::string input_name;
    std::string dispatch_name;
    std::vector<uint8_t> bitstream;
};

// GPU color, wavelet, and quantization stay on the device. Highpass entropy
// runs on the GPU. Only compressed band payloads and the uncompressed lowpass
// bands come back to the host for container assembly.
Phase2BRun RunPhase2B(encoder* encoder, transform* transforms[], int channel_count,
                      const uint8_t* bgra, int pitch, void* scratch, size_t scratch_bytes,
                      const Phase2BConfig& config);

int GpuEntropyScanOccupancy();

}  // namespace hypercine

#endif
