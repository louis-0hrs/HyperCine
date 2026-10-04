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

#include "hypercine/decode_phase2b.h"
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

        std::cout << "=== HyperCine Decode Phase 2B Verification ===" << std::endl;
        std::cout << "Testing Exact CUDA Entropy / VLC Decode & Native DeviceOutput..." << std::endl;

        CudaDecoderPipelinePhase2B pipeline;

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
                    throw std::runtime_error("failed to initialize CUDA Phase 2B pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }
            }

            if (p < 0 || p >= int(samples.size())) {
                throw std::runtime_error("invalid pattern index");
            }

            auto& s = samples[size_t(p)];
            if (s.input_hash != ih || s.compressed.size() != bytes || Hash(s.compressed.data(), bytes) != ch) {
                throw std::runtime_error("golden generation hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            // 1. Pristine CPU decode for BGRA reference
            ReferenceDecoder ref_bgra(s, CFHD_PIXEL_FORMAT_BGRA);
            ref_bgra.Decode(s);
            ref_bgra.VerifyThreads();
            auto* d_bgra = ref_bgra.GetNative();

            // Extract CPU reference coefficient bands
            auto cpu_bands = ExtractBandsFromNative(d_bgra);
            if (cpu_bands.size() != 30) {
                throw std::runtime_error("expected 30 coefficient bands from CPU decoder");
            }

            // 2. Test Phase 2B GPU Entropy + Inverse + BGRA DeviceOutput
            uint64_t ticket_bgra = pipeline.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );

            if (ticket_bgra == 0) {
                throw std::runtime_error("QueueDecode (BGRA) returned ticket 0");
            }

            // Poll check
            while (pipeline.PollDecode(ticket_bgra) == DecodeTicketStatus::Pending) {
                // busy wait
            }
            if (pipeline.PollDecode(ticket_bgra) != DecodeTicketStatus::Complete) {
                throw std::runtime_error("PollDecode failed for BGRA ticket");
            }

            // 3. Verify all 30 coefficient bands in GPU VRAM against CPU reference
            for (int b = 0; b < 30; ++b) {
                std::vector<int16_t> gpu_coeffs;
                int gw = 0, gh = 0, gpitch = 0;
                if (!pipeline.DownloadBandCoefficientsToHost(ticket_bgra, b, gpu_coeffs, &gw, &gh, &gpitch)) {
                    throw std::runtime_error("DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                const auto& ref_b = cpu_bands[b];

                if (gw != ref_b.width || gh != ref_b.height || gpitch != ref_b.pitch) {
                    throw std::runtime_error("Geometry mismatch for band " + std::to_string(b));
                }

                // Verify exact sample values
                int band_diffs = 0;
                size_t pitch_samples = gpitch / sizeof(int16_t);
                for (int y = 0; y < gh; ++y) {
                    const int16_t* ref_row = ref_b.host_data + y * (ref_b.pitch / sizeof(int16_t));
                    const int16_t* gpu_row = gpu_coeffs.data() + y * pitch_samples;
                    for (int x = 0; x < gw; ++x) {
                        if (ref_row[x] != gpu_row[x]) {
                            band_diffs++;
                            if (band_diffs <= 10) {
                                std::cout << "Band " << b << " diff at (x=" << x << ", y=" << y
                                          << " 1D=" << (y * pitch_samples + x) << "): ref="
                                          << ref_row[x] << " gpu=" << gpu_row[x] << std::endl;
                            }
                        }
                    }
                }

                if (band_diffs > 0) {
                    std::cout << "Debug for band " << b << ":" << std::endl;
                    // Print nearby ref and gpu samples around (x=0, y=12)
                    int target_y = 12;
                    for (int dy = -1; dy <= 1; ++dy) {
                        int y = target_y + dy;
                        if (y >= 0 && y < gh) {
                            const int16_t* rr = ref_b.host_data + y * (ref_b.pitch / sizeof(int16_t));
                            const int16_t* gr = gpu_coeffs.data() + y * pitch_samples;
                            std::cout << "  Row " << y << " [x=0..3] ref: " << rr[0] << "," << rr[1] << "," << rr[2] << "," << rr[3]
                                      << " | gpu: " << gr[0] << "," << gr[1] << "," << gr[2] << "," << gr[3]
                                      << " | [x=w-4..w-1] ref: " << rr[gw-4] << "," << rr[gw-3] << "," << rr[gw-2] << "," << rr[gw-1]
                                      << " | gpu: " << gr[gw-4] << "," << gr[gw-3] << "," << gr[gw-2] << "," << gr[gw-1] << std::endl;
                        }
                    }
                    throw std::runtime_error("Band " + std::to_string(b) + " coefficient mismatch: " +
                                             std::to_string(band_diffs) + " differences vs CPU reference!");
                }
            }

            // 4. Validate BGRA surface output
            DeviceBgraSurfaceDescriptor bgra_desc{};
            if (!pipeline.GetDeviceBgraOutput(ticket_bgra, &bgra_desc)) {
                throw std::runtime_error("GetDeviceBgraOutput failed");
            }

            int bgra_pitch = ref_bgra.pitch;
            std::vector<uint8_t> host_bgra(bgra_pitch * h, 0);
            if (!pipeline.DownloadBgraToHost(ticket_bgra, host_bgra.data(), bgra_pitch)) {
                throw std::runtime_error("DownloadBgraToHost failed");
            }

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

            if (Hash(host_bgra.data(), host_bgra.size()) != bgra_hash) {
                throw std::runtime_error("Manifest BGRA hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            // Query stage timings and diagnostics
            Phase2bSlotStageTimings timings{};
            if (!pipeline.GetStageTimings(ticket_bgra, &timings)) {
                throw std::runtime_error("GetStageTimings failed");
            }

            std::vector<GpuBandDecodeResult> diag_results;
            if (!pipeline.GetBandDecodeResults(ticket_bgra, diag_results) || diag_results.size() != 27) {
                throw std::runtime_error("GetBandDecodeResults failed");
            }

            for (size_t d = 0; d < diag_results.size(); ++d) {
                if (diag_results[d].status != GpuBandDecodeStatus::Complete) {
                    throw std::runtime_error("Band " + std::to_string(d) + " status not Complete");
                }
            }

            // Release BGRA ticket
            if (!pipeline.ReleaseOutput(ticket_bgra)) {
                throw std::runtime_error("ReleaseOutput failed");
            }

            // 5. Test Planar YUV422 DeviceOutput Path
            ReferenceDecoder ref_yu64(s, CFHD_PIXEL_FORMAT_YU64);
            ref_yu64.Decode(s);
            auto* d_yu64 = ref_yu64.GetNative();
            auto cpu_planar_ref = ComputeCpuReferenceIntermediates(d_yu64);

            uint64_t ticket_planar = pipeline.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
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
                throw std::runtime_error("ReleaseOutput (Planar) failed");
            }

            count++;
            std::cout << "  [PASS] " << std::setw(5) << w << "x" << std::setw(4) << h
                      << " " << std::setw(8) << PatternName(p)
                      << " | 30/30 Coeffs: EXACT"
                      << " | MaxErr: " << max_abs_err
                      << " | H2D: " << std::fixed << std::setprecision(3) << timings.compressed_h2d_ms << " ms"
                      << " | Entropy: " << timings.gpu_entropy_ms << " ms (LL: " << timings.gpu_ll_ms << " ms, VLC: " << timings.gpu_vlc_ms << " ms)"
                      << " | InvWavelet: " << timings.inverse_wavelet_ms << " ms"
                      << " | Color: " << timings.color_convert_ms << " ms"
                      << " | Total GPU: " << timings.total_gpu_ms << " ms"
                      << std::endl;
        }

        std::cout << "\nAll " << count << " Golden Test Vectors PASSED with 100% Bit-Exact Accuracy!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Verification failed: " << e.what() << std::endl;
        return 1;
    }
}
