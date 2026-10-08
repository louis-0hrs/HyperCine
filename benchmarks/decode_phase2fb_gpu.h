// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "../tests/decode_phase2fb_support.h"
namespace Phase2FB {
struct Measurement {
 double gpu_pre_ms=0,cpu_resolve_ms=0,checkpoint_transfer_ms=0,replay_ms=0,ready_ms=0,total_ms=0;
 size_t scratch=0; Resolved resolved; std::vector<int16_t> coefficients;
};
Measurement RunGpu(const uint8_t*,uint32_t,const GpuFsmEntry*,unsigned,uint16_t,uint32_t,int,bool,int,int,uint32_t);
double SummaryPilotGpu(const uint8_t*,uint32_t,const GpuFsmEntry*,unsigned,uint32_t,unsigned);
}

namespace Phase2FB {
struct BaselineMeasurement { double replay_ms=0,gpu_index_ms=-1; uint64_t max_index_launch_ns=0; };
BaselineMeasurement Baselines(const uint8_t*,uint32_t,const std::vector<HyperCine::GpuFsmEntry>&,
 const HyperCine::GpuEntropyBandDescriptor&,bool);
}

namespace Phase2FB { std::string DeviceDiagnostics(); }
