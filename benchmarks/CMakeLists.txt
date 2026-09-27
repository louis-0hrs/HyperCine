# HyperCine Benchmark Suite CMakeLists.txt

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/upstream/Common
    ${CMAKE_SOURCE_DIR}/upstream/Codec
    ${CMAKE_SOURCE_DIR}/upstream/EncoderSDK
    ${CMAKE_SOURCE_DIR}/upstream/DecoderSDK
)

add_executable(benchmark_codec
    benchmark_codec.cpp
)

target_link_libraries(benchmark_codec PRIVATE CFHDCodecStatic)

if(WIN32)
    target_link_libraries(benchmark_codec PRIVATE psapi)
endif()

if(HYPERCINE_ENABLE_CUDA)
    add_executable(benchmark_wavelet
        benchmark_wavelet.cpp
    )
    target_link_libraries(benchmark_wavelet PRIVATE hypercine_core)
    
    add_executable(benchmark_phase1b
        benchmark_phase1b.cpp
    )
    target_link_libraries(benchmark_phase1b PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(profile_encoder
        profile_encoder.cpp
    )
    target_link_libraries(profile_encoder PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(benchmark_phase1c
        benchmark_phase1c.cpp
    )
    target_link_libraries(benchmark_phase1c PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(benchmark_phase2a
        benchmark_phase2a.cpp
    )
    target_link_libraries(benchmark_phase2a PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(benchmark_phase2b
        benchmark_phase2b.cpp
    )
    target_link_libraries(benchmark_phase2b PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(benchmark_phase2c
        benchmark_phase2c.cpp
    )
    target_link_libraries(benchmark_phase2c PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(benchmark_phase2d
        benchmark_phase2d.cpp
    )
    target_link_libraries(benchmark_phase2d PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(benchmark_phase3a
        benchmark_phase3a.cpp
    )
    target_link_libraries(benchmark_phase3a PRIVATE hypercine_core CFHDCodecStatic)

    add_executable(benchmark_phase3a1
        benchmark_phase3a1.cpp
    )
    target_link_libraries(benchmark_phase3a1 PRIVATE hypercine_core CFHDCodecStatic)
    string(REPLACE ";" "," HYPERCINE_CUDA_ARCH_STR "${CMAKE_CUDA_ARCHITECTURES}")
    target_compile_definitions(benchmark_phase3a1 PRIVATE
        HYPERCINE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        HYPERCINE_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
        HYPERCINE_CUDA_COMPILER_VERSION="${CMAKE_CUDA_COMPILER_VERSION}"
        HYPERCINE_BUILD_TYPE="$<CONFIG>"
        HYPERCINE_CMAKE_CUDA_ARCH="${HYPERCINE_CUDA_ARCH_STR}"
        HYPERCINE_CMAKE_OPTS="HYPERCINE_ENABLE_CUDA=${HYPERCINE_ENABLE_CUDA},HYPERCINE_BUILD_UPSTREAM=${HYPERCINE_BUILD_UPSTREAM},HYPERCINE_BUILD_TESTS=${HYPERCINE_BUILD_TESTS},HYPERCINE_BUILD_BENCHMARKS=${HYPERCINE_BUILD_BENCHMARKS}")

    if(WIN32)
        target_link_libraries(benchmark_wavelet PRIVATE psapi)
        target_link_libraries(benchmark_phase1b PRIVATE psapi)
        target_link_libraries(profile_encoder PRIVATE psapi)
        target_link_libraries(benchmark_phase1c PRIVATE psapi)
        target_link_libraries(benchmark_phase2a PRIVATE psapi)
        target_link_libraries(benchmark_phase2b PRIVATE psapi)
        target_link_libraries(benchmark_phase2c PRIVATE psapi)
        target_link_libraries(benchmark_phase2d PRIVATE psapi)
        target_link_libraries(benchmark_phase3a PRIVATE psapi)
        target_link_libraries(benchmark_phase3a1 PRIVATE psapi)
    endif()
endif()

if(TARGET CFHDCodecThreaded)
    add_executable(benchmark_upstream_threaded EXCLUDE_FROM_ALL benchmark_upstream_threaded.cpp)
    target_link_libraries(benchmark_upstream_threaded PRIVATE CFHDCodecThreaded)
    target_include_directories(benchmark_upstream_threaded PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/upstream/Common)
    if(WIN32)
        target_link_libraries(benchmark_upstream_threaded PRIVATE psapi)
    endif()
endif()

