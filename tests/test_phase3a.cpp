// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/phase3a.h"
#include "hypercine/canonical.h"

#include <cstring>
#include <iostream>
#include <string>

namespace {

int Fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    bool quick_sanitize = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick-sanitize") == 0) {
            quick_sanitize = true;
        }
    }

    // 1. Default config check: deterministic_test_metadata must be false in production configs.
    {
        hypercine::Phase3AConfig c3a{};
        if (c3a.deterministic_test_metadata) {
            return Fail("Phase3AConfig::deterministic_test_metadata must default to false");
        }
        if (c3a.input_mode != hypercine::Phase3AInputMode::HostPageableEndToEnd) {
            return Fail("Phase3AConfig::input_mode must default to HostPageableEndToEnd");
        }
        if (c3a.quant_mode != hypercine::Phase3AQuantMode::DynamicUpstreamState) {
            return Fail("Phase3AConfig::quant_mode must default to DynamicUpstreamState");
        }
        if (c3a.encoding_quality != hypercine::EncodingQuality::FilmScan3) {
            return Fail("Phase3AConfig must retain the existing FilmScan3 default");
        }
    }

    // 2. Test 4: Multi-Frame Production Metadata Sequence Test (16 frames).
    {
        const int meta_frames = quick_sanitize ? 6 : 16;
        const int mw = quick_sanitize ? 640 : 1920;
        const int mh = quick_sanitize ? 360 : 1080;
        const auto meta = hypercine::VerifyPhase3AMetadataSequence(mw, mh, meta_frames);
        if (!meta.ok) {
            return Fail("VerifyPhase3AMetadataSequence failed: " + meta.error);
        }
        if (!meta.clip1_guid_non_zero || !meta.clip1_guid_constant_across_frames ||
            !meta.clip2_guid_distinct_from_clip1 || !meta.dates_and_times_valid ||
            !meta.unique_framenum_monotonic_0_to_n || !meta.timecode_progresses ||
            !meta.all_frames_match_upstream_masked || meta.clip1_frames.empty()) {
            return Fail("VerifyPhase3AMetadataSequence invariant failed");
        }
        std::cout << "[Phase 3A Test 4: Multi-Frame Metadata Sequence (" << meta.num_frames << " frames)] "
                  << "clip1_guid=" << meta.clip1_guid_hex
                  << " clip2_guid=" << meta.clip2_guid_hex
                  << " date=" << meta.clip1_frames.front().encode_date
                  << " " << meta.clip1_frames.front().encode_time
                  << " tc=" << meta.clip1_frames.front().timecode
                  << ".." << meta.clip1_frames.back().timecode
                  << " masked_upstream_eq=" << meta.all_frames_match_upstream_masked << std::endl;
    }

    // 3. Test 3: Adversarial Entropy & Edge-Case Verification (all 17 cases across subbands).
    {
        const auto adv = hypercine::VerifyPhase3AAdversarialEntropy();
        if (!adv.ok) {
            return Fail("VerifyPhase3AAdversarialEntropy failed: " + adv.error);
        }
        if (adv.total_cases_run < 17 || adv.total_cases_passed != adv.total_cases_run) {
            return Fail("Adversarial entropy cases mismatch: passed=" +
                        std::to_string(adv.total_cases_passed) + "/" +
                        std::to_string(adv.total_cases_run));
        }
        std::cout << "[Phase 3A Test 3: Adversarial Entropy] "
                  << adv.total_cases_passed << "/" << adv.total_cases_run << " pathological cases passed "
                  << "(Upstream == CPU_Staged == GPU_Ref == GPU_Fused == GPU_ScanOpt == GPU_Graph)" << std::endl;
    }

    // 4. Test 1: 120-Frame Sequence Equivalence Test (A: Upstream vs B: CPU Ref vs C: GPU Multi-Slot).
    {
        const int seq_frames = quick_sanitize ? 16 : 120;
        const int sw = quick_sanitize ? 640 : 1920;
        const int sh = quick_sanitize ? 360 : 1080;
        const auto seq = hypercine::VerifyPhase3ASequence120(sw, sh, seq_frames);
        if (!seq.ok || seq.matched_frames != seq.total_frames) {
            return Fail("VerifyPhase3ASequence120 failed (" +
                        std::to_string(seq.matched_frames) + "/" +
                        std::to_string(seq.total_frames) + "): " + seq.error);
        }
        if (seq.distinct_fs_rate_limiters_observed < 2 || seq.quant_transitions_observed < 2) {
            return Fail("Expected dynamic FSratelimiter transitions in changing-complexity sequence, got distinct=" +
                        std::to_string(seq.distinct_fs_rate_limiters_observed) +
                        " transitions=" + std::to_string(seq.quant_transitions_observed));
        }
        std::cout << "[Phase 3A Test 1: " << seq_frames << "-Frame Changing-Complexity Sequence] "
                  << "A==B==C across all " << seq.total_frames << " frames, "
                  << "distinct_FSratelimiters=" << seq.distinct_fs_rate_limiters_observed
                  << " quant_transitions=" << seq.quant_transitions_observed << std::endl;
    }

    // 4b. Every regular SDK quality must drive the complete upstream, CPU and
    // CUDA pipeline. The first 16 deterministic archetypes include flat,
    // gradient, edge/detail and seeded-noise inputs at 1080p.
    if (!quick_sanitize) {
        const hypercine::EncodingQuality qualities[] = {
            hypercine::EncodingQuality::Low, hypercine::EncodingQuality::Medium,
            hypercine::EncodingQuality::High, hypercine::EncodingQuality::FilmScan1,
            hypercine::EncodingQuality::FilmScan2, hypercine::EncodingQuality::FilmScan3};
        for (const auto quality : qualities) {
            const auto matrix = hypercine::VerifyPhase3ASequence120(
                1920, 1080, 16, 0x1337BEEFu, {}, quality);
            if (!matrix.ok || matrix.matched_frames != 16) {
                return Fail(std::string("quality matrix failed for ") +
                    hypercine::EncodingQualityName(quality) + ": " + matrix.error +
                    " (" + std::to_string(matrix.matched_frames) + "/16 frames)");
            }
            std::cout << "[Quality Matrix " << hypercine::EncodingQualityName(quality)
                      << "] upstream == CPU == CUDA, " << matrix.matched_frames
                      << "/16 frames" << std::endl;
        }
        const auto filmscan2_sequence = hypercine::VerifyPhase3ASequence120(
            1920, 1080, 120, 0x1337BEEFu, {}, hypercine::EncodingQuality::FilmScan2);
        if (!filmscan2_sequence.ok || filmscan2_sequence.matched_frames != 120 ||
            filmscan2_sequence.distinct_fs_rate_limiters_observed < 2) {
            return Fail("FilmScan2 120-frame rate-control sequence failed: " +
                filmscan2_sequence.error + " (distinct FSratelimiters=" +
                std::to_string(filmscan2_sequence.distinct_fs_rate_limiters_observed) + ")");
        }
    }

    // 5. Test 2: 300-Frame Distinct-Frame Multi-Slot Pipeline Test (plus 4K and 8K slot wrap).
    {
        const int d_frames_1080p = quick_sanitize ? 12 : 300;
        const int dw = quick_sanitize ? 640 : 1920;
        const int dh = quick_sanitize ? 360 : 1080;
        const auto dist1080 = hypercine::VerifyPhase3ADistinctFrames300(
            dw, dh, d_frames_1080p, hypercine::Phase3AInputMode::HostPageableEndToEnd,
            hypercine::Phase2DSlotPolicy::DefaultAuto);
        if (!dist1080.ok) {
            return Fail("VerifyPhase3ADistinctFrames300 (1080p) failed: " + dist1080.error);
        }
        if (!dist1080.strict_monotonic_indices || !dist1080.no_duplicates_or_missing ||
            !dist1080.no_stale_slot_reuse ||
            dist1080.decoded_stamp_matched_count != dist1080.decoded_checked_count ||
            dist1080.min_psnr_db <= 35.0) {
            return Fail("VerifyPhase3ADistinctFrames300 (1080p) invariant failed");
        }
        std::cout << "[Phase 3A Test 2: " << d_frames_1080p << "-Frame Distinct Multi-Slot] "
                  << "slots=" << dist1080.num_slots
                  << " stamps_verified=" << dist1080.decoded_stamp_matched_count << "/"
                  << dist1080.decoded_checked_count
                  << " min_psnr=" << dist1080.min_psnr_db << " dB" << std::endl;

        if (!quick_sanitize) {
            // Also verify multi-slot wrap & distinctness at 4K (24 frames) and 8K (8 frames).
            const auto dist4k = hypercine::VerifyPhase3ADistinctFrames300(
                3840, 2160, 24, hypercine::Phase3AInputMode::HostPageableEndToEnd,
                hypercine::Phase2DSlotPolicy::DefaultAuto);
            if (!dist4k.ok) {
                return Fail("VerifyPhase3ADistinctFrames300 (4K 24 frames) failed: " + dist4k.error);
            }
            std::cout << "[Phase 3A Test 2 (4K 24-Frame Slot Wrap)] slots=" << dist4k.num_slots
                      << " stamps_verified=" << dist4k.decoded_stamp_matched_count << "/"
                      << dist4k.decoded_checked_count
                      << " min_psnr=" << dist4k.min_psnr_db << " dB" << std::endl;

            const auto dist8k = hypercine::VerifyPhase3ADistinctFrames300(
                7680, 4320, 8, hypercine::Phase3AInputMode::HostPageableEndToEnd,
                hypercine::Phase2DSlotPolicy::DefaultAuto);
            if (!dist8k.ok) {
                return Fail("VerifyPhase3ADistinctFrames300 (8K 8 frames) failed: " + dist8k.error);
            }
            std::cout << "[Phase 3A Test 2 (8K 8-Frame Slot Wrap)] slots=" << dist8k.num_slots
                      << " stamps_verified=" << dist8k.decoded_stamp_matched_count << "/"
                      << dist8k.decoded_checked_count
                      << " min_psnr=" << dist8k.min_psnr_db << " dB" << std::endl;
        }
    }

    // 6. Test 5: Input Modes (HostPageableEndToEnd, HostPinnedApi, DeviceInput) & CUDA Graph A/B on dynamic quant sequence.
    {
        const int gw = quick_sanitize ? 640 : 1920;
        const int gh = quick_sanitize ? 360 : 1080;
        const int g_frames = quick_sanitize ? 8 : 24;
        const auto ab = hypercine::RunPhase3ACudaGraphABStudy(gw, gh, g_frames, quick_sanitize ? 1 : 2);
        if (!ab.ok || !ab.all_frames_byte_identical) {
            return Fail("RunPhase3ACudaGraphABStudy failed byte-identity across dynamic quant sequence: " + ab.error);
        }
        std::cout << "[Phase 3A Test 5: CUDA Graph vs Stream A/B on Dynamic Sequence] "
                  << "byte_identical=" << ab.all_frames_byte_identical
                  << " stream_fps=" << ab.graph_off_mean_fps
                  << " graph_fps=" << ab.graph_on_mean_fps << std::endl;

        // Verify HostPageableEndToEnd vs HostPinnedApi vs DeviceInput produce identical bitstreams across a multi-frame sequence.
        hypercine::Phase3AConfig cfg{};
        cfg.width = gw;
        cfg.height = gh;
        cfg.warmup_frames = 2;
        cfg.frames = g_frames;
        cfg.deterministic_test_metadata = true;
        cfg.retain_frame_records = true;

        cfg.input_mode = hypercine::Phase3AInputMode::HostPageableEndToEnd;
        const auto r_pageable = hypercine::RunPhase3A(cfg);
        cfg.input_mode = hypercine::Phase3AInputMode::HostPinnedApi;
        const auto r_pinned = hypercine::RunPhase3A(cfg);
        cfg.input_mode = hypercine::Phase3AInputMode::DeviceInput;
        const auto r_device = hypercine::RunPhase3A(cfg);

        if (!r_pageable.ok || !r_pinned.ok || !r_device.ok ||
            r_pageable.frame_records.size() != static_cast<size_t>(g_frames) ||
            r_pinned.frame_records.size() != static_cast<size_t>(g_frames) ||
            r_device.frame_records.size() != static_cast<size_t>(g_frames)) {
            return Fail("Input mode sequence run failed");
        }
        for (size_t i = 0; i < r_pageable.frame_records.size(); ++i) {
            if (r_pageable.frame_records[i].sample_sha256 != r_pinned.frame_records[i].sample_sha256 ||
                r_pageable.frame_records[i].sample_sha256 != r_device.frame_records[i].sample_sha256) {
                return Fail("Input mode SHA-256 mismatch at frame " + std::to_string(i));
            }
        }
        std::cout << "[Phase 3A Input Modes Equivalence] HostPageableEndToEnd == HostPinnedApi == DeviceInput "
                  << "across all " << g_frames << " frames (last_sha256="
                  << r_pageable.frame_records.back().sample_sha256 << ")" << std::endl;
    }

    std::cout << "PASS: All Phase 3A sequence correctness, adversarial entropy, and metadata tests succeeded." << std::endl;
    return 0;
}
