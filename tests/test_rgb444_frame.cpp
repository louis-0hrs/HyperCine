// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/rgb444.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace hypercine::rgb444;
using hypercine::standalone::AlignedVector;
namespace {
std::size_t checks = 0;
void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Error, class Function> void Reject(Function action) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; }
    Require(rejected, "malformed RGB444 input was accepted");
}
constexpr std::array<Channel, 3> channels{Channel::Red, Channel::Green, Channel::Blue};
std::uint16_t Sample(std::size_t channel, int x, int y) {
    // Fixed reproducible seed; independent asymmetric planes, including low bits.
    std::uint32_t n = 0x1337BEEFu ^ (static_cast<std::uint32_t>(channel + 1) * 0x9e3779b9u);
    n ^= static_cast<std::uint32_t>(x) * 0x85ebca6bu;
    n ^= static_cast<std::uint32_t>(y) * 0xc2b2ae35u;
    n ^= n >> 16;
    if (x == 0 && y == 0) return 0;
    if (x == 1 && y == 0) return 4095;
    return static_cast<std::uint16_t>(n & 4095);
}
struct Source {
    FrameView view;
    std::array<AlignedVector<std::uint16_t>, 3> planes;
    Source(int width, int height) {
        view.width = width; view.height = height;
        const auto base = (static_cast<std::size_t>(width) * 2 + 15) & ~std::size_t{15};
        std::array<PlaneView*, 3> views{&view.red, &view.green, &view.blue};
        for (std::size_t c = 0; c < 3; ++c) {
            const auto stride = base + c * 16;
            const auto elements = (height - 1) * stride / 2 + width;
            // Exactly the readable final row, surrounded by sentinel guards.
            planes[c].assign(elements + 16, 65535);
            auto* data = planes[c].data() + 8;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    data[y * stride / 2 + x] = Sample(c, x, y);
            *views[c] = {data, elements * 2, stride};
        }
    }
};
void Storage() {
    for (const auto dimensions : {std::array<int, 2>{1, 1}, {7, 3}, {8, 8}, {9, 5},
                                  {17, 9}, {128, 128}, {1920, 1080}, {4096, 512}}) {
        Source source(dimensions[0], dimensions[1]);
        const auto original = source.planes;
        auto frame = Rgb444Frame::Copy(source.view);
        Require(frame.width() == dimensions[0] && frame.height() == dimensions[1], "dimensions changed");
        Require(frame.stride_bytes() % 16 == 0, "output stride unaligned");
        std::size_t bytes = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            const auto& plane = frame.plane(channels[c]);
            const auto stride = frame.stride_bytes() / 2;
            Require(reinterpret_cast<std::uintptr_t>(plane.data()) % 32 == 0, "output address unaligned");
            Require(plane.size() == stride * frame.height(), "plane is not full resolution");
            bytes += plane.size() * 2;
            for (int y = 0; y < frame.height(); ++y) {
                for (int x = 0; x < frame.width(); ++x)
                    Require(plane[y * stride + x] == Sample(c, x, y), "sample/order/precision changed");
                for (std::size_t x = frame.width(); x < stride; ++x)
                    Require(plane[y * stride + x] == 0, "padding not deterministic");
            }
            Require(source.planes[c] == original[c], "source or guards changed");
            source.planes[c].assign(source.planes[c].size(), 0);
            Require(plane.back() == (stride == static_cast<std::size_t>(frame.width())
                    ? Sample(c, frame.width() - 1, frame.height() - 1) : 0), "source alias retained");
        }
        Require(bytes <= kMaxFrameStorageBytes, "frame storage budget exceeded");
        Require(frame.plane(Channel::Red).data() != frame.plane(Channel::Green).data() &&
                frame.plane(Channel::Green).data() != frame.plane(Channel::Blue).data(), "output planes alias");
    }
}
void Invalid() {
    Source source(9, 3);
    for (auto dimensions : {std::array<int, 2>{0, 3}, {-1, 3}, {9, 0}, {9, -1}}) {
        auto view = source.view; view.width = dimensions[0]; view.height = dimensions[1];
        Reject<std::invalid_argument>([&] { Rgb444Frame::Copy(view); });
    }
    for (auto dimensions : {std::array<int, 2>{4097, 3}, {9, 4097}, {4096, 513},
                            {3840, 2160}, {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}}) {
        auto view = source.view; view.width = dimensions[0]; view.height = dimensions[1];
        Reject<std::length_error>([&] { Rgb444Frame::Copy(view); });
    }
    for (std::size_t c = 0; c < 3; ++c) {
        for (int fault = 0; fault < 5; ++fault) {
            auto view = source.view;
            std::array<PlaneView*, 3> planes{&view.red, &view.green, &view.blue};
            auto& plane = *planes[c];
            if (fault == 0) plane.data = nullptr;
            if (fault == 1) ++plane.data;
            if (fault == 2) --plane.bytes;
            if (fault == 3) plane.stride_bytes = 16;
            if (fault == 4) plane.stride_bytes = 33;
            Reject<std::invalid_argument>([&] { Rgb444Frame::Copy(view); });
        }
        for (auto stride : {std::numeric_limits<std::size_t>::max() & ~std::size_t{15},
                            static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) + 1}) {
            auto view = source.view;
            std::array<PlaneView*, 3> planes{&view.red, &view.green, &view.blue};
            planes[c]->stride_bytes = stride;
            planes[c]->bytes = std::numeric_limits<std::size_t>::max();
            Reject<std::length_error>([&] { Rgb444Frame::Copy(view); });
        }
        auto view = source.view; view.height = 2;
        std::array<PlaneView*, 3> views{&view.red, &view.green, &view.blue};
        views[c]->stride_bytes = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) + 1;
        views[c]->bytes = std::numeric_limits<std::size_t>::max();
        Reject<std::length_error>([&] { Rgb444Frame::Copy(view); });
        // A rejected replacement must preserve an existing full-resolution frame.
        auto frame = Rgb444Frame::Copy(source.view);
        const auto original = frame.plane(channels[c]);
        auto& input = source.planes[c];
        const auto saved = input[input.size() - 9];
        for (auto invalid : {std::uint16_t{4096}, std::uint16_t{65535}}) {
            input[input.size() - 9] = invalid; // final active sample, after partial copy
            Reject<std::invalid_argument>([&] { frame = Rgb444Frame::Copy(source.view); });
            Require(frame.plane(channels[c]) == original, "failed replacement changed frame");
        }
        input[input.size() - 9] = saved;
        Require(Rgb444Frame::Copy(source.view).plane(channels[c]) == original, "copy did not recover");
    }
    auto frame = Rgb444Frame::Copy(source.view);
    Reject<std::invalid_argument>([&] { frame.plane(static_cast<Channel>(-1)); });
    Reject<std::invalid_argument>([&] { frame.plane(static_cast<Channel>(3)); });
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected storage or rejection test selector");
        const std::string test = argv[1];
        if (test == "storage") Storage();
        else if (test == "rejection") Invalid();
        else throw std::runtime_error("unknown test selector");
        std::cout << "PASS RGB444 " << test << ": " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
