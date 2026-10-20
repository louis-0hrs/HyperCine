# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
cmake_minimum_required(VERSION 3.25)
project(Gop2OriginalStageObserver LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(reference_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(sdk_scratch "${reference_root}/build/gop2-phaseb-sdk-v2")
if(NOT EXISTS "${sdk_scratch}/observer.json")
    message(FATAL_ERROR "Prepare the private SDK observer copies first")
endif()
add_executable(gop2_stage_extract "${reference_root}/tools/gop2_stage_extract.cpp"
    "${sdk_scratch}/Codec/decoder.c" "${sdk_scratch}/Codec/wavelet.c")
foreach(folder Common Tables Codec ConvertLib WarpLib Example EncoderSDK DecoderSDK)
    target_include_directories(gop2_stage_extract PRIVATE "${reference_root}/upstream/${folder}")
endforeach()
target_include_directories(gop2_stage_extract PRIVATE "${reference_root}/tests")
target_compile_definitions(gop2_stage_extract PRIVATE _ALLOCATOR=1 WARPSTUFF=1 HAVE_OPENMP=1
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
    HYPERCINE_SOURCE_COMMIT="6aa64bfbea7770ba93fb2ce2d47fd6b7e56bc938")
target_compile_options(gop2_stage_extract PRIVATE /EHs)
target_link_libraries(gop2_stage_extract PRIVATE "${reference_root}/build/upstream/Release/CFHDCodecStatic.lib")
