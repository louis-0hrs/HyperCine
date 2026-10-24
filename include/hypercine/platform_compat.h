// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

/**
 * @file platform_compat.h
 * @brief Small portability shims for the Windows-first test/benchmark tooling.
 *
 * The measurement code in tests/ and benchmarks/ was written against MSVC and
 * the Windows API ( _popen, GetSystemInfo, _MSC_VER ). Those are build tooling,
 * not codec logic: the shims below keep the same output on Windows while making
 * the tooling compile and run unchanged on Linux/macOS. Nothing in upstream/ or
 * in the CUDA kernels depends on this header.
 */

#ifndef HYPERCINE_PLATFORM_COMPAT_H
#define HYPERCINE_PLATFORM_COMPAT_H

/* stdio popen/pclose: POSIX spells them without the leading underscore. */
#if defined(_WIN32)
#  define HYPERCINE_POPEN  _popen
#  define HYPERCINE_PCLOSE _pclose
#else
#  define HYPERCINE_POPEN  popen
#  define HYPERCINE_PCLOSE pclose
#endif

/* Host operating system as reported into the benchmark JSON. */
#if defined(_WIN32)
#  define HYPERCINE_OS_STRING "Windows x64"
#elif defined(__APPLE__)
#  define HYPERCINE_OS_STRING "macOS"
#elif defined(__linux__)
#  define HYPERCINE_OS_STRING "Linux x86_64"
#else
#  define HYPERCINE_OS_STRING "unknown"
#endif

/* Compiler tag as reported into the benchmark JSON. On MSVC this keeps the
 * previous "MSVC <_MSC_VER>" wording byte for byte. */
#define HYPERCINE_STRINGIFY_DETAIL(x) #x
#define HYPERCINE_STRINGIFY(x) HYPERCINE_STRINGIFY_DETAIL(x)
#if defined(_MSC_VER)
#  define HYPERCINE_COMPILER_STRING "MSVC " HYPERCINE_STRINGIFY(_MSC_VER)
#elif defined(__clang__)
#  define HYPERCINE_COMPILER_STRING "Clang " __clang_version__
#elif defined(__GNUC__)
#  define HYPERCINE_COMPILER_STRING "GCC " __VERSION__
#else
#  define HYPERCINE_COMPILER_STRING "unknown"
#endif

/* Logical processor count without pthread/Windows API at the call site. */
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
static inline int hypercine_cpu_thread_count(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
}
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
static inline int hypercine_cpu_thread_count(void) {
    int count = 0;
    size_t len = sizeof(count);
    if (sysctlbyname("hw.logicalcpu", &count, &len, 0, 0) != 0 || count <= 0) {
        return 1;
    }
    return count;
}
#else
#  include <unistd.h>
static inline int hypercine_cpu_thread_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
}
#endif

#define HYPERCINE_CPU_THREAD_COUNT() hypercine_cpu_thread_count()

#endif /* HYPERCINE_PLATFORM_COMPAT_H */
