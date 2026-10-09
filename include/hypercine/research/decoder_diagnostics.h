// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/decoder.h"
#include <vector>
namespace HyperCine {
// Experimental verification surface. Not part of the production API/ABI.
enum class DecoderBandMethod { HostIndex, Hybrid, Trivial };
struct DecoderBandDecision {
    DecoderBandMethod method = DecoderBandMethod::HostIndex;
    bool fell_back = false;
    uint32_t corrected_nibbles = 0;
    uint32_t full_chunk_corrections = 0;
};
class DecoderInspector {
public:
    static bool BandDecisions(Decoder&, DecodeTicket, std::array<DecoderBandDecision, 27>*);
    static bool DownloadBand(Decoder&, DecodeTicket, int, std::vector<int16_t>&,
                             int* width = nullptr, int* height = nullptr, int* pitch = nullptr);
    static bool DownloadIntermediate(Decoder&, DecodeTicket, int channel, int level, std::vector<int16_t>&);
};
} // namespace HyperCine
