# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

# Compile the actual NVML sampler and JSON writer without SDK/CUDA linkage.
# Generated includes are verbatim source slices, not test copies of the logic.
function(hypercine_telemetry_slice input start end output)
    file(READ "${PROJECT_SOURCE_DIR}/${input}" source)
    string(FIND "${source}" "${start}" first)
    if(first LESS 0)
        message(FATAL_ERROR "Telemetry test source boundary missing")
    endif()
    string(SUBSTRING "${source}" ${first} -1 source)
    string(FIND "${source}" "${end}" last)
    if(last LESS 0)
        message(FATAL_ERROR "Telemetry test source end missing")
    endif()
    string(LENGTH "${end}" end_length)
    math(EXPR length "${last} + ${end_length}")
    string(SUBSTRING "${source}" 0 ${length} source)
    file(WRITE "${PROJECT_BINARY_DIR}/${output}" "${source}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/${input}")
endfunction()
hypercine_telemetry_slice(src/core/phase3a_encoder.cpp
    "struct NvmlUtilization {" "\n};\n\n// ===" telemetry_sampler.inc)
# Exclude the following section comment from the generated sampler include.
file(READ "${PROJECT_BINARY_DIR}/telemetry_sampler.inc" sampler)
string(REPLACE "\n// ===" "" sampler "${sampler}")
file(WRITE "${PROJECT_BINARY_DIR}/telemetry_sampler.inc" "${sampler}")
hypercine_telemetry_slice(benchmarks/benchmark_phase3a.cpp
    "void WritePhase3AResultJson(" "\n}\n" telemetry_writer.inc)
hypercine_telemetry_slice(benchmarks/benchmark_phase3a1.cpp
    "void WriteGpu(" "\n}\n" telemetry_canonical_writer.inc)
add_executable(test_phase3a_telemetry tests/test_phase3a_telemetry.cpp)
target_include_directories(test_phase3a_telemetry PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src/core" "${PROJECT_BINARY_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(test_phase3a_telemetry PRIVATE Threads::Threads)
add_test(NAME phase3a_telemetry COMMAND test_phase3a_telemetry)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME phase3a_telemetry_json COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/test_phase3a_telemetry_json.py" $<TARGET_FILE:test_phase3a_telemetry>)
