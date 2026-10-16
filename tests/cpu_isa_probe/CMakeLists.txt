# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
cmake_minimum_required(VERSION 3.25)
project(HyperCineCpuIsaProbe LANGUAGES CXX)
enable_testing()
add_executable(test_cpu_isa
    ../test_cpu_isa.cpp
    ../../src/cpu/cpu_isa.cpp
)
target_compile_features(test_cpu_isa PRIVATE cxx_std_17)
target_include_directories(test_cpu_isa PRIVATE ../../include)
if(MSVC)
    target_compile_options(test_cpu_isa PRIVATE /W4 /WX)
else()
    target_compile_options(test_cpu_isa PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME cpu_isa_probe COMMAND test_cpu_isa)
