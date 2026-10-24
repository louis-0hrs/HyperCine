// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Deterministic, SDK-free tests for the fail-closed reference-oracle contract.

#include "reference_oracle.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using hypercine::test_oracle::GateDecision;
using hypercine::test_oracle::PacketEquivalence;
namespace oracle = hypercine::test_oracle;

constexpr std::uint16_t kFrameNumberTag = 0x0045;  // CODEC_TAG_FRAME_NUMBER
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kPacketBytes = 64;

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

oracle::SampleHeaderView MakeView(std::uint32_t frame_number) {
    oracle::SampleHeaderView view;
    view.parsed = true;
    view.error = 0;
    view.width = 1920;
    view.height = 1080;
    view.frame_number = frame_number;
    view.encode_quality = 3;
    view.encoder_version = 0x00020000u;
    view.encoded_format = 4;
    view.input_format = 13;
    view.progressive = 1;
    view.interlaced_flags = 0;
    view.header_bytes = kHeaderBytes;
    return view;
}

void PutSegment(std::vector<std::uint8_t>* packet, std::size_t offset, std::uint16_t tag,
                std::uint32_t value) {
    (*packet)[offset] = static_cast<std::uint8_t>(tag >> 8);
    (*packet)[offset + 1] = static_cast<std::uint8_t>(tag & 0xffu);
    (*packet)[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    (*packet)[offset + 3] = static_cast<std::uint8_t>(value & 0xffu);
}

std::vector<std::uint8_t> MakePacket(std::uint32_t frame_number, std::uint8_t payload_seed,
                                     std::uint16_t stored_tag = kFrameNumberTag) {
    std::vector<std::uint8_t> packet(kPacketBytes, 0);
    // Fixed synthetic header bytes, then the parsed frame-number tag, then a
    // deterministic payload region starting at the parser boundary.
    for (std::size_t i = 0; i < kHeaderBytes; ++i) {
        packet[i] = static_cast<std::uint8_t>(0x11u + i);
    }
    PutSegment(&packet, 8, stored_tag, frame_number);
    for (std::size_t i = kHeaderBytes; i < kPacketBytes; ++i) {
        packet[i] = static_cast<std::uint8_t>(payload_seed + i * 7u);
    }
    return packet;
}

void TestPacketEquivalence() {
    const auto a = MakePacket(11, 0x5au);
    const auto b = MakePacket(14, 0x5au);
    const auto a_view = MakeView(11);
    const auto b_view = MakeView(14);

    Check(oracle::CompareSyntheticPackets(a_view, a, a_view, a, kFrameNumberTag) ==
              PacketEquivalence::Equivalent,
          "identical packets are equivalent");
    Check(oracle::CompareSyntheticPackets(a_view, a, b_view, b, kFrameNumberTag) ==
              PacketEquivalence::FrameNumberHeaderOnly,
          "frame-number-only header difference is attributed");

    auto payload_diff = b;
    payload_diff[40] ^= 0x01u;  // >= parser-derived header boundary
    Check(oracle::CompareSyntheticPackets(a_view, a, b_view, payload_diff, kFrameNumberTag) ==
              PacketEquivalence::DifferingPayload,
          "payload difference is never ignored");

    auto header_diff = b;
    header_diff[20] ^= 0x01u;  // inside header, not the frame-number value
    Check(oracle::CompareSyntheticPackets(a_view, a, b_view, header_diff, kFrameNumberTag) ==
              PacketEquivalence::DifferingHeader,
          "unrelated header difference fails closed");

    auto metadata_diff = b_view;
    metadata_diff.encode_quality = 4;
    Check(oracle::CompareSyntheticPackets(a_view, a, metadata_diff, b, kFrameNumberTag) ==
              PacketEquivalence::DifferingHeader,
          "parsed metadata difference fails closed");

    auto size_diff = b;
    size_diff.push_back(0);
    Check(oracle::CompareSyntheticPackets(a_view, a, b_view, size_diff, kFrameNumberTag) ==
              PacketEquivalence::DifferingPayload,
          "packet size difference fails closed");

    auto unparsable = a_view;
    unparsable.parsed = false;
    Check(oracle::CompareSyntheticPackets(unparsable, a, b_view, b, kFrameNumberTag) ==
              PacketEquivalence::Unparsable,
          "unparsable header fails closed");

    auto zero_header = a_view;
    zero_header.header_bytes = 0;
    Check(oracle::CompareSyntheticPackets(zero_header, a, b_view, b, kFrameNumberTag) ==
              PacketEquivalence::Unparsable,
          "missing parser boundary fails closed");

    auto oversized_header = a_view;
    oversized_header.header_bytes = a.size() + 1;
    Check(oracle::CompareSyntheticPackets(oversized_header, a, b_view, b, kFrameNumberTag) ==
              PacketEquivalence::Unparsable,
          "parser boundary beyond packet fails closed");

    auto ambiguous = b;
    PutSegment(&ambiguous, 16, kFrameNumberTag, 14);  // two matching segments
    Check(oracle::CompareSyntheticPackets(a_view, a, b_view, ambiguous, kFrameNumberTag) ==
              PacketEquivalence::DifferingHeader,
          "ambiguous frame-number segment fails closed");

    auto missing = b;
    PutSegment(&missing, 8, kFrameNumberTag, 99);  // does not match parsed frame
    Check(oracle::CompareSyntheticPackets(a_view, a, b_view, missing, kFrameNumberTag) ==
              PacketEquivalence::DifferingHeader,
          "missing frame-number segment fails closed");

    // The real encoder stores an optional tag, i.e. the tag id carries the
    // sign bit. Attribution must use that exact stored id.
    constexpr std::uint16_t kOptionalFrameNumberTag = 0xffbbu;
    const auto optional_a = MakePacket(11, 0x33, kOptionalFrameNumberTag);
    const auto optional_b = MakePacket(14, 0x33, kOptionalFrameNumberTag);
    Check(oracle::CompareSyntheticPackets(a_view, optional_a, b_view, optional_b,
                                         kOptionalFrameNumberTag) ==
              PacketEquivalence::FrameNumberHeaderOnly,
          "optional frame-number tag is attributed");
    Check(oracle::CompareSyntheticPackets(a_view, optional_a, b_view, optional_b,
                                         kFrameNumberTag) ==
              PacketEquivalence::DifferingHeader,
          "stored optional tag id must match exactly");

    auto misaligned = MakePacket(14, 0x5a);
    PutSegment(&misaligned, 8, kFrameNumberTag, 0);   // aligned segment no longer matches
    PutSegment(&misaligned, 9, kFrameNumberTag, 14);  // same tag at a non-word offset
    Check(oracle::CompareSyntheticPackets(a_view, a, b_view, misaligned, kFrameNumberTag) ==
              PacketEquivalence::DifferingHeader,
          "non-word-aligned tag segment fails closed");

    Check(std::string(oracle::PacketEquivalenceName(PacketEquivalence::FrameNumberHeaderOnly)) ==
              "FRAME_NUMBER_HEADER_ONLY",
          "equivalence name is stable");
}

void TestGateDecisions() {
    oracle::ReferenceGateInputs pass;
    pass.encoder_bitstream_verified = true;
    pass.decoder_returned_ok = true;
    pass.observed_decodes = 5;
    pass.distinct_decoded_outputs = 1;
    pass.quality_oracle_qualified = true;
    pass.quality_measured = true;
    pass.quality_meets_threshold = true;

    const GateDecision pass_decision = oracle::ClassifyReferenceGate(pass);
    Check(pass_decision == GateDecision::Pass, "fully verified stable qualified input passes");
    Check(oracle::GateExitCode(pass_decision) == oracle::kExitPass, "pass exits 0");
    Check(oracle::GateAllowsRelease(pass_decision), "only pass allows release");
    Check(!oracle::GateIsStrictFailure(pass_decision), "pass is not a strict failure");

    // (i) Byte-mismatched packet always fails, even if every other signal is good.
    auto encoder_mismatch = pass;
    encoder_mismatch.encoder_bitstream_verified = false;
    Check(oracle::ClassifyReferenceGate(encoder_mismatch) ==
              GateDecision::EncoderBitstreamMismatch,
          "encoder mismatch always fails");
    Check(oracle::GateExitCode(oracle::ClassifyReferenceGate(encoder_mismatch)) ==
              oracle::kExitFail,
          "encoder mismatch exits 1");
    Check(!oracle::GateAllowsRelease(oracle::ClassifyReferenceGate(encoder_mismatch)),
          "encoder mismatch blocks release");

    // (ii) Equivalent bytes with divergent original-SDK pixels are quarantined,
    // never an encoder failure and never a pixel-accuracy pass.
    auto unstable = pass;
    unstable.distinct_decoded_outputs = 2;
    const GateDecision unstable_decision = oracle::ClassifyReferenceGate(unstable);
    Check(unstable_decision == GateDecision::ReferenceUnstable,
          "divergent original decoder output is quarantined");
    Check(unstable_decision != GateDecision::Pass, "nondeterminism is not a pass");
    Check(unstable_decision != GateDecision::EncoderBitstreamMismatch,
          "nondeterminism is not an encoder failure");
    Check(!oracle::GateIsStrictFailure(unstable_decision),
          "nondeterminism is not a strict encoder failure");
    Check(oracle::GateExitCode(unstable_decision) == oracle::kExitBlocked,
          "quarantine uses the distinct blocked exit code");
    Check(!oracle::GateAllowsRelease(unstable_decision), "quarantine blocks release");

    // (iii) A stable bad image remains a real failure.
    auto stable_bad = pass;
    stable_bad.quality_meets_threshold = false;
    const GateDecision stable_bad_decision = oracle::ClassifyReferenceGate(stable_bad);
    Check(stable_bad_decision == GateDecision::QualityFailureStable,
          "stable bad quality remains a failure");
    Check(oracle::GateIsStrictFailure(stable_bad_decision), "stable bad quality is strict");
    Check(oracle::GateExitCode(stable_bad_decision) == oracle::kExitFail,
          "stable bad quality exits 1");

    // (iv) An unavailable/unqualified alternate oracle is explicit, never a pass.
    auto unqualified = pass;
    unqualified.quality_oracle_qualified = false;
    Check(oracle::ClassifyReferenceGate(unqualified) == GateDecision::OracleUnavailable,
          "unqualified oracle is explicitly blocked");
    Check(oracle::GateExitCode(oracle::ClassifyReferenceGate(unqualified)) ==
              oracle::kExitBlocked,
          "blocked oracle uses the distinct blocked exit code");
    Check(!oracle::GateAllowsRelease(oracle::ClassifyReferenceGate(unqualified)),
          "blocked oracle blocks release");

    auto unmeasured = pass;
    unmeasured.quality_measured = false;
    Check(oracle::ClassifyReferenceGate(unmeasured) == GateDecision::OracleUnavailable,
          "unmeasured quality is blocked, not passed");

    auto unqualified_bad = unqualified;
    unqualified_bad.quality_meets_threshold = false;
    Check(oracle::ClassifyReferenceGate(unqualified_bad) == GateDecision::OracleUnavailable,
          "unqualified oracle reports blocked before quality failure");

    auto decode_error = pass;
    decode_error.decoder_returned_ok = false;
    Check(oracle::ClassifyReferenceGate(decode_error) == GateDecision::ReferenceDecodeError,
          "decoder error fails");
    Check(oracle::GateExitCode(oracle::ClassifyReferenceGate(decode_error)) == oracle::kExitFail,
          "decoder error exits 1");

    auto no_evidence = pass;
    no_evidence.observed_decodes = 0;
    Check(oracle::ClassifyReferenceGate(no_evidence) == GateDecision::ReferenceDecodeError,
          "missing decode evidence fails closed");

    auto no_distinct = pass;
    no_distinct.distinct_decoded_outputs = 0;
    Check(oracle::ClassifyReferenceGate(no_distinct) == GateDecision::ReferenceDecodeError,
          "missing decoded output fails closed");

    Check(oracle::kExitBlocked == 77, "blocked exit code matches CTest SKIP_RETURN_CODE");

    Check(std::string(oracle::GateDecisionName(GateDecision::ReferenceUnstable)) ==
              "ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED",
          "quarantine label is explicit");
    Check(std::string(oracle::GateDecisionName(GateDecision::OracleUnavailable)) ==
              "BLOCKED_ORACLE_NOT_QUALIFIED",
          "blocked label is explicit");
    Check(std::string(oracle::GateDecisionName(GateDecision::Pass)) == "PASS",
          "pass label is explicit");
}

void TestRepeatedDecodeStability() {
    const GateDecision stable = oracle::ClassifyRepeatedDecodeStability(true);
    Check(stable == GateDecision::Pass, "identical repeated decodes are stable");
    Check(oracle::GateExitCode(stable) == oracle::kExitPass, "stable repeats exit 0");

    // (ii) at the decode-only callsite: divergence is quarantined, never a
    // failure of the code under test and never a pixel-accuracy pass.
    const GateDecision divergent = oracle::ClassifyRepeatedDecodeStability(false);
    Check(divergent == GateDecision::ReferenceUnstable,
          "divergent repeated decodes are quarantined");
    Check(oracle::GateExitCode(divergent) == oracle::kExitBlocked,
          "quarantined repeats use the distinct blocked exit code");
    Check(!oracle::GateIsStrictFailure(divergent),
          "quarantined repeats are not a strict failure");
    Check(!oracle::GateAllowsRelease(divergent), "quarantined repeats block release");
}

}  // namespace

int main() {
    TestPacketEquivalence();
    TestGateDecisions();
    TestRepeatedDecodeStability();
    if (failures != 0) {
        std::cerr << failures << " reference-oracle contract check(s) failed\n";
        return 1;
    }
    std::cout << "reference oracle contract checks passed\n";
    return 0;
}
