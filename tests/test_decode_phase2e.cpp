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
#include <thread>

#include "hypercine/decode_phase2b.h"
#include "hypercine/decode_phase2c.h"
#include "hypercine/decode_phase2d.h"
#include "hypercine/decode_phase2e.h"
#include "decode_phase0_support.h"
#include "decode_phase1_support.h"

using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;

static bool CheckpointsEqual(const std::vector<GpuEntropySegmentDescriptor>& a, const std::vector<GpuEntropySegmentDescriptor>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].band_index != b[i].band_index ||
            a[i].byte_offset != b[i].byte_offset ||
            a[i].nibble_idx != b[i].nibble_idx ||
            a[i].initial_state != b[i].initial_state ||
            a[i].out_coeff_pos != b[i].out_coeff_pos ||
            a[i].end_byte_offset != b[i].end_byte_offset ||
            a[i].end_nibble_idx != b[i].end_nibble_idx ||
            a[i].max_coeff_pos != b[i].max_coeff_pos) {
            return false;
        }
    }
    return true;
}

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

        std::cout << "=== HyperCine Decode Phase 2E Verification ===" << std::endl;
        std::cout << "Testing Asynchronous Band Streaming & CPU/GPU Pipeline Overlap..." << std::endl;

        CudaDecoderPipelinePhase2D pipeline2d;
        CudaDecoderPipelinePhase2E pipeline2e;

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

                if (!pipeline2d.Initialize(w, h, 3, 10, 0)) {
                    throw std::runtime_error("failed to initialize CUDA Phase 2D pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }

                if (!pipeline2e.Initialize(w, h, 3, 10, 0, 4)) {
                    throw std::runtime_error("failed to initialize CUDA Phase 2E pipeline for " + std::to_string(w) + "x" + std::to_string(h));
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

            // 2. Phase 2D baseline decode
            uint64_t ticket_2d = pipeline2d.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2d == 0 || !pipeline2d.WaitForDecode(ticket_2d)) {
                throw std::runtime_error("Phase 2D QueueDecode/WaitForDecode failed");
            }

            Phase2dSlotStageTimings timings2d{};
            pipeline2d.GetStageTimings(ticket_2d, &timings2d);

            // 3. Phase 2E Variant A: Entropy Overlap (Inverse barrier retained)
            pipeline2e.SetPipelineMode(Phase2EPipelineMode::Phase2EEntropyOverlap);
            pipeline2e.SetSchedulingPolicy(Phase2ESchedulingPolicy::PolicyA_LargestFirst);
            uint64_t ticket_2e_a = pipeline2e.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2e_a == 0 || !pipeline2e.WaitForDecode(ticket_2e_a)) {
                throw std::runtime_error("Phase 2E Variant A QueueDecode/WaitForDecode failed");
            }

            // 4. Phase 2E Variant B: Full Overlap (Host index + GPU replay + Inverse DAG)
            pipeline2e.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
            uint64_t ticket_2e_b = pipeline2e.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2e_b == 0 || !pipeline2e.WaitForDecode(ticket_2e_b)) {
                throw std::runtime_error("Phase 2E Variant B QueueDecode/WaitForDecode failed");
            }

            // 5. Verify all 30 coefficient bands in GPU VRAM across:
            //    CPU reference vs Phase 2D vs Phase 2E-A vs Phase 2E-B
            for (int b = 0; b < 30; ++b) {
                std::vector<int16_t> gpu_2d_coeffs, gpu_2e_a_coeffs, gpu_2e_b_coeffs;
                int w_2d = 0, h_2d = 0, p_2d = 0;
                int w_2e_a = 0, h_2e_a = 0, p_2e_a = 0;
                int w_2e_b = 0, h_2e_b = 0, p_2e_b = 0;

                if (!pipeline2d.DownloadBandCoefficientsToHost(ticket_2d, b, gpu_2d_coeffs, &w_2d, &h_2d, &p_2d)) {
                    throw std::runtime_error("Phase 2D DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }
                if (!pipeline2e.DownloadBandCoefficientsToHost(ticket_2e_a, b, gpu_2e_a_coeffs, &w_2e_a, &h_2e_a, &p_2e_a)) {
                    throw std::runtime_error("Phase 2E-A DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }
                if (!pipeline2e.DownloadBandCoefficientsToHost(ticket_2e_b, b, gpu_2e_b_coeffs, &w_2e_b, &h_2e_b, &p_2e_b)) {
                    throw std::runtime_error("Phase 2E-B DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                const auto& ref_b = cpu_bands[b];
                if (w_2e_b != ref_b.width || h_2e_b != ref_b.height || p_2e_b != ref_b.pitch ||
                    w_2e_a != ref_b.width || h_2e_a != ref_b.height || p_2e_a != ref_b.pitch ||
                    w_2d != ref_b.width || h_2d != ref_b.height || p_2d != ref_b.pitch) {
                    throw std::runtime_error("Geometry mismatch for band " + std::to_string(b));
                }

                size_t pitch_samples = p_2e_b / sizeof(int16_t);
                for (int y = 0; y < h_2e_b; ++y) {
                    const int16_t* ref_row = ref_b.host_data + y * (ref_b.pitch / sizeof(int16_t));
                    const int16_t* row_2d = gpu_2d_coeffs.data() + y * pitch_samples;
                    const int16_t* row_2e_a = gpu_2e_a_coeffs.data() + y * pitch_samples;
                    const int16_t* row_2e_b = gpu_2e_b_coeffs.data() + y * pitch_samples;

                    for (int x = 0; x < w_2e_b; ++x) {
                        if (row_2e_b[x] != ref_row[x] || row_2e_b[x] != row_2e_a[x] || row_2e_b[x] != row_2d[x]) {
                            throw std::runtime_error("Coefficient mismatch on band " + std::to_string(b) +
                                                     " at (" + std::to_string(x) + "," + std::to_string(y) +
                                                     "): CPU=" + std::to_string(ref_row[x]) +
                                                     " 2D=" + std::to_string(row_2d[x]) +
                                                     " 2E-A=" + std::to_string(row_2e_a[x]) +
                                                     " 2E-B=" + std::to_string(row_2e_b[x]));
                        }
                    }
                }
            }

            // 6. Validate Phase 2E BGRA surface output
            DeviceBgraSurfaceDescriptor bgra_desc{};
            if (!pipeline2e.GetDeviceBgraOutput(ticket_2e_b, &bgra_desc)) {
                throw std::runtime_error("GetDeviceBgraOutput failed");
            }

            int bgra_pitch = ref_bgra.pitch;
            std::vector<uint8_t> host_bgra(bgra_pitch * h, 0);
            if (!pipeline2e.DownloadBgraToHost(ticket_2e_b, host_bgra.data(), bgra_pitch)) {
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
            Phase2eSlotStageTimings timings2e_a{}, timings2e_b{};
            pipeline2e.GetStageTimings(ticket_2e_a, &timings2e_a);
            pipeline2e.GetStageTimings(ticket_2e_b, &timings2e_b);

            // 7. Test Planar YUV422 DeviceOutput Path under Phase 2E
            ReferenceDecoder ref_yu64(s, CFHD_PIXEL_FORMAT_YU64);
            ref_yu64.Decode(s);
            auto* d_yu64 = ref_yu64.GetNative();
            auto cpu_planar_ref = ComputeCpuReferenceIntermediates(d_yu64);

            uint64_t ticket_planar = pipeline2e.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::PlanarYUV422_16S,
                ColorMatrix::BT709,
                ColorRange::CG
            );

            if (ticket_planar == 0 || !pipeline2e.WaitForDecode(ticket_planar)) {
                throw std::runtime_error("Phase 2E QueueDecode/WaitForDecode (Planar) failed");
            }

            int y_w = w, y_h = h;
            int c_w = w / 2, c_h = h;
            std::vector<int16_t> host_y(y_w * y_h, 0);
            std::vector<int16_t> host_v(c_w * c_h, 0);
            std::vector<int16_t> host_u(c_w * c_h, 0);

            if (!pipeline2e.DownloadPlanarToHost(
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
                throw std::runtime_error("Planar channel mismatch in Phase 2E");
            }

            pipeline2e.ReleaseOutput(ticket_planar);
            pipeline2d.ReleaseOutput(ticket_2d);
            pipeline2e.ReleaseOutput(ticket_2e_a);
            pipeline2e.ReleaseOutput(ticket_2e_b);

            count++;
            std::cout << "  [PASS] " << std::setw(5) << w << "x" << std::setw(4) << h
                      << " " << std::setw(8) << PatternName(p)
                      << " | 30/30 Coeffs: EXACT"
                      << " | 2D: " << std::fixed << std::setprecision(2) << std::setw(5) << timings2d.total_decode_ms << " ms"
                      << " | 2E-A: " << std::setw(5) << timings2e_a.total_critical_path_ms << " ms"
                      << " | 2E-B: " << std::setw(5) << timings2e_b.total_critical_path_ms << " ms"
                      << " | Overlap Eff: " << std::setw(5) << (timings2e_b.overlap_efficiency * 100.0f) << "%"
                      << std::endl;
        }

        std::cout << "\nAll " << count << " Golden Test Vectors PASSED with 100% Bit-Exact Accuracy!" << std::endl;

        // 8. Synthetic Race Tests (Section 36)
        std::cout << "\n=== Synthetic Race & Completion Reordering Tests (Section 36) ===" << std::endl;
        {
            auto noise_1080p = Generate(1920, 1080, 5)[2];

            // Establish baseline hash without delays
            pipeline2e.SetTestDelayHook(nullptr);
            pipeline2e.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
            uint64_t t_base = pipeline2e.QueueDecode(noise_1080p.compressed.data(), noise_1080p.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2e.WaitForDecode(t_base);
            std::vector<uint8_t> base_bgra(1920 * 4 * 1080);
            pipeline2e.DownloadBgraToHost(t_base, base_bgra.data(), 1920 * 4);
            pipeline2e.ReleaseOutput(t_base);
            std::string baseline_hash = Hash(base_bgra.data(), base_bgra.size());

            // Case A: Delay one Y band (channel 0, level 2, band 1)
            pipeline2e.SetTestDelayHook([](int, int ch, int lvl, int b) {
                if (ch == 0 && lvl == 2 && b == 1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
            uint64_t t_d1 = pipeline2e.QueueDecode(noise_1080p.compressed.data(), noise_1080p.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2e.WaitForDecode(t_d1);
            std::vector<uint8_t> d1_bgra(1920 * 4 * 1080);
            pipeline2e.DownloadBgraToHost(t_d1, d1_bgra.data(), 1920 * 4);
            pipeline2e.ReleaseOutput(t_d1);
            if (Hash(d1_bgra.data(), d1_bgra.size()) != baseline_hash) {
                throw std::runtime_error("Synthetic race failure: Delay Y band output mismatch");
            }
            std::cout << "  [PASS] Delay Y L2 band: bit-exact match!" << std::endl;

            // Case B: Delay one U band (channel 2, level 2, band 2)
            pipeline2e.SetTestDelayHook([](int, int ch, int lvl, int b) {
                if (ch == 2 && lvl == 2 && b == 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
            uint64_t t_d2 = pipeline2e.QueueDecode(noise_1080p.compressed.data(), noise_1080p.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2e.WaitForDecode(t_d2);
            std::vector<uint8_t> d2_bgra(1920 * 4 * 1080);
            pipeline2e.DownloadBgraToHost(t_d2, d2_bgra.data(), 1920 * 4);
            pipeline2e.ReleaseOutput(t_d2);
            if (Hash(d2_bgra.data(), d2_bgra.size()) != baseline_hash) {
                throw std::runtime_error("Synthetic race failure: Delay U band output mismatch");
            }
            std::cout << "  [PASS] Delay U L2 band: bit-exact match!" << std::endl;

            // Case C: Delay one V band (channel 1, level 1, band 3)
            pipeline2e.SetTestDelayHook([](int, int ch, int lvl, int b) {
                if (ch == 1 && lvl == 1 && b == 3) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
            uint64_t t_d3 = pipeline2e.QueueDecode(noise_1080p.compressed.data(), noise_1080p.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2e.WaitForDecode(t_d3);
            std::vector<uint8_t> d3_bgra(1920 * 4 * 1080);
            pipeline2e.DownloadBgraToHost(t_d3, d3_bgra.data(), 1920 * 4);
            pipeline2e.ReleaseOutput(t_d3);
            if (Hash(d3_bgra.data(), d3_bgra.size()) != baseline_hash) {
                throw std::runtime_error("Synthetic race failure: Delay V band output mismatch");
            }
            std::cout << "  [PASS] Delay V L1 band: bit-exact match!" << std::endl;

            // Case D: Chroma completes before Luma (delay all Y bands by 5ms)
            pipeline2e.SetTestDelayHook([](int, int ch, int, int) {
                if (ch == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            });
            uint64_t t_d4 = pipeline2e.QueueDecode(noise_1080p.compressed.data(), noise_1080p.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2e.WaitForDecode(t_d4);
            std::vector<uint8_t> d4_bgra(1920 * 4 * 1080);
            pipeline2e.DownloadBgraToHost(t_d4, d4_bgra.data(), 1920 * 4);
            pipeline2e.ReleaseOutput(t_d4);
            if (Hash(d4_bgra.data(), d4_bgra.size()) != baseline_hash) {
                throw std::runtime_error("Synthetic race failure: Chroma before Luma output mismatch");
            }
            std::cout << "  [PASS] Chroma finishes before Luma: bit-exact match!" << std::endl;

            // Case E: Luma completes before Chroma (delay all U and V bands by 5ms)
            pipeline2e.SetTestDelayHook([](int, int ch, int, int) {
                if (ch != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            });
            uint64_t t_d5 = pipeline2e.QueueDecode(noise_1080p.compressed.data(), noise_1080p.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2e.WaitForDecode(t_d5);
            std::vector<uint8_t> d5_bgra(1920 * 4 * 1080);
            pipeline2e.DownloadBgraToHost(t_d5, d5_bgra.data(), 1920 * 4);
            pipeline2e.ReleaseOutput(t_d5);
            if (Hash(d5_bgra.data(), d5_bgra.size()) != baseline_hash) {
                throw std::runtime_error("Synthetic race failure: Luma before Chroma output mismatch");
            }
            std::cout << "  [PASS] Luma finishes before Chroma: bit-exact match!" << std::endl;

            pipeline2e.SetTestDelayHook(nullptr); // clear hook
            std::cout << "All Synthetic Race & Reordering Tests PASSED!" << std::endl;
        }

        // 9. Scheduling Policies Comparison & Exactness (Section 12, 13)
        std::cout << "\n=== Scheduling Policy Verification (Section 12 & 13) ===" << std::endl;
        {
            auto noise_4k = Generate(3840, 2160, 5)[2];

            std::vector<std::pair<std::string, Phase2ESchedulingPolicy>> policies = {
                {"Policy A (LargestFirst)", Phase2ESchedulingPolicy::PolicyA_LargestFirst},
                {"Policy B (InverseCritical)", Phase2ESchedulingPolicy::PolicyB_InverseCritical},
                {"Policy C (Hybrid)", Phase2ESchedulingPolicy::PolicyC_Hybrid},
                {"DescriptorOrder (FIFO)", Phase2ESchedulingPolicy::DescriptorOrder}
            };

            std::string baseline_hash;
            for (const auto& pol : policies) {
                pipeline2e.SetSchedulingPolicy(pol.second);
                uint64_t t = pipeline2e.QueueDecode(noise_4k.compressed.data(), noise_4k.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline2e.WaitForDecode(t);
                std::vector<uint8_t> bgra(3840 * 4 * 2160);
                pipeline2e.DownloadBgraToHost(t, bgra.data(), 3840 * 4);
                Phase2eSlotStageTimings tim{};
                pipeline2e.GetStageTimings(t, &tim);
                pipeline2e.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (baseline_hash.empty()) baseline_hash = h;
                else if (h != baseline_hash) {
                    throw std::runtime_error("Scheduling policy hash mismatch: " + pol.first);
                }

                std::cout << "  " << std::setw(26) << pol.first
                          << " | Index: " << std::fixed << std::setprecision(2) << std::setw(5) << tim.host_index_wall_ms << " ms"
                          << " | 1st GPU: " << std::setw(5) << tim.t_first_gpu_launch_ms << " ms"
                          << " | 1st Inv: " << std::setw(5) << tim.t_first_inverse_start_ms << " ms"
                          << " | Total: " << std::setw(5) << tim.total_critical_path_ms << " ms"
                          << " | Exact Hash MATCH" << std::endl;
            }
            pipeline2e.SetSchedulingPolicy(Phase2ESchedulingPolicy::PolicyA_LargestFirst); // restore
        }

        // 10. Stream Pool Size Sweep & Exactness (Section 32)
        std::cout << "\n=== Stream Pool Size Sweep (Section 32) ===" << std::endl;
        {
            auto noise_4k = Generate(3840, 2160, 5)[2];
            std::vector<int> stream_counts = {1, 2, 4, 8};
            std::string baseline_hash;

            for (int sc : stream_counts) {
                pipeline2e.SetReplayStreamCount(sc);
                uint64_t t = pipeline2e.QueueDecode(noise_4k.compressed.data(), noise_4k.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline2e.WaitForDecode(t);
                std::vector<uint8_t> bgra(3840 * 4 * 2160);
                pipeline2e.DownloadBgraToHost(t, bgra.data(), 3840 * 4);
                Phase2eSlotStageTimings tim{};
                pipeline2e.GetStageTimings(t, &tim);
                pipeline2e.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (baseline_hash.empty()) baseline_hash = h;
                else if (h != baseline_hash) {
                    throw std::runtime_error("Stream count output hash mismatch for streams=" + std::to_string(sc));
                }

                std::cout << "  Streams=" << sc
                          << " | Replay Wall: " << std::fixed << std::setprecision(2) << std::setw(5) << tim.gpu_replay_wall_ms << " ms"
                          << " | Total: " << std::setw(5) << tim.total_critical_path_ms << " ms"
                          << " | Overlap Eff: " << std::setw(5) << (tim.overlap_efficiency * 100.0f) << "%"
                          << " | Exact Hash MATCH" << std::endl;
            }
            pipeline2e.SetReplayStreamCount(4); // restore default
        }

        // 11. Malformed Input Under Overlap (Section 37)
        std::cout << "\n=== Malformed Input & Error Propagation Verification (Section 37) ===" << std::endl;
        {
            auto noise_sample = Generate(1920, 1080, 5)[2];
            auto corrupt = noise_sample.compressed;

            // Test A: Truncated bitstream
            corrupt.resize(corrupt.size() / 2);
            uint64_t t_trunc = pipeline2e.QueueDecode(corrupt.data(), corrupt.size(), DeviceSurfaceType::BGRA8);
            if (t_trunc != 0) {
                DecodeTicketStatus st = pipeline2e.PollDecode(t_trunc);
                if (st != DecodeTicketStatus::Failed) {
                    pipeline2e.WaitForDecode(t_trunc);
                    st = pipeline2e.PollDecode(t_trunc);
                }
                pipeline2e.ReleaseOutput(t_trunc);
                if (st != DecodeTicketStatus::Failed) {
                    throw std::runtime_error("Expected failure status for truncated payload");
                }
            }
            std::cout << "  [PASS] Truncated bitstream safely failed without crash or partial publication." << std::endl;

            // Test B: Corrupted tag length
            corrupt = noise_sample.compressed;
            if (corrupt.size() > 64) {
                corrupt[32] = 0xFF;
                corrupt[33] = 0xFF;
                corrupt[34] = 0xFF;
                corrupt[35] = 0xFF;
                uint64_t t_bad = pipeline2e.QueueDecode(corrupt.data(), corrupt.size(), DeviceSurfaceType::BGRA8);
                if (t_bad != 0) {
                    pipeline2e.WaitForDecode(t_bad);
                    pipeline2e.ReleaseOutput(t_bad);
                }
            }
            std::cout << "  [PASS] Corrupted tag structure handled safely." << std::endl;
        }

        // 12. Determinism Stress Testing (Section 35)
        std::cout << "\n=== Determinism Stress Testing (Section 35) ===" << std::endl;
        {
            std::cout << "Testing 1080p Noise Determinism (200 repeats)... ";
            auto noise_1080p = Generate(1920, 1080, 5)[2];
            std::string first_hash;

            for (int r = 0; r < 200; ++r) {
                uint64_t t = pipeline2e.QueueDecode(
                    noise_1080p.compressed.data(),
                    noise_1080p.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2e.WaitForDecode(t);
                std::vector<uint8_t> bgra(1920 * 4 * 1080);
                pipeline2e.DownloadBgraToHost(t, bgra.data(), 1920 * 4);
                pipeline2e.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (r == 0) first_hash = h;
                else if (h != first_hash) {
                    throw std::runtime_error("1080p noise nondeterminism on iteration " + std::to_string(r));
                }
            }
            std::cout << "PASSED (200/200 matches: " << first_hash << ")" << std::endl;
        }

        {
            std::cout << "Testing 4K Noise Determinism (100 repeats)... ";
            auto noise_4k = Generate(3840, 2160, 5)[2];
            std::string first_hash;

            for (int r = 0; r < 100; ++r) {
                uint64_t t = pipeline2e.QueueDecode(
                    noise_4k.compressed.data(),
                    noise_4k.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2e.WaitForDecode(t);
                std::vector<uint8_t> bgra(3840 * 4 * 2160);
                pipeline2e.DownloadBgraToHost(t, bgra.data(), 3840 * 4);
                pipeline2e.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (r == 0) first_hash = h;
                else if (h != first_hash) {
                    throw std::runtime_error("4K noise nondeterminism on iteration " + std::to_string(r));
                }
            }
            std::cout << "PASSED (100/100 matches: " << first_hash << ")" << std::endl;
        }

        {
            std::cout << "Testing 8K Flat Determinism (20 repeats)... ";
            auto flat_8k = Generate(7680, 4320, 1)[0];
            std::string first_hash;

            for (int r = 0; r < 20; ++r) {
                uint64_t t = pipeline2e.QueueDecode(
                    flat_8k.compressed.data(),
                    flat_8k.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2e.WaitForDecode(t);
                std::vector<uint8_t> bgra(7680 * 4 * 4320);
                pipeline2e.DownloadBgraToHost(t, bgra.data(), 7680 * 4);
                pipeline2e.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (r == 0) first_hash = h;
                else if (h != first_hash) {
                    throw std::runtime_error("8K flat nondeterminism on iteration " + std::to_string(r));
                }
            }
            std::cout << "PASSED (20/20 matches: " << first_hash << ")" << std::endl;
        }

        std::cout << "\nAll Phase 2E Determinism, Stress, and Race Tests PASSED successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Verification failed: " << e.what() << std::endl;
        return 1;
    }
}
