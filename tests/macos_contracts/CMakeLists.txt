# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
cmake_minimum_required(VERSION 3.25)
project(HyperCineMacContracts LANGUAGES CXX)
enable_testing()
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(mac_contracts contracts.cpp
    "${ROOT}/src/cpu/wavelet_cpu.cpp"
    "${ROOT}/src/cpu/standalone_frontend.cpp"
    "${ROOT}/src/core/quality_state.cpp")
target_include_directories(mac_contracts PRIVATE "${ROOT}/include")
target_compile_features(mac_contracts PRIVATE cxx_std_17)
foreach(contract geometry quality wavelet)
    add_test(NAME mac_${contract}_contract COMMAND mac_contracts ${contract})
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME mac_metal_memory_analysis COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/metal_budget.py")
set(HYPERCINE_P7_AUDIT_ROOT "" CACHE PATH "Existing reviewed P7.A worktree (optional)")
if(HYPERCINE_P7_AUDIT_ROOT)
    set(P7_BASELINE "${HYPERCINE_P7_AUDIT_ROOT}/tests/macos_decode_portability/baseline.py")
    if(NOT EXISTS "${P7_BASELINE}")
        message(FATAL_ERROR "P7.A baseline source unavailable")
    endif()
    add_test(NAME mac_p7_baseline_failures COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/p7_failures.py" "${P7_BASELINE}")
else()
    message(STATUS "P7 negative tests NOT REGISTERED: set HYPERCINE_P7_AUDIT_ROOT")
endif()
