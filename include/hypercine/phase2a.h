// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE2A_H
#define HYPERCINE_PHASE2A_H

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>

struct encoder;
struct transform;

namespace hypercine {

enum class Phase2Input {
    HostBgra = 1,
    DeviceBgra = 2
};

enum class Phase2Transport {
    FullInt16 = 0,
    Int16Stream = 1,
    CompactStream = 2
};

struct Phase2Frame {
    double latency_ms = 0;
    double color_ms = 0;
    double h2d_ms = 0;
    double gpu_ms = 0;
    double range_ms = 0;
    double pack_ms = 0;
    double d2h_ms = 0;
    double entropy_ms = 0;
    double assembly_ms = 0;
    double schedule_ms = 0;
    double overlap_gpu_d2h_ms = 0;
    double overlap_d2h_entropy_ms = 0;
    double assembly_delay_ms = 0;
    double mean_ready_bands = 0;
    double max_ready_bands = 0;
    double entropy_span_ms = 0;
    size_t h2d_bytes = 0;
    size_t d2h_bytes = 0;
    size_t encoded_bytes = 0;
    int int8_bands = 0;
    int int16_bands = 0;
};

struct Phase2Run {
    bool success = false;
    bool byte_match = false;
    bool verified = false;
    int coeff_mismatches = 0;
    int coeff_compared = 0;
    int expand_mismatches = 0;
    int frames = 0;
    int workers = 0;
    int pipeline_depth = 1;
    int entropy_workers_busy = 0;
    double wall_ms = 0;
    double sustained_fps = 0;
    double mean_latency_ms = 0;
    double mean_color_ms = 0;
    double mean_h2d_ms = 0;
    double mean_gpu_ms = 0;
    double mean_range_ms = 0;
    double mean_pack_ms = 0;
    double mean_d2h_ms = 0;
    double mean_entropy_ms = 0;
    double mean_assembly_ms = 0;
    double mean_schedule_ms = 0;
    double mean_overlap_gpu_d2h_ms = 0;
    double mean_overlap_d2h_entropy_ms = 0;
    double mean_assembly_delay_ms = 0;
    double mean_ready_bands = 0;
    double cpu_seconds = 0;
    double cpu_cores = 0;
    size_t h2d_bytes = 0;
    size_t d2h_bytes = 0;
    size_t encoded_bytes = 0;
    std::string error;
    std::string input_name;
    std::string transport_name;
    std::vector<uint8_t> bitstream;
    std::vector<Phase2Frame> frame_times;
};

struct Phase2Config {
    Phase2Input input = Phase2Input::HostBgra;
    Phase2Transport transport = Phase2Transport::FullInt16;
    int frames = 4;
    int warmup = 1;
    int workers = 16;
    int pipeline_depth = 2;
    bool verify = false;
};

// The encoder must already have encoded one sample so quantizers and codebooks exist.
// Host BGRA is registered for the duration of the call when the driver accepts it.
// Device BGRA uploads the frame once before the timed region.
Phase2Run RunPhase2(encoder* encoder,
                    transform* transforms[],
                    int channel_count,
                    const uint8_t* bgra,
                    int pitch,
                    void* scratch,
                    size_t scratch_bytes,
                    const Phase2Config& config);

}  // namespace hypercine

#endif
