# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

# Inject only this test target into a clean exact-SHA candidate checkout using
# CMAKE_PROJECT_HyperCine_INCLUDE. Never edit or commit candidate production code.
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE candidate_sha OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT candidate_sha STREQUAL "7ed4a82ed97598454c2680bd264dedf259e6e7dc")
    message(FATAL_ERROR "Issue #189 comparison requires the pinned #187 code SHA")
endif()
execute_process(COMMAND git diff --quiet HEAD -- CMakeLists.txt src include upstream
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" RESULT_VARIABLE candidate_dirty)
if(NOT candidate_dirty EQUAL 0)
    message(FATAL_ERROR "Issue #189 comparison requires unchanged candidate production sources")
endif()
find_package(CUDAToolkit REQUIRED)
add_executable(gop2_candidate_compare "${CMAKE_CURRENT_LIST_DIR}/../../../tools/gop2_candidate_compare.cpp")
target_compile_features(gop2_candidate_compare PRIVATE cxx_std_17)
target_include_directories(gop2_candidate_compare PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
target_include_directories(gop2_candidate_compare PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(gop2_candidate_compare PRIVATE hypercine_decoder hypercine_wavelet CFHDCodecStatic CUDA::cudart)
