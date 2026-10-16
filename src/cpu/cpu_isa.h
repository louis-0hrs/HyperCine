// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include <cstdint>

namespace hypercine { namespace detail {

// Internal capability/selection boundary; no codec dispatch is changed by P0.
enum class CpuIsa { Auto, Scalar, Avx2, Avx512, Neon };

struct CpuIsaCapabilities {
    bool avx2_hardware = false;
    bool avx512f_hardware = false;
    bool avx512bw_hardware = false;
    bool avx512vl_hardware = false;
    bool avx_os_state = false;     // XSAVE + OSXSAVE + AVX, XCR0 SSE/YMM
    bool avx512_os_state = false; // also XCR0 opmask/ZMM_hi256/Hi16_ZMM
    bool neon = false;            // native AArch64 ASIMD, not ARM32/SVE
};

// Raw probe seam for deterministic tests. Unavailable CPUID leaves stay zero.
// XCR0 must only be read after CPUID confirms XSAVE + OSXSAVE + AVX.
struct X86CpuIsaProbe {
    std::uint32_t leaf1_ecx = 0;
    std::uint32_t leaf7_ebx = 0;
    std::uint64_t xcr0 = 0;
};
CpuIsaCapabilities DecodeX86CpuIsa(const X86CpuIsaProbe&) noexcept;
CpuIsaCapabilities DetectCpuIsa() noexcept;

// Each true entry promises BOTH an implemented and bit-exact validated backend.
// Hardware availability alone must never populate this registry.
struct CpuIsaBackends {
    bool avx2 = false;
    bool avx512 = false;
    bool neon = false;
};
CpuIsaBackends ValidatedCpuIsaBackends() noexcept; // P0: scalar only
bool CpuIsaUsable(CpuIsa, const CpuIsaCapabilities&, const CpuIsaBackends&) noexcept;
// Auto alone may fall back; explicit unavailable/unknown requests throw.
CpuIsa ResolveCpuIsa(CpuIsa, const CpuIsaCapabilities&, const CpuIsaBackends&);
CpuIsa ResolveCpuIsa(CpuIsa);

}} // namespace hypercine::detail
