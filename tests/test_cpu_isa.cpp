// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "cpu/cpu_isa.h"

#include <iostream>
#include <stdexcept>

using namespace hypercine::detail;
namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Exception, class Function>
void Rejects(Function function) {
    try { function(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("unavailable/invalid request did not fail closed");
}

void X86StateMatrix() {
    // Every prerequisite bit and every required SSE/YMM/opmask/ZMM state bit.
    constexpr unsigned state_bits[] = {1, 2, 5, 6, 7};
    for (unsigned prerequisites = 0; prerequisites < 8; ++prerequisites) {
        for (unsigned hardware = 0; hardware < 16; ++hardware) {
            for (unsigned state = 0; state < 32; ++state) {
                X86CpuIsaProbe probe;
                probe.leaf1_ecx = prerequisites << 26;
                probe.leaf7_ebx = ((hardware & 1) ? 1u << 5 : 0) |
                    ((hardware & 2) ? 1u << 16 : 0) |
                    ((hardware & 4) ? 1u << 30 : 0) |
                    ((hardware & 8) ? 1u << 31 : 0);
                for (unsigned bit = 0; bit < 5; ++bit)
                    if (state & (1u << bit)) probe.xcr0 |= std::uint64_t{1} << state_bits[bit];
                const auto caps = DecodeX86CpuIsa(probe);
                Check(caps.avx2_hardware == ((hardware & 1) != 0), "AVX2 CPUID");
                Check(caps.avx512f_hardware == ((hardware & 2) != 0), "AVX512F CPUID");
                Check(caps.avx512bw_hardware == ((hardware & 4) != 0), "AVX512BW CPUID");
                Check(caps.avx512vl_hardware == ((hardware & 8) != 0), "AVX512VL CPUID");
                Check(caps.avx_os_state == (prerequisites == 7 && (state & 3) == 3), "AVX XSTATE");
                Check(caps.avx512_os_state == (prerequisites == 7 && state == 31), "AVX512 XSTATE");
                Check(!caps.neon, "x86 probe advertised NEON");
                Check(ResolveCpuIsa(CpuIsa::Auto, caps, {}) == CpuIsa::Scalar,
                      "hardware advertised an unimplemented backend");
            }
        }
    }
    // Irrelevant bits must not disable supported state or fabricate features.
    const auto caps = DecodeX86CpuIsa({0xffffffffu, 0xffffffffu, ~std::uint64_t{0}});
    Check(caps.avx_os_state && caps.avx512_os_state, "additional XCR0 state rejected");
    const auto empty = DecodeX86CpuIsa({0, 0, ~std::uint64_t{0}});
    Check(!empty.avx_os_state && !empty.avx512_os_state, "XCR0 trusted without prerequisites");
}

void SelectionMatrix() {
    for (unsigned features = 0; features < 128; ++features) {
        const CpuIsaCapabilities caps{bool(features & 1), bool(features & 2),
            bool(features & 4), bool(features & 8), bool(features & 16),
            bool(features & 32), bool(features & 64)};
        for (unsigned implementations = 0; implementations < 8; ++implementations) {
            const CpuIsaBackends backends{bool(implementations & 1),
                bool(implementations & 2), bool(implementations & 4)};
            const bool avx2 = (features & 17) == 17 && (implementations & 1);
            const bool avx512 = (features & 62) == 62 && (implementations & 2);
            const bool neon = (features & 64) && (implementations & 4);
            const CpuIsa expected = avx512 ? CpuIsa::Avx512 : avx2 ? CpuIsa::Avx2 :
                neon ? CpuIsa::Neon : CpuIsa::Scalar;
            Check(ResolveCpuIsa(CpuIsa::Auto, caps, backends) == expected, "Auto policy");
            Check(ResolveCpuIsa(CpuIsa::Scalar, caps, backends) == CpuIsa::Scalar, "forced scalar");
            for (const auto isa : {CpuIsa::Avx2, CpuIsa::Avx512, CpuIsa::Neon}) {
                const bool usable = isa == CpuIsa::Avx2 ? avx2 : isa == CpuIsa::Avx512 ? avx512 : neon;
                Check(CpuIsaUsable(isa, caps, backends) == usable, "ISA usability");
                if (usable) Check(ResolveCpuIsa(isa, caps, backends) == isa, "explicit selection");
                else Rejects<std::runtime_error>([&] { ResolveCpuIsa(isa, caps, backends); });
            }
            Check(!CpuIsaUsable(CpuIsa::Auto, caps, backends), "Auto is not a backend");
            Check(!CpuIsaUsable(static_cast<CpuIsa>(99), caps, backends), "unknown usability");
            Rejects<std::invalid_argument>([&] { ResolveCpuIsa(static_cast<CpuIsa>(99), caps, backends); });
        }
    }
}

void NativeProbe() {
    const auto caps = DetectCpuIsa();
    const auto backends = ValidatedCpuIsaBackends();
    Check(!backends.avx2 && !backends.avx512 && !backends.neon, "P0 backend registry changed");
    Check(ResolveCpuIsa(CpuIsa::Auto) == CpuIsa::Scalar, "P0 Auto must stay scalar");
    Check(ResolveCpuIsa(CpuIsa::Scalar) == CpuIsa::Scalar, "native scalar");
    for (const auto isa : {CpuIsa::Avx2, CpuIsa::Avx512, CpuIsa::Neon})
        Rejects<std::runtime_error>([&] { ResolveCpuIsa(isa); });
#if (defined(__aarch64__) && defined(__APPLE__)) || defined(_M_ARM64)
    Check(caps.neon, "native ARM64 NEON absent");
    Check(!caps.avx2_hardware && !caps.avx512f_hardware && !caps.avx_os_state,
          "ARM64 advertised x86 features");
#elif defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    Check(!caps.neon, "x86 advertised NEON");
    Check(!caps.avx512_os_state || caps.avx_os_state, "inconsistent native XSTATE");
#endif
    std::cout << "Native capabilities: avx2_hw=" << caps.avx2_hardware
              << " avx512f_hw=" << caps.avx512f_hardware
              << " avx512bw_hw=" << caps.avx512bw_hardware
              << " avx512vl_hw=" << caps.avx512vl_hardware
              << " avx_os=" << caps.avx_os_state
              << " avx512_os=" << caps.avx512_os_state
              << " neon=" << caps.neon << "; Auto=Scalar\n";
}
}
int main() {
    try {
        X86StateMatrix();
        SelectionMatrix();
        NativeProbe();
        std::cout << "PASS: 4096 x86 probes, 1024 selection configurations, native probe\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
