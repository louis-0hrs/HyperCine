// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/rgb444.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#ifdef _MSC_VER
#include <malloc.h>
#endif

// Executable-local interception of the production AlignedAllocator. Ordinary
// allocations are untouched; this single-threaded test links no other tests.
namespace {
int fail_after = -1;
int attempts = 0;
int live = 0;
int released = 0;
int failures = 0;
void Require(bool condition) {
    if (!condition) throw std::runtime_error("RG48 allocation contract failed");
}
struct FailureWindow {
    explicit FailureWindow(int ordinal) { attempts = 0; fail_after = ordinal; }
    ~FailureWindow() { fail_after = -1; }
    FailureWindow(const FailureWindow&) = delete;
    FailureWindow& operator=(const FailureWindow&) = delete;
};
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    ++attempts;
    if (fail_after == 0) {
        ++failures;
        throw std::bad_alloc();
    }
    if (fail_after > 0) --fail_after;
    void* memory = nullptr;
#ifdef _MSC_VER
    memory = _aligned_malloc(bytes ? bytes : 1, static_cast<std::size_t>(alignment));
#else
    if (posix_memalign(&memory, static_cast<std::size_t>(alignment), bytes ? bytes : 1))
        memory = nullptr;
#endif
    if (!memory) throw std::bad_alloc();
    ++live;
    return memory;
}
void operator delete(void* memory, std::align_val_t) noexcept {
    if (!memory) return;
    --live;
    ++released;
#ifdef _MSC_VER
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

namespace {
using namespace hypercine::rgb444;
constexpr std::array<Channel, 3> channels{Channel::Red, Channel::Green, Channel::Blue};

void Fill(const Rg48View& view, std::uint16_t* samples, unsigned salt) {
    for (int y = 0; y < view.height; ++y)
        for (std::size_t x = 0; x < view.stride_bytes / 2; ++x)
            samples[y * (view.stride_bytes / 2) + x] =
                static_cast<std::uint16_t>((salt + y * 997 + x * 313) & 0xffff);
}

void Check(const Rgb444Frame& frame, const Rg48View& source) {
    Require(frame.width() == source.width && frame.height() == source.height);
    const auto stride = (static_cast<std::size_t>(source.width) * 2 + 15) & ~std::size_t{15};
    Require(frame.stride_bytes() == stride);
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const auto& plane = frame.plane(channels[c]);
        Require(plane.size() == stride / 2 * source.height);
        Require(reinterpret_cast<std::uintptr_t>(plane.data()) % 32 == 0);
        for (int y = 0; y < source.height; ++y)
            for (std::size_t x = 0; x < stride / 2; ++x) {
                const auto expected = x < static_cast<std::size_t>(source.width)
                    ? source.data[y * (source.stride_bytes / 2) + 3 * x + c] / 16 : 0;
                Require(plane[y * (stride / 2) + x] == expected);
            }
    }
    Require(frame.plane(Channel::Red).data() != frame.plane(Channel::Green).data());
    Require(frame.plane(Channel::Blue).data() != frame.plane(Channel::Green).data());
    Require(frame.plane(Channel::Red).data() != frame.plane(Channel::Blue).data());
}

void Run() {
    alignas(32) std::array<std::uint16_t, 32> old_samples{};
    alignas(32) std::array<std::uint16_t, 96> new_samples{};
    const Rg48View old_view{5, 2, old_samples.data(), sizeof(old_samples), 32};
    // Exact readable extent: no requirement to supply trailing last-row padding.
    const Rg48View new_view{9, 3, new_samples.data(), 2 * 64 + 9 * 6, 64};
    Fill(old_view, old_samples.data(), 17);
    Fill(new_view, new_samples.data(), 40001);
    const auto old_snapshot = old_samples;
    const auto new_snapshot = new_samples;
    for (int ordinal = 0; ordinal < 3; ++ordinal) {
        Require(live == 0);
        {
            auto frame = Rgb444Frame::CopyRg48(old_view);
            Check(frame, old_view);
            Require(live == 3);
            const std::array<const std::uint16_t*, 3> addresses{
                frame.plane(Channel::Red).data(), frame.plane(Channel::Green).data(),
                frame.plane(Channel::Blue).data()};
            const auto released_before = released;
            const auto failures_before = failures;
            bool rejected = false;
            {
                FailureWindow failure(ordinal);
                try { frame = Rgb444Frame::CopyRg48(new_view); }
                catch (const std::bad_alloc&) { rejected = true; }
            }
            Require(rejected && attempts == ordinal + 1 && failures == failures_before + 1);
            Require(live == 3 && released == released_before + ordinal);
            Check(frame, old_view);
            for (std::size_t c = 0; c < channels.size(); ++c)
                Require(frame.plane(channels[c]).data() == addresses[c]);
            // A retry must replace geometry and all channels, with no stale bytes.
            attempts = 0;
            frame = Rgb444Frame::CopyRg48(new_view);
            Require(attempts == 3 && live == 3);
            Check(frame, new_view);
            Require(old_samples == old_snapshot && new_samples == new_snapshot);
        }
        Require(live == 0);
    }
    Require(failures == 3);
    // Malformed views must fail validation before even the first allocation.
    for (int invalid = 0; invalid < 4; ++invalid) {
        auto view = new_view;
        if (invalid == 0) --view.bytes;
        if (invalid == 1) ++view.data;
        if (invalid == 2) --view.stride_bytes;
        if (invalid == 3) view.width = 0;
        bool rejected = false;
        {
            FailureWindow failure(0);
            try { (void)Rgb444Frame::CopyRg48(view); }
            catch (const std::invalid_argument&) { rejected = true; }
        }
        Require(rejected && attempts == 0 && live == 0 && failures == 3);
    }
}
}

int main() {
    try {
        Run();
        std::cout << "PASS: RG48 three plane failures, unchanged prior frame, partial cleanup, "
                     "geometry-changing retries, four preallocation rejections, zero live allocations\n";
        return 0;
    } catch (const std::exception& error) {
        fail_after = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
