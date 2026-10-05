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
#include "hypercine/decode_phase2d.h"
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
        size_t total_checkpoints_compared = 0;
        std::string line;

        std::cout << "=== HyperCine Decode Phase 2D Verification ===" << std::endl;
        std::cout << "Testing Parallel Host Checkpoint Indexing & Scheduling..." << std::endl;

        CudaDecoderPipelinePhase2B pipeline2b;
        CudaDecoderPipelinePhase2C pipeline2c;
        CudaDecoderPipelinePhase2D pipeline2d;

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

                if (!pipeline2d.Initialize(w, h, 3, 10, 0)) { // 0: auto worker count
                    throw std::runtime_error("failed to initialize CUDA Phase 2D pipeline for " + std::to_string(w) + "x" + std::to_string(h));
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

            // 3. Phase 2C Parallel GPU decode (Single-Threaded Host Index)
            pipeline2c.SetEntropyMode(GpuEntropyMode::ParallelAdaptive);
            uint64_t ticket_2c = pipeline2c.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2c == 0 || !pipeline2c.WaitForDecode(ticket_2c)) {
                throw std::runtime_error("Phase 2C QueueDecode/WaitForDecode failed");
            }

            // 4. Phase 2D Parallel GPU decode (Parallel Host Indexing across persistent workers)
            pipeline2d.SetEntropyMode(GpuEntropyMode::ParallelAdaptive);
            pipeline2d.SetHostIndexMode(HostIndexExecutionMode::ParallelWorkerPool);
            pipeline2d.SetSchedulingPolicy(HostIndexSchedulingPolicy::LargestFirst);

            uint64_t ticket_2d_bgra = pipeline2d.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2d_bgra == 0 || !pipeline2d.WaitForDecode(ticket_2d_bgra)) {
                throw std::runtime_error("Phase 2D QueueDecode/WaitForDecode (BGRA) failed");
            }

            // 5. Verify all 30 coefficient bands in GPU VRAM across:
            //    CPU reference vs Phase 2B vs Phase 2C vs Phase 2D
            for (int b = 0; b < 30; ++b) {
                std::vector<int16_t> gpu_2b_coeffs, gpu_2c_coeffs, gpu_2d_coeffs;
                int w_2b = 0, h_2b = 0, p_2b = 0;
                int w_2c = 0, h_2c = 0, p_2c = 0;
                int w_2d = 0, h_2d = 0, p_2d = 0;

                if (!pipeline2b.DownloadBandCoefficientsToHost(ticket_2b, b, gpu_2b_coeffs, &w_2b, &h_2b, &p_2b)) {
                    throw std::runtime_error("Phase 2B DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                if (!pipeline2c.DownloadBandCoefficientsToHost(ticket_2c, b, gpu_2c_coeffs, &w_2c, &h_2c, &p_2c)) {
                    throw std::runtime_error("Phase 2C DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                if (!pipeline2d.DownloadBandCoefficientsToHost(ticket_2d_bgra, b, gpu_2d_coeffs, &w_2d, &h_2d, &p_2d)) {
                    throw std::runtime_error("Phase 2D DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                const auto& ref_b = cpu_bands[b];
                if (w_2d != ref_b.width || h_2d != ref_b.height || p_2d != ref_b.pitch ||
                    w_2c != ref_b.width || h_2c != ref_b.height || p_2c != ref_b.pitch ||
                    w_2b != ref_b.width || h_2b != ref_b.height || p_2b != ref_b.pitch) {
                    throw std::runtime_error("Geometry mismatch for band " + std::to_string(b));
                }

                size_t pitch_samples = p_2d / sizeof(int16_t);
                for (int y = 0; y < h_2d; ++y) {
                    const int16_t* ref_row = ref_b.host_data + y * (ref_b.pitch / sizeof(int16_t));
                    const int16_t* row_2b = gpu_2b_coeffs.data() + y * pitch_samples;
                    const int16_t* row_2c = gpu_2c_coeffs.data() + y * pitch_samples;
                    const int16_t* row_2d = gpu_2d_coeffs.data() + y * pitch_samples;

                    for (int x = 0; x < w_2d; ++x) {
                        if (row_2d[x] != ref_row[x] || row_2d[x] != row_2c[x] || row_2d[x] != row_2b[x]) {
                            throw std::runtime_error("Quad-way coefficient mismatch on band " + std::to_string(b) +
                                                     " at (" + std::to_string(x) + "," + std::to_string(y) +
                                                     "): CPU=" + std::to_string(ref_row[x]) +
                                                     " 2B=" + std::to_string(row_2b[x]) +
                                                     " 2C=" + std::to_string(row_2c[x]) +
                                                     " 2D=" + std::to_string(row_2d[x]));
                        }
                    }
                }
            }

            // 6. Validate Phase 2D BGRA surface output
            DeviceBgraSurfaceDescriptor bgra_desc{};
            if (!pipeline2d.GetDeviceBgraOutput(ticket_2d_bgra, &bgra_desc)) {
                throw std::runtime_error("GetDeviceBgraOutput failed");
            }

            int bgra_pitch = ref_bgra.pitch;
            std::vector<uint8_t> host_bgra(bgra_pitch * h, 0);
            if (!pipeline2d.DownloadBgraToHost(ticket_2d_bgra, host_bgra.data(), bgra_pitch)) {
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
            Phase2dSlotStageTimings timings2d{};
            if (!pipeline2d.GetStageTimings(ticket_2d_bgra, &timings2d)) {
                throw std::runtime_error("GetStageTimings failed");
            }

            Phase2cSlotStageTimings timings2c{};
            pipeline2c.GetStageTimings(ticket_2c, &timings2c);

            Phase2bSlotStageTimings timings2b{};
            pipeline2b.GetStageTimings(ticket_2b, &timings2b);

            // 7. Validate Canonical Checkpoint Equivalence (Section 12)
            // SingleThreadReference vs ParallelWorkerPool on pipeline2d
            pipeline2d.SetHostIndexMode(HostIndexExecutionMode::SingleThreadReference);
            uint64_t ticket_2d_st = pipeline2d.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8
            );
            if (ticket_2d_st == 0 || !pipeline2d.WaitForDecode(ticket_2d_st)) {
                throw std::runtime_error("SingleThreadReference QueueDecode failed");
            }

            std::vector<GpuSegmentDecodeResult> seg_results_par, seg_results_st;
            pipeline2d.GetSegmentDecodeResults(ticket_2d_bgra, seg_results_par);
            pipeline2d.GetSegmentDecodeResults(ticket_2d_st, seg_results_st);

            if (seg_results_par.size() != seg_results_st.size()) {
                throw std::runtime_error("Segment count mismatch between Parallel and SingleThread indexing");
            }

            std::vector<GpuEntropySegmentDescriptor> ck_par, ck_st;
            if (!pipeline2d.GetHostCheckpoints(ticket_2d_bgra, ck_par) ||
                !pipeline2d.GetHostCheckpoints(ticket_2d_st, ck_st)) {
                throw std::runtime_error("GetHostCheckpoints failed");
            }
            if (!CheckpointsEqual(ck_par, ck_st)) {
                throw std::runtime_error("Canonical checkpoint records differ between Parallel and SingleThread indexing");
            }
            for (size_t k = 0; k < seg_results_par.size(); ++k) {
                const auto& a = seg_results_par[k];
                const auto& b = seg_results_st[k];
                if (a.status != b.status || a.consumed_bytes != b.consumed_bytes ||
                    a.final_fsm_state != b.final_fsm_state || a.final_coeff_pos != b.final_coeff_pos ||
                    a.nonzero_count != b.nonzero_count) {
                    throw std::runtime_error("Per-segment GPU replay result mismatch at segment " + std::to_string(k));
                }
            }
            total_checkpoints_compared += ck_par.size();

            pipeline2d.ReleaseOutput(ticket_2d_st);
            pipeline2b.ReleaseOutput(ticket_2b);
            pipeline2c.ReleaseOutput(ticket_2c);
            pipeline2d.ReleaseOutput(ticket_2d_bgra);

            // Restore parallel mode for subsequent tests
            pipeline2d.SetHostIndexMode(HostIndexExecutionMode::ParallelWorkerPool);

            // 8. Test Planar YUV422 DeviceOutput Path under Phase 2D
            ReferenceDecoder ref_yu64(s, CFHD_PIXEL_FORMAT_YU64);
            ref_yu64.Decode(s);
            auto* d_yu64 = ref_yu64.GetNative();
            auto cpu_planar_ref = ComputeCpuReferenceIntermediates(d_yu64);

            uint64_t ticket_planar = pipeline2d.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::PlanarYUV422_16S,
                ColorMatrix::BT709,
                ColorRange::CG
            );

            if (ticket_planar == 0 || !pipeline2d.WaitForDecode(ticket_planar)) {
                throw std::runtime_error("Phase 2D QueueDecode/WaitForDecode (Planar) failed");
            }

            int y_w = w, y_h = h;
            int c_w = w / 2, c_h = h;
            std::vector<int16_t> host_y(y_w * y_h, 0);
            std::vector<int16_t> host_v(c_w * c_h, 0);
            std::vector<int16_t> host_u(c_w * c_h, 0);

            if (!pipeline2d.DownloadPlanarToHost(
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
                throw std::runtime_error("Planar channel mismatch in Phase 2D");
            }

            pipeline2d.ReleaseOutput(ticket_planar);

            count++;
            float index_speedup = (timings2d.host_index_ms > 0.0f) ? (timings2c.host_index_ms / timings2d.host_index_ms) : 1.0f;
            std::cout << "  [PASS] " << std::setw(5) << w << "x" << std::setw(4) << h
                      << " " << std::setw(8) << PatternName(p)
                      << " | 30/30 Coeffs: EXACT"
                      << " | 2C Index: " << std::fixed << std::setprecision(2) << std::setw(5) << timings2c.host_index_ms << " ms"
                      << " | 2D Index: " << std::setw(5) << timings2d.host_index_ms << " ms"
                      << " | Speedup: " << std::setw(4) << index_speedup << "x"
                      << " | 2D Total: " << std::setw(6) << timings2d.total_decode_ms << " ms"
                      << std::endl;
        }

        std::cout << "\nAll " << count << " Golden Test Vectors PASSED with 100% Bit-Exact Accuracy!" << std::endl;

        // 9. Scheduling Strategy Equivalence (Section 8 & 23)
        std::cout << "\n=== Scheduling Strategy Verification (Section 8 & 23) ===" << std::endl;
        {
            auto noise_4k = Generate(3840, 2160, 5)[2];

            pipeline2d.SetSchedulingPolicy(HostIndexSchedulingPolicy::DescriptorOrder);
            uint64_t t_fifo = pipeline2d.QueueDecode(noise_4k.compressed.data(), noise_4k.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2d.WaitForDecode(t_fifo);
            std::vector<uint8_t> bgra_fifo(3840 * 4 * 2160);
            pipeline2d.DownloadBgraToHost(t_fifo, bgra_fifo.data(), 3840 * 4);
            Phase2dSlotStageTimings tim_fifo{};
            pipeline2d.GetStageTimings(t_fifo, &tim_fifo);
            std::vector<GpuEntropySegmentDescriptor> ck_fifo;
            pipeline2d.GetHostCheckpoints(t_fifo, ck_fifo);
            pipeline2d.ReleaseOutput(t_fifo);

            pipeline2d.SetSchedulingPolicy(HostIndexSchedulingPolicy::LargestFirst);
            uint64_t t_lpt = pipeline2d.QueueDecode(noise_4k.compressed.data(), noise_4k.compressed.size(), DeviceSurfaceType::BGRA8);
            pipeline2d.WaitForDecode(t_lpt);
            std::vector<uint8_t> bgra_lpt(3840 * 4 * 2160);
            pipeline2d.DownloadBgraToHost(t_lpt, bgra_lpt.data(), 3840 * 4);
            Phase2dSlotStageTimings tim_lpt{};
            pipeline2d.GetStageTimings(t_lpt, &tim_lpt);
            std::vector<GpuEntropySegmentDescriptor> ck_lpt;
            pipeline2d.GetHostCheckpoints(t_lpt, ck_lpt);
            pipeline2d.ReleaseOutput(t_lpt);

            if (bgra_fifo != bgra_lpt) {
                throw std::runtime_error("Scheduling policy mismatch: DescriptorOrder != LargestFirst");
            }
            if (!CheckpointsEqual(ck_fifo, ck_lpt)) {
                throw std::runtime_error("Canonical checkpoints mismatch between FIFO and LargestFirst scheduling");
            }
            std::cout << "Scheduling comparison on 4K noise: FIFO=" << tim_fifo.host_index_ms << " ms, LargestFirst="
                      << tim_lpt.host_index_ms << " ms | Checkpoints & Hashes MATCH: PASSED!" << std::endl;
        }

        // 10. Worker Scaling Equivalence (Section 6 & 22)
        std::cout << "\n=== Worker Scaling Equivalence Verification (Section 6 & 22) ===" << std::endl;
        {
            auto noise_4k = Generate(3840, 2160, 5)[2];
            std::string baseline_hash;
            std::vector<GpuEntropySegmentDescriptor> baseline_ck;
            std::vector<int> worker_counts = {1, 2, 4, 8, 12, 16};

            for (int wc : worker_counts) {
                pipeline2d.SetWorkerCount(wc);
                uint64_t t = pipeline2d.QueueDecode(noise_4k.compressed.data(), noise_4k.compressed.size(), DeviceSurfaceType::BGRA8);
                pipeline2d.WaitForDecode(t);
                std::vector<uint8_t> bgra(3840 * 4 * 2160);
                pipeline2d.DownloadBgraToHost(t, bgra.data(), 3840 * 4);
                Phase2dSlotStageTimings tim{};
                pipeline2d.GetStageTimings(t, &tim);
                std::vector<GpuEntropySegmentDescriptor> cur_ck;
                pipeline2d.GetHostCheckpoints(t, cur_ck);
                pipeline2d.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (baseline_hash.empty()) {
                    baseline_hash = h;
                    baseline_ck = cur_ck;
                } else {
                    if (h != baseline_hash) {
                        throw std::runtime_error("Worker scaling output hash mismatch for workers=" + std::to_string(wc));
                    }
                    if (!CheckpointsEqual(baseline_ck, cur_ck)) {
                        throw std::runtime_error("Worker scaling checkpoint mismatch for workers=" + std::to_string(wc));
                    }
                }
                std::cout << "  Workers=" << std::setw(2) << wc
                          << " | Index: " << std::fixed << std::setprecision(2) << std::setw(5) << tim.host_index_ms << " ms"
                          << " | Total: " << std::setw(6) << tim.total_decode_ms << " ms"
                          << " | Checkpoints & Hash MATCH" << std::endl;
            }
            pipeline2d.SetWorkerCount(0); // restore default
            std::cout << "Worker scaling equivalence PASSED!" << std::endl;
        }

        // 11. Malformed Input & Error Propagation (Section 29 & 30)
        std::cout << "\n=== Malformed Input & Error Propagation Verification (Section 29 & 30) ===" << std::endl;
        {
            auto noise_sample = Generate(1920, 1080, 5)[2];
            auto corrupt = noise_sample.compressed;

            // Test A: Truncated bitstream
            corrupt.resize(corrupt.size() / 2);
            uint64_t t_trunc = pipeline2d.QueueDecode(corrupt.data(), corrupt.size(), DeviceSurfaceType::BGRA8);
            if (t_trunc != 0) {
                DecodeTicketStatus st = pipeline2d.PollDecode(t_trunc);
                if (st != DecodeTicketStatus::Failed) {
                    pipeline2d.WaitForDecode(t_trunc);
                    st = pipeline2d.PollDecode(t_trunc);
                }
                pipeline2d.ReleaseOutput(t_trunc);
                if (st != DecodeTicketStatus::Failed) {
                    throw std::runtime_error("Expected failure status for truncated payload");
                }
            }
            std::cout << "  [PASS] Truncated bitstream safely failed without crash or partial publication." << std::endl;

            // Test B: Corrupted tag length
            corrupt = noise_sample.compressed;
            if (corrupt.size() > 64) {
                corrupt[32] = 0xFF; // corrupt internal tag bytes
                corrupt[33] = 0xFF;
                corrupt[34] = 0xFF;
                corrupt[35] = 0xFF;
                uint64_t t_bad = pipeline2d.QueueDecode(corrupt.data(), corrupt.size(), DeviceSurfaceType::BGRA8);
                if (t_bad != 0) {
                    pipeline2d.WaitForDecode(t_bad);
                    pipeline2d.ReleaseOutput(t_bad);
                }
            }
            std::cout << "  [PASS] Corrupted tag structure handled safely." << std::endl;
        }

        // 12. Determinism Stress Testing (Section 27)
        std::cout << "\n=== Determinism Stress Testing (Section 27) ===" << std::endl;
        {
            std::cout << "Testing 1080p Noise Determinism (200 repeats)... ";
            auto noise_1080p = Generate(1920, 1080, 5)[2];
            std::string first_hash;

            for (int r = 0; r < 200; ++r) {
                uint64_t t = pipeline2d.QueueDecode(
                    noise_1080p.compressed.data(),
                    noise_1080p.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2d.WaitForDecode(t);
                std::vector<uint8_t> bgra(1920 * 4 * 1080);
                pipeline2d.DownloadBgraToHost(t, bgra.data(), 1920 * 4);
                pipeline2d.ReleaseOutput(t);

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
                uint64_t t = pipeline2d.QueueDecode(
                    noise_4k.compressed.data(),
                    noise_4k.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2d.WaitForDecode(t);
                std::vector<uint8_t> bgra(3840 * 4 * 2160);
                pipeline2d.DownloadBgraToHost(t, bgra.data(), 3840 * 4);
                pipeline2d.ReleaseOutput(t);

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
                uint64_t t = pipeline2d.QueueDecode(
                    flat_8k.compressed.data(),
                    flat_8k.compressed.size(),
                    DeviceSurfaceType::BGRA8
                );
                pipeline2d.WaitForDecode(t);
                std::vector<uint8_t> bgra(7680 * 4 * 4320);
                pipeline2d.DownloadBgraToHost(t, bgra.data(), 7680 * 4);
                pipeline2d.ReleaseOutput(t);

                std::string h = Hash(bgra.data(), bgra.size());
                if (r == 0) first_hash = h;
                else if (h != first_hash) {
                    throw std::runtime_error("8K flat nondeterminism on iteration " + std::to_string(r));
                }
            }
            std::cout << "PASSED (20/20 matches: " << first_hash << ")" << std::endl;
        }

        std::cout << "\nAll Phase 2D Determinism and Stress Tests PASSED successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Verification failed: " << e.what() << std::endl;
        return 1;
    }
}
