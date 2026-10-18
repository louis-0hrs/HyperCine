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

// Test executable only: intercept the actual allocator used by the frame.
// No threads or production allocator changes; ordinary allocations are untouched.
namespace {
int fail_after = -1;
int live_allocations = 0;
int injected_failures = 0;
void Require(bool condition) {
    if (!condition) throw std::runtime_error("RGB444 allocation contract failed");
}
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    if (fail_after == 0) {
        ++injected_failures;
        throw std::bad_alloc();
    }
    if (fail_after > 0) --fail_after;
    const auto align = static_cast<std::size_t>(alignment);
    void* memory = nullptr;
#ifdef _MSC_VER
    memory = _aligned_malloc(bytes ? bytes : 1, align);
#else
    if (posix_memalign(&memory, align, bytes ? bytes : 1) != 0) memory = nullptr;
#endif
    if (!memory) throw std::bad_alloc();
    ++live_allocations;
    return memory;
}
void operator delete(void* memory, std::align_val_t) noexcept {
    if (!memory) return;
    --live_allocations;
#ifdef _MSC_VER
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}
int main() {
    using namespace hypercine::rgb444;
    try {
        {
            alignas(32) const std::uint16_t samples[16] = {
                0, 1, 2, 3, 4, 5, 6, 4095, 7, 8, 9, 10, 11, 12, 13, 4094};
            // Aliased input planes are explicitly permitted; output must own copies.
            const PlaneView plane{samples, sizeof(samples), 16};
            const FrameView view{8, 2, plane, plane, plane};
            auto frame = Rgb444Frame::Copy(view);
            const auto baseline = live_allocations;
            Require(baseline == 3);
            for (int allocation = 0; allocation < 3; ++allocation) {
                fail_after = allocation;
                bool rejected = false;
                try { frame = Rgb444Frame::Copy(view); }
                catch (const std::bad_alloc&) { rejected = true; }
                fail_after = -1;
                Require(rejected && live_allocations == baseline);
                Require(frame.width() == 8 && frame.height() == 2 && frame.stride_bytes() == 16);
                for (auto channel : {Channel::Red, Channel::Green, Channel::Blue}) {
                    const auto& output = frame.plane(channel);
                    Require(output.size() == 16 && output.data() != samples);
                    for (int i = 0; i < 16; ++i) Require(output[i] == samples[i]);
                }
            }
            Require(injected_failures == 3);
            frame = Rgb444Frame::Copy(view);
            Require(live_allocations == baseline);
            Require(frame.plane(Channel::Red).data() != frame.plane(Channel::Green).data());
            Require(frame.plane(Channel::Blue).data() != frame.plane(Channel::Green).data());
        }
        Require(live_allocations == 0);
        std::cout << "PASS: three allocation failures, complete state preservation, recovery, zero live aligned allocations\n";
        return 0;
    } catch (const std::exception& error) {
        fail_after = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
