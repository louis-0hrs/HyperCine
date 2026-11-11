// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/wavelet.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
int fail_after = -1;
int allocation_attempts = 0;
int live_allocations = 0;
int rejected_cases = 0;
constexpr int16_t canary = 12345;
constexpr int int_max = (std::numeric_limits<int>::max)();
using Buffer = std::array<int16_t, 128>;
struct Buffers {
    alignas(32) Buffer input{};
    alignas(32) std::array<Buffer, 4> output{};
    Buffers() { input.fill(8); Clear(); }
    void Clear() { for (auto& plane : output) plane.fill(canary); }
    bool Untouched() const {
        for (const auto& plane : output)
            if (!std::all_of(plane.begin(), plane.end(), [](int16_t v) { return v == canary; })) return false;
        return std::all_of(input.begin(), input.end(), [](int16_t v) { return v == 8; });
    }
};
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct AllocationFailure {
    explicit AllocationFailure(int ordinal) { fail_after = ordinal; allocation_attempts = 0; }
    ~AllocationFailure() { fail_after = -1; }
    AllocationFailure(const AllocationFailure&) = delete;
    AllocationFailure& operator=(const AllocationFailure&) = delete;
};
template<class Call> void Rejected(Buffers& b, Call call) {
    b.Clear();
    const auto live_before = live_allocations;
    HyperCineStatus status;
    {
        // Invalid input must be rejected before even trying to reserve scratch.
        AllocationFailure failure(0);
        status = call();
    }
    Require(status == HYPERCINE_STATUS_INVALID_ARGUMENT, "malformed call was not rejected");
    Require(allocation_attempts == 0 && live_allocations == live_before,
            "malformed call allocated scratch");
    Require(b.Untouched(), "rejected call changed input/output");
    ++rejected_cases;
}
void CheckPlane(const Buffer& p, int columns, int rows, int stride, int16_t value) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const bool active = i / stride < static_cast<std::size_t>(rows) &&
                            i % stride < static_cast<std::size_t>(columns);
        Require(p[i] == (active ? value : canary), "valid result or output padding changed");
    }
}
HyperCineStatus Forward2D(Buffers& b, int width=6, int height=6, int source_stride=8,
                        int ll=5, int lh=5, int hl=5, int hh=5) {
    return HyperCine_ForwardSpatial2D_CPU(b.input.data(), width, height, source_stride,
        b.output[0].data(), ll, b.output[1].data(), lh,
        b.output[2].data(), hl, b.output[3].data(), hh);
}
HyperCineStatus Inverse2D(Buffers& b, int width=3, int height=3,
                        int ll=5, int lh=5, int hl=5, int hh=5, int destination_stride=8) {
    return HyperCine_InverseSpatial2D_CPU(b.input.data(), ll, b.input.data(), lh,
        b.input.data(), hl, b.input.data(), hh, width, height,
        b.output[0].data(), destination_stride);
}
void InvalidCalls() {
    Buffers b;
    for (auto function : {HyperCine_ForwardHorizontal26_CPU, HyperCine_ForwardVertical26_CPU}) {
        Rejected(b, [&] { return function(nullptr,6,6,8,b.output[0].data(),8,b.output[1].data(),8); });
        Rejected(b, [&] { return function(b.input.data(),6,6,8,nullptr,8,b.output[1].data(),8); });
        Rejected(b, [&] { return function(b.input.data(),6,6,8,b.output[0].data(),8,nullptr,8); });
        Rejected(b, [&] { return function(b.input.data(),6,0,8,b.output[0].data(),8,b.output[1].data(),8); });
        Rejected(b, [&] { return function(b.input.data(),0,6,8,b.output[0].data(),8,b.output[1].data(),8); });
        for (int stride : {-1, 0, 2, int_max}) {
            Rejected(b, [&] { return function(b.input.data(),6,6,stride,b.output[0].data(),8,b.output[1].data(),8); });
            Rejected(b, [&] { return function(b.input.data(),6,6,8,b.output[0].data(),stride,b.output[1].data(),8); });
            Rejected(b, [&] { return function(b.input.data(),6,6,8,b.output[0].data(),8,b.output[1].data(),stride); });
        }
    }
    Rejected(b, [&] { return HyperCine_ForwardHorizontal26_CPU(b.input.data(),5,6,8,b.output[0].data(),8,b.output[1].data(),8); });
    Rejected(b, [&] { return HyperCine_ForwardVertical26_CPU(b.input.data(),6,5,8,b.output[0].data(),8,b.output[1].data(),8); });
    for (auto function : {HyperCine_InverseHorizontal26_CPU, HyperCine_InverseVertical26_CPU}) {
        Rejected(b, [&] { return function(nullptr,5,b.input.data(),5,3,3,b.output[0].data(),8); });
        Rejected(b, [&] { return function(b.input.data(),5,nullptr,5,3,3,b.output[0].data(),8); });
        Rejected(b, [&] { return function(b.input.data(),5,b.input.data(),5,3,3,nullptr,8); });
        Rejected(b, [&] { return function(b.input.data(),5,b.input.data(),5,0,3,b.output[0].data(),8); });
        Rejected(b, [&] { return function(b.input.data(),5,b.input.data(),5,3,0,b.output[0].data(),8); });
        for (int stride : {-1, 0, 2, int_max}) {
            Rejected(b, [&] { return function(b.input.data(),stride,b.input.data(),5,3,3,b.output[0].data(),8); });
            Rejected(b, [&] { return function(b.input.data(),5,b.input.data(),stride,3,3,b.output[0].data(),8); });
            Rejected(b, [&] { return function(b.input.data(),5,b.input.data(),5,3,3,b.output[0].data(),stride); });
        }
    }
    // Original UBSan reproductions: overflow occurred in a guard, before data access.
    Rejected(b, [&] { return HyperCine_InverseHorizontal26_CPU(b.input.data(),int_max,b.input.data(),int_max,
        int_max,1,b.output[0].data(),1); });
    Rejected(b, [&] { return HyperCine_InverseVertical26_CPU(b.input.data(),5,b.input.data(),5,
        3,int_max,b.output[0].data(),8); });
    Rejected(b, [&] { return Inverse2D(b,3,int_max); });
    Rejected(b, [&] { return Inverse2D(b,int_max,3,int_max,int_max,int_max,int_max,int_max); });
    Rejected(b, [&] { return Forward2D(b,6,5); });
    Rejected(b, [&] { return Inverse2D(b,2,3); });
    for (int stride : {-1, 0, 2, int_max}) {
        Rejected(b, [&] { return Forward2D(b,6,6,stride); });
        Rejected(b, [&] { return Forward2D(b,6,6,8,stride); });
        Rejected(b, [&] { return Forward2D(b,6,6,8,5,stride); });
        Rejected(b, [&] { return Forward2D(b,6,6,8,5,5,stride); });
        Rejected(b, [&] { return Forward2D(b,6,6,8,5,5,5,stride); });
        Rejected(b, [&] { return Inverse2D(b,3,3,stride); });
        Rejected(b, [&] { return Inverse2D(b,3,3,5,stride); });
        Rejected(b, [&] { return Inverse2D(b,3,3,5,5,stride); });
        Rejected(b, [&] { return Inverse2D(b,3,3,5,5,5,stride); });
        Rejected(b, [&] { return Inverse2D(b,3,3,5,5,5,5,stride); });
    }
}
void ValidCalls() {
    Buffers b;
    Require(HyperCine_ForwardHorizontal26_CPU(b.input.data(),6,6,8,b.output[0].data(),5,b.output[1].data(),5)==0,
            "valid horizontal forward refused");
    CheckPlane(b.output[0],3,6,5,16); CheckPlane(b.output[1],3,6,5,0);
    b.Clear();
    Require(HyperCine_ForwardVertical26_CPU(b.input.data(),6,6,8,b.output[0].data(),8,b.output[1].data(),8)==0,
            "valid vertical forward refused");
    CheckPlane(b.output[0],6,3,8,16); CheckPlane(b.output[1],6,3,8,0);
    b.Clear();
    Require(Forward2D(b)==0,"valid spatial forward refused");
    CheckPlane(b.output[0],3,3,5,32);
    for(int i=1;i<4;++i) CheckPlane(b.output[i],3,3,5,0);
    alignas(32) Buffer zeros{};
    b.Clear();
    Require(HyperCine_InverseHorizontal26_CPU(b.input.data(),5,zeros.data(),5,3,3,b.output[0].data(),8)==0,
            "valid horizontal inverse refused");
    CheckPlane(b.output[0],6,3,8,4);
    b.Clear();
    Require(HyperCine_InverseVertical26_CPU(b.input.data(),5,zeros.data(),5,3,3,b.output[0].data(),5)==0,
            "valid vertical inverse refused");
    CheckPlane(b.output[0],3,6,5,4);
    b.Clear();
    Require(HyperCine_InverseSpatial2D_CPU(b.input.data(),5,zeros.data(),5,zeros.data(),5,zeros.data(),5,
        3,3,b.output[0].data(),8)==0,"valid spatial inverse refused");
    CheckPlane(b.output[0],6,6,8,2);
    Require(std::all_of(b.input.begin(),b.input.end(),[](int16_t v){return v==8;}),"source changed");
}
void AllocationFailures() {
    Buffers b;
    for (bool inverse : {false,true}) for (int ordinal : {0,1}) {
        b.Clear();
        const auto live_before=live_allocations;
        HyperCineStatus status;
        {
            AllocationFailure failure(ordinal);
            status=inverse ? Inverse2D(b) : Forward2D(b);
        }
        Require(status==HYPERCINE_STATUS_OUT_OF_MEMORY,"scratch failure escaped status contract");
        Require(allocation_attempts==ordinal+1,"failure injection missed scratch allocation");
        Require(live_allocations==live_before,"partial scratch allocation leaked");
        Require(b.Untouched(),"allocation failure changed caller storage");
        Require((inverse ? Inverse2D(b) : Forward2D(b))==0,"retry after allocation failure failed");
        Require(live_allocations==live_before,"successful retry leaked scratch");
    }
}
} // namespace

// Executable-local deterministic allocation failure. No codec test seam or
// process-global production state is introduced by this single-threaded test.
void* operator new(std::size_t bytes) {
    if(fail_after>=0) {
        ++allocation_attempts;
        if(fail_after==0) throw std::bad_alloc();
        --fail_after;
    }
    void* memory=std::malloc(bytes ? bytes : 1);
    if(!memory) throw std::bad_alloc();
    ++live_allocations;
    return memory;
}
void operator delete(void* memory) noexcept {
    if(memory) { --live_allocations; std::free(memory); }
}
void operator delete(void* memory,std::size_t) noexcept { ::operator delete(memory); }

int main() {
    try {
        InvalidCalls();
        ValidCalls();
        AllocationFailures();
        std::printf("PASS: %d rejected calls before allocation/stores, six valid padded transforms, "
                    "four scratch allocation failures with cleanup and retry\n",rejected_cases);
        return 0;
    } catch(...) {
        std::fputs("FAIL: C++ exception escaped CPU status API\n",stderr);
        return 1;
    }
}
