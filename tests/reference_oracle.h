// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_TESTS_REFERENCE_ORACLE_H_
#define HYPERCINE_TESTS_REFERENCE_ORACLE_H_

// Test-only decision contract for encoder tests that use the frozen original
// GoPro SDK as a pixel reference. The reference decoder is known to return
// different pixels for identical packets, so a decoded-pixel mismatch must
// never be folded into an encoder pass or fail by itself.
//
// This header is intentionally SDK-free and deterministic so the contract can
// be exercised on every platform, including builds without the original SDK.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hypercine {
namespace test_oracle {

// CTest uses SKIP_RETURN_CODE=77. A blocked reference gate is reported as a
// distinct non-green state, not as a pass and not as an encoder failure.
constexpr int kExitPass = 0;
constexpr int kExitFail = 1;
constexpr int kExitBlocked = 77;

enum class GateDecision {
    Pass,
    EncoderBitstreamMismatch,
    ReferenceDecodeError,
    ReferenceUnstable,
    OracleUnavailable,
    QualityFailureStable,
};

enum class PacketEquivalence {
    Equivalent,
    FrameNumberHeaderOnly,
    DifferingPayload,
    DifferingHeader,
    Unparsable,
};

struct SampleHeaderView {
    bool parsed = false;
    int error = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_number = 0;
    std::uint32_t encode_quality = 0;
    std::uint32_t encoder_version = 0;
    int encoded_format = 0;
    int input_format = 0;
    int progressive = 0;
    int interlaced_flags = 0;
    std::size_t header_bytes = 0;
};

struct ReferenceGateInputs {
    // Independent encoder -> coefficients -> entropy/bitstream verification.
    // This gate is always fatal and is never derived from decoded pixels.
    bool encoder_bitstream_verified = false;
    bool decoder_returned_ok = false;
    // Decode observation for equivalent packet bytes. observed_decodes == 0
    // means no decode evidence was collected, which fails closed.
    std::size_t observed_decodes = 0;
    std::size_t distinct_decoded_outputs = 0;
    // A qualified independent reference is required before a decoded-pixel
    // quality threshold may produce a pass or a stable failure.
    bool quality_oracle_qualified = false;
    bool quality_measured = false;
    bool quality_meets_threshold = false;
};

inline GateDecision ClassifyReferenceGate(const ReferenceGateInputs& inputs) {
    if (!inputs.encoder_bitstream_verified) return GateDecision::EncoderBitstreamMismatch;
    if (!inputs.decoder_returned_ok) return GateDecision::ReferenceDecodeError;
    if (inputs.observed_decodes == 0) return GateDecision::ReferenceDecodeError;
    if (inputs.distinct_decoded_outputs == 0) return GateDecision::ReferenceDecodeError;
    if (inputs.distinct_decoded_outputs > 1) return GateDecision::ReferenceUnstable;
    if (!inputs.quality_oracle_qualified) return GateDecision::OracleUnavailable;
    if (!inputs.quality_measured) return GateDecision::OracleUnavailable;
    if (!inputs.quality_meets_threshold) return GateDecision::QualityFailureStable;
    return GateDecision::Pass;
}

// A decode-only callsite has no encoder in the loop. Its strict signals (fixture
// storage/read, decoder API status, geometry, pitch, non-zero output) stay fatal
// and are enforced by the caller. A decoded-pixel comparison against a pinned
// reference may be measured and recorded, but until that reference is an
// independently qualified oracle it can only be BLOCKED_ORACLE. Independent
// decodes of the same immutable packet that disagree are the quarantined
// reference state. Neither outcome may become a pixel-accuracy pass or a stable
// quality failure.
inline GateDecision ClassifyDecodeOnlyReference(
    std::size_t observed_decodes,
    std::size_t distinct_decoded_outputs) {
    if (observed_decodes == 0 || distinct_decoded_outputs == 0) {
        return GateDecision::ReferenceDecodeError;
    }
    if (distinct_decoded_outputs > 1) return GateDecision::ReferenceUnstable;
    return GateDecision::OracleUnavailable;
}

inline const char* GateDecisionName(GateDecision decision) {
    switch (decision) {
    case GateDecision::Pass: return "PASS";
    case GateDecision::EncoderBitstreamMismatch: return "ENCODE_BITSTREAM_MISMATCH";
    case GateDecision::ReferenceDecodeError: return "REFERENCE_DECODE_ERROR";
    case GateDecision::ReferenceUnstable: return "ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED";
    case GateDecision::OracleUnavailable: return "BLOCKED_ORACLE_NOT_QUALIFIED";
    case GateDecision::QualityFailureStable: return "DECODE_QUALITY_FAILURE_STABLE";
    }
    return "UNKNOWN";
}

inline int GateExitCode(GateDecision decision) {
    switch (decision) {
    case GateDecision::Pass: return kExitPass;
    case GateDecision::ReferenceUnstable: return kExitBlocked;
    case GateDecision::OracleUnavailable: return kExitBlocked;
    case GateDecision::EncoderBitstreamMismatch: return kExitFail;
    case GateDecision::ReferenceDecodeError: return kExitFail;
    case GateDecision::QualityFailureStable: return kExitFail;
    }
    return kExitFail;
}

inline bool GateIsStrictFailure(GateDecision decision) {
    return decision == GateDecision::EncoderBitstreamMismatch ||
           decision == GateDecision::ReferenceDecodeError ||
           decision == GateDecision::QualityFailureStable;
}

inline bool GateAllowsRelease(GateDecision decision) {
    return decision == GateDecision::Pass;
}

inline const char* PacketEquivalenceName(PacketEquivalence equivalence) {
    switch (equivalence) {
    case PacketEquivalence::Equivalent: return "EQUIVALENT";
    case PacketEquivalence::FrameNumberHeaderOnly: return "FRAME_NUMBER_HEADER_ONLY";
    case PacketEquivalence::DifferingPayload: return "DIFFERING_PAYLOAD";
    case PacketEquivalence::DifferingHeader: return "DIFFERING_HEADER";
    case PacketEquivalence::Unparsable: return "UNPARSABLE";
    }
    return "UNKNOWN";
}

inline bool ValidHeaderView(const SampleHeaderView& view,
                            const std::vector<std::uint8_t>& packet) {
    return view.parsed && view.error == 0 && view.header_bytes > 0 &&
           view.header_bytes <= packet.size();
}

inline bool SameHeaderMetadata(const SampleHeaderView& a, const SampleHeaderView& b) {
    return a.width == b.width &&
           a.height == b.height &&
           a.encode_quality == b.encode_quality &&
           a.encoder_version == b.encoder_version &&
           a.encoded_format == b.encoded_format &&
           a.input_format == b.input_format &&
           a.progressive == b.progressive &&
           a.interlaced_flags == b.interlaced_flags;
}

// Locate the single big-endian tag segment for the parsed frame number inside
// the parser-derived header region. The segment layout is the upstream
// PutTagPair encoding: 16-bit tag id followed by a 16-bit value in a 32-bit
// tag word. Optional tags carry the sign bit in the stored tag id, so callers
// must pass the id exactly as stored (for example 0xffbb for the optional
// frame-number tag). Scanning is restricted to tag-word alignment; a missing
// or ambiguous segment fails closed.
inline bool FindFrameNumberSegment(const std::vector<std::uint8_t>& packet,
                                   std::size_t header_bytes,
                                   std::uint16_t stored_tag,
                                   std::uint32_t frame_number,
                                   std::size_t* offset) {
    if (header_bytes < 4 || header_bytes > packet.size()) return false;
    const std::uint8_t value_hi = static_cast<std::uint8_t>(frame_number >> 8);
    const std::uint8_t value_lo = static_cast<std::uint8_t>(frame_number & 0xffu);
    std::size_t matches = 0;
    std::size_t match_offset = 0;
    for (std::size_t i = 0; i + 3 < header_bytes; i += 4) {
        const std::uint32_t segment =
            (static_cast<std::uint32_t>(packet[i]) << 24) |
            (static_cast<std::uint32_t>(packet[i + 1]) << 16) |
            (static_cast<std::uint32_t>(packet[i + 2]) << 8) |
            static_cast<std::uint32_t>(packet[i + 3]);
        if (static_cast<std::uint16_t>(segment >> 16) != stored_tag) continue;
        if (packet[i + 2] != value_hi || packet[i + 3] != value_lo) continue;
        ++matches;
        match_offset = i;
    }
    if (matches != 1) return false;
    *offset = match_offset;
    return true;
}

// Compare two packets produced from the same synthetic input. Equivalence
// permits exactly the parsed frame-number header field to differ. The payload
// region starts at the parser-derived header boundary; no fixed prefix is
// skipped. Ambiguous or unparsable layouts fail closed.
inline PacketEquivalence CompareSyntheticPackets(
    const SampleHeaderView& a,
    const std::vector<std::uint8_t>& a_packet,
    const SampleHeaderView& b,
    const std::vector<std::uint8_t>& b_packet,
    std::uint16_t frame_number_tag) {
    if (!ValidHeaderView(a, a_packet) || !ValidHeaderView(b, b_packet)) {
        return PacketEquivalence::Unparsable;
    }
    if (a_packet.size() != b_packet.size()) return PacketEquivalence::DifferingPayload;
    if (!SameHeaderMetadata(a, b)) return PacketEquivalence::DifferingHeader;

    std::size_t a_segment = 0;
    std::size_t b_segment = 0;
    if (!FindFrameNumberSegment(a_packet, a.header_bytes, frame_number_tag,
                                a.frame_number, &a_segment) ||
        !FindFrameNumberSegment(b_packet, b.header_bytes, frame_number_tag,
                                b.frame_number, &b_segment)) {
        return PacketEquivalence::DifferingHeader;
    }
    if (a_segment != b_segment || a.header_bytes != b.header_bytes) {
        return PacketEquivalence::DifferingHeader;
    }

    for (std::size_t i = 0; i < a.header_bytes; ++i) {
        // The two value bytes of the parsed frame-number tag carry the only
        // permitted header difference.
        if (i == a_segment + 2 || i == a_segment + 3) continue;
        if (a_packet[i] != b_packet[i]) return PacketEquivalence::DifferingHeader;
    }
    for (std::size_t i = a.header_bytes; i < a_packet.size(); ++i) {
        if (a_packet[i] != b_packet[i]) return PacketEquivalence::DifferingPayload;
    }
    return a.frame_number == b.frame_number
               ? PacketEquivalence::Equivalent
               : PacketEquivalence::FrameNumberHeaderOnly;
}

}  // namespace test_oracle
}  // namespace hypercine

#endif  // HYPERCINE_TESTS_REFERENCE_ORACLE_H_
