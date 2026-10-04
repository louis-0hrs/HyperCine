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

// Find a path in the FSM table from start_state to a state having an entry with value0 == 32767
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

} // anonymous namespace

int main() {
    std::cout << "=== HyperCine CUDA Entropy Synthetic & Unit Verification ===" << std::endl;

    std::vector<GpuFsmEntry> host_tables;
    if (!ExtractCineFormBaseFsmTables(host_tables)) {
        std::cerr << "Failed to extract CineForm base FSM tables!" << std::endl;
        return 1;
    }

    GpuFsmTableManager mgr;
    if (!mgr.Initialize(0)) {
        std::cerr << "Failed to initialize GpuFsmTableManager on GPU 0!" << std::endl;
        return 1;
    }

    size_t cs9_base = mgr.GetCodebookOffset(0);
    size_t cs17_base = mgr.GetCodebookOffset(1);

    // Find trailer sequences for codebook 0 (cs9) and codebook 1 (cs17)
    auto cs9_trailer_nibs = FindTrailerPath(host_tables, cs9_base, 0);
    if (cs9_trailer_nibs.empty()) {
        std::cerr << "Could not find trailer path for cs9!" << std::endl;
        return 1;
    }
    std::cout << "[Setup] Found cs9 trailer sequence: " << cs9_trailer_nibs.size() << " nibbles" << std::endl;

    auto cs17_trailer_nibs = FindTrailerPath(host_tables, cs17_base, 0);
    if (cs17_trailer_nibs.empty()) {
        std::cerr << "Could not find trailer path for cs17!" << std::endl;
        return 1;
    }
    std::cout << "[Setup] Found cs17 trailer sequence: " << cs17_trailer_nibs.size() << " nibbles" << std::endl;

    // Helper lambda to run a test band descriptor on GPU and compare against expected
    auto RunTestBand = [&](
        const std::string& test_name,
        const std::vector<uint8_t>& payload,
        int width, int height,
        int codebook_id,
        int quant,
        bool diff_coding,
        GpuBandDecodeStatus expected_status,
        const std::vector<int16_t>* expected_coeffs = nullptr,
        bool pre_dirty_buffer = false
    ) -> bool {
        size_t pitch_bytes = (static_cast<size_t>(width) * sizeof(int16_t) + 15) & ~size_t(15);
        size_t pitch_samples = pitch_bytes / sizeof(int16_t);
        size_t total_samples = static_cast<size_t>(height) * pitch_samples;

        // Allocate device memory
        uint8_t* d_sample = nullptr;
        int16_t* d_dest = nullptr;
        GpuEntropyBandDescriptor* d_desc = nullptr;
        GpuBandDecodeResult* d_res = nullptr;

        cudaMalloc(&d_sample, std::max<size_t>(payload.size(), 16));
        cudaMalloc(&d_dest, total_samples * sizeof(int16_t));
        cudaMalloc(&d_desc, sizeof(GpuEntropyBandDescriptor));
        cudaMalloc(&d_res, sizeof(GpuBandDecodeResult));

        if (!payload.empty()) {
            cudaMemcpy(d_sample, payload.data(), payload.size(), cudaMemcpyHostToDevice);
        }

        if (pre_dirty_buffer) {
            cudaMemset(d_dest, 0x5A, total_samples * sizeof(int16_t));
        }

        GpuEntropyBandDescriptor h_desc{};
        h_desc.payload_offset = 0;
        h_desc.payload_bytes = static_cast<uint32_t>(payload.size());
        h_desc.safe_upper_bound = static_cast<uint32_t>(payload.size());
        h_desc.trailer_word = 0x00003800;
        h_desc.width = width;
        h_desc.height = height;
        h_desc.pitch_bytes = pitch_bytes;
        h_desc.pitch_samples = pitch_samples;
        h_desc.coefficient_count = total_samples;
        h_desc.method = BandEncodingMethod::RunLengthVLC;
        h_desc.codebook_id = codebook_id;
        h_desc.quantization = quant;
        h_desc.difference_coding = diff_coding;
        h_desc.d_destination = d_dest;

        cudaMemcpy(d_desc, &h_desc, sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice);

        cudaStream_t stream;
        cudaStreamCreate(&stream);

        cudaError_t err = LaunchDecodeEntropyGpu(
            d_sample,
            payload.size(),
            d_desc,
            1,
            mgr.GetDeviceTable(),
            d_res,
            stream
        );

        if (err != cudaSuccess) {
            std::cerr << "[" << test_name << "] Launch failed: " << cudaGetErrorString(err) << std::endl;
            cudaStreamDestroy(stream);
            cudaFree(d_sample); cudaFree(d_dest); cudaFree(d_desc); cudaFree(d_res);
            return false;
        }

        cudaStreamSynchronize(stream);

        GpuBandDecodeResult h_res{};
        cudaMemcpy(&h_res, d_res, sizeof(GpuBandDecodeResult), cudaMemcpyDeviceToHost);

        bool pass = true;
        if (h_res.status != expected_status) {
            std::cerr << "[" << test_name << "] Status mismatch: got "
                      << static_cast<int>(h_res.status) << " expected "
                      << static_cast<int>(expected_status) << std::endl;
            pass = false;
        }

        if (pass && expected_coeffs) {
            std::vector<int16_t> h_dest(total_samples);
            cudaMemcpy(h_dest.data(), d_dest, total_samples * sizeof(int16_t), cudaMemcpyDeviceToHost);

            int diffs = 0;
            for (size_t i = 0; i < total_samples; ++i) {
                if (h_dest[i] != (*expected_coeffs)[i]) {
                    diffs++;
                    if (diffs <= 5) {
                        std::cerr << "  Diff at " << i << ": got " << h_dest[i]
                                  << " expected " << (*expected_coeffs)[i] << std::endl;
                    }
                }
            }
            if (diffs > 0) {
                std::cerr << "[" << test_name << "] Total coeff diffs: " << diffs << std::endl;
                pass = false;
            }
        }

        cudaStreamDestroy(stream);
        cudaFree(d_sample);
        cudaFree(d_dest);
        cudaFree(d_desc);
        cudaFree(d_res);

        if (pass) {
            std::cout << "  [PASS] " << test_name << " (duration: " << h_res.duration_cycles << " cycles)" << std::endl;
        } else {
            std::cout << "  [FAIL] " << test_name << std::endl;
        }
        return pass;
    };

    bool all_passed = true;

    // 1. Zero band test: bitstream with immediate trailer
    {
        auto payload = PackNibbles(cs9_trailer_nibs);
        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        std::vector<int16_t> expected(total, 0);
        all_passed &= RunTestBand("Case 1: Zero Band (immediate trailer)", payload, w, h, 0, 1, false, GpuBandDecodeStatus::Complete, &expected);
    }

    // 2. Isolated nonzero values
    {
        // Find a nibble from state 0 that produces non-zero value and then a trailer
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        for (uint8_t n = 0; n < 16; ++n) {
            const auto& e = host_tables[cs9_base + n];
            if (e.value0 != 0 && e.value0 != 32767) {
                nibs.push_back(n);
                state = e.next_state;
                break;
            }
        }
        auto trail = FindTrailerPath(host_tables, cs9_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs9_base, total, 2);
        all_passed &= RunTestBand("Case 2: Isolated Nonzeros", payload, w, h, 0, 2, false, GpuBandDecodeStatus::Complete, &sim.coeffs);
    }

    // 3. Extreme zero runs (multiple skip nibbles)
    {
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        // Search for nibbles with maximum pre_skip
        for (int step = 0; step < 5; ++step) {
            uint32_t best_skip = 0;
            uint8_t best_n = 0;
            uint16_t next_s = state;
            for (uint8_t n = 0; n < 16; ++n) {
                const auto& e = host_tables[cs9_base + state * 16 + n];
                if (e.value0 != 32767) {
                    uint32_t s = (e.pre_post_skip & 0x1ff) + ((e.pre_post_skip >> 12) & 0x7);
                    if (s >= best_skip) {
                        best_skip = s;
                        best_n = n;
                        next_s = e.next_state;
                    }
                }
            }
            nibs.push_back(best_n);
            state = next_s;
        }
        auto trail = FindTrailerPath(host_tables, cs9_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 128, h = 128;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs9_base, total, 1);
        all_passed &= RunTestBand("Case 3: Extreme Zero Runs", payload, w, h, 0, 1, false, GpuBandDecodeStatus::Complete, &sim.coeffs);
    }

    // 4. Alternating signs (+ / -)
    {
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        bool want_positive = true;
        for (int step = 0; step < 8; ++step) {
            for (uint8_t n = 0; n < 16; ++n) {
                const auto& e = host_tables[cs9_base + state * 16 + n];
                if (e.value0 != 32767 && ((want_positive && e.value0 > 0) || (!want_positive && e.value0 < 0))) {
                    nibs.push_back(n);
                    state = e.next_state;
                    want_positive = !want_positive;
                    break;
                }
            }
        }
        auto trail = FindTrailerPath(host_tables, cs9_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs9_base, total, 3);
        all_passed &= RunTestBand("Case 4: Alternating Signs", payload, w, h, 0, 3, false, GpuBandDecodeStatus::Complete, &sim.coeffs);
    }

    // 5. Positive and negative extrema
    {
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        int max_val = 0;
        uint8_t max_n = 0;
        for (uint8_t n = 0; n < 16; ++n) {
            const auto& e = host_tables[cs9_base + n];
            if (e.value0 != 32767 && std::abs(e.value0) > max_val) {
                max_val = std::abs(e.value0);
                max_n = n;
            }
        }
        nibs.push_back(max_n);
        state = host_tables[cs9_base + max_n].next_state;
        auto trail = FindTrailerPath(host_tables, cs9_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs9_base, total, 4);
        all_passed &= RunTestBand("Case 5: Positive/Negative Extrema", payload, w, h, 0, 4, false, GpuBandDecodeStatus::Complete, &sim.coeffs);
    }

    // 6. Dense patterns (many nonzero values)
    {
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        for (int step = 0; step < 20; ++step) {
            bool found = false;
            for (uint8_t n = 0; n < 16; ++n) {
                const auto& e = host_tables[cs9_base + state * 16 + n];
                if (e.value0 != 0 && e.value0 != 32767) {
                    nibs.push_back(n);
                    state = e.next_state;
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }
        auto trail = FindTrailerPath(host_tables, cs9_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs9_base, total, 1);
        all_passed &= RunTestBand("Case 6: Dense Pattern", payload, w, h, 0, 1, false, GpuBandDecodeStatus::Complete, &sim.coeffs);
    }

    // 7. Sparse patterns (mostly zeros)
    {
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        for (int step = 0; step < 10; ++step) {
            for (uint8_t n = 0; n < 16; ++n) {
                const auto& e = host_tables[cs9_base + state * 16 + n];
                if (e.value0 == 0 && e.value1 == 0 && e.pre_post_skip > 0) {
                    nibs.push_back(n);
                    state = e.next_state;
                    break;
                }
            }
        }
        auto trail = FindTrailerPath(host_tables, cs9_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs9_base, total, 1);
        all_passed &= RunTestBand("Case 7: Sparse Pattern", payload, w, h, 0, 1, false, GpuBandDecodeStatus::Complete, &sim.coeffs);
    }

    // 8. Boundary end marker
    {
        auto payload = PackNibbles(cs9_trailer_nibs);
        int w = 16, h = 1;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        std::vector<int16_t> expected(total, 0);
        all_passed &= RunTestBand("Case 8: Boundary End Marker", payload, w, h, 0, 1, false, GpuBandDecodeStatus::Complete, &expected);
    }

    // 9. Premature end marker (valid termination before band end)
    {
        auto payload = PackNibbles(cs9_trailer_nibs);
        int w = 240, h = 135;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        std::vector<int16_t> expected(total, 0);
        all_passed &= RunTestBand("Case 9: Premature End Marker", payload, w, h, 0, 1, false, GpuBandDecodeStatus::Complete, &expected);
    }

    // 10. Truncated payload (no end marker in stream)
    {
        // 4 bytes of nonzero nibbles without any trailer
        std::vector<uint8_t> payload = {0x12, 0x34, 0x56, 0x78};
        all_passed &= RunTestBand("Case 10: Truncated Payload (Missing End Marker)", payload, 64, 64, 0, 1, false, GpuBandDecodeStatus::MissingEndMarker);
    }

    // 11. Malformed payload overflow attempt (bounds safety)
    {
        // Generate a tiny band with capacity of 16 samples, but feed many skip nibbles
        std::vector<uint8_t> payload(64, 0x11); // many skips
        all_passed &= RunTestBand("Case 11: Malformed Overflow Rejection", payload, 8, 2, 0, 1, false, GpuBandDecodeStatus::CoefficientOverflow);
    }

    // 12. Difference coding verification
    {
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        for (int step = 0; step < 6; ++step) {
            for (uint8_t n = 0; n < 16; ++n) {
                const auto& e = host_tables[cs17_base + state * 16 + n];
                if (e.value0 != 0 && e.value0 != 32767) {
                    nibs.push_back(n);
                    state = e.next_state;
                    break;
                }
            }
        }
        auto trail = FindTrailerPath(host_tables, cs17_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 64, h = 4;
        size_t pitch_samples = ((w * 2 + 15) & ~15) / 2;
        size_t total = static_cast<size_t>(h) * pitch_samples;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs17_base, total, 1);

        // Apply difference coding to sim expected
        for (int y = 0; y < h; ++y) {
            int16_t* line = sim.coeffs.data() + y * pitch_samples;
            for (int x = 1; x < w; ++x) {
                line[x] += line[x - 1];
            }
        }

        all_passed &= RunTestBand("Case 12: Difference Coding", payload, w, h, 1, 1, true, GpuBandDecodeStatus::Complete, &sim.coeffs);
    }

    // 13. Buffer reuse across frames (stale memory overwrite check)
    {
        auto payload = PackNibbles(cs9_trailer_nibs);
        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        std::vector<int16_t> expected(total, 0);
        all_passed &= RunTestBand("Case 13: Buffer Reuse / Stale Memory Clear", payload, w, h, 0, 1, false, GpuBandDecodeStatus::Complete, &expected, true);
    }

    // 14. Symbol equality test (Rule 18)
    {
        std::vector<uint8_t> nibs;
        uint16_t state = 0;
        for (int step = 0; step < 12; ++step) {
            for (uint8_t n = 0; n < 16; ++n) {
                const auto& e = host_tables[cs9_base + state * 16 + n];
                if (e.value0 != 32767) {
                    nibs.push_back(n);
                    state = e.next_state;
                    break;
                }
            }
        }
        auto trail = FindTrailerPath(host_tables, cs9_base, state);
        nibs.insert(nibs.end(), trail.begin(), trail.end());
        auto payload = PackNibbles(nibs);

        int w = 64, h = 64;
        size_t total = static_cast<size_t>(h) * ((w * 2 + 15) & ~15) / 2;
        auto sim = SimulateCpuFsm(payload.data(), payload.size(), host_tables, cs9_base, total, 1);

        // Run diagnostic kernel
        uint8_t* d_sample = nullptr;
        GpuEntropyBandDescriptor* d_desc = nullptr;
        DecodedSymbolDiagnostic* d_symbols = nullptr;
        int* d_actual = nullptr;
        GpuBandDecodeResult* d_res = nullptr;

        int max_syms = 256;
        cudaMalloc(&d_sample, payload.size());
        cudaMalloc(&d_desc, sizeof(GpuEntropyBandDescriptor));
        cudaMalloc(&d_symbols, max_syms * sizeof(DecodedSymbolDiagnostic));
        cudaMalloc(&d_actual, sizeof(int));
        cudaMalloc(&d_res, sizeof(GpuBandDecodeResult));

        cudaMemcpy(d_sample, payload.data(), payload.size(), cudaMemcpyHostToDevice);

        GpuEntropyBandDescriptor h_desc{};
        h_desc.payload_offset = 0;
        h_desc.payload_bytes = static_cast<uint32_t>(payload.size());
        h_desc.safe_upper_bound = static_cast<uint32_t>(payload.size());
        h_desc.width = w;
        h_desc.height = h;
        h_desc.pitch_bytes = ((w * 2 + 15) & ~15);
        h_desc.pitch_samples = h_desc.pitch_bytes / 2;
        h_desc.coefficient_count = total;
        h_desc.codebook_id = 0;
        h_desc.quantization = 1;

        cudaMemcpy(d_desc, &h_desc, sizeof(GpuEntropyBandDescriptor), cudaMemcpyHostToDevice);

        LaunchDecodeEntropyDiagnosticGpu(
            d_sample,
            payload.size(),
            d_desc,
            mgr.GetDeviceTable(),
            d_symbols,
            max_syms,
            d_actual,
            d_res,
            0
        );
        cudaDeviceSynchronize();

        int actual_syms = 0;
        cudaMemcpy(&actual_syms, d_actual, sizeof(int), cudaMemcpyDeviceToHost);

        std::vector<DecodedSymbolDiagnostic> h_symbols(actual_syms);
        cudaMemcpy(h_symbols.data(), d_symbols, actual_syms * sizeof(DecodedSymbolDiagnostic), cudaMemcpyDeviceToHost);

        bool sym_ok = (h_symbols.size() == sim.symbols.size());
        if (sym_ok) {
            for (size_t i = 0; i < h_symbols.size(); ++i) {
                if (h_symbols[i].nibble_index != sim.symbols[i].nibble_index ||
                    h_symbols[i].state_before != sim.symbols[i].state_before ||
                    h_symbols[i].state_after != sim.symbols[i].state_after ||
                    h_symbols[i].pre_skip != sim.symbols[i].pre_skip ||
                    h_symbols[i].post_skip != sim.symbols[i].post_skip ||
                    h_symbols[i].raw_val0 != sim.symbols[i].raw_val0 ||
                    h_symbols[i].raw_val1 != sim.symbols[i].raw_val1 ||
                    h_symbols[i].out_pos != sim.symbols[i].out_pos) {
                    sym_ok = false;
                    std::cerr << "Symbol mismatch at index " << i << std::endl;
                    break;
                }
            }
        } else {
            std::cerr << "Symbol count mismatch: GPU=" << h_symbols.size() << " CPU=" << sim.symbols.size() << std::endl;
        }

        cudaFree(d_sample);
        cudaFree(d_desc);
        cudaFree(d_symbols);
        cudaFree(d_actual);
        cudaFree(d_res);

        if (sym_ok) {
            std::cout << "  [PASS] Case 14: Symbol Equality vs CPU FSM (" << h_symbols.size() << " symbols identical)" << std::endl;
        } else {
            std::cout << "  [FAIL] Case 14: Symbol Equality vs CPU FSM" << std::endl;
            all_passed = false;
        }
    }

    if (!all_passed) {
        std::cerr << "\nOne or more synthetic unit tests failed!" << std::endl;
        return 1;
    }

    std::cout << "\nAll 14 Synthetic Entropy Unit Tests PASSED successfully!" << std::endl;
    return 0;
}
