// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/decode_gpu_index.h"
#include <cuda_runtime.h>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <string>
using namespace HyperCine;
void Check(cudaError_t e) { if (e != cudaSuccess) throw std::runtime_error(cudaGetErrorString(e)); }
template<class T> struct Device {
    T* p = nullptr;
    explicit Device(size_t n) { Check(cudaMalloc(&p, sizeof(T) * n)); }
    ~Device() { cudaFree(p); }
};
bool Equal(const GpuEntropySegmentDescriptor& a, const GpuEntropySegmentDescriptor& b) {
    return a.band_index == b.band_index && a.byte_offset == b.byte_offset &&
        a.nibble_idx == b.nibble_idx && a.initial_state == b.initial_state &&
        a.out_coeff_pos == b.out_coeff_pos && a.end_byte_offset == b.end_byte_offset &&
        a.end_nibble_idx == b.end_nibble_idx && a.max_coeff_pos == b.max_coeff_pos;
}
int main() {
 try {
    std::vector<GpuFsmEntry> tables(18752);
    for (int state = 0; state < 136; ++state) {
        tables[state * 16 + 15].value0 = 32767;
        tables[state * 16 + 1].value0 = 1;
        tables[state * 16 + 1].pre_post_skip = 1 << 12;
        tables[state * 16 + 2].value0 = -1;
        tables[state * 16 + 2].pre_post_skip = 1 << 12;
        tables[state * 16 + 3].next_state = 1;
        tables[state * 16 + 4].value0 = 7;
        tables[state * 16 + 4].pre_post_skip = 1 << 12;
        tables[state * 16 + 5].pre_post_skip = 511;
        tables[state * 16 + 6].next_state = 600;
    }
    Device<GpuFsmEntry> dt(tables.size());
    Check(cudaMemcpy(dt.p,tables.data(),tables.size()*sizeof(tables[0]),cudaMemcpyHostToDevice));
    Device<GpuEntropyBandDescriptor> dd(1);
    Device<GpuEntropySegmentDescriptor> ds(64);
    Device<GpuCheckpointIndexResult> dr(1);
    Device<uint32_t> de(1);
    int passed = 0;
    auto run = [&](std::string name, std::vector<uint8_t> payload, int fixed, uint32_t capacity,
                   GpuBandDecodeStatus expected, int mutation = 0) {
        Device<uint8_t> dp(std::max(size_t(1),payload.size()));
        if (!payload.empty()) Check(cudaMemcpy(dp.p,payload.data(),payload.size(),cudaMemcpyHostToDevice));
        GpuEntropyBandDescriptor d{};
        d.payload_bytes = static_cast<uint32_t>(payload.size());
        d.safe_upper_bound = d.payload_bytes;
        d.coefficient_count = 10000000;
        if (mutation == 1) ++d.payload_bytes;
        if (mutation == 2) d.codebook_id = 3;
        if (mutation == 3) d.payload_offset = std::numeric_limits<size_t>::max();
        if (mutation == 4) d.coefficient_count = 1;
        if (mutation == 5) d.initial_fsm_state = 600;
        if (mutation == 6) d.coefficient_count = std::numeric_limits<size_t>::max();
        Check(cudaMemcpy(dd.p,&d,sizeof(d),cudaMemcpyHostToDevice));
        Check(cudaMemset(de.p,0,sizeof(uint32_t)));
        Check(LaunchGenerateBandCheckpointsGpu(dp.p,payload.size(),dd.p,0,dt.p,tables.size(),
            fixed == 0 ? GpuEntropyMode::ParallelAdaptive : GpuEntropyMode::ParallelConfigurable,
            fixed,ds.p,capacity,dr.p,de.p,d.payload_bytes,nullptr));
        GpuCheckpointIndexResult result{};
        Check(cudaMemcpy(&result,dr.p,sizeof(result),cudaMemcpyDeviceToHost));
        uint32_t error = 0; Check(cudaMemcpy(&error,de.p,sizeof(error),cudaMemcpyDeviceToHost));
        if (result.status != expected || !result.finished ||
            (expected == GpuBandDecodeStatus::Complete ? error != 0 : error == 0))
            throw std::runtime_error(name + ": status " + std::to_string(int(result.status)));
        if (expected == GpuBandDecodeStatus::Complete) {
            std::vector<GpuEntropySegmentDescriptor> cpu,gpu(result.segment_count);
            if (!GenerateSingleBandCheckpointsCpu(payload.data(),payload.size(),d,0,tables.data(),tables.size(),
                fixed == 0 ? GpuEntropyMode::ParallelAdaptive : GpuEntropyMode::ParallelConfigurable,
                fixed,cpu)) throw std::runtime_error(name + ": CPU rejected");
            Check(cudaMemcpy(gpu.data(),ds.p,gpu.size()*sizeof(gpu[0]),cudaMemcpyDeviceToHost));
            if (cpu.size() != gpu.size()) throw std::runtime_error(name + ": count mismatch");
            for (size_t i=0;i<cpu.size();++i) if (!Equal(cpu[i],gpu[i])) throw std::runtime_error(name + ": field mismatch");
        } else if (result.segment_count) throw std::runtime_error(name + ": partial checkpoints published");
        std::cout << "[PASS] " << name << "\n"; ++passed;
    };
    const auto ok = GpuBandDecodeStatus::Complete;
    run("zero band", {0xf0},0,64,ok);
    run("tiny payload", {0x1f},0,64,ok);
    std::vector<uint8_t> zeros(1024,0x55); zeros.back()=0xf0;
    run("long zero runs",zeros,4,64,ok);
    std::vector<uint8_t> dense(1024,0x11); dense.back()=0x1f;
    run("dense symbols and byte boundary",dense,4,64,ok);
    std::vector<uint8_t> alternating(1025,0x12); alternating.back()=0xf0;
    run("alternating signs and odd nibble",alternating,4,64,ok);
    std::vector<uint8_t> transitions(1025,0x34); transitions.back()=0x3f;
    run("checkpoint near FSM transition",transitions,64,64,ok);
    run("checkpoint before marker",dense,64,64,ok);
    std::vector<uint8_t> maxsegments(4096,0x11); maxsegments.back()=0xf0;
    run("maximum segment count",maxsegments,64,64,ok);
    std::vector<uint8_t> continuation(262145,0x00); continuation.back()=0x1f;
    run("bounded continuation across launches",continuation,64,64,ok);
    run("truncated payload",dense,4,64,GpuBandDecodeStatus::InputTruncated,1);
    run("missing marker",std::vector<uint8_t>(1024,0),4,64,GpuBandDecodeStatus::MissingEndMarker);
    run("empty payload",{},4,64,GpuBandDecodeStatus::InputTruncated);
    run("invalid codebook",dense,4,64,GpuBandDecodeStatus::InvalidCodebook,2);
    run("invalid FSM transition",{0x6f},4,64,GpuBandDecodeStatus::InvalidFsmTransition);
    run("outside sample / address overflow",dense,4,64,GpuBandDecodeStatus::InputTruncated,3);
    run("coefficient overflow attempt",{0x11,0xf0},4,64,GpuBandDecodeStatus::CoefficientOverflow,4);
    run("invalid incoming FSM",dense,4,64,GpuBandDecodeStatus::InvalidDescriptor,5);
    run("coefficient address overflow",dense,4,64,GpuBandDecodeStatus::InvalidDescriptor,6);
    run("checkpoint capacity overflow",dense,64,4,GpuBandDecodeStatus::CheckpointCapacityOverflow);
    run("unexpected early termination",{0xf1},4,64,ok);
    std::cout << passed << "/" << passed << " synthetic cases passed\n";
    return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
