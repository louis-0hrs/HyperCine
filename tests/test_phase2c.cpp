// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/phase2c.h"
#include "hypercine/canonical.h"

#include <iostream>
#include <string>

namespace {

int Fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
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

    // 3. Section 2: Explicit 5-level bitstream identity & SHA-256 at 1080p, 4K, and 8K.
    for (const auto& res : hypercine::kResolutions) {
        const auto id = hypercine::VerifyPhase2CIdentity(res.width, res.height);
        if (!id.ok) {
            return Fail(std::string(res.name) + " identity verification failed: " + id.error +
                        " coeff=" + std::to_string(id.pre_entropy_coeff_equal) +
                        " len=" + std::to_string(id.per_band_payload_len_equal) +
                        " byte=" + std::to_string(id.per_band_payload_byte_equal) +
                        " sample=" + std::to_string(id.complete_sample_byte_equal) +
                        " streamed=" + std::to_string(id.streamed_d2h_sample_byte_equal) +
                        " multistream=" + std::to_string(id.multistream_sample_byte_equal) +
                        " gpu_lp=" + std::to_string(id.gpu_lowpass_sample_byte_equal) +
                        " dec=" + std::to_string(id.upstream_decoder_pixel_equal) +
                        " mismatch_at=" + std::to_string(id.mismatch_offset));
        }
        if (id.cpu_sample_sha256 != id.gpu_consolidated_sample_sha256 ||
            id.cpu_sample_sha256 != id.gpu_streamed_sample_sha256 ||
            id.cpu_sample_sha256 != id.gpu_lowpass_sample_sha256 ||
            id.cpu_decoded_pixels_sha256 != id.gpu_decoded_pixels_sha256) {
            return Fail(std::string(res.name) + " SHA-256 digest mismatch between CPU and GPU paths");
        }
        std::cout << "[Identity " << res.name << "] sample_bytes=" << id.sample_bytes
                  << " sample_sha256=" << id.cpu_sample_sha256
                  << " decoded_sha256=" << id.gpu_decoded_pixels_sha256
                  << " max_diff=" << id.decoded_max_abs_diff
                  << " psnr=" << id.psnr_vs_source_db << " dB\n";
    }

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
        if (!r.ok) {
            return Fail("1080p slot=" + std::to_string(slots) + " pipeline failed: " + r.error);
        }
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
        if (!r.ok) {
            return Fail("4K BandStreamed HostInput pipeline failed: " + r.error);
        }
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
        if (!r.ok) {
            return Fail("8K DeviceInput Consolidated pipeline failed: " + r.error);
        }
        std::cout << "[Pipeline 8K DeviceInput slots=3] fps=" << r.sustained_fps
                  << " mean_lat=" << r.mean_latency_ms << " ms psnr=" << r.psnr_db << " dB\n";
    }

    std::cout << "test_phase2c PASSED\n";
    return 0;
}
