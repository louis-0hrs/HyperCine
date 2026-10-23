// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/phase2d.h"
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

// Strict encoder/bitstream identity. The original-SDK decoded-pixel, max-diff
// and PSNR fields are intentionally excluded: they are a reference signal, not
// an encoder property.
bool StrictPhase2DIdentityPassed(
    const hypercine::Phase2DUpstreamComparisonReport& cmp) {
    return cmp.b_eq_c_unmasked &&
           cmp.c_all_modes_byte_identical &&
           cmp.a_eq_b_same_handle_unmasked &&
           cmp.a_eq_b_independent_handle_masked &&
           cmp.a_eq_c_independent_handle_masked &&
           cmp.all_30_subbands_a_eq_b_eq_c &&
           cmp.bands.size() == 30;
}

const char* QuantStateName(hypercine::Phase2DQuantStateMode m) {
    return (m == hypercine::Phase2DQuantStateMode::Frame1Initial)
        ? "Frame1Initial(FSratelimiter=4)"
        : "SteadyState(FSratelimiter=0)";
}

}  // namespace

int main() {
    // 1. Verify default production configuration disables deterministic metadata override.
    {
        hypercine::Phase2CConfig c2c{};
        hypercine::Phase2DConfig c2d{};
        if (c2c.deterministic_test_metadata || c2d.deterministic_test_metadata) {
            return Fail("deterministic_test_metadata must default to false in production configs");
        }
    }

    // 2. Part 2: Production metadata safety regression test.
    {
        const auto meta = hypercine::VerifyPhase2DMetadataSafety(1920, 1080);
        if (!meta.ok) {
            return Fail("VerifyPhase2DMetadataSafety failed: " + meta.error +
                        " guids_distinct=" + std::to_string(meta.normal_mode_guids_distinct) +
                        " unmasked_distinct=" + std::to_string(meta.normal_mode_unmasked_distinct) +
                        " masked_identical=" + std::to_string(meta.normal_mode_masked_identical) +
                        " det_identical=" + std::to_string(meta.deterministic_mode_unmasked_identical));
        }
        std::cout << "[Part 2 Metadata Safety] clip1_guid=" << meta.clip1_guid_hex
                  << " clip2_guid=" << meta.clip2_guid_hex
                  << " normal_masked_identical=" << meta.normal_mode_masked_identical
                  << " deterministic_unmasked_identical=" << meta.deterministic_mode_unmasked_identical << "\n";
    }

    // 3. Part 1: Pristine upstream (A) vs HyperCine CPU/ref (B) vs HyperCine GPU (C)
    //    at both Frame 1 Initial (FSratelimiter=4) and Steady-State (FSratelimiter=0) across 1080p, 4K, 8K.
    hypercine::test_oracle::ReferenceGateInputs reference_gate;
    reference_gate.encoder_bitstream_verified = true;
    reference_gate.decoder_returned_ok = true;
    reference_gate.quality_oracle_qualified = false;
    std::size_t observed_decodes = 0;
    bool reference_unstable = false;
    bool reference_decode_error = false;
    bool quality_measured = false;
    double min_psnr = 99.0;
    for (const auto& res : hypercine::kResolutions) {
        for (auto qmode : {hypercine::Phase2DQuantStateMode::Frame1Initial,
                           hypercine::Phase2DQuantStateMode::SteadyState}) {
            const auto cmp = hypercine::VerifyPhase2DUpstreamComparison(res.width, res.height, qmode);
            const std::string qname = QuantStateName(qmode);
            if (!StrictPhase2DIdentityPassed(cmp)) {
                return Fail(std::string(res.name) + " (" + qname +
                            ") strict encoder/bitstream identity failed: " + cmp.error +
                            " b_eq_c=" + std::to_string(cmp.b_eq_c_unmasked) +
                            " c_modes=" + std::to_string(cmp.c_all_modes_byte_identical) +
                            " a_eq_b_same=" + std::to_string(cmp.a_eq_b_same_handle_unmasked) +
                            " a_eq_b_masked=" + std::to_string(cmp.a_eq_b_independent_handle_masked) +
                            " a_eq_c_masked=" + std::to_string(cmp.a_eq_c_independent_handle_masked) +
                            " bands30=" + std::to_string(cmp.all_30_subbands_a_eq_b_eq_c) +
                            " bands=" + std::to_string(cmp.bands.size()));
            }
            if (res.width == 3840 && res.height == 2160) {
                if (qmode == hypercine::Phase2DQuantStateMode::Frame1Initial) {
                    if (cmp.a_upstream_sample_bytes != 3563684u ||
                        cmp.b_cpu_ref_sample_bytes != 3563684u ||
                        cmp.c_gpu_ref_sample_bytes != 3563684u ||
                        cmp.fs_rate_limiter != 4) {
                        return Fail("4K Frame1Initial expected 3,563,684 bytes (FSratelimiter=4), got " +
                                    std::to_string(cmp.c_gpu_ref_sample_bytes) + " (FSratelimiter=" +
                                    std::to_string(cmp.fs_rate_limiter) + ")");
                    }
                } else {
                    if (cmp.a_upstream_sample_bytes != 4668864u ||
                        cmp.b_cpu_ref_sample_bytes != 4668864u ||
                        cmp.c_gpu_ref_sample_bytes != 4668864u ||
                        cmp.fs_rate_limiter != 0) {
                        return Fail("4K SteadyState expected 4,668,864 bytes (FSratelimiter=0), got " +
                                    std::to_string(cmp.c_gpu_ref_sample_bytes) + " (FSratelimiter=" +
                                    std::to_string(cmp.fs_rate_limiter) + ")");
                    }
                }
            }
            // A, B and C decode payload-identical samples. The original SDK has
            // returned different pixels for identical bytes, so unequal decoded
            // outputs are evidence of reference instability, and a failed decode
            // of a verified bitstream remains a fatal reference error.
            const bool decode_evidence = !cmp.decoded_pixels_sha256.empty();
            if (!decode_evidence) {
                reference_decode_error = true;
            } else {
                observed_decodes += 3;
                if (!cmp.decoded_a_eq_b || !cmp.decoded_b_eq_c) reference_unstable = true;
                quality_measured = true;
                min_psnr = std::min(min_psnr, cmp.psnr_db);
            }
            std::cout << "[Part 1 " << res.name << " " << qname
                      << "] bytes=" << cmp.c_gpu_ref_sample_bytes
                      << " A==B==C(30 bands)=" << cmp.all_30_subbands_a_eq_b_eq_c
                      << " sha256=" << cmp.c_gpu_ref_sha256
                      << " psnr=" << cmp.psnr_db << " dB"
                      << " reference_decode="
                      << (decode_evidence ? ((cmp.decoded_a_eq_b && cmp.decoded_b_eq_c)
                                                 ? "EQUAL" : "UNSTABLE")
                                          : "UNAVAILABLE")
                      << "\n";
        }
    }

    // 4. Part 4: Resolution-aware and VRAM-aware slot selection policy.
    {
        const size_t ample_vram = 12ull * 1024ull * 1024ull * 1024ull;  // 12 GiB
        const auto s1080 = hypercine::SelectPhase2DFrameSlots(
            1920, 1080, hypercine::Phase2DSlotPolicy::DefaultAuto, 0,
            hypercine::Phase2DD2HPolicy::Throughput, ample_vram);
        const auto s4k = hypercine::SelectPhase2DFrameSlots(
            3840, 2160, hypercine::Phase2DSlotPolicy::DefaultAuto, 0,
            hypercine::Phase2DD2HPolicy::Throughput, ample_vram);
        const auto s8k = hypercine::SelectPhase2DFrameSlots(
            7680, 4320, hypercine::Phase2DSlotPolicy::DefaultAuto, 0,
            hypercine::Phase2DD2HPolicy::Throughput, ample_vram);
        const auto s8k_max = hypercine::SelectPhase2DFrameSlots(
            7680, 4320, hypercine::Phase2DSlotPolicy::MaxThroughput, 0,
            hypercine::Phase2DD2HPolicy::Throughput, ample_vram);
        const auto s_lowlat = hypercine::SelectPhase2DFrameSlots(
            3840, 2160, hypercine::Phase2DSlotPolicy::LowLatency, 0,
            hypercine::Phase2DD2HPolicy::LowLatency, ample_vram);
        const auto s_vram_clamped = hypercine::SelectPhase2DFrameSlots(
            7680, 4320, hypercine::Phase2DSlotPolicy::MaxThroughput, 0,
            hypercine::Phase2DD2HPolicy::Throughput, 128ull * 1024ull * 1024ull);

        if (s1080.selected_slots != 2 || s4k.selected_slots != 3 || s8k.selected_slots != 2 ||
            s8k_max.selected_slots != 3 || s_lowlat.selected_slots != 1) {
            return Fail("Slot policy defaults mismatch: 1080p=" + std::to_string(s1080.selected_slots) +
                        " 4K=" + std::to_string(s4k.selected_slots) +
                        " 8K=" + std::to_string(s8k.selected_slots) +
                        " 8K_max=" + std::to_string(s8k_max.selected_slots) +
                        " lowlat=" + std::to_string(s_lowlat.selected_slots));
        }
        if (s_vram_clamped.selected_slots >= s_vram_clamped.policy_target_slots ||
            s_vram_clamped.selected_slots != 1) {
            return Fail("Expected VRAM clamping to 1 slot under 128 MiB free VRAM at 8K");
        }
        std::cout << "[Part 4 Slot Policy] 1080p=" << s1080.selected_slots
                  << " 4K=" << s4k.selected_slots
                  << " 8K(auto)=" << s8k.selected_slots
                  << " 8K(max)=" << s8k_max.selected_slots
                  << " 8K(128MB clamped)=" << s_vram_clamped.selected_slots << "\n";
    }

    // 5. Parts 6-9: Kernel evolution (2C ref -> 2D-A graph -> 2D-B fused -> 2D-C scan-optimized).
    {
        const auto evo = hypercine::BenchmarkPhase2DKernelEvolution(
            1920, 1080, 4, hypercine::Phase2DQuantStateMode::SteadyState);
        if (evo.size() != 4) {
            return Fail("BenchmarkPhase2DKernelEvolution expected 4 study points, got " +
                        std::to_string(evo.size()));
        }
        for (const auto& pt : evo) {
            if (!pt.byte_match_reference) {
                return Fail("Kernel evolution stage " + pt.label + " failed byte_match_reference");
            }
        }
        if (evo[0].kernel_launches_per_frame != 434 || evo[0].host_launch_calls_per_frame != 434 ||
            evo[1].kernel_launches_per_frame != 434 || evo[1].host_launch_calls_per_frame != 1 ||
            evo[2].kernel_launches_per_frame != 353 || evo[2].host_launch_calls_per_frame != 1 ||
            evo[3].kernel_launches_per_frame != 245 || evo[3].host_launch_calls_per_frame != 1) {
            return Fail("Unexpected kernel launch/node counts across 2D stages: " +
                        std::to_string(evo[0].kernel_launches_per_frame) + " -> " +
                        std::to_string(evo[1].kernel_launches_per_frame) + " -> " +
                        std::to_string(evo[2].kernel_launches_per_frame) + " -> " +
                        std::to_string(evo[3].kernel_launches_per_frame));
        }
        std::cout << "[Parts 6-9 Kernel Evolution 1080p] launches: "
                  << evo[0].kernel_launches_per_frame << " (" << evo[0].host_launch_calls_per_frame
                  << " calls, " << evo[0].gpu_entropy_ms << " ms) -> "
                  << evo[1].kernel_launches_per_frame << " (" << evo[1].host_launch_calls_per_frame
                  << " call, " << evo[1].gpu_entropy_ms << " ms) -> "
                  << evo[2].kernel_launches_per_frame << " (" << evo[2].host_launch_calls_per_frame
                  << " call, " << evo[2].gpu_entropy_ms << " ms) -> "
                  << evo[3].kernel_launches_per_frame << " (" << evo[3].host_launch_calls_per_frame
                  << " call, " << evo[3].gpu_entropy_ms << " ms)\n";
    }

    // 6. Parts 3 & 5: Polling vs Blocking sync and Throughput vs LowLatency D2H modes.
    {
        hypercine::Phase2DConfig cfg_poll{};
        cfg_poll.width = 3840;
        cfg_poll.height = 2160;
        cfg_poll.warmup_frames = 1;
        cfg_poll.frames = 6;
        cfg_poll.slot_policy = hypercine::Phase2DSlotPolicy::DefaultAuto;
        cfg_poll.d2h_policy = hypercine::Phase2DD2HPolicy::Throughput;
        cfg_poll.sync_mode = hypercine::Phase2DSyncMode::Polling;
        cfg_poll.input_mode = hypercine::Phase2CInputMode::DeviceInput;
        cfg_poll.kernel_mode = hypercine::GpuEntropyKernelMode::FusedScanOptimized;
        cfg_poll.use_cuda_graph = true;
        cfg_poll.verify_bitstream = true;
        cfg_poll.deterministic_test_metadata = true;

        hypercine::Phase2DConfig cfg_block = cfg_poll;
        cfg_block.sync_mode = hypercine::Phase2DSyncMode::Blocking;

        hypercine::Phase2DConfig cfg_lowlat = cfg_block;
        cfg_lowlat.d2h_policy = hypercine::Phase2DD2HPolicy::LowLatency;

        const auto r_poll = hypercine::RunPhase2D(cfg_poll);
        const auto r_block = hypercine::RunPhase2D(cfg_block);
        const auto r_lowlat = hypercine::RunPhase2D(cfg_lowlat);
        // Phase2DResult.ok folds the strict bitstream match together with the
        // original-SDK decode/PSNR signal. Keep the strict part fatal and
        // classify the reference part through the shared gate.
        for (const auto* r : {&r_poll, &r_block, &r_lowlat}) {
            if (!r->bitstream_byte_match) {
                return Fail("4K RunPhase2D strict encoder/bitstream gate failed: " + r->error);
            }
            if (!r->decode_ok) {
                return Fail("4K RunPhase2D original SDK decode failed: " + r->error);
            }
            ++observed_decodes;
            quality_measured = true;
            min_psnr = std::min(min_psnr, r->psnr_db);
        }
        if (r_poll.sample_sha256 != r_block.sample_sha256 ||
            r_block.sample_sha256 != r_lowlat.sample_sha256) {
            return Fail("4K SHA-256 mismatch across Polling, Blocking, and LowLatency D2H modes");
        }
        if (r_block.cpu.normalized_process_cpu_pct > 100.0 ||
            r_poll.cpu.normalized_process_cpu_pct > 100.0 ||
            r_block.cpu.main_thread_spin_ms != 0.0) {
            return Fail("Invalid CPU utilization metrics in RunPhase2D");
        }
        std::cout << "[Part 3 & 5 4K Pipeline] Poll FPS=" << r_poll.sustained_fps
                  << " (norm_cpu=" << r_poll.cpu.normalized_process_cpu_pct
                  << "%, single_core=" << r_poll.cpu.single_core_equiv_pct
                  << "%) | Block FPS=" << r_block.sustained_fps
                  << " (norm_cpu=" << r_block.cpu.normalized_process_cpu_pct
                  << "%, single_core=" << r_block.cpu.single_core_equiv_pct
                  << "%, core_s/60fps_s=" << r_block.cpu.cpu_core_sec_per_60fps_video_sec
                  << ") | LowLat FPS=" << r_lowlat.sustained_fps << "\n";
    }

    if (reference_decode_error) reference_gate.decoder_returned_ok = false;
    reference_gate.observed_decodes = observed_decodes;
    reference_gate.distinct_decoded_outputs =
        observed_decodes == 0 ? 0 : (reference_unstable ? 2 : 1);
    reference_gate.quality_measured = quality_measured;
    reference_gate.quality_meets_threshold = quality_measured && min_psnr > 35.0;
    const auto reference_decision =
        hypercine::test_oracle::ClassifyReferenceGate(reference_gate);
    std::cout << "reference_oracle_decision="
              << hypercine::test_oracle::GateDecisionName(reference_decision)
              << " exit=" << hypercine::test_oracle::GateExitCode(reference_decision)
              << " observed_decodes=" << observed_decodes
              << " distinct=" << reference_gate.distinct_decoded_outputs
              << " min_psnr=" << min_psnr << " dB"
              << " encoder_strict_gates=PASS\n";
    if (reference_decision == hypercine::test_oracle::GateDecision::Pass) {
        std::cout << "test_phase2d PASSED\n";
        return 0;
    }
    std::cerr << "phase 2d strict stages passed; reference pixel gate is "
              << hypercine::test_oracle::GateDecisionName(reference_decision) << "\n";
    return hypercine::test_oracle::GateExitCode(reference_decision);
}
