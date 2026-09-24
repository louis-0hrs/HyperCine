cmake_minimum_required(VERSION 3.16...3.31)
project(HyperCine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Global options
option(HYPERCINE_BUILD_UPSTREAM "Build upstream CineForm SDK reference components" ON)
option(HYPERCINE_BUILD_TESTS "Build HyperCine reference tests" ON)
option(HYPERCINE_BUILD_BENCHMARKS "Build HyperCine benchmark tool" ON)

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

    add_subdirectory(upstream)
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

# Placeholder target for HyperCine library integration
add_library(hypercine_core INTERFACE)
target_include_directories(hypercine_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/Common
)
if(TARGET CFHDCodecStatic)
    target_link_libraries(hypercine_core INTERFACE CFHDCodecStatic)
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
