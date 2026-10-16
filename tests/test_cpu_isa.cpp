// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/cpu_isa.h"
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
}

int main() {
    using namespace hypercine;
    const CpuIsaCapabilities all{true, true, true, true, true};
    const CpuIsaCapabilities empty{};
    const auto host = QueryHostCpuIsa();
    Require(ResolveCpuIsa(CpuIsa::Auto, all, empty).selected == CpuIsa::Scalar,
            "Auto must not pick a detected but not implemented ISA");
    Require(ResolveCpuIsa(CpuIsa::Scalar, empty, empty).ok,
            "Scalar always allowed");
    for (auto requested : {CpuIsa::Avx2, CpuIsa::Avx512, CpuIsa::Neon}) {
        Require(!ResolveCpuIsa(requested, all, empty).ok,
                "Forced ISA not allowed without an implemented backend");
        Require(!ResolveCpuIsa(requested, empty, all).ok,
                "Forced ISA not allowed without platform support");
    }
    Require(!ResolveCpuIsa(static_cast<CpuIsa>(999), all, all).ok,
            "Unknown ISA fails closed");
    Require(ResolveCpuIsa(CpuIsa::Auto, all, all).selected == CpuIsa::Avx512,
            "Prefer AVX512 for a hypothetical validated x86 host");
    CpuIsaCapabilities only_avx2{};
    only_avx2.avx2 = true;
    Require(ResolveCpuIsa(CpuIsa::Auto, only_avx2, all).selected == CpuIsa::Avx2,
            "AVX2 only CPU selected");
    CpuIsaCapabilities missing_vl = all;
    missing_vl.avx512vl = false;
    Require(!ResolveCpuIsa(CpuIsa::Avx512, missing_vl, all).ok,
            "Missing AVX512VL rejected");
    Require(ResolveCpuIsa(CpuIsa::Auto, missing_vl, all).selected == CpuIsa::Avx2,
            "Missing AVX512VL falls back only under Auto policy");
    CpuIsaCapabilities arm{};
    arm.neon = true;
    Require(ResolveCpuIsa(CpuIsa::Auto, arm, arm).selected == CpuIsa::Neon,
            "AArch64 NEON can be selected when implemented");
    Require(!ResolveCpuIsa(CpuIsa::Avx2, arm, all).ok,
            "No x86 ISA on ARM host");
    const auto actual = ResolveCpuIsa(CpuIsa::Auto, host, empty);
    Require(actual.ok && actual.selected == CpuIsa::Scalar,
            "P0 production capability probe has no registered SIMD backend");
    for (auto isa : {CpuIsa::Avx2, CpuIsa::Avx512, CpuIsa::Neon}) {
        Require(!ResolveCpuIsa(isa, host, empty).ok,
                "P0 may not execute arbitrary instructions on the host");
    }
    std::cout << "CPU ISA probe PASS: host avx2=" << host.avx2
              << " avx512f=" << host.avx512f << " bw=" << host.avx512bw
              << " vl=" << host.avx512vl << " neon=" << host.neon << '\n';
    return 0;
}
