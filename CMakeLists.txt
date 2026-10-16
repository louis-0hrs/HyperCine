cmake_minimum_required(VERSION 3.16...3.31)
project(HyperCine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Global options
option(HYPERCINE_BUILD_UPSTREAM "Build upstream CineForm SDK reference components" ON)
option(HYPERCINE_BUILD_TESTS "Build HyperCine reference tests" ON)
option(HYPERCINE_BUILD_BENCHMARKS "Build HyperCine benchmark tool" ON)
option(HYPERCINE_ENABLE_CUDA "Enable CUDA acceleration backends" OFF)

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

# Linux portability shims. upstream/ itself is never modified.
# 1. upstream/Common/CFHDAllocator.h declares size_t based typedefs without
#    including <stddef.h>; MSVC supplied it transitively, GCC 14 does not.
#    Joined -include form is required because a generator expression cannot
#    expand to two command line arguments and nvcc rejects the joined form.
# 2. CFHDCodecStatic is a static archive with an empty link interface, but
#    upstream/EncoderSDK/SampleEncoder.cpp needs uuid_generate on UNIX.
if(UNIX AND NOT APPLE)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux_size_t_prefix.h>")

    if(TARGET CFHDCodecStatic)
        find_library(HYPERCINE_UUID_LIBRARY uuid)
        if(NOT HYPERCINE_UUID_LIBRARY)
            message(FATAL_ERROR "libuuid not found. upstream/EncoderSDK needs uuid_generate on UNIX.")
        endif()
        target_link_libraries(CFHDCodecStatic INTERFACE ${HYPERCINE_UUID_LIBRARY})
    endif()
endif()

# Placeholder target for HyperCine library integration
add_library(hypercine_core INTERFACE)
target_include_directories(hypercine_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/Common
)
if(TARGET CFHDCodecStatic)
    target_link_libraries(hypercine_core INTERFACE CFHDCodecStatic)
endif()

# Core HyperCine library source
add_subdirectory(src)

# Tests
if(HYPERCINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarks
if(HYPERCINE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

