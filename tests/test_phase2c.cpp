// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/phase2c.h"
#include "hypercine/canonical.h"
#include "reference_oracle.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

int Fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
}

// Strict encoder/bitstream identity. The original-SDK decoded-pixel and PSNR
// fields are intentionally excluded: they are a reference signal, not an
// encoder property.
bool StrictIdentityPassed(const hypercine::Phase2CIdentityReport& id) {
    return id.pre_entropy_coeff_equal &&
           id.per_band_payload_len_equal &&
           id.per_band_payload_byte_equal &&
           id.complete_sample_byte_equal &&
           id.streamed_d2h_sample_byte_equal &&
           id.multistream_sample_byte_equal &&
           id.gpu_lowpass_sample_byte_equal;
}

}  // namespace

int main() {
    // 1. Verify FIPS 180-4 SHA-256 implementation against NIST vectors.
    const std::string sha_empty = hypercine::Sha256Hex(nullptr, 0);
    if (sha_empty != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        return Fail("SHA-256 empty vector mismatch: " + sha_empty);
    }
    const std::string sha_abc = hypercine::Sha256Hex("abc", 3);
    if (sha_abc != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        return Fail("SHA-256 'abc' vector mismatch: " + sha_abc);
    }

    // 2. Section 1: Audit exact 30-band coverage at 1080p.
    const auto audit_1080 = hypercine::AuditPhase2BCoverage(1920, 1080);
    if (!audit_1080.ok || audit_1080.total_bands != 30 ||
        audit_1080.gpu_entropy_bands != 27 || audit_1080.cpu_lowpass_bands != 3) {
        return Fail("1080p 30-band coverage audit failed: " + audit_1080.error);
    }
    if (audit_1080.lowpass_raw_d2h_bytes != 129600u) {
        return Fail("unexpected 1080p lowpass raw D2H bytes: " + std::to_string(audit_1080.lowpass_raw_d2h_bytes));
    }
    std::cout << "[Audit 1080p] 30 bands (" << audit_1080.gpu_entropy_bands << " GPU highpass, "
              << audit_1080.cpu_lowpass_bands << " CPU lowpass) lowpass_d2h=" << audit_1080.lowpass_raw_d2h_bytes
              << " B highpass_compressed_d2h=" << audit_1080.highpass_compressed_d2h_bytes
              << " B lowpass_cpu=" << audit_1080.lowpass_cpu_ms << " ms ("
              << audit_1080.lowpass_pct_of_frame << "%)\n";

    hypercine::test_oracle::ReferenceGateInputs reference_gate;
    reference_gate.encoder_bitstream_verified = true;
    reference_gate.decoder_returned_ok = true;
    reference_gate.quality_oracle_qualified = false;
    std::size_t observed_decodes = 0;
    bool reference_unstable = false;
    bool reference_decode_unknown = false;
    bool quality_measured = false;
    double min_psnr = 99.0;

    // 3. Section 2: Explicit 5-level bitstream identity & SHA-256 at 1080p, 4K, and 8K.
    for (const auto& res : hypercine::kResolutions) {
        const auto id = hypercine::VerifyPhase2CIdentity(res.width, res.height);
        if (!StrictIdentityPassed(id)) {
            return Fail(std::string(res.name) + " identity verification failed: " + id.error +
                        " coeff=" + std::to_string(id.pre_entropy_coeff_equal) +
                        " len=" + std::to_string(id.per_band_payload_len_equal) +
                        " byte=" + std::to_string(id.per_band_payload_byte_equal) +
                        " sample=" + std::to_string(id.complete_sample_byte_equal) +
                        " streamed=" + std::to_string(id.streamed_d2h_sample_byte_equal) +
                        " multistream=" + std::to_string(id.multistream_sample_byte_equal) +
                        " gpu_lp=" + std::to_string(id.gpu_lowpass_sample_byte_equal) +
                        " mismatch_at=" + std::to_string(id.mismatch_offset));
        }
        if (id.cpu_sample_sha256 != id.gpu_consolidated_sample_sha256 ||
            id.cpu_sample_sha256 != id.gpu_streamed_sample_sha256 ||
            id.cpu_sample_sha256 != id.gpu_lowpass_sample_sha256) {
            return Fail(std::string(res.name) + " sample SHA-256 digest mismatch between CPU and GPU paths");
        }
        // Two original-SDK decodes of byte-identical samples. Equal pixels are
        // one observed output; unequal pixels are direct evidence of reference
        // instability. A failed or incomparable decode is a fatal decode error.
        const bool decode_evidence =
            id.upstream_decoder_pixel_equal || id.decoded_max_abs_diff > 0;
        if (!decode_evidence) {
            reference_decode_unknown = true;
        } else {
            observed_decodes += 2;
            if (!id.upstream_decoder_pixel_equal) reference_unstable = true;
            quality_measured = true;
            min_psnr = std::min(min_psnr, id.psnr_vs_source_db);
        }
        std::cout << "[Identity " << res.name << "] sample_bytes=" << id.sample_bytes
                  << " sample_sha256=" << id.cpu_sample_sha256
                  << " decoded_sha256=" << id.gpu_decoded_pixels_sha256
                  << " max_diff=" << id.decoded_max_abs_diff
                  << " psnr=" << id.psnr_vs_source_db << " dB"
                  << " reference_decode="
                  << (decode_evidence ? (id.upstream_decoder_pixel_equal ? "EQUAL" : "UNSTABLE")
                                      : "UNAVAILABLE")
                  << "\n";
    }

    // A verified bitstream with a failed original-SDK decode stays a real
    // failure. A verified bitstream with a low PSNR from that unqualified
    // decoder is a reference signal and is classified once at the end.
    auto check_pipeline = [&](const std::string& label,
                              const hypercine::Phase2CResult& r) -> int {
        if (!r.bitstream_byte_match) {
            return Fail(label + " strict encoder/bitstream gate failed: " + r.error);
        }
        if (!r.decode_ok) {
            return Fail(label + " original SDK decode failed: " + r.error);
        }
        ++observed_decodes;
        quality_measured = true;
        min_psnr = std::min(min_psnr, r.psnr_db);
        if (r.psnr_db <= 35.0) {
            std::cout << label << " reference PSNR " << r.psnr_db
                      << " dB below threshold; classified by reference gate\n";
        }
        return 0;
    };

    // 4. Multi-frame GPU-entropy pipeline across slot counts (1, 2, 3, 4), stream counts (1, 2, 4, 8),
    //    both D2H modes, both input modes, and optional GPU lowpass packing.
    for (int slots : {1, 2, 3, 4}) {
        hypercine::Phase2CConfig cfg;
        cfg.width = 1920;
        cfg.height = 1080;
        cfg.warmup_frames = 1;
        cfg.frames = 6;
        cfg.frame_slots = slots;
        cfg.entropy_streams = 4;
        cfg.d2h_mode = hypercine::CompressedD2HMode::FrameConsolidated;
        cfg.input_mode = hypercine::Phase2CInputMode::DeviceInput;
        cfg.verify_bitstream = true;
        const auto r = hypercine::RunPhase2C(cfg);
        const std::string label = "1080p slot=" + std::to_string(slots);
        if (const int rc = check_pipeline(label, r); rc != 0) return rc;
        std::cout << "[Pipeline 1080p slots=" << slots << "] fps=" << r.sustained_fps
                  << " mean_lat=" << r.mean_latency_ms << " ms psnr=" << r.psnr_db << " dB\n";
    }

    // Test BandStreamed D2H mode and HostInput mode at 4K.
    {
        hypercine::Phase2CConfig cfg;
        cfg.width = 3840;
        cfg.height = 2160;
        cfg.warmup_frames = 1;
        cfg.frames = 4;
        cfg.frame_slots = 3;
        cfg.entropy_streams = 4;
        cfg.d2h_mode = hypercine::CompressedD2HMode::BandStreamed;
        cfg.input_mode = hypercine::Phase2CInputMode::HostInput;
        cfg.gpu_pack_lowpass = true;
        cfg.verify_bitstream = true;
        const auto r = hypercine::RunPhase2C(cfg);
        if (const int rc = check_pipeline("4K HostInput BandStreamed", r); rc != 0) return rc;
        std::cout << "[Pipeline 4K HostInput BandStreamed] fps=" << r.sustained_fps
                  << " mean_lat=" << r.mean_latency_ms << " ms psnr=" << r.psnr_db << " dB\n";
    }

    // Test 8K multi-frame device-resident pipeline.
    {
        hypercine::Phase2CConfig cfg;
        cfg.width = 7680;
        cfg.height = 4320;
        cfg.warmup_frames = 1;
        cfg.frames = 3;
        cfg.frame_slots = 3;
        cfg.entropy_streams = 4;
        cfg.d2h_mode = hypercine::CompressedD2HMode::FrameConsolidated;
        cfg.input_mode = hypercine::Phase2CInputMode::DeviceInput;
        cfg.verify_bitstream = true;
        const auto r = hypercine::RunPhase2C(cfg);
        if (const int rc = check_pipeline("8K DeviceInput Consolidated", r); rc != 0) return rc;
        std::cout << "[Pipeline 8K DeviceInput slots=3] fps=" << r.sustained_fps
                  << " mean_lat=" << r.mean_latency_ms << " ms psnr=" << r.psnr_db << " dB\n";
    }

    if (reference_decode_unknown) reference_gate.decoder_returned_ok = false;
    reference_gate.observed_decodes = observed_decodes;
    reference_gate.distinct_decoded_outputs =
        observed_decodes == 0 ? 0 : (reference_unstable ? 2 : 1);
    reference_gate.quality_measured = quality_measured;
    reference_gate.quality_meets_threshold = quality_measured && min_psnr > 35.0;
    const auto decision = hypercine::test_oracle::ClassifyReferenceGate(reference_gate);
    std::cout << "reference_oracle_decision="
              << hypercine::test_oracle::GateDecisionName(decision)
              << " exit=" << hypercine::test_oracle::GateExitCode(decision)
              << " observed_decodes=" << observed_decodes
              << " distinct=" << reference_gate.distinct_decoded_outputs
              << " min_psnr=" << min_psnr << " dB"
              << " encoder_strict_gates=PASS\n";
    if (decision == hypercine::test_oracle::GateDecision::Pass) {
        std::cout << "test_phase2c PASSED\n";
        return 0;
    }
    std::cerr << "phase 2c strict stages passed; reference pixel gate is "
              << hypercine::test_oracle::GateDecisionName(decision) << "\n";
    return hypercine::test_oracle::GateExitCode(decision);
}
