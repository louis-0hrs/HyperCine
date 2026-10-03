// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_profile_hooks.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
namespace {
using Clock = std::chrono::steady_clock;
struct Scope { int stage; Clock::time_point start; long long accounted_at_start; };
thread_local std::array<Scope, 64> stack;
thread_local int depth = 0;
std::array<std::atomic<long long>, 8> nanoseconds{};
std::array<std::atomic<unsigned long long>, 8> calls{};
std::atomic<long long> accounted{0};
}
// Valid only for this serialized reference: caller waits while its sole output
// worker runs. Global accounting subtracts that worker's nested scopes from the
// caller's reconstruction wall interval. Never use for overlapping frame jobs.
extern "C" void DecodeProfileReset() {
    depth = 0; accounted.store(0);
    for (int k = 0; k < 8; ++k) { nanoseconds[k].store(0); calls[k].store(0); }
}
extern "C" void DecodeProfileEnter(int stage) {
    if (depth == int(stack.size())) std::abort();
    stack[depth++] = {stage, Clock::now(), accounted.load()};
}
extern "C" void DecodeProfileLeave() {
    const auto end = Clock::now();
    auto& s = stack[--depth];
    const auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(end - s.start).count();
    const long long own = total - (accounted.load() - s.accounted_at_start);
    if (own < 0) std::abort();
    nanoseconds[s.stage] += own;
    ++calls[s.stage];
    accounted += own;
}
extern "C" double DecodeProfileMilliseconds(int stage) { return double(nanoseconds[stage].load()) / 1e6; }
extern "C" unsigned long long DecodeProfileCalls(int stage) { return calls[stage].load(); }
