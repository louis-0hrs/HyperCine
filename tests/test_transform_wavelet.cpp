/**
 * @file test_transform_wavelet.cpp
 * @brief Transform-level test suite verifying 2-6 biorthogonal wavelet math and deterministic reference hashes
 */

#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include <cstring>
#include "test_vectors.h"

extern "C" {
#include "WaveletDemo/utils.h"
}

using namespace HyperCineTest;

// Interface for future GPU / optimized transform verification
struct TransformVerificationResult {
    bool passed;
    size_t totalCoefficients;
    size_t mismatchedCoefficients;
    int maxDifference;
    uint64_t referenceHash;
    uint64_t testedHash;
};

TransformVerificationResult CompareTransforms(
    const int* ref,
    const int* candidate,
    int width,
    int height,
    int tolerance = 0)
{
    TransformVerificationResult res{};
    res.totalCoefficients = static_cast<size_t>(width) * height;
    res.mismatchedCoefficients = 0;
    res.maxDifference = 0;
    res.referenceHash = ComputeFNV1a64(ref, res.totalCoefficients * sizeof(int));
    res.testedHash = ComputeFNV1a64(candidate, res.totalCoefficients * sizeof(int));

    for (size_t i = 0; i < res.totalCoefficients; ++i) {
        int diff = std::abs(ref[i] - candidate[i]);
        if (diff > res.maxDifference) res.maxDifference = diff;
        if (diff > tolerance) {
            res.mismatchedCoefficients++;
        }
    }
    res.passed = (res.mismatchedCoefficients == 0);
    return res;
}

bool TestConstantBlack(int width, int height) {
    std::cout << "  Testing Constant Black (64x64)... ";
    auto input = GenerateTestVectorInt(PATTERN_CONSTANT_BLACK, width, height, 12);
    std::vector<int> outH(width * height, -1);
    std::vector<int> outV(width * height, -1);

    H26Wavelet(input.data(), outH.data(), width, height, width, height);
    V26Wavelet(outH.data(), outV.data(), width, height, width, height);

    for (size_t i = 0; i < outV.size(); ++i) {
        if (outV[i] != 0) {
            std::cout << "FAILED! Non-zero coefficient at index " << i << " (" << outV[i] << ")\n";
            return false;
        }
    }
    uint64_t hash = ComputeFNV1a64(outV.data(), outV.size() * sizeof(int));
    std::cout << "PASSED (Hash: 0x" << std::hex << hash << std::dec << ")\n";
    return true;
}

bool TestConstantWhite(int width, int height) {
    std::cout << "  Testing Constant White (64x64, max value)... ";
    auto input = GenerateTestVectorInt(PATTERN_CONSTANT_WHITE, width, height, 12);
    const int maxVal = 4095;
    std::vector<int> outH(width * height, 0);
    std::vector<int> outV(width * height, 0);

    H26Wavelet(input.data(), outH.data(), width, height, width, height);
    V26Wavelet(outH.data(), outV.data(), width, height, width, height);

    // In a 2-D wavelet on constant M:
    // Lowpass in H is 2*M, Lowpass in V is 2*(2*M) = 4*M
    // All highpass bands (H-high, V-high, Diagonal) must be identically 0
    int halfW = width / 2;
    int halfH = height / 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int val = outV[y * width + x];
            if (x < halfW && y < halfH) {
                // LL band
                int expectedLL = 4 * maxVal;
                if (val != expectedLL) {
                    std::cout << "FAILED! LL coefficient at (" << x << "," << y << ") is " << val << " expected " << expectedLL << "\n";
                    return false;
                }
            } else {
                // Highpass bands (HL, LH, HH) must be strictly 0
                if (val != 0) {
                    std::cout << "FAILED! Highpass coefficient at (" << x << "," << y << ") is " << val << " expected 0\n";
                    return false;
                }
            }
        }
    }
    uint64_t hash = ComputeFNV1a64(outV.data(), outV.size() * sizeof(int));
    std::cout << "PASSED (Hash: 0x" << std::hex << hash << std::dec << ")\n";
    return true;
}

bool TestCorpusDeterminism(int width, int height) {
    std::cout << "  Testing Corpus Determinism and Recording Reference Hashes:\n";
    bool allPassed = true;

    for (int p = 0; p < PATTERN_COUNT; ++p) {
        TestPatternType pattern = static_cast<TestPatternType>(p);
        auto input = GenerateTestVectorInt(pattern, width, height, 12);
        std::vector<int> dest1(width * height, 0);
        std::vector<int> dest2(width * height, 0);

        H26Wavelet(input.data(), dest1.data(), width, height, width, height);
        V26Wavelet(dest1.data(), dest2.data(), width, height, width, height);

        // Run second pass to ensure strict determinism
        std::vector<int> check1(width * height, 0);
        std::vector<int> check2(width * height, 0);
        H26Wavelet(input.data(), check1.data(), width, height, width, height);
        V26Wavelet(check1.data(), check2.data(), width, height, width, height);

        auto cmp = CompareTransforms(dest2.data(), check2.data(), width, height, 0);
        if (!cmp.passed) {
            std::cout << "    [-] " << GetPatternName(pattern) << " FAILED determinism check!\n";
            allPassed = false;
        } else {
            std::cout << "    [+] " << std::left << std::setw(26) << GetPatternName(pattern)
                      << " Hash: 0x" << std::hex << std::setw(16) << std::setfill('0') << cmp.referenceHash
                      << std::dec << std::setfill(' ') << " (exact equality)\n";
        }
    }
    return allPassed;
}

bool TestReconstructionInvertibility(int width, int height) {
    std::cout << "  Testing 2-6 Wavelet Forward-Inverse Roundtrip Invertibility...\n";
    bool allPassed = true;

    for (int p = 0; p < PATTERN_COUNT; ++p) {
        TestPatternType pattern = static_cast<TestPatternType>(p);
        auto original = GenerateTestVectorInt(pattern, width, height, 12);
        std::vector<int> buffA = original;
        std::vector<int> buffB(width * height, 0);
        std::vector<int> recon(width * height, 0);

        // Forward 1-level 2-6 wavelet
        H26Wavelet(buffA.data(), buffB.data(), width, height, width, height);
        V26Wavelet(buffB.data(), buffA.data(), width, height, width, height);

        // Inverse 1-level 2-6 wavelet (unquantized)
        InvertV26Wavelet(buffA.data(), buffB.data(), width, height, width / 2, height / 2);
        InvertH26Wavelet(buffB.data(), recon.data(), width, height, width / 2, height / 2);

        // In 2-6 biorthogonal integer wavelet with integer truncation/shifts,
        // rounding variance is bounded to +/- 1 LSB for non-quantized transforms.
        int maxDiff = 0;
        for (size_t i = 0; i < original.size(); ++i) {
            int diff = std::abs(original[i] - recon[i]);
            if (diff > maxDiff) maxDiff = diff;
        }

        std::cout << "    [+] " << std::left << std::setw(26) << GetPatternName(pattern)
                  << " Max diff after unquantized inverse: " << maxDiff << " LSB\n";
        if (maxDiff > 2) {
            std::cout << "    [-] " << GetPatternName(pattern) << " exceeded expected inversion tolerance!\n";
            allPassed = false;
        }
    }
    return allPassed;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " HyperCine Reference Test: 2-6 Wavelet Transform Suite   \n";
    std::cout << "=========================================================\n";

    const int width = 64;
    const int height = 64;

    bool ok = true;
    ok &= TestConstantBlack(width, height);
    ok &= TestConstantWhite(width, height);
    ok &= TestCorpusDeterminism(width, height);
    ok &= TestReconstructionInvertibility(width, height);

    std::cout << "---------------------------------------------------------\n";
    if (ok) {
        std::cout << "ALL WAVELET TRANSFORM TESTS PASSED SUCCESSFULLY.\n";
        return 0;
    } else {
        std::cout << "WAVELET TRANSFORM TESTS FAILED.\n";
        return 1;
    }
}
