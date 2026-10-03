// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <fstream>
#include <iostream>
#include "decode_phase0_support.h"
int main(int argc, char** argv) {
    using namespace DecodePhase0;
    try {
        if (argc != 2) throw std::runtime_error("expected corpus manifest path");
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("missing golden manifest");
        std::vector<Sample> samples;
        int current_width = 0, count = 0;
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') continue;
            int w, h, p; size_t bytes;
            std::string ih, ch, bgra, yu64;
            std::istringstream row(line);
            if (!(row >> w >> h >> p >> ih >> ch >> bytes >> bgra >> yu64))
                throw std::runtime_error("malformed manifest row");
            if (w != current_width) { samples = Generate(w, h, w == 7680 ? 1 : 5); current_width = w; }
            if (p < 0 || p >= int(samples.size())) throw std::runtime_error("invalid pattern");
            auto& s = samples[size_t(p)];
            if (s.input_hash != ih || s.compressed.size() != bytes || Hash(s.compressed.data(), bytes) != ch)
                throw std::runtime_error("golden generation hash mismatch: " + std::to_string(w) + " " + PatternName(p));
            for (int f = 0; f < 2; ++f) {
                const auto fmt = f == 0 ? CFHD_PIXEL_FORMAT_BGRA : CFHD_PIXEL_FORMAT_YU64;
                ReferenceDecoder a(s, fmt), b(s, fmt);
                a.SeedDither(); a.Decode(s); a.VerifyThreads();
                b.SeedDither(); b.Decode(s); b.VerifyThreads();
                if (a.output != b.output || Hash(a.output.data(), a.output.size()) != (f == 0 ? bgra : yu64))
                    throw std::runtime_error("golden pixel mismatch: " + std::to_string(w) + " " + PatternName(p));
                if (f == 1) {
                    const auto original = MakeInput(w, h, p);
                    Bytes rounded(size_t(w) * h * 2);
                    const auto* words = reinterpret_cast<const uint16_t*>(a.output.data());
                    // YU64 native little-endian words: Y0,V,Y1,U.
                    for (size_t i = 0; i < rounded.size(); i += 4) {
                        rounded[i] = uint8_t(words[i] >> 8);
                        rounded[i + 1] = uint8_t(words[i + 3] >> 8);
                        rounded[i + 2] = uint8_t(words[i + 2] >> 8);
                        rounded[i + 3] = uint8_t(words[i + 1] >> 8);
                    }
                    const double psnr = HyperCineTest::ComputePSNR_YUY2(original.data(), rounded.data(), w, h, false);
                    std::cout << w << 'x' << h << ' ' << PatternName(p) << " codec_bits=" << a.CodecPrecision()
                              << " PSNR=" << psnr << "dB\n";
                    if (psnr <= 35) throw std::runtime_error("FilmScan3 PSNR <= 35dB");
                }
            }
            ++count;
        }
        if (count != 11) throw std::runtime_error("expected eleven golden samples");
        std::cout << "11/11 golden samples: frozen hashes and independent byte-exact decodes passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
