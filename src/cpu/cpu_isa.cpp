// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "cpu_isa.h"

#include <initializer_list>
#include <stdexcept>

#if defined(_M_IX86) || defined(_M_X64)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

namespace hypercine { namespace detail {
namespace {
constexpr std::uint32_t kXsave = 1u << 26;
constexpr std::uint32_t kOsxsave = 1u << 27;
constexpr std::uint32_t kAvx = 1u << 28;
constexpr std::uint32_t kAvxPrerequisites = kXsave | kOsxsave | kAvx;
constexpr std::uint64_t kYmmState = 0x6;
constexpr std::uint64_t kZmmState = 0xe6;
}

CpuIsaCapabilities DecodeX86CpuIsa(const X86CpuIsaProbe& probe) noexcept {
    CpuIsaCapabilities caps;
    caps.avx2_hardware = (probe.leaf7_ebx & (1u << 5)) != 0;
    caps.avx512f_hardware = (probe.leaf7_ebx & (1u << 16)) != 0;
    caps.avx512bw_hardware = (probe.leaf7_ebx & (1u << 30)) != 0;
    caps.avx512vl_hardware = (probe.leaf7_ebx & (1u << 31)) != 0;
    caps.avx_os_state = (probe.leaf1_ecx & kAvxPrerequisites) == kAvxPrerequisites &&
        (probe.xcr0 & kYmmState) == kYmmState;
    caps.avx512_os_state = caps.avx_os_state &&
        (probe.xcr0 & kZmmState) == kZmmState;
    return caps;
}

CpuIsaCapabilities DetectCpuIsa() noexcept {
    X86CpuIsaProbe probe;
#if defined(_M_IX86) || defined(_M_X64)
    int regs[4] = {};
    __cpuidex(regs, 0, 0);
    const auto max_leaf = static_cast<unsigned>(regs[0]);
    if (max_leaf >= 1) {
        __cpuidex(regs, 1, 0);
        probe.leaf1_ecx = static_cast<std::uint32_t>(regs[2]);
        if ((probe.leaf1_ecx & kAvxPrerequisites) == kAvxPrerequisites)
            probe.xcr0 = _xgetbv(0);
    }
    if (max_leaf >= 7) {
        __cpuidex(regs, 7, 0);
        probe.leaf7_ebx = static_cast<std::uint32_t>(regs[1]);
    }
#elif defined(__i386__) || defined(__x86_64__)
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    const auto max_leaf = __get_cpuid_max(0, nullptr);
    if (max_leaf >= 1) {
        __cpuid_count(1, 0, eax, ebx, ecx, edx);
        probe.leaf1_ecx = ecx;
        if ((probe.leaf1_ecx & kAvxPrerequisites) == kAvxPrerequisites) {
            // Only this guarded instruction needs XSAVE, no TU-wide ISA flags.
            unsigned lo = 0, hi = 0;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            probe.xcr0 = (static_cast<std::uint64_t>(hi) << 32) | lo;
        }
    }
    if (max_leaf >= 7) {
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        probe.leaf7_ebx = ebx;
    }
#elif defined(__aarch64__) && defined(__linux__)
    CpuIsaCapabilities caps;
    caps.neon = (getauxval(AT_HWCAP) & HWCAP_ASIMD) != 0;
    return caps;
#elif (defined(__aarch64__) && defined(__APPLE__)) || defined(_M_ARM64)
    // Apple Silicon / Windows ARM64 ABI guarantees native Advanced SIMD.
    CpuIsaCapabilities caps;
    caps.neon = true;
    return caps;
#endif
    return DecodeX86CpuIsa(probe); // unknown platforms remain scalar
}

CpuIsaBackends ValidatedCpuIsaBackends() noexcept { return {}; }

bool CpuIsaUsable(CpuIsa isa, const CpuIsaCapabilities& caps,
                  const CpuIsaBackends& backends) noexcept {
    switch (isa) {
    case CpuIsa::Scalar: return true;
    case CpuIsa::Avx2: return backends.avx2 && caps.avx2_hardware && caps.avx_os_state;
    case CpuIsa::Avx512: return backends.avx512 && caps.avx512f_hardware &&
        caps.avx512bw_hardware && caps.avx512vl_hardware && caps.avx_os_state &&
        caps.avx512_os_state;
    case CpuIsa::Neon: return backends.neon && caps.neon;
    default: return false;
    }
}

CpuIsa ResolveCpuIsa(CpuIsa requested, const CpuIsaCapabilities& caps,
                     const CpuIsaBackends& backends) {
    if (requested == CpuIsa::Auto) {
        for (const auto isa : {CpuIsa::Avx512, CpuIsa::Avx2, CpuIsa::Neon})
            if (CpuIsaUsable(isa, caps, backends)) return isa;
        return CpuIsa::Scalar;
    }
    switch (requested) {
    case CpuIsa::Scalar: case CpuIsa::Avx2: case CpuIsa::Avx512: case CpuIsa::Neon:
        if (CpuIsaUsable(requested, caps, backends)) return requested;
        throw std::runtime_error("requested CPU ISA has no usable validated backend");
    default: throw std::invalid_argument("unknown CPU ISA request");
    }
}

CpuIsa ResolveCpuIsa(CpuIsa requested) {
    return ResolveCpuIsa(requested, DetectCpuIsa(), ValidatedCpuIsaBackends());
}
}} // namespace hypercine::detail
