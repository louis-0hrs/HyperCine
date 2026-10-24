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

# The directory-level include_directories() above injects upstream/Codec, whose
# cpuid.h claims the same _CPUID_H guard as the toolchain header. benchmark_codec
# only needs CFHDEncoder.h/CFHDDecoder.h from upstream/Common, so the CPUID
# brand-string probe would silently resolve the frozen GoPro header and lose
# __get_cpuid/__get_cpuid_max. Restore an explicit, minimal search path for this
# target only; real CineForm SDK consumers keep the upstream directories.
target_include_directories(benchmark_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/upstream/Common)
set_property(TARGET benchmark_codec PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/upstream/Common)

# Record the real compiler that built this binary instead of a hardcoded
# toolchain string, so Linux/macOS runs report truthful metadata.
function(hypercine_bench_compiler_metadata target)
    target_compile_definitions(${target} PRIVATE
        HYPERCINE_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        HYPERCINE_BENCH_BUILD_TYPE="$<CONFIG>")
endfunction()
hypercine_bench_compiler_metadata(benchmark_codec)

if(WIN32)
    target_link_libraries(benchmark_codec PRIVATE psapi)
endif()

if(HYPERCINE_ENABLE_CUDA)
    add_executable(benchmark_wavelet
        benchmark_wavelet.cpp
    )
    # Link the wavelet archive before CFHDCodecStatic. hypercine_core lists
    # the codec archive first, and GNU ld then drops decoder symbols that
    # only the wavelet objects reference.
    target_link_libraries(benchmark_wavelet PRIVATE hypercine_wavelet CFHDCodecStatic)
    hypercine_bench_compiler_metadata(benchmark_wavelet)

    add_executable(benchmark_phase1b
        benchmark_phase1b.cpp
    )
    target_link_libraries(benchmark_phase1b PRIVATE hypercine_wavelet CFHDCodecStatic)
    hypercine_bench_compiler_metadata(benchmark_phase1b)

    add_executable(profile_encoder
        profile_encoder.cpp
    )
    target_link_libraries(profile_encoder PRIVATE hypercine_wavelet CFHDCodecStatic)

    add_executable(benchmark_phase1c
        benchmark_phase1c.cpp
    )
    target_link_libraries(benchmark_phase1c PRIVATE hypercine_wavelet CFHDCodecStatic)
    hypercine_bench_compiler_metadata(benchmark_phase1c)

    add_executable(benchmark_phase2a
        benchmark_phase2a.cpp
    )
    target_link_libraries(benchmark_phase2a PRIVATE hypercine_wavelet CFHDCodecStatic)

    add_executable(benchmark_phase2b
        benchmark_phase2b.cpp
    )
    target_link_libraries(benchmark_phase2b PRIVATE hypercine_wavelet CFHDCodecStatic)

    add_executable(benchmark_phase2c
        benchmark_phase2c.cpp
    )
    target_link_libraries(benchmark_phase2c PRIVATE hypercine_wavelet CFHDCodecStatic)

    add_executable(benchmark_phase2d
        benchmark_phase2d.cpp
    )
    target_link_libraries(benchmark_phase2d PRIVATE hypercine_wavelet CFHDCodecStatic)

    add_executable(benchmark_phase3a
        benchmark_phase3a.cpp
    )
    target_link_libraries(benchmark_phase3a PRIVATE hypercine_wavelet CFHDCodecStatic)

    add_executable(benchmark_phase3a1
        benchmark_phase3a1.cpp
    )
    target_link_libraries(benchmark_phase3a1 PRIVATE hypercine_wavelet CFHDCodecStatic)
    string(REPLACE ";" "," HYPERCINE_CUDA_ARCH_STR "${CMAKE_CUDA_ARCHITECTURES}")
    target_compile_definitions(benchmark_phase3a1 PRIVATE
        HYPERCINE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        HYPERCINE_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
        HYPERCINE_CUDA_COMPILER_VERSION="${CMAKE_CUDA_COMPILER_VERSION}"
        HYPERCINE_BUILD_TYPE="$<CONFIG>"
        HYPERCINE_CMAKE_CUDA_ARCH="${HYPERCINE_CUDA_ARCH_STR}"
        HYPERCINE_TARGET_CUDA_ARCH="$<JOIN:$<TARGET_PROPERTY:hypercine_wavelet,CUDA_ARCHITECTURES>,;>"
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


# Issue #33: the benchmark tool must not report success when its measurement
# artifact was not written. Registered here, after the target exists; the
# tests/ directory is configured before benchmarks/, so the guard cannot live
# there. Skipped when tests are disabled.
if(HYPERCINE_BUILD_TESTS)
    add_test(NAME benchmark_artifact_contract
        COMMAND "${CMAKE_COMMAND}"
            "-DBENCH=$<TARGET_FILE:benchmark_codec>"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/benchmark-artifact-contract"
            -P "${CMAKE_SOURCE_DIR}/tests/verify_benchmark_artifact_contract.cmake")
    set_tests_properties(benchmark_artifact_contract PROPERTIES TIMEOUT 600)

    # Same contract for the CUDA benchmark tools that can run a single 1080p
    # configuration. Registered only when those targets exist (CUDA ON); the
    # tests/ directory is configured before benchmarks/, so this stays here.
    # Each run checks a writable nested path, a directory sink, and a character
    # device (/dev/null or NUL), and parses the artifact with string(JSON).
    # Heavier suites (phase 2C/2D/3A/3A1) still fail closed, but their full
    # measurement is not a ctest default.
    if(TARGET benchmark_wavelet)
        add_test(NAME benchmark_wavelet_artifact_contract
            COMMAND "${CMAKE_COMMAND}"
                "-DBENCH=$<TARGET_FILE:benchmark_wavelet>"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/benchmark-wavelet-artifact-contract"
                "-DEXTRA_ARG=--1080p-only"
                "-DSUCCESS_BANNER=Saved benchmark results"
                "-DJSON_KEY=benchmarks"
                -P "${CMAKE_SOURCE_DIR}/tests/verify_benchmark_fail_closed.cmake")
        set_tests_properties(benchmark_wavelet_artifact_contract PROPERTIES TIMEOUT 600)
    endif()
    if(TARGET benchmark_phase1b)
        add_test(NAME benchmark_phase1b_artifact_contract
            COMMAND "${CMAKE_COMMAND}"
                "-DBENCH=$<TARGET_FILE:benchmark_phase1b>"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/benchmark-phase1b-artifact-contract"
                "-DEXTRA_ARG=--1080p-only"
                "-DSUCCESS_BANNER=Successfully saved"
                "-DJSON_KEY=benchmarks"
                -P "${CMAKE_SOURCE_DIR}/tests/verify_benchmark_fail_closed.cmake")
        set_tests_properties(benchmark_phase1b_artifact_contract PROPERTIES TIMEOUT 600)
    endif()
endif()
