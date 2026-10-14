// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include "hypercine/decode_entropy_cuda.h"
#include "hypercine/decoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace HyperCine {

constexpr int kFieldPlusChannelCount = 3;
constexpr int kFieldPlusFrameCount = 2;
constexpr int kFieldPlusLogicalBandsPerChannel = 17;
constexpr int kFieldPlusChunksPerChannel = 18;
constexpr int kFieldPlusBandCount = kFieldPlusChannelCount * kFieldPlusLogicalBandsPerChannel;
constexpr int kFieldPlusPhysicalChunkCount = kFieldPlusChannelCount * kFieldPlusChunksPerChannel;

enum class FieldPlusWaveletRole : uint8_t {
    Frame0Base,
    Frame1Base,
    GopTemporal,
    TemporalHighSpatial,
    TemporalLowSpatial1,
    TemporalLowSpatial2
};

enum class FieldPlusTemporalRole : uint8_t {
    Frame0Detail,
    Frame1Detail,
    GopLow,
    GopHigh
};

enum class FieldPlusFrameAssociation : uint8_t {
    Frame0,
    Frame1,
    BothFrames
};

enum class FieldPlusChunkKind : uint8_t {
    Coefficients,
    TemporalHighControl
};

// One coefficient-bearing logical band. The embedded entropy descriptor is
// routed to the existing VLC/FSM/checkpoint/Peak machinery; the remaining
// fields keep FIELDPLUS ownership and temporal semantics explicit.
struct FieldPlusBandDescriptor {
    GpuEntropyBandDescriptor entropy{};
    int logical_subband = -1;
    int physical_chunk = -1;
    int wavelet_owner = -1;
    int band_owner = -1;
    FieldPlusWaveletRole wavelet_role = FieldPlusWaveletRole::Frame0Base;
    FieldPlusTemporalRole temporal_role = FieldPlusTemporalRole::GopLow;
    FieldPlusFrameAssociation frame_association = FieldPlusFrameAssociation::BothFrames;
};

// Physical chunk records include the three empty index=255 bookkeeping
// chunks. Control records have payload_bytes == 0 and are never entropy input.
struct FieldPlusChunkDescriptor {
    FieldPlusChunkKind kind = FieldPlusChunkKind::Coefficients;
    int physical_index = -1;
    int channel = -1;
    int channel_chunk_index = -1;
    int logical_subband = -1;
    size_t chunk_offset = 0;
    size_t chunk_bytes = 0;
    size_t payload_offset = 0;
    size_t payload_bytes = 0;
    int codebook = 0;
    bool difference_coding = false;
    bool has_peaks = false;
    int peak_level = 0;
    size_t peak_offset = 0;
    int peak_count = 0;
    int wavelet_owner = -1;
    int band_owner = -1;
    FieldPlusWaveletRole wavelet_role = FieldPlusWaveletRole::Frame0Base;
    FieldPlusTemporalRole temporal_role = FieldPlusTemporalRole::GopLow;
    FieldPlusFrameAssociation frame_association = FieldPlusFrameAssociation::BothFrames;
    int width = 0;
    int height = 0;
};

struct FieldPlusSampleDescriptor {
    int width = 0;
    int height = 0;
    int coded_precision = 0;
    int channel_count = 0;
    int frame_count = 0;
    int transform_type = -1;
    int wavelet_count = 0;
    int logical_subbands_per_channel = 0;
    bool progressive = false;
    std::array<FieldPlusBandDescriptor, kFieldPlusBandCount> bands{};
    std::array<FieldPlusChunkDescriptor, kFieldPlusPhysicalChunkCount> chunks{};
    std::array<int, kFieldPlusChannelCount> control_chunk_indices{{-1, -1, -1}};
};

// Strict parser for the supported progressive FIELDPLUS / GOP-2 YUV422 p10
// profile. It does not modify or reuse the GOP-1 30-band parser representation.
bool ParseProgressiveFieldPlusSampleValidated(const uint8_t* sample, size_t size,
    int pixel_format_channeloffset, FieldPlusSampleDescriptor& out,
    std::string* error = nullptr, DecodeError* status = nullptr);

} // namespace HyperCine
