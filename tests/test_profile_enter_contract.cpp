// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Issue #46: the six shared profile tables must enter FilmScan3Encoder through
// the single SDK-free calculator (#29), never through a second local table set.
// This suite pins that seam and the fail-closed profile boundary.
#include "hypercine/filmscan3.h"
#include "hypercine/quality_state.h"
#include "../src/core/quality_profile_bridge.h"
#include "reference_frame_generator.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hypercine;
using namespace hypercine::standalone;

namespace {
void Require(bool value, const char* label) {
    if (!value) throw std::runtime_error(label);
}
template <class F> void Reject(F f, const char* label) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, label);
}

// Independent statement of the frozen FilmScan3 rate feedback, copied from the
// pre-#46 encoder. It must agree with the shared calculator for every reachable
// state, otherwise the encoder's cross-check would fire on real input.
int FrozenLimiter(int limiter, std::uint64_t previous_bytes, std::size_t pixels) {
    if (previous_bytes) {
        float compression = static_cast<float>(pixels * 3 * 10 / 8) /
                            static_cast<float>(previous_bytes);
        compression = static_cast<float>(compression / 1.5);
        if (compression > 4.5f) {
            --limiter; if (compression > 5.5f) --limiter; if (compression > 6.5f) limiter -= 2;
        } else if (compression < 3.0f) {
            ++limiter; if (compression < 2.5f) ++limiter;
            if (compression < 2.0f) ++limiter; if (compression < 1.5f) limiter += 2;
        }
    }
    return limiter < 0 ? 0 : (limiter > 20 ? 20 : limiter);
}
}  // namespace

int main() {
    try {
        // 1. Every legal limiter resolves to the frozen FilmScan3 table pair.
        // This is the actual "where do the six tables enter" contract: the
        // shared calculator is the producer, and the frozen helper is the
        // reference it must reproduce exactly. No prior sample means the rate
        // feedback is identity, so the input limiter is the one under test
        // rather than a feedback-derived one.
        for (int limiter = 0; limiter <= 20; ++limiter) {
            ProfileTableRequest request;
            request.quality = EncodingQuality::FilmScan3;
            request.fs_rate_limiter = limiter;
            request.frame_index = 1;
            request.previous_completed_sample_bytes = 0;
            const auto result = ComputeProfileTables(request);
            Require(result.fs_rate_limiter == limiter,
                    "rate feedback must be identity without a prior sample");
            const auto frozen = ComputeFilmScan3Tables(limiter);
            for (unsigned i = 0; i < 36; ++i) {
                Require(result.tables.quant[i] == frozen.quant[i],
                        "quant diverged from frozen FilmScan3 table");
                Require(result.tables.scale[i] == frozen.scale[i],
                        "scale diverged from frozen FilmScan3 table");
            }
        }

        // 2. The writer's tables come from the shared calculator, and the frozen
        // helper is only its oracle. A deliberately drifted pair must trip the
        // guard, so the cross-check is provably live rather than decorative.
        {
            const auto frozen = ComputeFilmScan3Tables(4);
            std::array<int, 36> quant{};
            std::array<int, 36> scale{};
            for (unsigned i = 0; i < 36; ++i) {
                quant[i] = static_cast<int>(frozen.quant[i]);
                scale[i] = static_cast<int>(frozen.scale[i]);
            }
            RequireTablesMatchFrozen(quant, scale, 4);  // identical pair passes
            quant[17] += 1;
            Reject([&] { RequireTablesMatchFrozen(quant, scale, 4); },
                   "drifted quant slot was not reported");
            quant[17] -= 1;
            scale[31] += 1;
            Reject([&] { RequireTablesMatchFrozen(quant, scale, 4); },
                   "drifted scale slot was not reported");
            scale[31] -= 1;
            RequireTablesMatchFrozen(quant, scale, 4);  // restored pair passes
            std::array<int, 36> negative = quant;
            negative[0] = -1;
            Reject([&] { RequireTablesMatchFrozen(negative, scale, 4); },
                   "negative shared entry was accepted");
        }

        // 1b. The returned pair is the shared calculator's own output, not a
        // value rebuilt elsewhere: recompute CalculateQualityState here and
        // require the seam to hand back exactly those entries. The shared
        // calculator's validation must also govern this path, so a request it
        // rejects cannot be answered from the frozen limiter-only helper.
        for (int limiter : {0, 4, 9, 16, 20}) {
            ProfileTableRequest request;
            request.fs_rate_limiter = limiter;
            request.frame_index = 3;
            request.previous_completed_sample_bytes = 0;
            const auto result = ComputeProfileTables(request);
            QualityStateInput input;
            input.quality = EncodingQuality::FilmScan3;
            input.coding_width = request.width;
            input.coding_height = request.height;
            input.encoded_precision = 10;
            input.frame_rate_numerator = 30;
            input.frame_rate_denominator = 1;
            input.gop_length = 1;
            input.full_resolution_chroma = false;
            input.frame_index = request.frame_index;
            input.previous_fs_rate_limiter = request.fs_rate_limiter;
            input.previous_overbitrate = 0;
            input.previous_completed_sample_bytes = request.previous_completed_sample_bytes;
            input.prescale = {{0, 0, 0}};
            const auto shared = CalculateQualityState(input);
            Require(shared.ok, "shared calculator rejected a legal request");
            Require(result.fs_rate_limiter == shared.fs_rate_limiter,
                    "seam did not return the shared limiter");
            for (unsigned i = 0; i < 36; ++i) {
                Require(result.tables.quant[i] == static_cast<unsigned>(shared.quant[i]),
                        "seam did not return the shared quant entry");
                Require(result.tables.scale[i] == static_cast<unsigned>(shared.scale[i]),
                        "seam did not return the shared scale entry");
            }
        }
        {
            ProfileTableRequest odd;
            odd.width = 1921;
            Reject([&] { ComputeProfileTables(odd); },
                   "shared calculator's odd-width rejection did not govern the seam");
            ProfileTableRequest zero_height;
            zero_height.height = 0;
            Reject([&] { ComputeProfileTables(zero_height); },
                   "shared calculator's zero-height rejection did not govern the seam");
        }

        // 3. Rate feedback through the shared calculator equals the frozen
        // formula across the whole reachable window, including the clamps.
        for (std::uint64_t prior = 0; prior <= 4000000; prior += 7919) {
            for (int start : {0, 4, 8, 16, 20}) {
                ProfileTableRequest request;
                request.quality = EncodingQuality::FilmScan3;
                request.fs_rate_limiter = start;
                request.frame_index = 7;
                request.previous_completed_sample_bytes = prior;
                const auto shared = ComputeProfileTables(request);
                const auto frozen = FrozenLimiter(start, prior, 1920u * 1080u);
                Require(shared.fs_rate_limiter == frozen,
                        "shared rate feedback diverged from frozen FilmScan3 formula");
            }
        }

        // 4. Frame 0 seeds the profile limiter, not a carried-over value.
        {
            ProfileTableRequest request;
            request.quality = EncodingQuality::FilmScan3;
            request.frame_index = 0;
            request.fs_rate_limiter = 20;
            request.previous_completed_sample_bytes = 0;
            Require(ComputeProfileTables(request).fs_rate_limiter == 4,
                    "frame 0 must seed the FilmScan3 limiter");
        }

        // 5. The five unqualified profiles are refused, not mislabelled.
        for (EncodingQuality quality : {EncodingQuality::Low, EncodingQuality::Medium,
                                        EncodingQuality::High, EncodingQuality::FilmScan1,
                                        EncodingQuality::FilmScan2}) {
            Require(!IsQualifiedSessionProfile(quality),
                    "unqualified profile reported as qualified");
            Require(*SessionProfileRefusalReason(quality) != '\0',
                    "refused profile must state a reason");
            ProfileTableRequest request;
            request.quality = quality;
            Reject([&] { ComputeProfileTables(request); },
                   "unqualified profile produced tables");
            FilmScan3Encoder encoder;
            Reject([&] { encoder.SetSessionQuality(quality); },
                   "encoder accepted an unqualified profile");
        }
        Require(IsQualifiedSessionProfile(EncodingQuality::FilmScan3),
                "FilmScan3 must be qualified");
        Require(*SessionProfileRefusalReason(EncodingQuality::FilmScan3) == '\0',
                "FilmScan3 must not carry a refusal reason");

        // 6. Invalid enum values are rejected, including the boundary values.
        for (int raw : {0, 7, 8, 255}) {
            const auto quality = static_cast<EncodingQuality>(raw);
            ProfileTableRequest request;
            request.quality = quality;
            Reject([&] { ComputeProfileTables(request); },
                   "invalid quality profile produced tables");
            FilmScan3Encoder encoder;
            Reject([&] { encoder.SetSessionQuality(quality); },
                   "encoder accepted an invalid quality profile");
        }

        // 7. An explicit FilmScan3 session is byte-identical to the default
        // session, and the default stays FilmScan3.
        {
            std::vector<uint8_t> bgra(1920u * 1080u * 4, 0);
            reference_fixture::GeneratePhase3AFrame(
                1920, 1080, 0, reference_fixture::Phase3APattern::FlatBlack,
                bgra.data(), 7680, 0x1337beef);
            FilmScan3Encoder plain;
            FilmScan3Encoder explicit_fs3;
            explicit_fs3.SetSessionQuality(EncodingQuality::FilmScan3);
            Require(plain.session_quality() == EncodingQuality::FilmScan3,
                    "default session profile changed");
            const auto a = plain.Encode(bgra.data(), bgra.size(), 7680);
            const auto b = explicit_fs3.Encode(bgra.data(), bgra.size(), 7680);
            Require(a.sample == b.sample, "explicit FilmScan3 session changed bytes");
            Require(a.sample.size() == 185648, "deterministic reference sample size changed");
            Require(a.tables.quant == b.tables.quant && a.tables.scale == b.tables.scale,
                    "explicit FilmScan3 session changed tables");
        }

        // 8. Reset keeps the session profile: it is a session property, not a
        // per-frame one, and rate state resets underneath it.
        {
            FilmScan3Encoder encoder;
            encoder.SetSessionQuality(EncodingQuality::FilmScan3);
            encoder.Reset();
            Require(encoder.session_quality() == EncodingQuality::FilmScan3,
                    "Reset dropped the session profile");
            Require(encoder.state().fs_rate_limiter == 4,
                    "Reset must restore the FilmScan3 seed limiter");
        }

        std::cout << "profile_enter_contract: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "profile_enter_contract: FAIL: " << error.what() << "\n";
        return 1;
    }
}
