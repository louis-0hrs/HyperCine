cmake_minimum_required(VERSION 3.16...3.31)
project(HyperCine VERSION 0.1.0 LANGUAGES C CXX)

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE HYPERCINE_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT HYPERCINE_SOURCE_COMMIT)
    set(HYPERCINE_SOURCE_COMMIT unknown)
endif()
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE HYPERCINE_SOURCE_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(HYPERCINE_SOURCE_STATUS)
    set(HYPERCINE_SOURCE_DIRTY true)
else()
    set(HYPERCINE_SOURCE_DIRTY false)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Global options
option(HYPERCINE_BUILD_UPSTREAM "Build upstream CineForm SDK reference components" ON)
option(HYPERCINE_BUILD_TESTS "Build HyperCine reference tests" ON)
option(HYPERCINE_BUILD_BENCHMARKS "Build HyperCine benchmark tool" ON)
option(HYPERCINE_ENABLE_CUDA "Enable CUDA acceleration backends" OFF)
option(HYPERCINE_ENABLE_METAL "Enable experimental Apple Metal FilmScan3 frontend" OFF)
option(HYPERCINE_BUILD_QUALITY_STATE_ONLY "Build only the portable quality-state calculator and its tests" OFF)

# Private synthetic scheduler: no SDK, codec kernels or GPU linkage.
option(HYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY "Build only the synthetic decode task graph" OFF)
if(HYPERCINE_BUILD_TESTS OR HYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY)
    find_package(Threads REQUIRED)
    add_subdirectory(tests/decode_task_graph)
endif()
if(HYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY)
    # Keep the regular SDK/CPU tests on the project's advertised CMake range.
    cmake_minimum_required(VERSION 3.25)
    if(HYPERCINE_BUILD_UPSTREAM OR HYPERCINE_ENABLE_CUDA OR HYPERCINE_ENABLE_METAL OR
       HYPERCINE_BUILD_STANDALONE OR HYPERCINE_BUILD_QUALITY_STATE_ONLY OR HYPERCINE_REQUIRE_EXTERNAL_DECODE)
        message(FATAL_ERROR "Task graph only mode requires SDK/GPU/other standalone modes OFF")
    endif()
    if(HYPERCINE_BUILD_TESTS)
        enable_testing()
    endif()
    return()
endif()

# Internal P0 probe/selection boundary, built without ISA flags or SDK headers.
# Codec targets deliberately do not dispatch through it until lane validation.
add_library(hypercine_cpu_isa STATIC src/cpu/cpu_isa.cpp)
target_include_directories(hypercine_cpu_isa PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(HYPERCINE_BUILD_TESTS)
    enable_testing()
    add_executable(test_cpu_isa tests/test_cpu_isa.cpp)
    target_include_directories(test_cpu_isa PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_cpu_isa PRIVATE hypercine_cpu_isa)
    add_test(NAME cpu_isa_selection COMMAND test_cpu_isa)
endif()

# This path intentionally returns before any SDK include directory or target.
option(HYPERCINE_BUILD_STANDALONE "Build experimental SDK-free fixed-profile encoder" OFF)
option(HYPERCINE_REQUIRE_EXTERNAL_DECODE "Require standalone FFmpeg decode tests" OFF)
set(HYPERCINE_QUALITY_ORACLE_ROOT "" CACHE PATH "Optional verified Windows quality-profile corpus root")
add_library(hypercine_quality_state STATIC src/core/quality_state.cpp)
target_include_directories(hypercine_quality_state PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(hypercine_quality_state PUBLIC cxx_std_17)
if(HYPERCINE_BUILD_QUALITY_STATE_ONLY)
    enable_testing()
    add_executable(test_quality_state tests/test_quality_state.cpp)
    target_link_libraries(test_quality_state PRIVATE hypercine_quality_state)
    add_test(NAME quality_state_unit COMMAND test_quality_state)
    add_executable(quality_state_probe tests/quality_state_probe.cpp)
    target_link_libraries(quality_state_probe PRIVATE hypercine_quality_state)
    if(HYPERCINE_QUALITY_ORACLE_ROOT)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME quality_state_windows_oracle COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_quality_state_corpus.py"
            --probe $<TARGET_FILE:quality_state_probe> --corpus "${HYPERCINE_QUALITY_ORACLE_ROOT}")
        set_tests_properties(quality_state_windows_oracle PROPERTIES
            ENVIRONMENT "HYPERCINE_TEST_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            TIMEOUT 3600)
    endif()
    return()
endif()
if(HYPERCINE_REQUIRE_EXTERNAL_DECODE AND (NOT HYPERCINE_BUILD_STANDALONE OR NOT HYPERCINE_BUILD_TESTS))
    message(FATAL_ERROR "External decode gate requires standalone mode and tests")
endif()
if(HYPERCINE_ENABLE_METAL AND (NOT APPLE OR NOT HYPERCINE_BUILD_STANDALONE))
    message(FATAL_ERROR "Metal requires Apple and the SDK-free standalone build")
endif()
if(HYPERCINE_BUILD_STANDALONE)
    cmake_minimum_required(VERSION 3.25)
    if(HYPERCINE_BUILD_UPSTREAM OR HYPERCINE_ENABLE_CUDA)
        message(FATAL_ERROR "Standalone requires HYPERCINE_BUILD_UPSTREAM=OFF and HYPERCINE_ENABLE_CUDA=OFF")
    endif()
    # Issue #37 API/storage P0 only; no RGB bitstream encoder or SDK dependency.
    add_library(hypercine_rgb444_frame STATIC src/cpu/rgb444_frontend.cpp)
    target_include_directories(hypercine_rgb444_frame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(hypercine_rgb444_frame PUBLIC cxx_std_17)
    add_library(hypercine_standalone STATIC
        src/cpu/wavelet_cpu.cpp
        src/cpu/standalone_frontend.cpp
        src/core/standalone_encoder.cpp
        src/cpu/filmscan3_frontend.cpp
        src/core/filmscan3_encoder.cpp
        src/core/filmscan3_backend.cpp)
    target_include_directories(hypercine_standalone PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(HYPERCINE_ENABLE_METAL)
        enable_language(OBJCXX)
        set(CMAKE_OBJCXX_STANDARD 17)
        set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/metal/filmscan3.metal" HYPERCINE_FILMSCAN3_MSL)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "src/metal/filmscan3.metal")
        configure_file(src/metal/filmscan3_msl.h.in generated/filmscan3_msl.h @ONLY)
        target_sources(hypercine_standalone PRIVATE src/metal/filmscan3_metal.mm)
        target_include_directories(hypercine_standalone PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        set_source_files_properties(src/metal/filmscan3_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(hypercine_standalone PRIVATE HYPERCINE_METAL_AVAILABLE=1)
        target_link_libraries(hypercine_standalone PRIVATE "-framework Metal" "-framework Foundation")
    endif()
    add_executable(hypercine_encode_smoke tools/hypercine_encode_smoke.cpp)
    target_link_libraries(hypercine_encode_smoke PRIVATE hypercine_standalone)
    add_executable(hypercine_encode_filmscan3 tools/hypercine_encode_filmscan3.cpp)
    target_link_libraries(hypercine_encode_filmscan3 PRIVATE hypercine_standalone)
    add_executable(hypercine_reference_probe tools/hypercine_reference_probe.cpp)
    target_link_libraries(hypercine_reference_probe PRIVATE hypercine_standalone)
    if(HYPERCINE_BUILD_BENCHMARKS AND HYPERCINE_ENABLE_METAL)
        # Native SDK-free encode benchmark; legacy SDK benchmark remains below
        # the standalone early return and is never linked into this target.
        add_executable(benchmark_codec benchmarks/benchmark_metal_p2.cpp)
        target_link_libraries(benchmark_codec PRIVATE hypercine_standalone)
    endif()
    if(HYPERCINE_BUILD_TESTS)
        enable_testing()
        add_executable(test_rgb444_frame tests/test_rgb444_frame.cpp)
        target_link_libraries(test_rgb444_frame PRIVATE hypercine_rgb444_frame)
        add_test(NAME rgb444_frame_storage COMMAND test_rgb444_frame storage)
        add_test(NAME rgb444_frame_rejection COMMAND test_rgb444_frame rejection)
        add_executable(test_rgb444_rg48 tests/test_rgb444_rg48.cpp)
        target_link_libraries(test_rgb444_rg48 PRIVATE hypercine_rgb444_frame)
        add_test(NAME rgb444_rg48_import COMMAND test_rgb444_rg48 import)
        add_test(NAME rgb444_rg48_rejection COMMAND test_rgb444_rg48 rejection)
        # Isolate the allocation interceptor while compiling the actual importer.
        add_executable(test_rgb444_rg48_allocation tests/test_rgb444_rg48_allocation.cpp
            src/cpu/rgb444_frontend.cpp)
        target_include_directories(test_rgb444_rg48_allocation PRIVATE include)
        target_compile_features(test_rgb444_rg48_allocation PRIVATE cxx_std_17)
        add_test(NAME rgb444_rg48_allocation_failure COMMAND test_rgb444_rg48_allocation)
        set_tests_properties(rgb444_rg48_allocation_failure PROPERTIES TIMEOUT 20)
        add_executable(test_filmscan3_backend tests/test_filmscan3_backend.cpp)
        target_link_libraries(test_filmscan3_backend PRIVATE hypercine_standalone)
        add_test(NAME filmscan3_backend_selection COMMAND test_filmscan3_backend)
        if(HYPERCINE_ENABLE_METAL)
            add_executable(test_metal_filmscan3 tests/test_metal_filmscan3.cpp)
            target_link_libraries(test_metal_filmscan3 PRIVATE hypercine_standalone)
            add_test(NAME filmscan3_metal_required COMMAND test_metal_filmscan3)
            set_tests_properties(filmscan3_metal_required PROPERTIES LABELS "metal;gpu-required")
            # Same host tool/writer, with mandatory Metal and per-stage scalar audit.
            add_executable(hypercine_encode_filmscan3_metal tools/hypercine_encode_filmscan3.cpp)
            target_compile_definitions(hypercine_encode_filmscan3_metal PRIVATE HYPERCINE_METAL_AUDIT=1)
            target_link_libraries(hypercine_encode_filmscan3_metal PRIVATE hypercine_standalone)
        endif()
        add_executable(test_filmscan3 tests/test_filmscan3.cpp)
        target_link_libraries(test_filmscan3 PRIVATE hypercine_standalone)
        add_test(NAME filmscan3_api COMMAND test_filmscan3)
        add_test(NAME filmscan3_fixture_integrity COMMAND "${CMAKE_COMMAND}"
            "-DROOT=${CMAKE_SOURCE_DIR}" -P "${CMAKE_SOURCE_DIR}/tests/verify_filmscan3_fixtures.cmake")
        add_executable(test_entropy_bitexact tests/test_entropy_bitexact.cpp)
        target_link_libraries(test_entropy_bitexact PRIVATE hypercine_standalone)
        add_test(NAME entropy_bitexact COMMAND test_entropy_bitexact)
        add_executable(test_standalone tests/test_standalone.cpp)
        target_link_libraries(test_standalone PRIVATE hypercine_standalone)
        foreach(stage color wavelet quantization entropy sample geometry)
            add_test(NAME standalone_${stage} COMMAND test_standalone ${stage})
        endforeach()
        add_test(NAME standalone_oracle_fixtures COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/standalone"
            "-DENCODER=$<TARGET_FILE:hypercine_encode_smoke>"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/oracle-fixture-check"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_oracle_fixtures.cmake")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        # RGB444 (this branch): replay contract plus optional private F1 root.
        add_test(NAME rgb444_rg48_replay_contract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rgb444_rg48_replay.py"
            --probe $<TARGET_FILE:test_rgb444_rg48>)
        set(HYPERCINE_RGB444_F1_ROOT "" CACHE PATH "Optional existing F1 root with 128 and 1080p-RGB_444 directories")
        if(HYPERCINE_RGB444_F1_ROOT)
            add_test(NAME rgb444_rg48_f1_sources COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_rgb444_rg48.py"
                --f1-root "${HYPERCINE_RGB444_F1_ROOT}" --probe $<TARGET_FILE:test_rgb444_rg48>
                --report "${CMAKE_CURRENT_BINARY_DIR}/rgb444-rg48-f1-report.json")
            set_tests_properties(rgb444_rg48_f1_sources PROPERTIES TIMEOUT 300)
        endif()
        # main: scalar inverse research and GOP2 contract/inventory.
        add_subdirectory(tests/scalar_inverse_research)
        add_test(NAME gop2_original_stage_contract
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gop2_stage_contract.py")
        add_test(NAME gop2_reference_inventory_integrity
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gop2_reference_inventory.py")
        add_test(NAME windows_reference_inputs COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_windows_reference.py"
            --probe $<TARGET_FILE:hypercine_reference_probe> --inputs-only)
        add_test(NAME windows_reference_guardrails COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_reference.py")
        add_test(NAME windows_reference_parser COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_sample.py")
        add_test(NAME windows_reference_sdk_coefficients COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_sample.py"
            --probe $<TARGET_FILE:hypercine_reference_probe>)
        set(HYPERCINE_REFERENCE_CORPUS_ROOT "" CACHE PATH "Optional extracted Windows Tier A corpus")
        if(HYPERCINE_REFERENCE_CORPUS_ROOT)
            add_test(NAME windows_reference_binaries COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_sample.py"
                --corpus-root "${HYPERCINE_REFERENCE_CORPUS_ROOT}" --probe $<TARGET_FILE:hypercine_reference_probe>)
        endif()
        add_test(NAME filmscan3_autonomous_manifest COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
            --probe $<TARGET_FILE:hypercine_reference_probe> --encoder $<TARGET_FILE:hypercine_encode_filmscan3>)
        add_test(NAME filmscan3_sequence_manifest COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
            --probe $<TARGET_FILE:hypercine_reference_probe> --encoder $<TARGET_FILE:hypercine_encode_filmscan3> --sequence)
        if(HYPERCINE_REFERENCE_CORPUS_ROOT)
            add_test(NAME filmscan3_actual_tier_a COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
                --probe $<TARGET_FILE:hypercine_reference_probe> --encoder $<TARGET_FILE:hypercine_encode_filmscan3>
                --corpus-root "${HYPERCINE_REFERENCE_CORPUS_ROOT}" --inject-metadata)
        endif()
        set(HYPERCINE_FS3_SEQUENCE_ROOT "" CACHE PATH "Optional verified 120-frame Windows FS3 corpus")
        if(HYPERCINE_FS3_SEQUENCE_ROOT)
            add_test(NAME filmscan3_actual_sequence COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
                --probe $<TARGET_FILE:hypercine_reference_probe> --encoder $<TARGET_FILE:hypercine_encode_filmscan3>
                --corpus-root "${HYPERCINE_FS3_SEQUENCE_ROOT}" --sequence)
        endif()
        find_program(HYPERCINE_FFMPEG_EXECUTABLE NAMES ffmpeg)
        find_program(HYPERCINE_FFPROBE_EXECUTABLE NAMES ffprobe)
        if(HYPERCINE_REQUIRE_EXTERNAL_DECODE AND
           (NOT HYPERCINE_FFMPEG_EXECUTABLE OR NOT HYPERCINE_FFPROBE_EXECUTABLE))
            message(FATAL_ERROR "External decode gate requires both FFmpeg and ffprobe")
        endif()
        if(HYPERCINE_FFMPEG_EXECUTABLE AND HYPERCINE_FFPROBE_EXECUTABLE)
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            add_test(NAME filmscan3_external_decode COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
                --probe $<TARGET_FILE:hypercine_reference_probe> --encoder $<TARGET_FILE:hypercine_encode_filmscan3>
                --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}")
            add_test(NAME standalone_external_decode COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_standalone.py"
                --encoder $<TARGET_FILE:hypercine_encode_smoke>
                --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}"
                --ffprobe "${HYPERCINE_FFPROBE_EXECUTABLE}"
                --output-dir "${CMAKE_CURRENT_BINARY_DIR}/samples")
            foreach(geometry 256x256 1920x1080 128x24 384x40 4096x24 128x4096 2048x1024)
                string(REPLACE "x" ";" dimensions "${geometry}")
                list(GET dimensions 0 width)
                list(GET dimensions 1 height)
                set(patterns flat canonical)
                if(geometry STREQUAL "256x256" OR geometry STREQUAL "1920x1080")
                    set(patterns flat gradient canonical noise checker edges impulse)
                endif()
                add_test(NAME standalone_external_${geometry} COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_standalone.py"
                    --encoder $<TARGET_FILE:hypercine_encode_smoke>
                    --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}" --ffprobe "${HYPERCINE_FFPROBE_EXECUTABLE}"
                    --width ${width} --height ${height} --patterns ${patterns}
                    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/samples-${geometry}")
            endforeach()
            add_test(NAME standalone_windows_oracle_replay COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_sdk_oracle.py"
                --windows-fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/windows-oracle"
                --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}"
                --ffprobe "${HYPERCINE_FFPROBE_EXECUTABLE}"
                --output "${CMAKE_CURRENT_BINARY_DIR}/windows-oracle-replay")
        else()
            message(STATUS "Optional standalone external decode test not registered: FFmpeg/ffprobe unavailable")
        endif()
        if(HYPERCINE_ENABLE_METAL)
            foreach(gate autonomous_manifest sequence_manifest actual_tier_a actual_sequence external_decode)
                if(TEST filmscan3_${gate})
                    set(extra)
                    if(gate STREQUAL "sequence_manifest" OR gate STREQUAL "actual_sequence")
                        list(APPEND extra --sequence)
                    endif()
                    if(gate STREQUAL "actual_tier_a")
                        list(APPEND extra --corpus-root "${HYPERCINE_REFERENCE_CORPUS_ROOT}" --inject-metadata)
                    elseif(gate STREQUAL "actual_sequence")
                        list(APPEND extra --corpus-root "${HYPERCINE_FS3_SEQUENCE_ROOT}" --inject-metadata)
                    elseif(gate STREQUAL "external_decode")
                        list(APPEND extra --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}")
                    endif()
                    add_test(NAME filmscan3_metal_${gate} COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
                        --probe $<TARGET_FILE:hypercine_reference_probe>
                        --encoder $<TARGET_FILE:hypercine_encode_filmscan3_metal>
                        --cpu-encoder $<TARGET_FILE:hypercine_encode_filmscan3>
                        --report "${CMAKE_CURRENT_BINARY_DIR}/metal-${gate}.json" ${extra})
                    set_tests_properties(filmscan3_metal_${gate} PROPERTIES
                        LABELS "metal;gpu-required"
                        ENVIRONMENT "HYPERCINE_TEST_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
                endif()
            endforeach()
        endif()
        # Record the compiler that built these binaries, including on MSVC hosts
        # where a Unix-style c++ command need not exist.
        set_tests_properties(windows_reference_inputs filmscan3_autonomous_manifest filmscan3_sequence_manifest PROPERTIES
            ENVIRONMENT "HYPERCINE_TEST_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
        if(TEST filmscan3_external_decode)
            set_tests_properties(filmscan3_external_decode PROPERTIES
                ENVIRONMENT "HYPERCINE_TEST_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
        endif()
        foreach(binary_test filmscan3_actual_tier_a filmscan3_actual_sequence)
            if(TEST ${binary_test})
                set_tests_properties(${binary_test} PROPERTIES
                    ENVIRONMENT "HYPERCINE_TEST_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
            endif()
        endforeach()
    endif()
    return()
endif()

if(HYPERCINE_ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(NOT CMAKE_CUDA_COMPILER AND DEFINED ENV{CUDA_PATH})
        if(EXISTS "$ENV{CUDA_PATH}/bin/nvcc.exe")
            set(CMAKE_CUDA_COMPILER "$ENV{CUDA_PATH}/bin/nvcc.exe" CACHE FILEPATH "CUDA compiler" FORCE)
        elseif(EXISTS "$ENV{CUDA_PATH}/bin/nvcc")
            set(CMAKE_CUDA_COMPILER "$ENV{CUDA_PATH}/bin/nvcc" CACHE FILEPATH "CUDA compiler" FORCE)
        endif()
    endif()
    if(CMAKE_CUDA_COMPILER)
        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR "${CMAKE_CUDA_ARCHITECTURES}" STREQUAL "")
            set(CMAKE_CUDA_ARCHITECTURES native CACHE STRING "Target CUDA architectures" FORCE)
        endif()
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        message(STATUS "CUDA enabled. Compiler: ${CMAKE_CUDA_COMPILER} (Version: ${CMAKE_CUDA_COMPILER_VERSION}, Architectures: ${CMAKE_CUDA_ARCHITECTURES})")
    else()
        message(FATAL_ERROR "HYPERCINE_ENABLE_CUDA is ON, but no CUDA compiler was detected on this system.")
    endif()
endif()

# Configure build settings for MSVC
if(MSVC)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
    )
endif()

# Build pristine upstream reference SDK
if(HYPERCINE_BUILD_UPSTREAM)
    # Upstream options
    set(BUILD_LIBS ON CACHE BOOL "Build codec libraries" FORCE)
    set(BUILD_TOOLS ON CACHE BOOL "Build test tools" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "Build static component" FORCE)
    set(BUILD_SEPARATED OFF CACHE BOOL "Build separate de/encoder libraries" FORCE)

    # Upstream quirk workaround (documented in docs/UPSTREAM.md):
    # Upstream line 35 does string(STRIP ${ADDITIONAL_LIBS} ADDITIONAL_LIBS) without quotes,
    # and find_package(OpenMP) in upstream fails when CUDA is an enabled language unless OpenMP_CUDA is satisfied.
    set(ADDITIONAL_LIBS " ")
    set(OpenMP_CUDA_FOUND TRUE CACHE BOOL "CUDA OpenMP placeholder" FORCE)
    set(OpenMP_CUDA_FLAGS "" CACHE STRING "CUDA OpenMP flags placeholder" FORCE)

    add_subdirectory(upstream)

    # Second compile of the pristine sources. upstream/ is not modified.
    # _THREADED_ENCODER is a compile-time switch in upstream/Codec/config.h.
    file(GLOB HYPERCINE_THREADED_CODEC CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/Codec/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/Codec/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/WarpLib/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/EncoderSDK/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/DecoderSDK/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/ConvertLib/*.cpp")
    # The threaded sources do not compile (see docs/UPSTREAM.md). The target is
    # kept so the failure can be reproduced, and it is excluded from the default build.
    add_library(CFHDCodecThreaded STATIC EXCLUDE_FROM_ALL ${HYPERCINE_THREADED_CODEC})
    target_compile_definitions(CFHDCodecThreaded PRIVATE
        _THREADED_ENCODER=1
        _ALLOCATOR=1
        WARPSTUFF=1)
    target_include_directories(CFHDCodecThreaded PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/Common"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/Tables"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/Codec"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/ConvertLib"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/WarpLib"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/Example"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/EncoderSDK"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/DecoderSDK")
    if(MSVC)
        target_compile_options(CFHDCodecThreaded PRIVATE
            /wd4244 /wd4267 /wd4101 /wd4018
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/threaded_encoder_prefix.h")
    endif()
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/Common
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/Codec
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/EncoderSDK
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/DecoderSDK
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/ConvertLib
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/WarpLib
)

# The root SDK include_directories() also mutates targets declared earlier in
# this CMakeLists.txt. hypercine_cpu_isa is SDK-free and needs only src/, but
# upstream/Codec/cpuid.h otherwise shadows GCC/Clang's <cpuid.h> and breaks
# __get_cpuid_max/__cpuid_count in the Linux regular SDK-enabled build.
# Restore the isolated header search path for these two pre-existing targets;
# keep the upstream includes intact for actual CineForm SDK consumers.
set_property(TARGET hypercine_cpu_isa PROPERTY INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(TARGET test_cpu_isa)
    set_property(TARGET test_cpu_isa PROPERTY INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

# Placeholder target for HyperCine library integration
add_library(hypercine_core INTERFACE)
target_include_directories(hypercine_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/Common
)
if(TARGET CFHDCodecStatic)
    target_link_libraries(hypercine_core INTERFACE CFHDCodecStatic)

    # upstream/EncoderSDK calls uuid_generate() (MetadataWriter.cpp,
    # SampleEncoder.cpp), but the frozen upstream build only appends -luuid to
    # its own tools. Every target that links the CFHDCodecStatic archive
    # directly therefore leaves the symbol unresolved on Linux. Resolve it on
    # the archive's INTERFACE so all downstream consumers link it without manual
    # -luuid flags. A missing library is a clear configure error. upstream/ is
    # not modified; MSVC and Apple (libSystem) need no extra library here.
    if(UNIX AND NOT APPLE)
        find_library(HYPERCINE_UUID_LIBRARY uuid)
        if(NOT HYPERCINE_UUID_LIBRARY)
            message(FATAL_ERROR
                "libuuid not found; upstream/EncoderSDK needs uuid_generate on Linux. "
                "Install the libuuid development package (Debian/Ubuntu: uuid-dev).")
        endif()
        target_link_libraries(CFHDCodecStatic INTERFACE ${HYPERCINE_UUID_LIBRARY})
    endif()
endif()

# Core HyperCine library source
add_subdirectory(src)

# Bounded diagnostic probe for constrained CUDA devices (issue #116).
if(HYPERCINE_ENABLE_CUDA)
    add_executable(hypercine_m4_resolution_probe tools/m4_sm52_resolution_probe.cpp)
    target_include_directories(hypercine_m4_resolution_probe PRIVATE
        ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
    target_link_libraries(hypercine_m4_resolution_probe PRIVATE hypercine_wavelet)
    if(TARGET CFHDCodecStatic)
        target_link_libraries(hypercine_m4_resolution_probe PRIVATE CFHDCodecStatic)
    endif()
endif()

# Tests
if(HYPERCINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarks
if(HYPERCINE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

