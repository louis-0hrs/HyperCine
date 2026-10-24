// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/rgb444.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace hypercine::rgb444;
using hypercine::standalone::AlignedVector;
namespace {
std::size_t checks = 0;
constexpr std::array<Channel, 3> channels{Channel::Red, Channel::Green, Channel::Blue};
void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Error, class Function> void Reject(Function action) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; }
    Require(rejected, "malformed RG48 input accepted");
}
std::uint16_t Sample(std::size_t c, int x, int y, int width) {
    // 256x256 exhausts every uint16 value independently in all three channels.
    const auto n = static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(width) + x;
    return static_cast<std::uint16_t>(n ^ (c * 0x5555u));
}
struct Source {
    AlignedVector<std::uint16_t> pixels;
    Rg48View view;
    Source(int width, int height, std::size_t extra = 0) {
        const auto stride = ((static_cast<std::size_t>(width) * 6 + 15) & ~std::size_t{15}) + extra;
        const auto bytes = (height - 1) * stride + width * 6;
        pixels.assign(bytes / 2, 0xfedc); // No final-row padding allocation.
        view = {width, height, pixels.data(), bytes, stride};
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                for (std::size_t c = 0; c < 3; ++c)
                    pixels[y * stride / 2 + x * 3 + c] = Sample(c, x, y, width);
    }
};
void Import() {
    for (auto size : {std::array<int, 2>{1, 1}, {7, 3}, {8, 2}, {9, 5}, {17, 9},
                      {128, 128}, {256, 256}, {1920, 1080}, {4096, 512}}) {
        for (auto extra : {std::size_t{0}, std::size_t{32}}) {
            Source source(size[0], size[1], extra);
            const auto original = source.pixels;
            const auto frame = Rgb444Frame::CopyRg48(source.view);
            Require(frame.width() == size[0] && frame.height() == size[1], "geometry changed");
            Require(frame.stride_bytes() % 16 == 0, "unaligned output pitch");
            for (std::size_t c = 0; c < 3; ++c) {
                const auto& plane = frame.plane(channels[c]);
                Require(reinterpret_cast<std::uintptr_t>(plane.data()) % 32 == 0, "unaligned output");
                Require(plane.size() == frame.stride_bytes() / 2 * size[1], "not full resolution");
                for (int y = 0; y < size[1]; ++y)
                    for (std::size_t x = 0; x < frame.stride_bytes() / 2; ++x)
                        Require(plane[y * frame.stride_bytes() / 2 + x] ==
                            (x < static_cast<std::size_t>(size[0])
                             ? Sample(c, static_cast<int>(x), y, size[0]) / 16 : 0),
                            "RG48 sample, channel, orientation, truncation or padding differs");
            }
            Require(source.pixels == original, "source modified");
            source.pixels.assign(source.pixels.size(), 0);
            Require(frame.plane(Channel::Blue)[0] == 0xaaaa / 16, "output aliases source");
        }
    }
}
void Invalid() {
    Source source(9, 3);
    auto retained = Rgb444Frame::CopyRg48(source.view);
    const auto before = retained.plane(Channel::Blue);
    for (auto size : {std::array<int, 2>{0, 3}, {-1, 3}, {9, 0}, {9, -1}}) {
        auto v = source.view; v.width = size[0]; v.height = size[1];
        Reject<std::invalid_argument>([&] { retained = Rgb444Frame::CopyRg48(v); });
    }
    for (auto size : {std::array<int, 2>{4097, 3}, {9, 4097}, {4096, 513}, {3840, 2160},
                      {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}}) {
        auto v = source.view; v.width = size[0]; v.height = size[1];
        Reject<std::length_error>([&] { retained = Rgb444Frame::CopyRg48(v); });
    }
    for (int fault = 0; fault < 6; ++fault) {
        auto v = source.view;
        if (fault == 0) v.data = nullptr;
        if (fault == 1) ++v.data;
        if (fault == 2) --v.bytes;
        if (fault == 3) v.stride_bytes = 48; // Active packed row is 54 B.
        if (fault == 4) v.stride_bytes = 65;
        if (fault == 5) v.bytes = 0;
        Reject<std::invalid_argument>([&] { retained = Rgb444Frame::CopyRg48(v); });
    }
    for (auto stride : {std::numeric_limits<std::size_t>::max() & ~std::size_t{15},
                       static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) + 1}) {
        for (int height : {2, 3}) {
            auto v = source.view; v.height = height; v.stride_bytes = stride;
            v.bytes = std::numeric_limits<std::size_t>::max();
            Reject<std::length_error>([&] { retained = Rgb444Frame::CopyRg48(v); });
        }
    }
    Require(retained.plane(Channel::Blue) == before, "failed replacement changed frame");
    Require(Rgb444Frame::CopyRg48(source.view).plane(Channel::Blue) == before, "retry failed");
}

// Test-only bridge for the independently hashed F1 source replay. Output is
// tightly packed top-down planar p12 uint16 LITTLE endian, R then G then B.
std::size_t Number(const char* arg, std::size_t limit) {
    const std::string s(arg);
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid unsigned argument");
    std::size_t end = 0;
    const auto n = std::stoull(s, &end);
    if (!n || end != s.size() || n > limit) throw std::length_error("argument exceeds budget");
    return static_cast<std::size_t>(n);
}
void Export(char** argv) {
    const auto width = Number(argv[2], kMaxFrameDimension);
    const auto height = Number(argv[3], kMaxFrameDimension);
    const auto pitch = Number(argv[4], kMaxFrameStorageBytes);
    if (width * height > kMaxFramePixels || pitch < width * 6 || pitch % 16 ||
        pitch > kMaxFrameStorageBytes / height)
        throw std::length_error("input file exceeds source replay budget");
    std::ifstream input(argv[5], std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    const auto required = (height - 1) * pitch + width * 6;
    if (!input || length < 0 || static_cast<std::size_t>(length) < required ||
        static_cast<std::size_t>(length) > height * pitch)
        throw std::invalid_argument("invalid RG48 file extent");
    AlignedVector<std::uint16_t> source((static_cast<std::size_t>(length) + 1) / 2, 0);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(source.data()), length);
    if (!input) throw std::runtime_error("RG48 read failed");
    const auto frame = Rgb444Frame::CopyRg48({static_cast<int>(width), static_cast<int>(height),
                                            source.data(), static_cast<std::size_t>(length), pitch});
    std::ofstream output(argv[6], std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open planar output");
    for (auto c : channels)
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x) {
                const auto value = frame.plane(c)[y * frame.stride_bytes() / 2 + x];
                const char bytes[]{static_cast<char>(value & 255), static_cast<char>(value >> 8)};
                output.write(bytes, 2);
            }
    output.close();
    if (!output) throw std::runtime_error("planar output write failed");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "import") Import();
        else if (argc == 2 && std::string(argv[1]) == "rejection") Invalid();
        else if (argc == 7 && std::string(argv[1]) == "--export") Export(argv);
        else throw std::invalid_argument("expected import, rejection or --export width height pitch input output");
        std::cout << "PASS RG48 source: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
