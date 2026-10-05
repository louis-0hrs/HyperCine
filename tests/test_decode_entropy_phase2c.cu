// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_entropy_cuda.h"
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <stdexcept>
#include <cstring>
#include <cmath>

namespace HyperCine {
bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>& out_tables);
}

using namespace HyperCine;

namespace {

struct CpuFsmSimResult {
    std::vector<int16_t> coeffs;
    std::vector<DecodedSymbolDiagnostic> symbols;
    GpuBandDecodeStatus status;
    uint32_t consumed_bytes;
    uint16_t final_state;
    uint32_t final_out_pos;
    uint32_t nonzero_count;
};

CpuFsmSimResult SimulateCpuFsm(
    const uint8_t* payload,
    size_t payload_bytes,
    const std::vector<GpuFsmEntry>& table,
    size_t codebook_base,
    size_t total_samples,
    int quant
) {
    CpuFsmSimResult res{};
    res.coeffs.resize(total_samples, 0);
    res.status = GpuBandDecodeStatus::Complete;

    uint32_t byte_idx = 0;
    uint32_t out_pos = 0;
    uint16_t state = 0;
    bool terminated = false;

    while (byte_idx < payload_bytes && !terminated) {
        uint8_t byte = payload[byte_idx++];

        for (int nibble_idx = 0; nibble_idx < 2; ++nibble_idx) {
            uint8_t nibble = (nibble_idx == 0) ? (byte >> 4) : (byte & 0x0f);
            size_t entry_idx = codebook_base + state * 16 + nibble;
            const auto& entry = table[entry_idx];

            if (entry.value0 == 32767) {
                terminated = true;
                break;
            }

            uint32_t pre = entry.pre_post_skip & 0x1ff;
            uint32_t post = (entry.pre_post_skip >> 12) & 0x7;

            DecodedSymbolDiagnostic diag{};
            diag.nibble_index = static_cast<uint16_t>((byte_idx - 1) * 2 + nibble_idx);
            diag.state_before = state;
            diag.state_after = entry.next_state;
            diag.pre_skip = static_cast<uint16_t>(pre);
            diag.post_skip = static_cast<uint16_t>(post);
            diag.raw_val0 = entry.value0;
            diag.raw_val1 = entry.value1;
            diag.out_pos = out_pos + pre;
            res.symbols.push_back(diag);

            if (out_pos + pre + (entry.value1 ? 2 : (entry.value0 ? 1 : 0)) + post > total_samples) {
                res.status = GpuBandDecodeStatus::CoefficientOverflow;
                terminated = true;
                break;
            }

            out_pos += pre;
            if (entry.value0 != 0) {
                res.coeffs[out_pos] = static_cast<int16_t>(entry.value0 * quant);
                res.nonzero_count++;
            }
            if (entry.value1 != 0) {
                res.coeffs[out_pos + 1] = static_cast<int16_t>(entry.value1 * quant);
                res.nonzero_count++;
            }
            out_pos += post;
            state = entry.next_state;
        }
    }

    if (!terminated && res.status == GpuBandDecodeStatus::Complete) {
        res.status = GpuBandDecodeStatus::MissingEndMarker;
    }

    res.consumed_bytes = byte_idx;
    res.final_state = state;
    res.final_out_pos = out_pos;
    return res;
}

std::vector<uint8_t> FindTrailerPath(
    const std::vector<GpuFsmEntry>& table,
    size_t codebook_base,
    uint16_t start_state,
    size_t max_depth = 8
) {
    struct Step {
        uint16_t state;
        std::vector<uint8_t> nibbles;
    };

    std::vector<Step> queue = {{start_state, {}}};
    std::vector<bool> visited(518, false);
    visited[start_state] = true;

    while (!queue.empty()) {
        Step curr = queue.front();
        queue.erase(queue.begin());

        if (curr.nibbles.size() >= max_depth) continue;

        for (uint8_t nib = 0; nib < 16; ++nib) {
            size_t idx = codebook_base + curr.state * 16 + nib;
            const auto& entry = table[idx];

            if (entry.value0 == 32767) {
                auto res = curr.nibbles;
                res.push_back(nib);
                return res;
            }

            if (!visited[entry.next_state]) {
                visited[entry.next_state] = true;
                auto next_nibbles = curr.nibbles;
                next_nibbles.push_back(nib);
                queue.push_back({entry.next_state, next_nibbles});
            }
        }
    }

    return {};
}

std::vector<uint8_t> PackNibbles(const std::vector<uint8_t>& nibbles) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < nibbles.size(); i += 2) {
        uint8_t high = nibbles[i];
        uint8_t low = (i + 1 < nibbles.size()) ? nibbles[i + 1] : 0;
        bytes.push_back(static_cast<uint8_t>((high << 4) | (low & 0x0f)));
    }
    return bytes;
}

void AppendTrailer(
    std::vector<uint8_t>& nibbles,
    const std::vector<GpuFsmEntry>& table,
    size_t codebook_base,
    uint16_t current_state
) {
    auto path = FindTrailerPath(table, codebook_base, current_state);
    if (path.empty()) {
        throw std::runtime_error("no trailer path found from state " + std::to_string(current_state));
    }
    nibbles.insert(nibbles.end(), path.begin(), path.end());
}

} // anonymous namespace

int main() {
    try {
        std::cout << "=== HyperCine Phase 2C Synthetic Parallel-Boundary Test Suite ===" << std::endl;

        std::vector<GpuFsmEntry> host_tables;
        if (!ExtractCineFormBaseFsmTables(host_tables)) {
            throw std::runtime_error("failed to extract base FSM tables");
        }

        GpuFsmTableManager fsm_mgr;
        if (!fsm_mgr.Initialize(0)) {
            throw std::runtime_error("failed to initialize GpuFsmTableManager");
        }

        cudaStream_t stream = nullptr;
        cudaStreamCreate(&stream);

        size_t cs17_base = fsm_mgr.GetCodebookOffset(1);

        // Helper to test parallel decoding on a bitstream
        auto RunParallelEntropyTest = [&](
            const std::string& test_name,
            const std::vector<uint8_t>& payload,
            int width,
            int height,
            int quant,
            bool diff_coding,
            const std::vector<int>& test_segment_counts
        ) {
            std::cout << "Running: " << test_name << " (Payload: " << payload.size() << " bytes)... ";

            size_t total_samples = width * height;
            size_t pitch_bytes = (static_cast<size_t>(width) * sizeof(int16_t) + 15) & ~size_t(15);
            size_t pitch_samples = pitch_bytes / sizeof(int16_t);

            // 1. CPU Reference Simulation
            auto ref_res = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs17_base, total_samples, quant);
            if (diff_coding && ref_res.status == GpuBandDecodeStatus::Complete) {
                for (int y = 0; y < height; ++y) {
                    int16_t* line = ref_res.coeffs.data() + y * width;
                    for (int x = 1; x < width; ++x) {
                        line[x] += line[x - 1];
                    }
                }
            }

            // Allocate device memory
            uint8_t* d_payload = nullptr;
            cudaMalloc(&d_payload, payload.size() + 16);
            cudaMemcpy(d_payload, payload.data(), payload.size(), cudaMemcpyHostToDevice);

            int16_t* d_dst = nullptr;
            cudaMalloc(&d_dst, total_samples * sizeof(int16_t));

            GpuEntropyBandDescriptor desc{};
            desc.payload_offset = 0;
            desc.payload_bytes = static_cast<uint32_t>(payload.size());
            desc.safe_upper_bound = static_cast<uint32_t>(payload.size());
            desc.width = width;
            desc.height = height;
            desc.pitch_bytes = pitch_samples * sizeof(int16_t);
            desc.pitch_samples = pitch_samples;
            desc.coefficient_count = total_samples;
            desc.codebook_id = 1; // cs17
            desc.initial_fsm_state = 0;
            desc.quantization = quant;
            desc.difference_coding = diff_coding;
            desc.d_destination = d_dst;

            GpuEntropyBandDescriptor* d_desc = nullptr;
            cudaMalloc(&d_desc, sizeof(GpuEntropyBandDescriptor));
            cudaMemcpy(d_desc, &desc, sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice);

            // Test across different segment counts
            for (int segs : test_segment_counts) {
                std::vector<GpuEntropySegmentDescriptor> host_segs;
                std::vector<GpuEntropyBandDescriptor> band_vec = {desc};
                GenerateBandCheckpointsCpu(
                    payload.data(),
                    payload.size(),
                    band_vec,
                    host_tables,
                    GpuEntropyMode::ParallelConfigurable,
                    segs,
                    host_segs
                );

                GpuEntropySegmentDescriptor* d_segs = nullptr;
                cudaMalloc(&d_segs, host_segs.size() * sizeof(GpuEntropySegmentDescriptor));
                cudaMemcpy(d_segs, host_segs.data(), host_segs.size() * sizeof(GpuEntropySegmentDescriptor), cudaMemcpyHostToDevice);

                GpuSegmentDecodeResult* d_results = nullptr;
                cudaMalloc(&d_results, host_segs.size() * sizeof(GpuSegmentDecodeResult));

                // Launch zeroing
                LaunchZeroVlcBandsGpu(d_desc, 1, stream);

                // Launch parallel replay
                LaunchDecodeEntropyParallelGpu(
                    d_payload,
                    payload.size(),
                    d_desc,
                    1,
                    d_segs,
                    static_cast<int>(host_segs.size()),
                    fsm_mgr.GetDeviceTable(),
                    d_results,
                    stream
                );

                // Launch difference coding if enabled
                if (diff_coding) {
                    LaunchCooperativeDifferenceCodingGpu(d_desc, 1, stream);
                }

                cudaStreamSynchronize(stream);

                // Download coefficients and compare
                std::vector<int16_t> gpu_coeffs(total_samples);
                cudaMemcpy(gpu_coeffs.data(), d_dst, total_samples * sizeof(int16_t), cudaMemcpyDeviceToHost);

                // Download segment results
                std::vector<GpuSegmentDecodeResult> seg_results(host_segs.size());
                cudaMemcpy(seg_results.data(), d_results, host_segs.size() * sizeof(GpuSegmentDecodeResult), cudaMemcpyDeviceToHost);

                // Verify exactness
                if (ref_res.status == GpuBandDecodeStatus::Complete) {
                    for (size_t i = 0; i < total_samples; ++i) {
                        if (ref_res.coeffs[i] != gpu_coeffs[i]) {
                            throw std::runtime_error(test_name + " failed sample match at index " +
                                std::to_string(i) + " for segs=" + std::to_string(segs) +
                                ": ref=" + std::to_string(ref_res.coeffs[i]) + " gpu=" + std::to_string(gpu_coeffs[i]));
                        }
                    }
                }

                cudaFree(d_segs);
                cudaFree(d_results);
            }

            cudaFree(d_payload);
            cudaFree(d_dst);
            cudaFree(d_desc);

            std::cout << "PASSED across all segment counts!" << std::endl;
        };

        // ---------------------------------------------------------------------
        // Case 1: Simple Pattern across 1, 2, 4, 8, 16, 32, 64 segments
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 500; ++i) {
                nibbles.push_back(1); // FSM transition
                nibbles.push_back(2);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 1: Multiscale Segments", payload, 240, 135, 1, false, {1, 2, 4, 8, 16, 32, 64});
        }

        // ---------------------------------------------------------------------
        // Case 2: Boundary Immediately Before Symbol
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 200; ++i) {
                nibbles.push_back(0); // zero run
                nibbles.push_back(5); // symbol emission
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 2: Boundary Before Symbol", payload, 240, 135, 2, false, {2, 4, 8, 16});
        }

        // ---------------------------------------------------------------------
        // Case 3: Boundary Inside Multi-Nibble Sequence (Intermediate State)
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 300; ++i) {
                nibbles.push_back(3);
                nibbles.push_back(7);
                nibbles.push_back(9);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 3: Inside Multi-Nibble State", payload, 240, 135, 3, false, {3, 7, 13, 29});
        }

        // ---------------------------------------------------------------------
        // Case 4: Long Zero Runs
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 150; ++i) {
                nibbles.push_back(0);
                nibbles.push_back(0);
                nibbles.push_back(1);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 4: Long Zero Runs", payload, 240, 135, 1, false, {2, 4, 8, 16});
        }

        // ---------------------------------------------------------------------
        // Case 5: Alternating Sign Bits
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 200; ++i) {
                nibbles.push_back(4);
                nibbles.push_back(8);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 5: Alternating Sign Bits", payload, 240, 135, 4, false, {4, 8, 16, 32});
        }

        // ---------------------------------------------------------------------
        // Case 6: Boundary Near End Marker
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 30; ++i) {
                nibbles.push_back(1);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 6: Near End Marker", payload, 240, 135, 1, false, {2, 4, 8});
        }

        // ---------------------------------------------------------------------
        // Case 7: Difference Coding with Parallel Replay
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 400; ++i) {
                nibbles.push_back(2);
                nibbles.push_back(5);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 7: Difference Coding Parallel", payload, 240, 135, 2, true, {2, 4, 8, 16, 32});
        }

        // ---------------------------------------------------------------------
        // Case 8: Odd Nibble Boundary Splits
        // ---------------------------------------------------------------------
        {
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 350; ++i) {
                nibbles.push_back(1);
                nibbles.push_back(6);
                nibbles.push_back(2);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);
            RunParallelEntropyTest("Case 8: Odd Nibble Boundaries", payload, 240, 135, 1, false, {3, 5, 9, 17});
        }

        // ---------------------------------------------------------------------
        // Case 9: Diagnostic Kernel Validation (Rule 18)
        // ---------------------------------------------------------------------
        {
            std::cout << "Running: Case 9: Diagnostic Kernel Validation... ";
            std::vector<uint8_t> nibbles;
            for (int i = 0; i < 100; ++i) {
                nibbles.push_back(1);
                nibbles.push_back(2);
            }
            AppendTrailer(nibbles, host_tables, cs17_base, 0);
            auto payload = PackNibbles(nibbles);

            GpuEntropyBandDescriptor desc{};
            desc.payload_offset = 0;
            desc.payload_bytes = static_cast<uint32_t>(payload.size());
            desc.width = 100;
            desc.height = 10;
            desc.coefficient_count = 1000;
            desc.codebook_id = 1;

            std::vector<GpuEntropyBandDescriptor> bvec = {desc};
            std::vector<GpuEntropySegmentDescriptor> segs;
            GenerateBandCheckpointsCpu(payload.data(), payload.size(), bvec, host_tables, GpuEntropyMode::ParallelConfigurable, 4, segs);

            uint8_t* d_payload = nullptr;
            cudaMalloc(&d_payload, payload.size());
            cudaMemcpy(d_payload, payload.data(), payload.size(), cudaMemcpyHostToDevice);

            GpuEntropyBandDescriptor* d_desc = nullptr;
            cudaMalloc(&d_desc, sizeof(GpuEntropyBandDescriptor));
            cudaMemcpy(d_desc, &desc, sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice);

            GpuEntropySegmentDescriptor* d_segs = nullptr;
            cudaMalloc(&d_segs, segs.size() * sizeof(GpuEntropySegmentDescriptor));
            cudaMemcpy(d_segs, segs.data(), segs.size() * sizeof(GpuEntropySegmentDescriptor), cudaMemcpyHostToDevice);

            GpuSegmentDiagnostic* d_diag = nullptr;
            cudaMalloc(&d_diag, segs.size() * sizeof(GpuSegmentDiagnostic));

            LaunchDecodeEntropyParallelDiagnosticGpu(d_payload, payload.size(), d_desc, d_segs, static_cast<int>(segs.size()), fsm_mgr.GetDeviceTable(), d_diag, stream);
            cudaStreamSynchronize(stream);

            std::vector<GpuSegmentDiagnostic> diag_host(segs.size());
            cudaMemcpy(diag_host.data(), d_diag, segs.size() * sizeof(GpuSegmentDiagnostic), cudaMemcpyDeviceToHost);

            for (size_t s = 0; s < segs.size(); ++s) {
                if (diag_host[s].termination_status != GpuBandDecodeStatus::Complete) {
                    throw std::runtime_error("Diagnostic status failure on segment " + std::to_string(s));
                }
                if (s + 1 < segs.size()) {
                    // Continuity check between segments: end state of seg s must equal start state of seg s+1
                    if (diag_host[s].end_state != segs[s + 1].initial_state) {
                        throw std::runtime_error("State continuity failure at boundary " + std::to_string(s));
                    }
                    if (diag_host[s].end_out_pos != segs[s + 1].out_coeff_pos) {
                        throw std::runtime_error("Out_pos continuity failure at boundary " + std::to_string(s));
                    }
                }
            }

            cudaFree(d_payload);
            cudaFree(d_desc);
            cudaFree(d_segs);
            cudaFree(d_diag);
            std::cout << "PASSED!" << std::endl;
        }

        // ---------------------------------------------------------------------
        // Case 10: Malformed Truncated Input
        // ---------------------------------------------------------------------
        {
            std::cout << "Running: Case 10: Malformed Truncated Input Safeguard... ";
            std::vector<uint8_t> payload = {0x12, 0x34, 0x56}; // truncated, no trailer

            GpuEntropyBandDescriptor desc{};
            desc.payload_offset = 0;
            desc.payload_bytes = 100; // descriptor claims 100 bytes, sample only 3 bytes
            desc.width = 100;
            desc.height = 10;
            desc.coefficient_count = 1000;
            desc.codebook_id = 1;

            GpuEntropySegmentDescriptor seg{};
            seg.byte_offset = 0;
            seg.end_byte_offset = 50;

            uint8_t* d_payload = nullptr;
            cudaMalloc(&d_payload, payload.size());
            cudaMemcpy(d_payload, payload.data(), payload.size(), cudaMemcpyHostToDevice);

            int16_t* d_dst = nullptr;
            cudaMalloc(&d_dst, desc.coefficient_count * sizeof(int16_t));
            desc.d_destination = d_dst;

            GpuEntropyBandDescriptor* d_desc = nullptr;
            cudaMalloc(&d_desc, sizeof(GpuEntropyBandDescriptor));
            cudaMemcpy(d_desc, &desc, sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice);

            GpuEntropySegmentDescriptor* d_seg = nullptr;
            cudaMalloc(&d_seg, sizeof(GpuEntropySegmentDescriptor));
            cudaMemcpy(d_seg, &seg, sizeof(GpuEntropySegmentDescriptor), cudaMemcpyHostToDevice);

            GpuSegmentDecodeResult* d_res = nullptr;
            cudaMalloc(&d_res, sizeof(GpuSegmentDecodeResult));

            LaunchDecodeEntropyParallelGpu(d_payload, payload.size(), d_desc, 1, d_seg, 1, fsm_mgr.GetDeviceTable(), d_res, stream);
            cudaStreamSynchronize(stream);

            GpuSegmentDecodeResult h_res{};
            cudaMemcpy(&h_res, d_res, sizeof(GpuSegmentDecodeResult), cudaMemcpyDeviceToHost);

            if (h_res.status != GpuBandDecodeStatus::InputTruncated) {
                throw std::runtime_error("expected InputTruncated error for truncated bitstream");
            }

            cudaFree(d_dst);
            cudaFree(d_payload);
            cudaFree(d_desc);
            cudaFree(d_seg);
            cudaFree(d_res);
            std::cout << "PASSED!" << std::endl;
        }

        cudaStreamDestroy(stream);
        fsm_mgr.Release();

        std::cout << "\nAll 10 Synthetic Parallel-Boundary Unit Tests PASSED successfully!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
}
