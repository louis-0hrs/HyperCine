// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_HYBRID_ENCODER_H
#define HYPERCINE_HYBRID_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>

struct encoder;
struct transform;

namespace hypercine {

struct HybridFrame {
    double color_ms = 0;
    double pack_ms = 0;
    double h2d_ms = 0;
    double gpu_ms = 0;
    double d2h_ms = 0;
    double entropy_ms = 0;
    double assembly_ms = 0;
    double latency_ms = 0;
    size_t h2d_bytes = 0;
    size_t d2h_bytes = 0;
    size_t encoded_bytes = 0;
};

struct HybridRun {
    bool success = false;
    bool pipelined = false;
    bool byte_match = false;
    bool decoded = false;
    int coeff_mismatches = 0;
    int coeff_compared = 0;
    int entropy_workers = 0;
    int frames = 0;
    double startup_ms = 0;
    double wall_ms = 0;
    double sustained_fps = 0;
    double mean_latency_ms = 0;
    double mean_color_ms = 0;
    double mean_pack_ms = 0;
    double mean_h2d_ms = 0;
    double mean_d2h_ms = 0;
    double mean_gpu_ms = 0;
    double mean_entropy_ms = 0;
    double mean_assembly_ms = 0;
    double gpu_compute_duty = 0;
    double process_cpu_seconds = 0;
    size_t h2d_bytes = 0;
    size_t d2h_bytes = 0;
    size_t encoded_bytes = 0;
    std::string error;
    std::vector<uint8_t> bitstream;
    std::vector<HybridFrame> frame_times;
};

// Synchronous or pipelined hybrid encode.
// The caller passes an encoder that has already encoded one warmup sample.
// Timed work is color conversion, GPU pyramid, minimum D2H, parallel entropy, assembly.
// Coefficient comparison against the upstream pyramid is performed once, outside the FPS window.
HybridRun RunHybrid(encoder* encoder,
                    transform* transforms[],
                    int channel_count,
                    const uint8_t* bgra,
                    int pitch,
                    void* scratch,
                    size_t scratch_bytes,
                    int frame_count,
                    int entropy_workers,
                    bool pipelined);

}  // namespace hypercine

#endif
