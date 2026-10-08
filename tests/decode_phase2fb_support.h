// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/decode_entropy_cuda.h"
#include "../src/cuda/decode_hybrid_entropy.cuh"

namespace Phase2FB {
using namespace HyperCine::HybridEntropy;

// Full-state summary composition retains position and absorbing termination.
// Used only by bounded research tests; a full critical-band matrix is rejected.
inline Summary Compose(const Summary& a, uint32_t left_nibbles, const std::vector<Summary>& right) {
    if (a.terminal != 65535) return a;
    if (a.state >= right.size()) throw std::runtime_error("composition state bound");
    auto b = right[a.state];
    uint64_t advance = uint64_t(a.advance) + b.advance;
    uint64_t terminal = b.terminal == 65535 ? 65535 : uint64_t(left_nibbles) + b.terminal;
    if (advance > UINT32_MAX || terminal > 65535) throw std::runtime_error("composition overflow");
    return {uint32_t(advance), b.state, uint16_t(terminal)};
}

} // namespace Phase2FB
