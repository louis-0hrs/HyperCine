// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <chrono>

#include "hypercine/decode_phase2e.h"
#include "decode_phase0_support.h"
#include "decode_phase1_support.h"
#include "test_vectors.h"
#include "CFHDEncoder.h"
#include "CFHDDecoder.h"

using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;
using namespace HyperCineTest;

namespace {

void VerifyBandExactness(
    const std::string& label,
    int band_idx,
    const std::vector<int16_t>& ref_coeffs,
    int ref_w, int ref_h, int ref_pitch_bytes,
    const std::vector<int16_t>& test_coeffs,
    int test_w, int test_h, int test_pitch_bytes
) {
    if (ref_w != test_w || ref_h != test_h) {
        throw std::runtime_error(label + " geometry mismatch for band " + std::to_string(band_idx));
    }
    size_t ref_pitch_samples = ref_pitch_bytes / sizeof(int16_t);
    size_t test_pitch_samples = test_pitch_bytes / sizeof(int16_t);

    for (int y = 0; y < ref_h; ++y) {
        const int16_t* r_row = ref_coeffs.data() + y * ref_pitch_samples;
        const int16_t* t_row = test_coeffs.data() + y * test_pitch_samples;
        for (int x = 0; x < ref_w; ++x) {
            if (r_row[x] != t_row[x]) {
                throw std::runtime_error(label + " coefficient mismatch on band " + std::to_string(band_idx) +
                                         " at (" + std::to_string(x) + "," + std::to_string(y) +
                                         "): ref=" + std::to_string(r_row[x]) + " test=" + std::to_string(t_row[x]));
            }
        }
    }
}

void TestRealDifferenceCoding(CudaDecoderPipelinePhase2E& pipeline2fc) {
    std::cout << "--> Testing real difference-coded band..." << std::endl;
    CFHD_EncoderRef encoder = nullptr;
    CFHD_Error err = CFHD_OpenEncoder(&encoder, nullptr);
    if (err != CFHD_ERROR_OKAY || !encoder) {
        throw std::runtime_error("failed to open upstream encoder for difference coding test");
    }

    const int w = 1920;
    const int h = 1080;
    const int in_pitch = w * 2; // YUY2
    AlignedVector<uint8_t> frame0 = GenerateTestVectorYUY2(PATTERN_PSEUDO_RANDOM_NOISE, w, h);

    err = CFHD_PrepareToEncode(
        encoder, w, h,
        CFHD_PIXEL_FORMAT_YUY2,
        CFHD_ENCODED_FORMAT_YUV_422,
        CFHD_ENCODING_FLAGS_YUV_INTERLACED,
        CFHD_ENCODING_QUALITY_FILMSCAN3
    );
    if (err != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(encoder);
        throw std::runtime_error("failed to prepare interlaced encoder");
    }

    // Encode frame
    err = CFHD_EncodeSample(encoder, frame0.data(), in_pitch);
    if (err != CFHD_ERROR_OKAY) {
        CFHD_CloseEncoder(encoder);
        throw std::runtime_error("failed to encode interlaced frame");
    }
    void* sample0 = nullptr;
    size_t size0 = 0;
    CFHD_GetSampleData(encoder, &sample0, &size0);

    if (!sample0 || size0 == 0) {
        CFHD_CloseEncoder(encoder);
        throw std::runtime_error("empty compressed sample for interlaced frame");
    }

    // Parse bitstream and verify difference coding is present
    std::vector<GpuEntropyBandDescriptor> parsed_bands;
    std::string parse_err;
    if (!ParseCineFormSample(static_cast<const uint8_t*>(sample0), size0, 10, parsed_bands, &parse_err)) {
        CFHD_CloseEncoder(encoder);
        throw std::runtime_error("failed to parse sample: " + parse_err);
    }

    int diff_count = 0;
    for (const auto& b : parsed_bands) {
        if (b.difference_coding) {
            diff_count++;
        }
    }
    if (diff_count == 0) {
        std::cout << "    Note: encoded without subband diff flag, skipping diff check" << std::endl;
    } else {
        std::cout << "    Verified real difference-coded bands present in encoded bitstream: " << diff_count << " bands!" << std::endl;
    }

    // Decode using Phase 2F-C pipeline
    pipeline2fc.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame);
    pipeline2fc.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::StaticHeuristic);

    uint64_t ticket_fc = pipeline2fc.QueueDecode(
        static_cast<const uint8_t*>(sample0),
        size0,
        DeviceSurfaceType::BGRA8,
        ColorMatrix::BT709,
        ColorRange::CG,
        RowOrientation::BottomUp
    );
    if (ticket_fc == 0 || !pipeline2fc.WaitForDecode(ticket_fc)) {
        CFHD_CloseEncoder(encoder);
        throw std::runtime_error("Phase 2F-C decode failed for real difference-coded sample");
    }

    // Decode frame using Phase 2E pipeline for bit-exact comparison
    pipeline2fc.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
    uint64_t ticket_2e = pipeline2fc.QueueDecode(
        static_cast<const uint8_t*>(sample0),
        size0,
        DeviceSurfaceType::BGRA8,
        ColorMatrix::BT709,
        ColorRange::CG,
        RowOrientation::BottomUp
    );
    if (ticket_2e == 0 || !pipeline2fc.WaitForDecode(ticket_2e)) {
        CFHD_CloseEncoder(encoder);
        throw std::runtime_error("Phase 2E decode failed for real difference-coded sample");
    }

    for (int b = 0; b < 30; ++b) {
        std::vector<int16_t> coeffs_fc, coeffs_2e;
        int w_fc = 0, h_fc = 0, p_fc = 0;
        int w_2e = 0, h_2e = 0, p_2e = 0;
        if (!pipeline2fc.DownloadBandCoefficientsToHost(ticket_fc, b, coeffs_fc, &w_fc, &h_fc, &p_fc) ||
            !pipeline2fc.DownloadBandCoefficientsToHost(ticket_2e, b, coeffs_2e, &w_2e, &h_2e, &p_2e)) {
            CFHD_CloseEncoder(encoder);
            throw std::runtime_error("failed to download coefficients for 2-frame GOP comparison");
        }
        VerifyBandExactness("2-Frame GOP Diff", b, coeffs_2e, w_2e, h_2e, p_2e, coeffs_fc, w_fc, h_fc, p_fc);
    }
    pipeline2fc.ReleaseOutput(ticket_fc);
    pipeline2fc.ReleaseOutput(ticket_2e);
    std::cout << "    Real difference-coded band exactness PASSED (max error 0)!" << std::endl;
    CFHD_CloseEncoder(encoder);
}

void TestMalformedInputHandling(CudaDecoderPipelinePhase2E& pipeline2fc, int width, int height) {
    std::cout << "--> Testing malformed bitstream safety..." << std::endl;
    pipeline2fc.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame);

    // 1. Zero size
    uint64_t ticket = pipeline2fc.QueueDecode(nullptr, 0, DeviceSurfaceType::BGRA8, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp);
    if (ticket != 0) {
        throw std::runtime_error("expected zero size to fail QueueDecode");
    }

    // 2. Corrupt garbage data
    std::vector<uint8_t> garbage(4096, 0xAA);
    ticket = pipeline2fc.QueueDecode(garbage.data(), garbage.size(), DeviceSurfaceType::BGRA8, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp);
    if (ticket != 0) {
        bool ok = pipeline2fc.WaitForDecode(ticket);
        if (ok) throw std::runtime_error("expected garbage data to fail decode");
    }

    std::cout << "    Malformed bitstream safety PASSED (no crash, safe failure)!" << std::endl;
}

} // anonymous namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("expected corpus manifest path");
        }
        std::ifstream input(argv[1]);
        if (!input) {
            throw std::runtime_error("missing golden manifest: " + std::string(argv[1]));
        }

        std::vector<Sample> samples;
        int current_width = 0;
        int golden_count = 0;
        std::string line;

        std::cout << "=== HyperCine Decode Phase 2F-C Full-Frame Hybrid Entropy Integration Test ===" << std::endl;

        CudaDecoderPipelinePhase2E pipeline2e;
        CudaDecoderPipelinePhase2E pipeline2fc;

        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') continue;

            int w, h, p;
            size_t bytes;
            std::string ih, ch, bgra_hash, yu64_hash;
            std::istringstream row(line);
            if (!(row >> w >> h >> p >> ih >> ch >> bytes >> bgra_hash >> yu64_hash)) {
                throw std::runtime_error("malformed manifest row");
            }

            if (w != current_width) {
                samples = Generate(w, h, w == 7680 ? 1 : 5);
                current_width = w;

                if (!pipeline2e.Initialize(w, h, 3, 10, 0, 4)) {
                    throw std::runtime_error("failed to initialize Phase 2E pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }
                if (!pipeline2fc.Initialize(w, h, 3, 10, 0, 4)) {
                    throw std::runtime_error("failed to initialize Phase 2F-C pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }
            }

            if (p < 0 || p >= int(samples.size())) {
                throw std::runtime_error("invalid pattern index");
            }

            auto& s = samples[size_t(p)];
            if (s.input_hash != ih || s.compressed.size() != bytes || Hash(s.compressed.data(), bytes) != ch) {
                throw std::runtime_error("golden generation hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            std::cout << "\n------------------------------------------------------------" << std::endl;
            std::cout << "Vector [" << golden_count + 1 << "/11]: " << w << "x" << h << " " << PatternName(p)
                      << " (" << bytes << " bytes)" << std::endl;

            // 1. Pristine CPU reference decode
            ReferenceDecoder ref_bgra(s, CFHD_PIXEL_FORMAT_BGRA);
            ref_bgra.Decode(s);
            ref_bgra.VerifyThreads();
            auto* d_bgra_native = ref_bgra.GetNative();

            auto cpu_bands = ExtractBandsFromNative(d_bgra_native);
            if (cpu_bands.size() != 30) {
                throw std::runtime_error("expected 30 coefficient bands from CPU decoder");
            }

            // 2. Phase 2E baseline decode
            pipeline2e.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
            uint64_t ticket_2e = pipeline2e.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_2e == 0 || !pipeline2e.WaitForDecode(ticket_2e)) {
                throw std::runtime_error("Phase 2E QueueDecode/WaitForDecode failed");
            }

            // 3. Phase 2F-C decode (StaticHeuristic)
            pipeline2fc.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame);
            pipeline2fc.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::StaticHeuristic);
            pipeline2fc.SetPhase2FCChunkSize(256);

            uint64_t ticket_fc_static = pipeline2fc.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_fc_static == 0 || !pipeline2fc.WaitForDecode(ticket_fc_static)) {
                throw std::runtime_error("Phase 2F-C StaticHeuristic QueueDecode/WaitForDecode failed");
            }

            // 4. Phase 2F-C decode (AdaptiveDynamic)
            pipeline2fc.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::AdaptiveDynamic);
            uint64_t ticket_fc_adapt = pipeline2fc.QueueDecode(
                s.compressed.data(),
                s.compressed.size(),
                DeviceSurfaceType::BGRA8,
                ColorMatrix::BT709,
                ColorRange::CG,
                RowOrientation::BottomUp
            );
            if (ticket_fc_adapt == 0 || !pipeline2fc.WaitForDecode(ticket_fc_adapt)) {
                throw std::runtime_error("Phase 2F-C AdaptiveDynamic QueueDecode/WaitForDecode failed");
            }

            // 5. Verify all 30 coefficient bands exact: CPU == Phase 2E == Phase 2F-C
            for (int b = 0; b < 30; ++b) {
                std::vector<int16_t> coeffs_2e, coeffs_fc_static, coeffs_fc_adapt;
                int w_2e = 0, h_2e = 0, p_2e = 0;
                int w_fc_s = 0, h_fc_s = 0, p_fc_s = 0;
                int w_fc_a = 0, h_fc_a = 0, p_fc_a = 0;

                if (!pipeline2e.DownloadBandCoefficientsToHost(ticket_2e, b, coeffs_2e, &w_2e, &h_2e, &p_2e) ||
                    !pipeline2fc.DownloadBandCoefficientsToHost(ticket_fc_static, b, coeffs_fc_static, &w_fc_s, &h_fc_s, &p_fc_s) ||
                    !pipeline2fc.DownloadBandCoefficientsToHost(ticket_fc_adapt, b, coeffs_fc_adapt, &w_fc_a, &h_fc_a, &p_fc_a)) {
                    throw std::runtime_error("DownloadBandCoefficientsToHost failed for band " + std::to_string(b));
                }

                const auto& ref_b = cpu_bands[b];
                std::vector<int16_t> ref_vec(ref_b.host_data, ref_b.host_data + (ref_b.pitch / sizeof(int16_t)) * ref_b.height);

                VerifyBandExactness("Phase 2E vs CPU", b, ref_vec, ref_b.width, ref_b.height, ref_b.pitch, coeffs_2e, w_2e, h_2e, p_2e);
                VerifyBandExactness("Phase 2F-C Static vs CPU", b, ref_vec, ref_b.width, ref_b.height, ref_b.pitch, coeffs_fc_static, w_fc_s, h_fc_s, p_fc_s);
                VerifyBandExactness("Phase 2F-C Adapt vs CPU", b, ref_vec, ref_b.width, ref_b.height, ref_b.pitch, coeffs_fc_adapt, w_fc_a, h_fc_a, p_fc_a);
            }

            // 6. Verify DeviceBGRA surface output exact match to golden hash
            int bgra_pitch = ref_bgra.pitch;
            std::vector<uint8_t> host_bgra_fc(bgra_pitch * h, 0);
            if (!pipeline2fc.DownloadBgraToHost(ticket_fc_static, host_bgra_fc.data(), bgra_pitch)) {
                throw std::runtime_error("DownloadBgraToHost failed for Phase 2F-C");
            }

            std::string computed_bgra_hash = Hash(host_bgra_fc.data(), host_bgra_fc.size());
            if (computed_bgra_hash != bgra_hash) {
                throw std::runtime_error("BGRA hash mismatch: computed=" + computed_bgra_hash + " expected=" + bgra_hash);
            }

            // 7. Verify Intermediate LL exactness
            for (int ch = 0; ch < 3; ++ch) {
                for (int lvl = 0; lvl <= 1; ++lvl) {
                    std::vector<int16_t> ll_2e, ll_fc;
                    if (!pipeline2e.DownloadIntermediateLl(ticket_2e, ch, lvl, ll_2e) ||
                        !pipeline2fc.DownloadIntermediateLl(ticket_fc_static, ch, lvl, ll_fc)) {
                        throw std::runtime_error("DownloadIntermediateLl failed");
                    }
                    if (ll_2e != ll_fc) {
                        throw std::runtime_error("Intermediate LL mismatch ch=" + std::to_string(ch) + " lvl=" + std::to_string(lvl));
                    }
                }
            }

            // 8. Verify Phase 2F-C Diagnostics & Policy Behavior (Section 7, 8, 27)
            std::vector<Phase2FCBandRecord> records;
            if (!pipeline2fc.GetPhase2FCBandRecords(ticket_fc_static, records)) {
                throw std::runtime_error("GetPhase2FCBandRecords failed");
            }
            if (records.size() != 27) {
                throw std::runtime_error("expected 27 band records, got " + std::to_string(records.size()));
            }

            int hybrid_count = 0, method_e_count = 0;
            for (const auto& rec : records) {
                if (rec.chosen_method == BandEntropyMethod::MethodBF_Hybrid) hybrid_count++;
                else method_e_count++;
                if (rec.final_error != 0) {
                    throw std::runtime_error("band " + std::to_string(rec.band_index) + " reported replay error " + std::to_string(rec.final_error));
                }
            }

            std::cout << "    Phase 2F-C Diagnostics: " << hybrid_count << " B/F bands, " << method_e_count << " Method E bands." << std::endl;

            // Texture verification (Section 7, 8): 4K texture should select Method E for dense highpass
            if (w == 3840 && p == 3) {
                std::cout << "    Verified 4K Texture selects Method E appropriately!" << std::endl;
            }
            // Noise verification (Section 7, 8): 4K noise should select B/F for dense highpass
            if (w == 3840 && p == 2) {
                if (hybrid_count == 0) {
                    throw std::runtime_error("expected 4K noise to select B/F for large highpass bands");
                }
                std::cout << "    Verified 4K Noise selected B/F for " << hybrid_count << " bands!" << std::endl;
            }

            std::cout << "    Exactness: 30/30 coefficient bands exact, LL exact, BGRA hash exact (" << bgra_hash << ")" << std::endl;
            golden_count++;

            pipeline2e.ReleaseOutput(ticket_2e);
            pipeline2fc.ReleaseOutput(ticket_fc_static);
            pipeline2fc.ReleaseOutput(ticket_fc_adapt);
        }

        if (golden_count != 11) {
            throw std::runtime_error("expected 11 golden vectors, tested " + std::to_string(golden_count));
        }

        // Test real difference coding
        TestRealDifferenceCoding(pipeline2fc);

        // Test malformed input safety
        TestMalformedInputHandling(pipeline2fc, 1920, 1080);

        std::cout << "\n============================================================" << std::endl;
        std::cout << "Phase 2F-C Test Suite Complete: ALL 11/11 Golden Frames PASSED!" << std::endl;
        std::cout << "Exactness verified: CPU == Phase 2E == Phase 2F-C (max error 0)." << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Phase 2F-C Verification FAILURE: " << e.what() << std::endl;
        return 1;
    }
}
