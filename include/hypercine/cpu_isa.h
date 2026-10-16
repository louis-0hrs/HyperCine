// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

namespace hypercine {

// CPU execution permissions only. A detected ISA is not proof a SIMD kernel
// exists or passes codec bit-exactness tests. Scalar is always available.
enum class CpuIsa { Auto, Scalar, Avx2, Avx512, Neon };

struct CpuIsaCapabilities {
    bool avx2 = false;
    bool avx512f = false;
    bool avx512bw = false;
    bool avx512vl = false;
    bool neon = false;
};

struct CpuIsaSelection {
    bool ok = false;
    CpuIsa selected = CpuIsa::Scalar;
    const char* reason = "unsupported ISA request";
};

// Includes operating-system SIMD-register state where applicable. Never use
// CPU model name alone as permission to execute AVX2/AVX-512 instructions.
CpuIsaCapabilities QueryHostCpuIsa() noexcept;

// The implementation mask MUST be sourced from backends that have passed their
// bit-exact conformance gates. P0 ships no SIMD kernel, so pass {} in production
// until a validated backend has been registered. Auto only uses intersection of
// hardware/OS capabilities AND implemented backends; forced missing ISA rejects.
CpuIsaSelection ResolveCpuIsa(CpuIsa requested,
                              const CpuIsaCapabilities& host,
                              const CpuIsaCapabilities& implemented) noexcept;
const char* CpuIsaName(CpuIsa isa) noexcept;

} // namespace hypercine
