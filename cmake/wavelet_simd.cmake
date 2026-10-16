# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
# Experimental kernels are linked only to explicit validation/benchmark seams.
add_library(hypercine_wavelet_simd STATIC
    src/cpu/wavelet_simd.cpp src/cpu/wavelet_cpu.cpp)
target_include_directories(hypercine_wavelet_simd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hypercine_wavelet_simd PRIVATE hypercine_cpu_isa)
option(HYPERCINE_BUILD_X86_WAVELET_SIMD "Build experimental x86 wavelet validation kernels" ON)
if(HYPERCINE_BUILD_X86_WAVELET_SIMD
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|i.86)$"
   AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]"
   AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    target_sources(hypercine_wavelet_simd PRIVATE
        src/cpu/wavelet_avx2.cpp src/cpu/wavelet_avx512.cpp)
    target_compile_definitions(hypercine_wavelet_simd PRIVATE HYPERCINE_WAVELET_X86=1)
    if(MSVC)
        set_source_files_properties(src/cpu/wavelet_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        set_source_files_properties(src/cpu/wavelet_avx512.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    else()
        set_source_files_properties(src/cpu/wavelet_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
        set_source_files_properties(src/cpu/wavelet_avx512.cpp PROPERTIES
            COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512vl")
    endif()
endif()
if(HYPERCINE_BUILD_TESTS)
    add_executable(test_wavelet_simd tests/test_wavelet_simd.cpp)
    target_link_libraries(test_wavelet_simd PRIVATE hypercine_wavelet_simd)
    add_test(NAME wavelet_simd_exact COMMAND test_wavelet_simd)
endif()
