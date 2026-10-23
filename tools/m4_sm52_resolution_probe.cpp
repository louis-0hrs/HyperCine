// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Bounded Tesla M4 / SM 5.2 Phase 2C resolution probe for issue #116.
// It exposes only public Phase2C entry points so a constrained device can
// exercise one identity or one pipeline configuration without the complete
// test_phase2c executable (which includes unrelated 8K pipeline defaults).

#include "hypercine/phase2c.h"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void PrintDevice() {
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) {
        std::printf("device status=unavailable\n");
        return;
    }
    std::printf("device name=%s cc=%d.%d total_global_mem=%zu multi_processor_count=%d\n",
                prop.name, prop.major, prop.minor, prop.totalGlobalMem, prop.multiProcessorCount);
}

void PrintMem(const char* tag) {
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    const cudaError_t status = cudaMemGetInfo(&free_bytes, &total_bytes);
    if (status != cudaSuccess) {
        std::printf("mem %s status=%s\n", tag, cudaGetErrorString(status));
        return;
    }
    std::printf("mem %s free=%zu total=%zu used=%zu\n", tag, free_bytes, total_bytes,
                total_bytes - free_bytes);
}

int Identity(int width, int height) {
    PrintMem("before");
    const auto start = std::chrono::steady_clock::now();
    const auto report = hypercine::VerifyPhase2CIdentity(width, height);
    const auto end = std::chrono::steady_clock::now();
    PrintMem("after");
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    std::printf(
        "identity %dx%d ok=%d error=%s coeff=%d len=%d payload=%d sample=%d streamed=%d "
        "multistream=%d gpulowpass=%d decoder_pixels=%d mismatch_at=%d max_diff=%d "
        "psnr=%.6f sample_bytes=%zu bands=%zu elapsed_ms=%.3f\n",
        width, height, report.ok ? 1 : 0, report.error.c_str(),
        report.pre_entropy_coeff_equal, report.per_band_payload_len_equal,
        report.per_band_payload_byte_equal, report.complete_sample_byte_equal,
        report.streamed_d2h_sample_byte_equal, report.multistream_sample_byte_equal,
        report.gpu_lowpass_sample_byte_equal, report.upstream_decoder_pixel_equal,
        report.mismatch_offset, report.decoded_max_abs_diff, report.psnr_vs_source_db,
        report.sample_bytes, report.bands.size(), elapsed_ms);
    std::printf("identity_sha cpu=%s gpu_consolidated=%s gpu_streamed=%s gpu_lowpass=%s "
                "cpu_decoded=%s gpu_decoded=%s\n",
                report.cpu_sample_sha256.c_str(),
                report.gpu_consolidated_sample_sha256.c_str(),
                report.gpu_streamed_sample_sha256.c_str(),
                report.gpu_lowpass_sample_sha256.c_str(),
                report.cpu_decoded_pixels_sha256.c_str(),
                report.gpu_decoded_pixels_sha256.c_str());
    return report.ok ? 0 : 1;
}

int Pipeline(int width, int height, int slots, int frames, int streams, bool host_input) {
    hypercine::Phase2CConfig config;
    config.width = width;
    config.height = height;
    config.warmup_frames = 1;
    config.frames = frames;
    config.frame_slots = slots;
    config.entropy_streams = streams;
    config.d2h_mode = host_input ? hypercine::CompressedD2HMode::BandStreamed
                                 : hypercine::CompressedD2HMode::FrameConsolidated;
    config.input_mode = host_input ? hypercine::Phase2CInputMode::HostInput
                                   : hypercine::Phase2CInputMode::DeviceInput;
    config.gpu_pack_lowpass = host_input;
    config.verify_bitstream = true;
    PrintMem("before");
    const auto start = std::chrono::steady_clock::now();
    const auto result = hypercine::RunPhase2C(config);
    const auto end = std::chrono::steady_clock::now();
    PrintMem("after");
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    std::printf(
        "pipeline %dx%d host=%d slots=%d frames=%d streams=%d ok=%d error=%s encoded=%d "
        "fps=%.6f mean_lat=%.6f p95_lat=%.6f psnr=%.6f wall_ms=%.3f bitstream_match=%d "
        "decode_ok=%d vram_used=%zu slot_vram=%zu slot_pinned=%zu elapsed_ms=%.3f\n",
        width, height, host_input ? 1 : 0, slots, frames, streams, result.ok ? 1 : 0,
        result.error.c_str(), result.encoded_frames, result.sustained_fps,
        result.mean_latency_ms, result.p95_latency_ms, result.psnr_db, result.wall_ms,
        result.bitstream_byte_match ? 1 : 0, result.decode_ok ? 1 : 0,
        result.vram_used_bytes, result.slot_vram_bytes, result.slot_pinned_host_bytes,
        elapsed_ms);
    return result.ok ? 0 : 1;
}

void Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  m4_sm52_resolution_probe identity <width> <height>\n"
                 "  m4_sm52_resolution_probe pipeline <width> <height> <slots> <frames> "
                 "<streams> <host|device>\n");
}

}  // namespace

int main(int argc, char** argv) {
    PrintDevice();
    if (argc == 4 && std::strcmp(argv[1], "identity") == 0) {
        return Identity(std::atoi(argv[2]), std::atoi(argv[3]));
    }
    if (argc == 8 && std::strcmp(argv[1], "pipeline") == 0) {
        const bool host_input = std::strcmp(argv[7], "host") == 0;
        if (!host_input && std::strcmp(argv[7], "device") != 0) {
            Usage();
            return 2;
        }
        return Pipeline(std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]),
                        std::atoi(argv[5]), std::atoi(argv[6]), host_input);
    }
    Usage();
    return 2;
}
