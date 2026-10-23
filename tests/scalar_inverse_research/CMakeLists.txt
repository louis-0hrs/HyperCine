# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
# Research-only arithmetic: no decoder API/profile, SDK or GPU dependencies.
add_library(hypercine_scalar_inverse_research STATIC ../../src/cpu/decode_spatial_scalar.cpp)
target_include_directories(hypercine_scalar_inverse_research PUBLIC ../../src/cpu PRIVATE ../../src/core)
target_compile_features(hypercine_scalar_inverse_research PUBLIC cxx_std_17)
set_target_properties(hypercine_scalar_inverse_research PROPERTIES CXX_EXTENSIONS OFF)
add_executable(test_decode_spatial_scalar ../test_decode_spatial_scalar.cpp)
target_link_libraries(test_decode_spatial_scalar PRIVATE hypercine_scalar_inverse_research hypercine_decode_task_graph)
foreach(mode arithmetic rejection)
    add_test(NAME scalar_inverse_${mode} COMMAND test_decode_spatial_scalar ${mode})
endforeach()
foreach(workers 1 2 4 16)
    add_test(NAME scalar_inverse_graph_workers_${workers} COMMAND test_decode_spatial_scalar ${workers})
    set_tests_properties(scalar_inverse_graph_workers_${workers} PROPERTIES TIMEOUT 30)
endforeach()
add_test(NAME scalar_inverse_vectors COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tests/reference_scalar_inverse/generate_vectors.py" --check)
add_test(NAME scalar_inverse_replay_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tests/test_scalar_inverse_replay.py" --probe $<TARGET_FILE:test_decode_spatial_scalar>)
set(HYPERCINE_SCALAR_ORACLE_ROOT "" CACHE PATH "Optional read-only original six-pair scalar corpus")
if(HYPERCINE_SCALAR_ORACLE_ROOT)
    add_test(NAME scalar_inverse_original_six_pairs COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validate_decode_spatial_scalar.py"
        --probe $<TARGET_FILE:test_decode_spatial_scalar> --golden-dir "${HYPERCINE_SCALAR_ORACLE_ROOT}")
    set_tests_properties(scalar_inverse_original_six_pairs PROPERTIES TIMEOUT 300)
endif()
