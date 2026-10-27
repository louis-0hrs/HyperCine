# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(Rgb444BoundaryQualification LANGUAGES CXX)
    enable_testing()
endif()
add_executable(test_rgb444_allocation test_allocation.cpp ../../src/cpu/rgb444_frontend.cpp)
target_include_directories(test_rgb444_allocation PRIVATE ../../include)
target_compile_features(test_rgb444_allocation PRIVATE cxx_std_17)
add_test(NAME rgb444_allocation_failure COMMAND test_rgb444_allocation)
set_tests_properties(rgb444_allocation_failure PROPERTIES TIMEOUT 20)
