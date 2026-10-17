# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
cmake_minimum_required(VERSION 3.25)
project(HyperCineScalarPlanAudit LANGUAGES CXX)
enable_testing()
get_filename_component(HYPERCINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(scalar_plan_oracle scalar_plan_oracle.cpp "${HYPERCINE_ROOT}/src/cpu/wavelet_cpu.cpp")
target_include_directories(scalar_plan_oracle PRIVATE "${HYPERCINE_ROOT}/include")
target_compile_features(scalar_plan_oracle PRIVATE cxx_std_17)
add_test(NAME scalar_plan_oracle COMMAND scalar_plan_oracle)
set_tests_properties(scalar_plan_oracle PROPERTIES TIMEOUT 120)
