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
#include "hypercine/decode_phase2c.h"
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

        std::cout << "=== HyperCine Decode Phase 2C Verification ===" << std::endl;
        std::cout << "Testing Parallel Intra-Band CUDA Entropy / VLC Decode & Native DeviceOutput..." << std::endl;

        CudaDecoderPipelinePhase2B pipeline2b;
        CudaDecoderPipelinePhase2C pipeline2c;

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

                if (!pipeline2b.Initialize(w, h, 3, 10)) {
                    throw std::runtime_error("failed to initialize CUDA Phase 2B pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }

                if (!pipeline2c.Initialize(w, h, 3, 10)) {
                    throw std::runtime_error("failed to initialize CUDA Phase 2C pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }
            }

            if (p < 0 || p >= int(samples.size())) {
                throw std::runtime_error("invalid pattern index");
            }

            auto& s = samples[size_t(p)];
            if (s.input_hash != ih || s.compressed.size() != bytes || Hash(s.compressed.data(), bytes) != ch) {
                throw std::runtime_error("golden generation hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            // 1. Pristine CPU reference decode
            ReferenceDecoder ref_bgra(s, CFHD_PIXEL_FORMAT_BGRA);
            ref_bgra.Decode(s);
            ref_bgra.VerifyThreads();
            auto* d_bgra = ref_bgra.GetNative();

            auto cpu_bands = ExtractBandsFromNative(d_bgra);
            if (cpu_bands.size() != 30) {
                throw std::runtime_error("expected 30 coefficient bands from CPU decoder");
            }

            // 2. Phase 2B Serial GPU reference decode
            uint64_t ticket_2b = pipeline2b.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2b == 0 || !pipeline2b.WaitForDecode(ticket_2b)) {
                throw std::runtime_error("Phase 2B QueueDecode/WaitForDecode failed");
            }

            // 3. Phase 2C Parallel GPU decode (Adaptive Mode)
            pipeline2c.SetEntropyMode(GpuEntropyMode::ParallelAdaptive);

            uint64_t ticket_2c_bgra = pipeline2c.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2c_bgra == 0 || !pipeline2c.WaitForDecode(ticket_2c_bgra)) {
                throw std::runtime_error("Phase 2C QueueDecode/WaitForDecode (BGRA) failed");
            }

            // 4. Verify all 30 coefficient bands in GPU VRAM across:
            //    CPU reference vs Phase 2B Serial reference vs Phase 2C Parallel implementation
            for (int b = 0; b < 30; ++b) {
                std::vector<int16_t> gpu_2b_coeffs, gpu_2c_coeffs;
                int w_2b = 0, h_2b = 0, p_2b = 0;
                int w_2c = 0, h_2c = 0, p_2c = 0;

                if (!pipeline2b.DownloadBandCoefficientsToHost(ticket_2b, b, gpu_2b_coeffs, &w_2b, &h_2b, &p_2b)) {
                    throw std::runtime_error("Phase 2B DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                if (!pipeline2c.DownloadBandCoefficientsToHost(ticket_2c_bgra, b, gpu_2c_coeffs, &w_2c, &h_2c, &p_2c)) {
                    throw std::runtime_error("Phase 2C DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                const auto& ref_b = cpu_bands[b];
                if (w_2c != ref_b.width || h_2c != ref_b.height || p_2c != ref_b.pitch ||
                    w_2b != ref_b.width || h_2b != ref_b.height || p_2b != ref_b.pitch) {
                    throw std::runtime_error("Geometry mismatch for band " + std::to_string(b));
                }

                size_t pitch_samples = p_2c / sizeof(int16_t);
                for (int y = 0; y < h_2c; ++y) {
                    const int16_t* ref_row = ref_b.host_data + y * (ref_b.pitch / sizeof(int16_t));
                    const int16_t* row_2b = gpu_2b_coeffs.data() + y * pitch_samples;
                    const int16_t* row_2c = gpu_2c_coeffs.data() + y * pitch_samples;

                    for (int x = 0; x < w_2c; ++x) {
                        if (row_2c[x] != ref_row[x] || row_2c[x] != row_2b[x]) {
                            throw std::runtime_error("Tri-way coefficient mismatch on band " + std::to_string(b) +
                                                     " at (" + std::to_string(x) + "," + std::to_string(y) +
                                                     "): CPU=" + std::to_string(ref_row[x]) +
                                                     " 2B=" + std::to_string(row_2b[x]) +
                                                     " 2C=" + std::to_string(row_2c[x]));
                        }
                    }
                }
            }

            // 5. Validate Phase 2C BGRA surface output
            DeviceBgraSurfaceDescriptor bgra_desc{};
            if (!pipeline2c.GetDeviceBgraOutput(ticket_2c_bgra, &bgra_desc)) {
                throw std::runtime_error("GetDeviceBgraOutput failed");
            }

            int bgra_pitch = ref_bgra.pitch;
            std::vector<uint8_t> host_bgra(bgra_pitch * h, 0);
            if (!pipeline2c.DownloadBgraToHost(ticket_2c_bgra, host_bgra.data(), bgra_pitch)) {
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

            // Query stage timings
            Phase2cSlotStageTimings timings2c{};
            if (!pipeline2c.GetStageTimings(ticket_2c_bgra, &timings2c)) {
                throw std::runtime_error("GetStageTimings failed");
            }

            Phase2bSlotStageTimings timings2b{};
            pipeline2b.GetStageTimings(ticket_2b, &timings2b);

            pipeline2b.ReleaseOutput(ticket_2b);
            pipeline2c.ReleaseOutput(ticket_2c_bgra);

            // 6. Test Planar YUV422 DeviceOutput Path under Phase 2C
            ReferenceDecoder ref_yu64(s, CFHD_PIXEL_FORMAT_YU64);
            ref_yu64.Decode(s);
            auto* d_yu64 = ref_yu64.GetNative();
            auto cpu_planar_ref = ComputeCpuReferenceIntermediates(d_yu64);

            uint64_t ticket_planar = pipeline2c.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::PlanarYUV422_16S,
                ColorMatrix::BT709,
                ColorRange::CG
            );

            if (ticket_planar == 0 || !pipeline2c.WaitForDecode(ticket_planar)) {
                throw std::runtime_error("Phase 2C QueueDecode/WaitForDecode (Planar) failed");
            }

            int y_w = w, y_h = h;
            int c_w = w / 2, c_h = h;
            std::vector<int16_t> host_y(y_w * y_h, 0);
            std::vector<int16_t> host_v(c_w * c_h, 0);
            std::vector<int16_t> host_u(c_w * c_h, 0);

            if (!pipeline2c.DownloadPlanarToHost(
                ticket_planar,
                host_y.data(), y_w * sizeof(int16_t),
                host_v.data(), c_w * sizeof(int16_t),
                host_u.data(), c_w * sizeof(int16_t)))
            {
                throw std::runtime_error("DownloadPlanarToHost failed");
            }

            if (std::memcmp(host_y.data(), cpu_planar_ref.plane[0].data(), host_y.size() * sizeof(int16_t)) != 0 ||
                std::memcmp(host_v.data(), cpu_planar_ref.plane[1].data(), host_v.size() * sizeof(int16_t)) != 0 ||
                std::memcmp(host_u.data(), cpu_planar_ref.plane[2].data(), host_u.size() * sizeof(int16_t)) != 0) {
                throw std::runtime_error("Planar channel mismatch in Phase 2C");
            }

            pipeline2c.ReleaseOutput(ticket_planar);

            count++;
            float speedup = (timings2c.gpu_entropy_ms > 0.0f) ? (timings2b.gpu_entropy_ms / timings2c.gpu_entropy_ms) : 1.0f;
            std::cout << "  [PASS] " << std::setw(5) << w << "x" << std::setw(4) << h
                      << " " << std::setw(8) << PatternName(p)
                      << " | 30/30 Coeffs: EXACT"
                      << " | 2B Entropy: " << std::fixed << std::setprecision(2) << std::setw(6) << timings2b.gpu_entropy_ms << " ms"
                      << " | 2C Entropy: " << std::setw(6) << timings2c.gpu_entropy_ms << " ms"
                      << " | Speedup: " << std::setw(5) << speedup << "x"
                      << " | Total GPU: " << std::setw(6) << timings2c.total_gpu_ms << " ms"
                      << std::endl;
        }

        std::cout << "\nAll " << count << " Golden Test Vectors PASSED with 100% Bit-Exact Accuracy!" << std::endl;

        // 7. Determinism Stress Testing (Section 36)
        std::cout << "\n=== Running Determinism Stress Tests (Rule 6 & Section 36) ===" << std::endl;
        {
            std::cout << "Testing 1080p Noise Determinism (100 repeats)... ";
            auto noise_1080p = Generate(1920, 1080, 5)[2];
            std::string first_hash;

            for (int r = 0; r < 100; ++r) {
                uint64_t t = pipeline2c.QueueDecode(
                    noise_1080p.compressed.data(),
                    noise_1080p.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2c.WaitForDecode(t);
                std::vector<uint8_t> bgra(1920 * 4 * 1080);
                pipeline2c.DownloadBgraToHost(t, bgra.data(), 1920 * 4);
                pipeline2c.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (r == 0) first_hash = h;
                else if (h != first_hash) {
                    throw std::runtime_error("1080p noise nondeterminism on iteration " + std::to_string(r));
                }
            }
            std::cout << "PASSED (100/100 matches: " << first_hash << ")" << std::endl;
        }

        {
            std::cout << "Testing 4K Noise Determinism (50 repeats)... ";
            auto noise_4k = Generate(3840, 2160, 5)[2];
            std::string first_hash;

            for (int r = 0; r < 50; ++r) {
                uint64_t t = pipeline2c.QueueDecode(
                    noise_4k.compressed.data(),
                    noise_4k.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2c.WaitForDecode(t);
                std::vector<uint8_t> bgra(3840 * 4 * 2160);
                pipeline2c.DownloadBgraToHost(t, bgra.data(), 3840 * 4);
                pipeline2c.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (r == 0) first_hash = h;
                else if (h != first_hash) {
                    throw std::runtime_error("4K noise nondeterminism on iteration " + std::to_string(r));
                }
            }
            std::cout << "PASSED (50/50 matches: " << first_hash << ")" << std::endl;
        }

        {
            std::cout << "Testing 8K Flat Determinism (10 repeats)... ";
            auto flat_8k = Generate(7680, 4320, 1)[0];
            std::string first_hash;

            for (int r = 0; r < 10; ++r) {
                uint64_t t = pipeline2c.QueueDecode(
                    flat_8k.compressed.data(),
                    flat_8k.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2c.WaitForDecode(t);
                std::vector<uint8_t> bgra(7680 * 4 * 4320);
                pipeline2c.DownloadBgraToHost(t, bgra.data(), 7680 * 4);
                pipeline2c.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (r == 0) first_hash = h;
                else if (h != first_hash) {
                    throw std::runtime_error("8K flat nondeterminism on iteration " + std::to_string(r));
                }
            }
            std::cout << "PASSED (10/10 matches: " << first_hash << ")" << std::endl;
        }

        std::cout << "\nAll Phase 2C Determinism Stress Tests PASSED successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Verification failed: " << e.what() << std::endl;
        return 1;
    }
}
