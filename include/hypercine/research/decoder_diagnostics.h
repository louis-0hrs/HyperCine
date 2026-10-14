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
// Borrowed final inverse surfaces from the same ticket, including BGRA tickets.
// Research inspection only; does not change the public DeviceOutput contract.
struct DecoderReconstruction {
    std::array<DeviceOutputPlane, 3> planes{}; // Y/U/V, always top-down
    std::array<size_t, 3> allocation_bytes{};
    int slot_index = -1;
};
class DecoderInspector {
public:
    static bool BandDecisions(Decoder&, DecodeTicket, std::array<DecoderBandDecision, 27>*);
    static bool DownloadBand(Decoder&, DecodeTicket, int, std::vector<int16_t>&,
                             int* width = nullptr, int* height = nullptr, int* pitch = nullptr);
    // FIELDPLUS only: index is channel * 17 + logical_subband in the 51-band model.
    static bool DownloadFieldPlusBand(Decoder&, DecodeTicket, int channel, int logical_subband,
                             std::vector<int16_t>&, int* width = nullptr, int* height = nullptr,
                             int* pitch = nullptr);
    static bool DownloadFieldPlusTemporalStages(Decoder&, DecodeTicket, int channel,
                             std::vector<int16_t>& temporal_low, std::vector<int16_t>& temporal_high,
                             std::vector<int16_t>& wavelet4_ll, std::vector<int16_t>& frame_base_ll,
                             int* width = nullptr, int* height = nullptr);
    static bool DownloadFieldPlusSpatialStages(Decoder&, DecodeTicket, int channel,
                             std::vector<int16_t>& vertical_low, std::vector<int16_t>& vertical_high,
                             int* width = nullptr, int* height = nullptr);
    static bool DownloadIntermediate(Decoder&, DecodeTicket, int channel, int level, std::vector<int16_t>&);
    static bool Reconstruction(Decoder&, DecodeTicket, DecoderReconstruction*);
};
} // namespace HyperCine
