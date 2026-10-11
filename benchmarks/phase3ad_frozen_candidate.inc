// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// Frozen by analyze_decode_phase3ad.py. Do not edit by hand after hold-out starts.
static constexpr bool kPhase3adCandidateFrozen = true;
static constexpr const char* kPhase3adCandidateName = "phase3ad_baseline";
inline HyperCine::DecodeStrategy Phase3adFrozenCandidate(const HyperCine::Phase3adFeatures& f) {
    using HyperCine::DecodeStrategy;
    DecodeFrameInfo info;
    (void)info; (void)f;
    const bool concentrated = f.l1_share >= 750 && f.total_payload > 0;
    const bool distributed = f.total_payload >= 262144 && f.n_bytes_ge_65536 > 0;
    return !concentrated && (f.dense_count > 0 || distributed) ? DecodeStrategy::Hybrid : DecodeStrategy::Lightweight;
}
