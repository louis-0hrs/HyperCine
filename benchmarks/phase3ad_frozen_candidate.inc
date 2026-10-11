// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// Placeholder until analyze_decode_phase3ad.py freezes a candidate.
// Hold-out mode refuses to run while kPhase3adCandidateFrozen is false.
static constexpr bool kPhase3adCandidateFrozen = false;
static constexpr const char* kPhase3adCandidateName = "UNFROZEN";
inline HyperCine::DecodeStrategy Phase3adFrozenCandidate(const HyperCine::Phase3adFeatures&) {
    return HyperCine::DecodeStrategy::Lightweight;
}
