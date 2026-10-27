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
# Declared here, ahead of the CUDA configure block, so the policy guards below can
# see it. The standalone path itself still lives further down; only the option
# declaration moved.
option(HYPERCINE_BUILD_STANDALONE "Build experimental SDK-free fixed-profile encoder" OFF)

# Policy guards run BEFORE CUDA compiler detection, and that ordering is the point.
# If detection ran first, a host without nvcc would fail with "no CUDA compiler was
# detected", which masks the real reason the configuration is unsupported. The
# contract tests assert the specific message, so the specific reason has to be the
# one that surfaces.
if(HYPERCINE_BUILD_QUALITY_STATE_ONLY AND
   (HYPERCINE_BUILD_STANDALONE OR HYPERCINE_ENABLE_CUDA OR HYPERCINE_ENABLE_METAL OR
    HYPERCINE_ENABLE_VULKAN OR HYPERCINE_REQUIRE_EXTERNAL_DECODE))
    message(FATAL_ERROR
        "Quality-state-only cannot satisfy standalone, CUDA, Metal, Vulkan or external decode requests")
endif()
if(HYPERCINE_ENABLE_CUDA AND NOT HYPERCINE_BUILD_UPSTREAM AND NOT HYPERCINE_BUILD_STANDALONE)
    message(FATAL_ERROR
        "CUDA requires HYPERCINE_BUILD_UPSTREAM=ON outside SDK-free standalone mode; hypercine_wavelet still depends on frozen SDK encoder symbols")
endif()

# Configure CUDA before the standalone early-return path. The standalone
# FilmScan3 target below deliberately uses only HyperCine's SDK-free scalar
# frontend/writer and CUDA wavelet sources; it never links the GoPro wrapper.
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

# SDK-free Python contracts, registered by both the regular test tree and the
# standalone build, which returns before add_subdirectory(tests). Kept in one
# function so the two build modes cannot drift apart.
function(hypercine_register_sdk_free_contracts)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND)
        return()
    endif()
    set(contracts "${PROJECT_SOURCE_DIR}/tests")
    add_test(NAME qualification_report_contract
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/ci/test_summarize_qualification.py")
    set_tests_properties(qualification_report_contract PROPERTIES LABELS "validation;reference-oracle")
    add_test(NAME cuda_output_dir_platform_contract
        COMMAND "${Python3_EXECUTABLE}" -B "${contracts}/test_cuda_output_dir_platform.py")
    add_test(NAME test_reference_release_inventory
        COMMAND "${Python3_EXECUTABLE}" "${contracts}/test_reference_release.py")
    set_tests_properties(test_reference_release_inventory PROPERTIES LABELS "reference-oracle")
    add_test(NAME test_compare_sdk_oracle
        COMMAND "${Python3_EXECUTABLE}" "${contracts}/test_compare_sdk_oracle.py")
    set_tests_properties(test_compare_sdk_oracle PROPERTIES LABELS "reference-oracle")
    # Recorded RCA-159 band-0 witness (#234). Recomputed statistics and the
    # sanitized records only; no private corpus and no GPU are involved.
    add_test(NAME test_rca159_witness_contract
        COMMAND "${Python3_EXECUTABLE}" "${contracts}/test_rca159_witness.py")
    set_tests_properties(test_rca159_witness_contract PROPERTIES LABELS "validation")
endfunction()

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
    add_subdirectory(tests/geometry_extreme)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME diagnostic_summary_contract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diagnostic_summary.py")
    endif()
    add_executable(test_cpu_isa tests/test_cpu_isa.cpp)
    target_include_directories(test_cpu_isa PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_cpu_isa PRIVATE hypercine_cpu_isa)
    add_test(NAME cpu_isa_selection COMMAND test_cpu_isa)
endif()

option(HYPERCINE_ENABLE_VULKAN "Enable experimental Vulkan compute FilmScan3 frontend" OFF)
option(HYPERCINE_REQUIRE_EXTERNAL_DECODE "Require standalone FFmpeg decode tests" OFF)
# This path intentionally returns before any SDK include directory or target.
set(HYPERCINE_QUALITY_ORACLE_ROOT "" CACHE PATH "Optional verified Windows quality-profile corpus root")
add_library(hypercine_quality_state STATIC src/core/quality_state.cpp)
target_include_directories(hypercine_quality_state PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(hypercine_quality_state PUBLIC cxx_std_17)
# The incompatibility guard for quality-state-only moved to the top of the file so
# it runs before CUDA compiler detection; this block only builds that mode.
if(HYPERCINE_BUILD_QUALITY_STATE_ONLY)
    add_executable(quality_state_probe tests/quality_state_probe.cpp)
    target_link_libraries(quality_state_probe PRIVATE hypercine_quality_state)
    if(NOT HYPERCINE_BUILD_TESTS)
        return()
    endif()
    enable_testing()
    add_executable(test_quality_state tests/test_quality_state.cpp)
    target_link_libraries(test_quality_state PRIVATE hypercine_quality_state)
    add_test(NAME quality_state_unit COMMAND test_quality_state)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME quality_cmake_mode_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/ci/test_quality_cmake_mode.py")
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
if(HYPERCINE_ENABLE_VULKAN AND NOT HYPERCINE_BUILD_STANDALONE)
    message(FATAL_ERROR "The Vulkan FilmScan3 frontend requires the SDK-free standalone build")
endif()
if(HYPERCINE_BUILD_STANDALONE)
    cmake_minimum_required(VERSION 3.25)
    if(HYPERCINE_BUILD_UPSTREAM)
        message(FATAL_ERROR "Standalone requires HYPERCINE_BUILD_UPSTREAM=OFF")
    endif()
    add_library(hypercine_standalone STATIC
        src/cpu/wavelet_cpu.cpp
        src/cpu/standalone_frontend.cpp
        src/core/standalone_encoder.cpp
        src/cpu/filmscan3_frontend.cpp
        src/core/filmscan3_encoder.cpp
        src/core/filmscan3_backend.cpp
        src/core/encoder_capability.cpp)
    target_include_directories(hypercine_standalone PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(HYPERCINE_ENABLE_CUDA)
        find_package(CUDAToolkit REQUIRED)
        target_sources(hypercine_standalone PRIVATE
            src/cuda/wavelet_cuda_reference.cu
            src/cuda/wavelet_cuda_optimized.cu
            src/cuda/wavelet_cuda_api.cu
            src/cuda/filmscan3_cuda_frontend.cpp)
        target_include_directories(hypercine_standalone PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/cuda"
            ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
        target_compile_definitions(hypercine_standalone PRIVATE HYPERCINE_CUDA_AVAILABLE=1)
        target_link_libraries(hypercine_standalone PUBLIC CUDA::cudart)
        set_target_properties(hypercine_standalone PROPERTIES
            CUDA_STANDARD 17
            CUDA_STANDARD_REQUIRED ON
            CUDA_SEPARABLE_COMPILATION OFF)
    endif()
    if(HYPERCINE_ENABLE_VULKAN)
        # Vulkan is loaded through dlopen at runtime, so only the headers are a
        # build dependency; VK_NO_PROTOTYPES removes every link-time symbol.
        find_package(Vulkan REQUIRED)
        find_package(Threads REQUIRED)
        set(VULKAN_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders")
        set(VULKAN_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
        set(VULKAN_SHADER_SOURCES
            "${VULKAN_SHADER_DIR}/color_cg709.comp"
            "${VULKAN_SHADER_DIR}/wavelet_h26.comp"
            "${VULKAN_SHADER_DIR}/wavelet_v26.comp")
        # Deterministic GLSL -> SPIR-V embedder; dlopen("libshaderc") keeps the
        # shader compiler optional on the build host (see the tool's header).
        add_executable(vulkan_spirv_embed tools/vulkan_spirv_embed.cpp)
        target_link_libraries(vulkan_spirv_embed PRIVATE ${CMAKE_DL_LIBS})
        add_custom_command(
            OUTPUT "${VULKAN_GEN_DIR}/vulkan_shaders_generated.h"
                   "${VULKAN_GEN_DIR}/vulkan_spirv_manifest.txt"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${VULKAN_GEN_DIR}"
            COMMAND $<TARGET_FILE:vulkan_spirv_embed>
                    --out "${VULKAN_GEN_DIR}/vulkan_shaders_generated.h"
                    --manifest "${VULKAN_GEN_DIR}/vulkan_spirv_manifest.txt"
                    ${VULKAN_SHADER_SOURCES}
            DEPENDS vulkan_spirv_embed ${VULKAN_SHADER_SOURCES}
            COMMENT "Embedding HyperCine Vulkan compute shaders (SPIR-V)"
            VERBATIM)
        add_custom_target(hypercine_vulkan_shaders DEPENDS
            "${VULKAN_GEN_DIR}/vulkan_shaders_generated.h")
        add_dependencies(hypercine_standalone hypercine_vulkan_shaders)
        target_sources(hypercine_standalone PRIVATE src/vulkan/vulkan_frontend.cpp)
        target_include_directories(hypercine_standalone PRIVATE
            "${VULKAN_GEN_DIR}" ${Vulkan_INCLUDE_DIRS})
        target_compile_definitions(hypercine_standalone PRIVATE
            HYPERCINE_VULKAN_AVAILABLE=1 VK_NO_PROTOTYPES)
        set_property(GLOBAL PROPERTY HYPERCINE_HAS_VULKAN TRUE)
    endif()
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
    if(HYPERCINE_BUILD_BENCHMARKS)
        # Issue #70: NEON vs scalar, kernel and whole frame. Registered here and
        # not in benchmarks/CMakeLists.txt because the standalone build returns
        # before add_subdirectory(benchmarks), so a registration there would
        # silently never produce a target. It is a benchmark, not a gate: built,
        # not run by ctest.
        add_executable(benchmark_neon_wavelet benchmarks/benchmark_neon_wavelet.cpp)
        target_link_libraries(benchmark_neon_wavelet PRIVATE hypercine_standalone)
        set_property(TARGET benchmark_neon_wavelet PROPERTY INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
    endif()
    if(HYPERCINE_BUILD_TESTS)
        enable_testing()
        # This branch returns before add_subdirectory(tests), so the SDK-free
        # contracts must be registered here as well.
        hypercine_register_sdk_free_contracts()
        # Issue #70: NEON proof. Registered from the SDK-free block so it runs in
        # the configuration CI actually uses.
        #
        # No SKIP_RETURN_CODE on purpose. The mandatory standalone matrix treats a
        # skip as a failure, and that is correct: a skipped test proves nothing
        # about the build it ran in. The test therefore asserts the architecture
        # contract instead -- real NEON-vs-scalar comparison on arm64, and
        # NOT_SUPPORTED plus an intact scalar path everywhere else.
        add_executable(test_neon_wavelet tests/test_neon_wavelet.cpp)
        target_link_libraries(test_neon_wavelet PRIVATE hypercine_standalone)
        add_test(NAME neon_wavelet_bitexact COMMAND test_neon_wavelet)
        set_tests_properties(neon_wavelet_bitexact PROPERTIES
            LABELS "cpu;neon" TIMEOUT 600)

        # Issue #70 / #259: the dispatch switch was a process-global plain bool.
        # This binary answers two questions that an atomic does not answer by
        # itself: does ThreadSanitizer see a race (it must not), and does one
        # session actually own its kernel (it does not -- proven, not described).
        find_package(Threads REQUIRED)
        add_executable(test_neon_concurrency tests/test_neon_concurrency.cpp)
        target_link_libraries(test_neon_concurrency PRIVATE hypercine_standalone Threads::Threads)
        add_test(NAME neon_concurrency_contract COMMAND test_neon_concurrency)
        set_tests_properties(neon_concurrency_contract PROPERTIES
            LABELS "cpu;neon;concurrency" TIMEOUT 900)
        add_executable(test_filmscan3_backend tests/test_filmscan3_backend.cpp)
        target_link_libraries(test_filmscan3_backend PRIVATE hypercine_standalone)
        add_test(NAME filmscan3_backend_selection COMMAND test_filmscan3_backend)
        if(HYPERCINE_ENABLE_METAL)
            add_executable(test_metal_filmscan3 tests/test_metal_filmscan3.cpp)
            target_link_libraries(test_metal_filmscan3 PRIVATE hypercine_standalone)
            add_test(NAME filmscan3_metal_required COMMAND test_metal_filmscan3)
            set_tests_properties(filmscan3_metal_required PROPERTIES LABELS "metal;gpu-required")
            # #45: a complete 4K CFHD encoder on Metal. A GPU-wavelet PASS alone is
            # not sufficient, so this compares whole samples and every hash.
            # #45 / #259: geometry independence across the qualified shape range.
            # A 1080p+4K pair does not prove the encoder is resolution-agnostic.
            add_executable(test_metal_filmscan3_geometry_matrix tests/test_metal_filmscan3_geometry_matrix.cpp)
            target_link_libraries(test_metal_filmscan3_geometry_matrix PRIVATE hypercine_standalone)
            add_test(NAME filmscan3_metal_geometry_matrix COMMAND test_metal_filmscan3_geometry_matrix)
            set_tests_properties(filmscan3_metal_geometry_matrix PROPERTIES
                LABELS "metal;gpu-required" TIMEOUT 3600)
            add_executable(test_metal_filmscan3_4k tests/test_metal_filmscan3_4k.cpp)
            target_link_libraries(test_metal_filmscan3_4k PRIVATE hypercine_standalone)
            add_test(NAME filmscan3_metal_4k_encoder COMMAND test_metal_filmscan3_4k)
            set_tests_properties(filmscan3_metal_4k_encoder PROPERTIES LABELS "metal;gpu-required" TIMEOUT 1800)
            # Same host tool/writer, with mandatory Metal and per-stage scalar audit.
            add_executable(hypercine_encode_filmscan3_metal tools/hypercine_encode_filmscan3.cpp)
            target_compile_definitions(hypercine_encode_filmscan3_metal PRIVATE HYPERCINE_METAL_AUDIT=1)
            target_link_libraries(hypercine_encode_filmscan3_metal PRIVATE hypercine_standalone)
        endif()
        if(HYPERCINE_ENABLE_VULKAN)
            # CPU<->Vulkan parity. GPU-required run fails closed when no Vulkan
            # compute device is usable; the same binary is a skip when not required.
            add_executable(test_vulkan_filmscan3 tests/test_vulkan_filmscan3.cpp)
            target_link_libraries(test_vulkan_filmscan3 PRIVATE hypercine_standalone)
            add_test(NAME vulkan_filmscan3_cpu_parity COMMAND test_vulkan_filmscan3)
            set_tests_properties(vulkan_filmscan3_cpu_parity PROPERTIES
                LABELS "vulkan;gpu-required"
                SKIP_RETURN_CODE 77)
        endif()
        if(HYPERCINE_ENABLE_CUDA)
            add_executable(hypercine_cuda_frontend_compare tools/hypercine_cuda_frontend_compare.cpp)
            target_include_directories(hypercine_cuda_frontend_compare PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
            string(REPLACE ";" "," HYPERCINE_CUDA_ARCHITECTURES_REPORT "${CMAKE_CUDA_ARCHITECTURES}")
            target_compile_definitions(hypercine_cuda_frontend_compare PRIVATE
                HYPERCINE_CUDA_ARCHITECTURES_STRING="${HYPERCINE_CUDA_ARCHITECTURES_REPORT}")
            target_link_libraries(hypercine_cuda_frontend_compare PRIVATE hypercine_standalone CUDA::cudart)
            add_test(NAME cuda_filmscan3_complete_packet COMMAND hypercine_cuda_frontend_compare
                --output-dir "${CMAKE_CURRENT_BINARY_DIR}/cuda-filmscan3")
            set_tests_properties(cuda_filmscan3_complete_packet PROPERTIES
                LABELS "cuda;gpu-required" SKIP_RETURN_CODE 77 TIMEOUT 300)
            add_test(NAME cuda_filmscan3_negative_skip_kernel COMMAND hypercine_cuda_frontend_compare
                --negative-control skip-kernel)
            set_tests_properties(cuda_filmscan3_negative_skip_kernel PROPERTIES
                LABELS "cuda;negative-control" WILL_FAIL TRUE SKIP_RETURN_CODE 77 TIMEOUT 300)
            add_test(NAME cuda_filmscan3_negative_altered_output COMMAND hypercine_cuda_frontend_compare
                --negative-control alter-output)
            set_tests_properties(cuda_filmscan3_negative_altered_output PROPERTIES
                LABELS "cuda;negative-control" WILL_FAIL TRUE SKIP_RETURN_CODE 77 TIMEOUT 300)
            # A single proved difference in a band claimed bit-exact before
            # quantization must fail on its own. Quantization can absorb the
            # difference, so coefficient hashes and the packet stay byte-equal
            # here; only the stage check can catch it.
            add_test(NAME cuda_filmscan3_negative_pre_quantized_stage COMMAND hypercine_cuda_frontend_compare
                --negative-control alter-pre-quantized-stage)
            set_tests_properties(cuda_filmscan3_negative_pre_quantized_stage PROPERTIES
                LABELS "cuda;negative-control" WILL_FAIL TRUE SKIP_RETURN_CODE 77 TIMEOUT 300)
        endif()
        add_executable(test_filmscan3 tests/test_filmscan3.cpp)
        target_link_libraries(test_filmscan3 PRIVATE hypercine_standalone)
        add_test(NAME filmscan3_api COMMAND test_filmscan3)
        add_executable(test_filmscan3_geometry tests/test_filmscan3_geometry.cpp)
        target_link_libraries(test_filmscan3_geometry PRIVATE hypercine_standalone)
        add_test(NAME filmscan3_geometry COMMAND test_filmscan3_geometry)
        # Separate current wavelet shape from the bounded FilmScan3 pixel budget.
        add_executable(test_filmscan3_geometry_budget tests/test_filmscan3_geometry_budget.cpp)
        target_link_libraries(test_filmscan3_geometry_budget PRIVATE hypercine_standalone)
        add_test(NAME filmscan3_geometry_budget COMMAND test_filmscan3_geometry_budget)

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
        add_test(NAME filmscan3_geometry_reference_p10 COMMAND "${Python3_EXECUTABLE}"
            -O "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_geometry_reference_p10.py")
        add_test(NAME filmscan3_geometry_validation COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_filmscan3_geometry_validation.py")
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
                --corpus-root "${HYPERCINE_FS3_SEQUENCE_ROOT}" --sequence --inject-metadata)
        endif()
        find_program(HYPERCINE_FFMPEG_EXECUTABLE NAMES ffmpeg)
        find_program(HYPERCINE_FFPROBE_EXECUTABLE NAMES ffprobe)
        if(HYPERCINE_REQUIRE_EXTERNAL_DECODE AND
           (NOT HYPERCINE_FFMPEG_EXECUTABLE OR NOT HYPERCINE_FFPROBE_EXECUTABLE))
            message(FATAL_ERROR "External decode gate requires both FFmpeg and ffprobe")
        endif()
        if(HYPERCINE_FFMPEG_EXECUTABLE AND HYPERCINE_FFPROBE_EXECUTABLE)
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            if(HYPERCINE_ENABLE_CUDA)
                add_test(NAME cuda_filmscan3_ffmpeg_decode COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cuda_filmscan3.py"
                    --compare $<TARGET_FILE:hypercine_cuda_frontend_compare>
                    --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}"
                    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/cuda-filmscan3-ffmpeg")
                set_tests_properties(cuda_filmscan3_ffmpeg_decode PROPERTIES
                    LABELS "cuda;gpu-required;external-decode" SKIP_RETURN_CODE 77 TIMEOUT 300)
                # The single-frame gate above cannot speak for a sequence: it decodes
                # one packet per side, and the comparator writes per-frame files only
                # when --frames > 1. A recorded "3 ordered frames" run therefore had
                # no independent decode behind it at all. This registers the same
                # validator with three ordered frames, so every frame of the session
                # is decoded separately and its 30 stage hashes are read back.
                add_test(NAME cuda_filmscan3_sequence_ffmpeg_decode COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cuda_filmscan3.py"
                    --compare $<TARGET_FILE:hypercine_cuda_frontend_compare>
                    --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}"
                    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/cuda-filmscan3-sequence-ffmpeg"
                    --frames 3)
                set_tests_properties(cuda_filmscan3_sequence_ffmpeg_decode PROPERTIES
                    LABELS "cuda;gpu-required;external-decode" SKIP_RETURN_CODE 77 TIMEOUT 600)
            endif()
            # #249 review: prove the validator never disturbs a caller's output
            # directory. Needs no GPU and no CUDA build, so it is registered
            # outside the CUDA gate and runs on every FFmpeg-enabled builder.
            add_test(NAME cuda_filmscan3_output_dir_negative COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cuda_filmscan3_output_dir.py")
            set_tests_properties(cuda_filmscan3_output_dir_negative PROPERTIES
                SKIP_RETURN_CODE 77
                LABELS "cuda;output-dir;no-gpu;platform-required" TIMEOUT 120
                PASS_REGULAR_EXPRESSION "CUDA-OUTPUT-DIR-NEGATIVE: PASS")
            # #45: a real 4K FFmpeg gate with its own negative controls. Kept apart
            # from the 1080p evidence so a 4K claim always has a 4K decode behind it.
            if(HYPERCINE_ENABLE_METAL)
                add_test(NAME filmscan3_metal_4k_ffmpeg_gate COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3_4k_ffmpeg.py"
                    --encoder $<TARGET_FILE:hypercine_encode_filmscan3>
                    --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}"
                    --ffprobe "${HYPERCINE_FFPROBE_EXECUTABLE}")
                set_tests_properties(filmscan3_metal_4k_ffmpeg_gate PROPERTIES TIMEOUT 1800)
            endif()
            # #259 capability contract: refuses before allocating, then encodes a
            # geometry matrix and decodes each packet with FFmpeg. Registered here
            # because it needs both ffmpeg and ffprobe -- without them the matrix
            # half would be untested and the test would pass on the contract alone.
            add_executable(test_encoder_capability
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_encoder_capability.cpp")
            target_include_directories(test_encoder_capability PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/src"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests")
            target_link_libraries(test_encoder_capability PRIVATE hypercine_standalone)
            add_test(NAME encoder_capability_contract COMMAND test_encoder_capability)
            set_tests_properties(encoder_capability_contract PROPERTIES
                LABELS "cpu;capability;external-decode" TIMEOUT 1800
                ENVIRONMENT "HYPERCINE_FFMPEG_EXECUTABLE=${HYPERCINE_FFMPEG_EXECUTABLE};HYPERCINE_FFPROBE_EXECUTABLE=${HYPERCINE_FFPROBE_EXECUTABLE}"
                PASS_REGULAR_EXPRESSION "PASS capability contract and encoded matrix")
            add_test(NAME filmscan3_geometry_external_decode COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3_geometry.py"
                --encoder $<TARGET_FILE:test_filmscan3_geometry>
                --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}" --ffprobe "${HYPERCINE_FFPROBE_EXECUTABLE}"
                --output-dir "${CMAKE_CURRENT_BINARY_DIR}/geometry-p4")
            set_tests_properties(filmscan3_geometry_external_decode PROPERTIES TIMEOUT 600)
            add_test(NAME filmscan3_external_decode COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
                --probe $<TARGET_FILE:hypercine_reference_probe> --encoder $<TARGET_FILE:hypercine_encode_filmscan3>
                --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}")
            # #45: the geometry contract of the real validator, not a wrapper
            # around it. Wrong geometries must be rejected with the reason that
            # was actually intended, and the manifest geometry must still pass.
            add_test(NAME filmscan3_geometry_negative_gate COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3_geometry_negative.py"
                --validator "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_filmscan3.py"
                --probe $<TARGET_FILE:hypercine_reference_probe>
                --encoder $<TARGET_FILE:hypercine_encode_filmscan3>)
            set_tests_properties(filmscan3_geometry_negative_gate PROPERTIES
                LABELS "validation;negative-control" TIMEOUT 900)
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

# #256/#249 merge: the fail-closed guard from #256 is kept, the CUDA enable block
# below it is deliberately NOT taken. This branch enables CUDA near the top of the
# file (before the standalone return() boundary), because the SDK-free standalone
# build needs CUDA language support -- #249 is exactly that configuration. Taking
# main's block verbatim would call enable_language(CUDA) twice.
#
# Placing the guard after the standalone return() preserves main's semantics:
# standalone + CUDA is allowed, non-standalone CUDA without the original SDK is not.
if(HYPERCINE_ENABLE_CUDA AND NOT HYPERCINE_BUILD_UPSTREAM)
    message(FATAL_ERROR
        "CUDA requires HYPERCINE_BUILD_UPSTREAM=ON outside SDK-free standalone mode; hypercine_wavelet still depends on frozen SDK encoder symbols")
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