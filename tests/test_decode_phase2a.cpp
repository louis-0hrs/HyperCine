// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include "hypercine/decode_phase2a.h"
#include "decode_phase0_support.h"
#include "decode_phase1_support.h"

using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("expected corpus manifest path");
        }
        std::ifstream input(argv[1]);
        if (!input) {
            throw std::runtime_error("missing golden manifest: " + std::string(argv[1]));
        }

        std::vector<Sample> samples;
        int current_width = 0, count = 0;
        std::string line;

        std::cout << "=== HyperCine Decode Phase 2A Verification ===" << std::endl;
        std::cout << "Testing CUDA Color Conversion & Native DeviceOutput..." << std::endl;

        CudaDecoderPipelinePhase2A pipeline;

        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') continue;

            int w, h, p;
            size_t bytes;
            std::string ih, ch, bgra_hash, yu64_hash;
            std::istringstream row(line);
            if (!(row >> w >> h >> p >> ih >> ch >> bytes >> bgra_hash >> yu64_hash)) {
                throw std::runtime_error("malformed manifest row");
            }

            if (w != current_width) {
                samples = Generate(w, h, w == 7680 ? 1 : 5);
                current_width = w;

                if (!pipeline.Initialize(w, h, 3, 10)) {
                    throw std::runtime_error("failed to initialize CUDA pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }
            }

            if (p < 0 || p >= int(samples.size())) {
                throw std::runtime_error("invalid pattern index");
            }

            auto& s = samples[size_t(p)];
            if (s.input_hash != ih || s.compressed.size() != bytes || Hash(s.compressed.data(), bytes) != ch) {
                throw std::runtime_error("golden generation hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            // 1. Pristine CPU decode for BGRA
            ReferenceDecoder ref_bgra(s, CFHD_PIXEL_FORMAT_BGRA);
            ref_bgra.Decode(s);
            ref_bgra.VerifyThreads();
            auto* d_bgra = ref_bgra.GetNative();

            // Extract coefficient bands from native structure
            auto bands_bgra = ExtractBandsFromNative(d_bgra);
            if (bands_bgra.size() != 30) {
                throw std::runtime_error("expected 30 coefficient bands for BGRA");
            }

            // 2. Test BGRA DeviceOutput Path
            uint64_t ticket_bgra = pipeline.QueueDecode(
                bands_bgra,
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );

            if (ticket_bgra == 0) {
                throw std::runtime_error("QueueDecode (BGRA) returned ticket 0");
            }

            // Test non-blocking PollDecode / WaitForDecode
            while (pipeline.PollDecode(ticket_bgra) == DecodeTicketStatus::Pending) {
                // busy-wait / poll check
            }
            if (pipeline.PollDecode(ticket_bgra) != DecodeTicketStatus::Complete) {
                throw std::runtime_error("PollDecode failed for BGRA ticket");
            }

            // Retrieve BGRA DeviceOutput descriptor
            DeviceBgraSurfaceDescriptor bgra_desc{};
            if (!pipeline.GetDeviceBgraOutput(ticket_bgra, &bgra_desc)) {
                throw std::runtime_error("GetDeviceBgraOutput failed");
            }

            if (bgra_desc.d_bgra == nullptr || bgra_desc.width != w || bgra_desc.height != h) {
                throw std::runtime_error("Invalid BGRA descriptor values");
            }

            // Host validation path: download BGRA
            int bgra_pitch = ref_bgra.pitch;
            std::vector<uint8_t> host_bgra(bgra_pitch * h, 0);
            if (!pipeline.DownloadBgraToHost(ticket_bgra, host_bgra.data(), bgra_pitch)) {
                throw std::runtime_error("DownloadBgraToHost failed");
            }

            // Verify bit-exact equality with pristine CPU decode (0 max error)
            int max_abs_err = 0;
            int diff_bytes = 0;
            const uint8_t* ref_data = ref_bgra.output.data();
            for (size_t i = 0; i < host_bgra.size(); ++i) {
                int err = std::abs(int(host_bgra[i]) - int(ref_data[i]));
                if (err > max_abs_err) max_abs_err = err;
                if (err > 0) diff_bytes++;
            }

            if (max_abs_err != 0 || diff_bytes != 0) {
                throw std::runtime_error("BGRA DeviceOutput mismatch: max_abs_err=" + std::to_string(max_abs_err) +
                                         ", diff_bytes=" + std::to_string(diff_bytes));
            }

            // Verify hash matches golden manifest
            if (Hash(host_bgra.data(), host_bgra.size()) != bgra_hash) {
                throw std::runtime_error("Manifest BGRA hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            // Query stage timings
            SlotStageTimings timings{};
            if (!pipeline.GetStageTimings(ticket_bgra, &timings)) {
                throw std::runtime_error("GetStageTimings failed");
            }

            // Release ticket ownership
            if (!pipeline.ReleaseOutput(ticket_bgra)) {
                throw std::runtime_error("ReleaseOutput failed");
            }

            // 3. Test Planar YUV422 DeviceOutput Path
            ReferenceDecoder ref_yu64(s, CFHD_PIXEL_FORMAT_YU64);
            ref_yu64.Decode(s);
            auto* d_yu64 = ref_yu64.GetNative();
            auto bands_planar = ExtractBandsFromNative(d_yu64);
            auto cpu_planar_ref = ComputeCpuReferenceIntermediates(d_yu64);

            uint64_t ticket_planar = pipeline.QueueDecode(
                bands_planar,
                DeviceSurfaceType::PlanarYUV422_16S,
                ColorMatrix::BT709,
                ColorRange::CG
            );

            if (ticket_planar == 0) {
                throw std::runtime_error("QueueDecode (Planar) returned ticket 0");
            }

            if (!pipeline.WaitForDecode(ticket_planar)) {
                throw std::runtime_error("WaitForDecode failed for Planar ticket");
            }

            DevicePlanarSurfaceDescriptor planar_desc{};
            if (!pipeline.GetDevicePlanarOutput(ticket_planar, &planar_desc)) {
                throw std::runtime_error("GetDevicePlanarOutput failed");
            }

            if (!planar_desc.d_y || !planar_desc.d_u || !planar_desc.d_v ||
                planar_desc.width != w || planar_desc.height != h) {
                throw std::runtime_error("Invalid Planar descriptor values");
            }

            int y_w = w, y_h = h;
            int c_w = w / 2, c_h = h;
            std::vector<int16_t> host_y(y_w * y_h, 0);
            std::vector<int16_t> host_v(c_w * c_h, 0);
            std::vector<int16_t> host_u(c_w * c_h, 0);

            if (!pipeline.DownloadPlanarToHost(
                ticket_planar,
                host_y.data(), y_w * sizeof(int16_t),
                host_v.data(), c_w * sizeof(int16_t),
                host_u.data(), c_w * sizeof(int16_t)))
            {
                throw std::runtime_error("DownloadPlanarToHost failed");
            }

            if (std::memcmp(host_y.data(), cpu_planar_ref.plane[0].data(), host_y.size() * sizeof(int16_t)) != 0) {
                throw std::runtime_error("Planar Y channel mismatch");
            }
            if (std::memcmp(host_v.data(), cpu_planar_ref.plane[1].data(), host_v.size() * sizeof(int16_t)) != 0) {
                throw std::runtime_error("Planar V channel mismatch");
            }
            if (std::memcmp(host_u.data(), cpu_planar_ref.plane[2].data(), host_u.size() * sizeof(int16_t)) != 0) {
                throw std::runtime_error("Planar U channel mismatch");
            }

            if (!pipeline.ReleaseOutput(ticket_planar)) {
                throw std::runtime_error("ReleaseOutput failed for planar ticket");
            }

            std::cout << "  " << std::setw(5) << w << "x" << std::setw(4) << h << " Pattern " << p
                      << " (" << std::left << std::setw(8) << PatternName(p) << "): "
                      << "H2D=" << std::fixed << std::setprecision(3) << timings.h2d_ms << "ms "
                      << "Inv=" << timings.inverse_ms << "ms "
                      << "Color=" << timings.color_ms << "ms "
                      << "TotalGPU=" << timings.total_gpu_ms << "ms "
                      << "[PASS: 0 diff on BGRA8 DeviceOutput & Planar YUV422]" << std::endl;

            ++count;
        }

        if (count != 11) {
            throw std::runtime_error("expected 11 golden samples, got " + std::to_string(count));
        }

        std::cout << "\n--- Slot Pool & Stream Synchronization Contract Testing ---" << std::endl;
        {
            // Test slot pool capacity (3 slots)
            CudaDecoderPipelinePhase2A pool_pipe;
            pool_pipe.Initialize(1920, 1080, 3, 10);

            auto sample_vec = Generate(1920, 1080, 1);
            ReferenceDecoder ref(sample_vec[0], CFHD_PIXEL_FORMAT_BGRA);
            ref.Decode(sample_vec[0]);
            auto bands = ExtractBandsFromNative(ref.GetNative());

            uint64_t t1 = pool_pipe.QueueDecode(bands, DeviceSurfaceType::BGRA8);
            uint64_t t2 = pool_pipe.QueueDecode(bands, DeviceSurfaceType::BGRA8);
            uint64_t t3 = pool_pipe.QueueDecode(bands, DeviceSurfaceType::BGRA8);
            uint64_t t4 = pool_pipe.QueueDecode(bands, DeviceSurfaceType::BGRA8);

            if (t1 == 0 || t2 == 0 || t3 == 0) {
                throw std::runtime_error("first 3 slots should be granted");
            }
            if (t4 != 0) {
                throw std::runtime_error("4th slot request must fail when pool size is 3 and none released");
            }

            // Release slot 2 and ensure new queue succeeds
            pool_pipe.ReleaseOutput(t2);
            uint64_t t5 = pool_pipe.QueueDecode(bands, DeviceSurfaceType::BGRA8);
            if (t5 == 0) {
                throw std::runtime_error("slot should be reusable after ReleaseOutput");
            }

            // Test StreamWait
            cudaStream_t caller_stream;
            cudaStreamCreateWithFlags(&caller_stream, cudaStreamNonBlocking);
            if (!pool_pipe.StreamWait(t5, caller_stream)) {
                throw std::runtime_error("StreamWait failed");
            }
            cudaStreamSynchronize(caller_stream);
            cudaStreamDestroy(caller_stream);

            pool_pipe.ReleaseOutput(t1);
            pool_pipe.ReleaseOutput(t3);
            pool_pipe.ReleaseOutput(t5);
            std::cout << "  [PASS] Slot Pool Lifecycle & StreamWait verified." << std::endl;
        }

        std::cout << "\nSUCCESS: All 11/11 golden corpus tests passed Phase 2A validation with 0 diff!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "PHASE 2A TEST ERROR: " << e.what() << std::endl;
        return 1;
    }
}
