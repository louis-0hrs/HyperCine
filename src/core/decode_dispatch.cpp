// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decoder_backend.h"
#include <algorithm>
namespace HyperCine {
DecodeStrategy ClassifyDecodeFrame(const std::vector<GpuEntropyBandDescriptor>& bands, DecodeFrameInfo& info) {
    info = {};
    for (const auto& d : bands) {
        if (!d.subband) continue;
        info.highpass_bytes += d.payload_bytes;
        info.coefficient_count += d.coefficient_count;
        info.maximum_band_bytes = (std::max)(info.maximum_band_bytes, d.payload_bytes);
        info.level_payload_bytes[size_t(d.level)] += d.payload_bytes;
        // Integer arithmetic makes boundary decisions independent of FP rounding.
        if (d.payload_bytes >= 32768 && uint64_t(d.payload_bytes) * 100 >= uint64_t(d.coefficient_count) * 105)
            ++info.dense_band_count;
    }
    // Dense bands benefit from summaries. Sparse, distributed detail can also
    // benefit; concentrated texture stays on the barrier path. This fixed rule
    // is evaluated offline against OracleFrame, never trained during decoding.
    const bool concentrated = info.highpass_bytes && info.level_payload_bytes[1] * 100 >= info.highpass_bytes * 75;
    const bool distributed = info.highpass_bytes >= 262144 && info.maximum_band_bytes >= 65536;
    info.strategy = !concentrated && (info.dense_band_count || distributed)
        ? DecodeStrategy::Hybrid : DecodeStrategy::Lightweight;
    return info.strategy;
}
} // namespace HyperCine
