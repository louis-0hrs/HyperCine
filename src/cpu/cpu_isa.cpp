// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/cpu_isa.h"
#include <initializer_list>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#  include <intrin.h>
#endif

namespace hypercine {
namespace {
bool Available(CpuIsa isa, const CpuIsaCapabilities& c) noexcept {
    switch (isa) {
    case CpuIsa::Scalar: return true;
    case CpuIsa::Avx2: return c.avx2;
    case CpuIsa::Avx512: return c.avx512f && c.avx512bw && c.avx512vl;
    case CpuIsa::Neon: return c.neon;
    case CpuIsa::Auto: break;
    }
    return false;
}
} // namespace

CpuIsaCapabilities QueryHostCpuIsa() noexcept {
    CpuIsaCapabilities caps{};
#if defined(__aarch64__) || defined(_M_ARM64)
    // ASIMD is mandatory in the AArch64 base architecture on supported OSes.
    caps.neon = true;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    // 32-bit ARM NEON binaries are built for NEON-capable processors.
    caps.neon = true;
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    // GCC/Clang builtins check CPU flags AND XSAVE/XCR0 OS register support.
    __builtin_cpu_init();
    caps.avx2 = __builtin_cpu_supports("avx2") != 0;
    caps.avx512f = __builtin_cpu_supports("avx512f") != 0;
    caps.avx512bw = __builtin_cpu_supports("avx512bw") != 0;
    caps.avx512vl = __builtin_cpu_supports("avx512vl") != 0;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int leaf[4] = {};
    __cpuid(leaf, 0);
    const int max_leaf = leaf[0];
    if (max_leaf < 7) return caps;
    __cpuid(leaf, 1);
    const bool osxsave = (static_cast<unsigned>(leaf[2]) & (1u << 27)) != 0;
    const bool avx_cpu = (static_cast<unsigned>(leaf[2]) & (1u << 28)) != 0;
    if (!osxsave || !avx_cpu) return caps;
    const unsigned __int64 xcr0 = _xgetbv(0);
    // XMM+YMM state required for AVX2; opmask/ZMM state additionally for AVX512.
    const bool ymm_ok = (xcr0 & 0x6u) == 0x6u;
    const bool zmm_ok = (xcr0 & 0xE6u) == 0xE6u;
    __cpuidex(leaf, 7, 0);
    const unsigned ebx = static_cast<unsigned>(leaf[1]);
    caps.avx2 = ymm_ok && (ebx & (1u << 5)) != 0;
    caps.avx512f = zmm_ok && (ebx & (1u << 16)) != 0;
    caps.avx512bw = zmm_ok && (ebx & (1u << 30)) != 0;
    caps.avx512vl = zmm_ok && (ebx & (1u << 31)) != 0;
#endif
    return caps;
}

CpuIsaSelection ResolveCpuIsa(CpuIsa requested,
                              const CpuIsaCapabilities& host,
                              const CpuIsaCapabilities& implemented) noexcept {
    if (requested == CpuIsa::Auto) {
        // Only the actual backends supplied by the caller can be selected.
        // Never promote a SIMD feature merely because CPUID reports it.
        for (CpuIsa candidate : {CpuIsa::Avx512, CpuIsa::Avx2, CpuIsa::Neon}) {
            if (Available(candidate, host) && Available(candidate, implemented))
                return {true, candidate, "available validated backend"};
        }
        return {true, CpuIsa::Scalar, "portable scalar backend"};
    }
    if (Available(requested, host) && Available(requested, implemented))
        return {true, requested, "explicit supported backend"};
    return {false, CpuIsa::Scalar, "requested SIMD ISA unavailable or unimplemented"};
}

const char* CpuIsaName(CpuIsa isa) noexcept {
    switch (isa) {
    case CpuIsa::Auto: return "auto";
    case CpuIsa::Scalar: return "scalar";
    case CpuIsa::Avx2: return "avx2";
    case CpuIsa::Avx512: return "avx512bw";
    case CpuIsa::Neon: return "neon";
    }
    return "invalid";
}
} // namespace hypercine
