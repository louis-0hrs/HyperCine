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
#include "hypercine/decode_inverse_cuda.h"
#include "decode_phase0_support.h"
#include "decode_phase1_support.h"

using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;

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
        int current_width = 0, count = 0;
        std::string line;

        std::cout << "=== HyperCine Decode Phase 1 Verification ===" << std::endl;
        std::cout << "Testing Golden Corpus (1080p, 4K UHD, 8K Flat)..." << std::endl;

        CudaInverseWaveletPipeline pipeline;

        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') continue;

            int w, h, p;
            size_t bytes;
            std::string ih, ch, bgra, yu64;
            std::istringstream row(line);
            if (!(row >> w >> h >> p >> ih >> ch >> bytes >> bgra >> yu64)) {
                throw std::runtime_error("malformed manifest row");
            }

            if (w != current_width) {
                samples = Generate(w, h, w == 7680 ? 1 : 5);
                current_width = w;

                if (!pipeline.Initialize(w, h, 10)) {
                    throw std::runtime_error("failed to initialize CUDA pipeline for " + std::to_string(w) + "x" + std::to_string(h));
                }
            }

            if (p < 0 || p >= int(samples.size())) {
                throw std::runtime_error("invalid pattern index");
            }

            auto& s = samples[size_t(p)];
            if (s.input_hash != ih || s.compressed.size() != bytes || Hash(s.compressed.data(), bytes) != ch) {
                throw std::runtime_error("golden generation hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            // 1. Pristine CPU entropy decode to populate native structures
            ReferenceDecoder ref_yu64(s, CFHD_PIXEL_FORMAT_YU64);
            ref_yu64.Decode(s);
            ref_yu64.VerifyThreads();

            auto* d = ref_yu64.GetNative();

            // 2. Extract dequantized signed-16 coefficient bands after CPU entropy decode
            auto bands = ExtractBandsFromNative(d);
            if (bands.size() != 30) {
                throw std::runtime_error("expected 30 coefficient bands (10 per channel x 3 channels)");
            }

            // 3. Compute CPU reference intermediates for exactness baseline
            auto cpu_ref = ComputeCpuReferenceIntermediates(d);

            // 4. Upload coefficient bands to GPU (timed H2D)
            if (!pipeline.UploadCoefficients(bands)) {
                throw std::runtime_error("UploadCoefficients failed for " + std::to_string(w) + " " + PatternName(p));
            }

            // 5. CUDA inverse wavelet reconstruction DAG (timed Reconstruct)
            if (!pipeline.Reconstruct()) {
                throw std::runtime_error("Reconstruct failed for " + std::to_string(w) + " " + PatternName(p));
            }

            // 6. Verify Level 1 Intermediate LL (output of Level 2) across all 3 channels
            for (int ch = 0; ch < 3; ++ch) {
                int w_ll1 = cpu_ref.ll1_width[ch];
                int h_ll1 = cpu_ref.ll1_height[ch];
                std::vector<int16_t> gpu_ll1(w_ll1 * h_ll1, 0);

                if (!pipeline.DownloadIntermediateLL(1, ch, gpu_ll1.data(), w_ll1 * sizeof(int16_t))) {
                    throw std::runtime_error("DownloadIntermediateLL(1) failed");
                }

                int diff = std::memcmp(gpu_ll1.data(), cpu_ref.ll1[ch].data(), gpu_ll1.size() * sizeof(int16_t));
                if (diff != 0) {
                    for (size_t i = 0; i < gpu_ll1.size(); ++i) {
                        if (gpu_ll1[i] != cpu_ref.ll1[ch][i]) {
                            throw std::runtime_error("LL1 mismatch at sample " + std::to_string(i) +
                                                     " ch=" + std::to_string(ch) + " CPU=" + std::to_string(cpu_ref.ll1[ch][i]) +
                                                     " GPU=" + std::to_string(gpu_ll1[i]));
                        }
                    }
                }
            }

            // 7. Verify Level 0 Intermediate LL (output of Level 1) across all 3 channels
            for (int ch = 0; ch < 3; ++ch) {
                int w_ll0 = cpu_ref.ll0_width[ch];
                int h_ll0 = cpu_ref.ll0_height[ch];
                std::vector<int16_t> gpu_ll0(w_ll0 * h_ll0, 0);

                if (!pipeline.DownloadIntermediateLL(0, ch, gpu_ll0.data(), w_ll0 * sizeof(int16_t))) {
                    throw std::runtime_error("DownloadIntermediateLL(0) failed");
                }

                int diff = std::memcmp(gpu_ll0.data(), cpu_ref.ll0[ch].data(), gpu_ll0.size() * sizeof(int16_t));
                if (diff != 0) {
                    for (size_t i = 0; i < gpu_ll0.size(); ++i) {
                        if (gpu_ll0[i] != cpu_ref.ll0[ch][i]) {
                            throw std::runtime_error("LL0 mismatch at sample " + std::to_string(i) +
                                                     " ch=" + std::to_string(ch) + " CPU=" + std::to_string(cpu_ref.ll0[ch][i]) +
                                                     " GPU=" + std::to_string(gpu_ll0[i]));
                        }
                    }
                }
            }

            // 8. Download reconstructed planar Y, V, U channels (timed D2H)
            int y_w = w, y_h = h;
            int c_w = w / 2, c_h = h;
            std::vector<int16_t> gpu_y(y_w * y_h, 0);
            std::vector<int16_t> gpu_v(c_w * c_h, 0);
            std::vector<int16_t> gpu_u(c_w * c_h, 0);

            if (!pipeline.DownloadPlanes(
                gpu_y.data(), y_w * sizeof(int16_t),
                gpu_v.data(), c_w * sizeof(int16_t),
                gpu_u.data(), c_w * sizeof(int16_t)))
            {
                throw std::runtime_error("DownloadPlanes failed");
            }

            // Verify final reconstructed planar channels match CPU reference
            if (std::memcmp(gpu_y.data(), cpu_ref.plane[0].data(), gpu_y.size() * sizeof(int16_t)) != 0) {
                throw std::runtime_error("Final planar Y channel mismatch");
            }
            if (std::memcmp(gpu_v.data(), cpu_ref.plane[1].data(), gpu_v.size() * sizeof(int16_t)) != 0) {
                throw std::runtime_error("Final planar V channel mismatch");
            }
            if (std::memcmp(gpu_u.data(), cpu_ref.plane[2].data(), gpu_u.size() * sizeof(int16_t)) != 0) {
                throw std::runtime_error("Final planar U channel mismatch");
            }

            // 9. Convert planar planes to final YU64 output and compare with pristine CPU decode
            int pitch = ref_yu64.pitch;
            std::vector<uint8_t> hybrid_yu64(pitch * h, 0);
            ConvertPlanarToYU64(
                gpu_y.data(), y_w * sizeof(int16_t),
                gpu_v.data(), c_w * sizeof(int16_t),
                gpu_u.data(), c_w * sizeof(int16_t),
                w, h, d->codec.precision,
                hybrid_yu64.data(), pitch
            );

            if (std::memcmp(hybrid_yu64.data(), ref_yu64.output.data(), hybrid_yu64.size()) != 0) {
                throw std::runtime_error("Final YU64 output byte mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            // Verify against golden manifest hash
            if (Hash(hybrid_yu64.data(), hybrid_yu64.size()) != yu64) {
                throw std::runtime_error("Manifest YU64 hash mismatch for " + std::to_string(w) + " " + PatternName(p));
            }

            std::cout << "  " << std::setw(5) << w << "x" << std::setw(4) << h << " Pattern " << p
                      << " (" << std::left << std::setw(8) << PatternName(p) << "): "
                      << "H2D=" << std::fixed << std::setprecision(3) << pipeline.GetH2DTimeMs() << "ms "
                      << "Inv=" << pipeline.GetInverseTimeMs() << "ms "
                      << "D2H=" << pipeline.GetD2HTimeMs() << "ms "
                      << "[PASS: 0 diff on LL1, LL0, Planes, YU64]" << std::endl;

            ++count;
        }

        if (count != 11) {
            throw std::runtime_error("expected 11 golden samples, got " + std::to_string(count));
        }

        std::cout << "\nSUCCESS: 11/11 golden samples passed with 100% bit-exact equivalence (0 max absolute error)!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
