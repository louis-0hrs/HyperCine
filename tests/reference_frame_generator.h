// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Extracted from Windows source at addd8f46ca78a968b9c235baf3f7e3d53d8238f7; see import_provenance.json.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
namespace reference_fixture {
using std::size_t;
enum class Phase3APattern {
    FlatBlack = 0,
    FlatMidGray = 1,
    FlatWhite = 2,
    HorizontalGradient = 3,
    VerticalGradient = 4,
    Checkerboard = 5,
    HardEdges = 6,
    DeterministicNoise = 7,
    HighFrequencyPattern = 8,
    LowDetailPhotographicSynthetic = 9,
    HighDetailPhotographicSynthetic = 10
};

uint32_t NextXorShift32(uint32_t& state) {
    uint32_t x = state ? state : 0x1337BEEFu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

inline int FloorHalf(int x) { return x >= 0 ? x/2 : -((-x+1)/2); }
void StampFrameIdentityBlocks(uint8_t* bgra, int width, int height, int pitch,
                              int frame_index, Phase3APattern pattern) {
    if (!bgra || width < 512 || height < 64 || pitch < width * 4) return;
    const uint16_t words[2] = {
        static_cast<uint16_t>((0xA5u << 8) | (static_cast<uint8_t>(pattern) & 0xFFu)),
        static_cast<uint16_t>(frame_index & 0xFFFF)
    };
    for (int row_blk = 0; row_blk < 2; ++row_blk) {
        const uint16_t w = words[row_blk];
        for (int col_blk = 0; col_blk < 16; ++col_blk) {
            const int bit = (w >> (15 - col_blk)) & 1;
            const uint8_t v = bit ? 235u : 20u;
            const uint32_t px = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                                (static_cast<uint32_t>(v) << 8) | static_cast<uint32_t>(v);
            const int x0 = col_blk * 32;
            const int y0 = row_blk * 32;
            for (int dy = 0; dy < 32; ++dy) {
                uint32_t* row32 = reinterpret_cast<uint32_t*>(bgra + static_cast<size_t>(y0 + dy) * pitch) + x0;
                for (int dx = 0; dx < 32; ++dx) {
                    row32[dx] = px;
                }
            }
        }
    }
    // Also stamp a frame-unique 4:2:2-aligned detail band across rows [64 .. min(height, 96))
    // so every frame k has distinct wavelet coefficients and bitstream hash even when sharing an archetype.
    const int mod_y_end = std::min(height, 96);
    uint32_t rng = 0x9E3779B9u ^ (static_cast<uint32_t>(frame_index + 1) * 0x85EBCA6Bu);
    for (int y = 64; y < mod_y_end; ++y) {
        uint32_t* row32 = reinterpret_cast<uint32_t*>(bgra + static_cast<size_t>(y) * pitch);
        const uint32_t row_seed = NextXorShift32(rng);
        const int span_w = std::min(width, 512);
        for (int x = 0; x < span_w; x += 4) {
            const uint32_t h = (row_seed ^ (static_cast<uint32_t>(x) * 2654435761u)) >> 16;
            const int base_y = 64 + static_cast<int>(h & 0x7Fu);
            const int cr = static_cast<int>((h >> 7) & 0x1Fu) - 16;
            const int cb = static_cast<int>((h >> 11) & 0x1Fu) - 16;
            for (int dx = 0; dx < 4 && (x + dx) < span_w; ++dx) {
                const uint8_t b = static_cast<uint8_t>(std::clamp(base_y + cb, 20, 235));
                const uint8_t g = static_cast<uint8_t>(std::clamp(base_y, 20, 235));
                const uint8_t r = static_cast<uint8_t>(std::clamp(base_y + cr, 20, 235));
                row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
        }
    }
}

void GeneratePhase3AFrame(int width, int height, int frame_index, Phase3APattern pattern,
                          uint8_t* bgra_out, int pitch, uint32_t base_seed) {
    if (!bgra_out || width <= 0 || height <= 0 || pitch < width * 4) return;
    const int phase = (frame_index * 12) & 254;

    for (int y = 0; y < height; ++y) {
        uint32_t* row32 = reinterpret_cast<uint32_t*>(bgra_out + static_cast<size_t>(y) * static_cast<size_t>(pitch));
        switch (pattern) {
        case Phase3APattern::FlatBlack: {
            const uint32_t px = (255u << 24) | (16u << 16) | (16u << 8) | 16u;
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::FlatMidGray: {
            const uint32_t px = (255u << 24) | (128u << 16) | (128u << 8) | 128u;
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::FlatWhite: {
            const uint32_t px = (255u << 24) | (235u << 16) | (235u << 8) | 235u;
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::HorizontalGradient: {
            for (int x = 0; x < width; ++x) {
                const int xc = x & ~1;
                const int v = 24 + (((xc + phase) % std::max(1, width)) * 196) / std::max(1, width);
                const int u = 64 + (((xc + phase) % std::max(1, width)) * 128) / std::max(1, width);
                row32[x] = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                           (static_cast<uint32_t>(u) << 8) | static_cast<uint32_t>(255 - v);
            }
            break;
        }
        case Phase3APattern::VerticalGradient: {
            const int v = 24 + (((y + phase) % std::max(1, height)) * 196) / std::max(1, height);
            const int g = 48 + (((y + phase) % std::max(1, height)) * 160) / std::max(1, height);
            const uint32_t px = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                                (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(255 - v);
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::Checkerboard: {
            const int by = ((y + (phase >> 2)) >> 3) & 1;
            for (int x = 0; x < width; ++x) {
                const int xc = x & ~1;
                const int bx = ((xc + (phase >> 2)) >> 3) & 1;
                const uint8_t v = (bx ^ by) ? 216u : 40u;
                const uint8_t c = ((xc ^ y) & 16) ? 184u : 72u;
                row32[x] = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(255u - c);
            }
            break;
        }
        case Phase3APattern::HardEdges: {
            for (int x = 0; x < width; ++x) {
                const int xc = x & ~1;
                const bool vert_bar = ((xc + phase) % 160) < 80;
                const bool horiz_bar = ((y + phase) % 120) < 60;
                const bool diag_bar = (((xc + (y & ~1) + phase) % 180) < 90);
                const uint8_t r = vert_bar ? 216u : 40u;
                const uint8_t g = horiz_bar ? 212u : 44u;
                const uint8_t b = diag_bar ? 208u : 48u;
                row32[x] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
            break;
        }
        case Phase3APattern::DeterministicNoise: {
            // 4:2:2-chroma-aligned high-entropy pseudorandom noise:
            // Luma varies at every pixel x (exciting L1..L3 LH/HL/HH and pushing compression < 3.0),
            // while chroma offset (cr, cb) is shared across each 2x1 horizontal pair so 4:2:2 subsampling is lossless.
            uint32_t rng = base_seed ^ (static_cast<uint32_t>(frame_index + 1) * 0x9E3779B9u) ^
                           (static_cast<uint32_t>(y + 1) * 0x85EBCA6Bu);
            for (int x = 0; x < width; x += 2) {
                const uint32_t v = NextXorShift32(rng);
                const int y0 = 60 + static_cast<int>(v & 0x7Fu);
                const int dy = static_cast<int>((v >> 8) & 0x1Fu) - 16;
                const int cr = static_cast<int>((v >> 16) & 0x3Fu) - 32;
                const int cb = static_cast<int>((v >> 24) & 0x3Fu) - 32;
                for (int dx = 0; dx < 2 && (x + dx) < width; ++dx) {
                    const int yv = y0 + (dx ? dy : -dy);
                    const uint8_t r = static_cast<uint8_t>(std::clamp(yv + cr, 16, 235));
                    const uint8_t g = static_cast<uint8_t>(std::clamp(yv, 16, 235));
                    const uint8_t b = static_cast<uint8_t>(std::clamp(yv + cb, 16, 235));
                    row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                    (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
            }
            break;
        }
        case Phase3APattern::HighFrequencyPattern: {
            // 4:2:2-chroma-aligned high-frequency grating:
            for (int x = 0; x < width; x += 2) {
                const int p_chr = (((x >> 1) * 3) ^ (y * 5) ^ frame_index) & 7;
                const int cr = (p_chr & 3) * 16 - 24;
                const int cb = ((p_chr >> 1) & 3) * 16 - 24;
                for (int dx = 0; dx < 2 && (x + dx) < width; ++dx) {
                    const int p_lum = ((x + dx) ^ y ^ frame_index) & 1;
                    const int p_fine = (((x + dx) >> 1) + (y >> 1) + frame_index) & 3;
                    const int yv = (p_lum ? 156 : 96) + (p_fine * 8 - 12);
                    const uint8_t r = static_cast<uint8_t>(std::clamp(yv + cr, 16, 235));
                    const uint8_t g = static_cast<uint8_t>(std::clamp(yv, 16, 235));
                    const uint8_t b = static_cast<uint8_t>(std::clamp(yv + cb, 16, 235));
                    row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                    (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
            }
            break;
        }
        case Phase3APattern::LowDetailPhotographicSynthetic: {
            const int sky = 40 + (y * 140) / std::max(1, height);
            for (int x = 0; x < width; ++x) {
                const int wave = ((x + phase) >> 5) & 15;
                const uint8_t b = static_cast<uint8_t>(std::clamp(sky + wave, 16, 235));
                const uint8_t g = static_cast<uint8_t>(std::clamp(sky - 10 + (wave >> 1), 16, 235));
                const uint8_t r = static_cast<uint8_t>(std::clamp(sky - 24 + (wave >> 2), 16, 235));
                row32[x] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
            break;
        }
        case Phase3APattern::HighDetailPhotographicSynthetic:
        default: {
            uint32_t rng = base_seed ^ (static_cast<uint32_t>(y + 1) * 0x27D4EB2Du) ^
                           (static_cast<uint32_t>(frame_index) * 0x165667B1u);
            const int base_y = (y * 180) / std::max(1, height);
            for (int x = 0; x < width; x += 2) {
                const int base_x = (x * 180) / std::max(1, width);
                const int edge = (((x + phase) % 256) < 128 && ((y + phase) % 256) < 128) ? 36 : -18;
                const uint32_t rv = NextXorShift32(rng);
                const int tex0 = static_cast<int>(rv & 31u) - 16;
                const int tex1 = static_cast<int>((rv >> 8) & 31u) - 16;
                for (int dx = 0; dx < 2 && (x + dx) < width; ++dx) {
                    const int tex = dx ? tex1 : tex0;
                    const uint8_t r = static_cast<uint8_t>(std::clamp(32 + base_x + edge + tex, 16, 235));
                    const uint8_t g = static_cast<uint8_t>(std::clamp(32 + base_y + FloorHalf(edge) + tex, 16, 235));
                    const uint8_t b = static_cast<uint8_t>(std::clamp(128 + FloorHalf(base_x - base_y) + tex, 16, 235));
                    row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                    (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
            }
            break;
        }
        }
    }
    StampFrameIdentityBlocks(bgra_out, width, height, pitch, frame_index, pattern);
}

}
