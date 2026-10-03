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

# Decode Phase 0 is independent of all encoder benchmark contracts.
add_executable(benchmark_decode_phase0 benchmark_decode_phase0.cpp)
target_link_libraries(benchmark_decode_phase0 PRIVATE CFHDCodecStatic)
target_compile_definitions(benchmark_decode_phase0 PRIVATE _ALLOCATOR=1)
execute_process(COMMAND git -c safe.directory=${CMAKE_SOURCE_DIR} rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE DECODE_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE)
target_compile_definitions(benchmark_decode_phase0 PRIVATE
    HYPERCINE_DECODE_SOURCE_COMMIT="${DECODE_SOURCE_COMMIT}")
if(WIN32)
    target_link_libraries(benchmark_decode_phase0 PRIVATE psapi)
endif()

# Opt-in isolated profile: generated copies live ONLY in the binary directory.
# Link their objects ahead of the stock archive; production targets are unchanged.
option(HYPERCINE_DECODE_STAGE_PROFILE "Build isolated decoder stage profiler" OFF)
if(HYPERCINE_DECODE_STAGE_PROFILE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(DECODE_PROFILE_DIR "${CMAKE_CURRENT_BINARY_DIR}/decode_profile_generated")
    file(MAKE_DIRECTORY "${DECODE_PROFILE_DIR}")
    foreach(DECODE_PROFILE_SOURCE decoder.c bayer.c lutpath.cpp convert.c quantize.c frame.c spatial.c)
        execute_process(COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate_decode_profile.py"
            "${CMAKE_SOURCE_DIR}" "${DECODE_PROFILE_SOURCE}"
            OUTPUT_FILE "${DECODE_PROFILE_DIR}/${DECODE_PROFILE_SOURCE}"
            COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        generate_decode_profile.py
        "${CMAKE_SOURCE_DIR}/upstream/Codec/decoder.c"
        "${CMAKE_SOURCE_DIR}/upstream/Codec/bayer.c"
        "${CMAKE_SOURCE_DIR}/upstream/Codec/lutpath.cpp"
        "${CMAKE_SOURCE_DIR}/upstream/Codec/convert.c"
        "${CMAKE_SOURCE_DIR}/upstream/Codec/quantize.c"
        "${CMAKE_SOURCE_DIR}/upstream/Codec/frame.c"
        "${CMAKE_SOURCE_DIR}/upstream/Codec/spatial.c"
        "${CMAKE_SOURCE_DIR}/upstream/Codec/wavelet.h")
    add_executable(profile_decode_phase0 benchmark_decode_phase0.cpp decode_profile_hooks.cpp
        "${DECODE_PROFILE_DIR}/decoder.c" "${DECODE_PROFILE_DIR}/bayer.c"
        "${DECODE_PROFILE_DIR}/lutpath.cpp" "${DECODE_PROFILE_DIR}/convert.c"
        "${DECODE_PROFILE_DIR}/quantize.c" "${DECODE_PROFILE_DIR}/frame.c"
        "${DECODE_PROFILE_DIR}/spatial.c")
    target_include_directories(profile_decode_phase0 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(profile_decode_phase0 BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/upstream/Codec")
    target_compile_definitions(profile_decode_phase0 PRIVATE _ALLOCATOR=1 WARPSTUFF=1
        HYPERCINE_DECODE_PROFILE=1 HYPERCINE_DECODE_SOURCE_COMMIT="${DECODE_SOURCE_COMMIT}")
    target_link_libraries(profile_decode_phase0 PRIVATE CFHDCodecStatic)
    if(WIN32)
        target_link_libraries(profile_decode_phase0 PRIVATE psapi)
    endif()
endif()

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

