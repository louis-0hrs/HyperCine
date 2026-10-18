# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
add_library(hypercine_decode_task_graph STATIC ../../src/core/decode_task_graph.cpp)
target_compile_features(hypercine_decode_task_graph PUBLIC cxx_std_17)
set_target_properties(hypercine_decode_task_graph PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(hypercine_decode_task_graph PUBLIC ../../src/core)
target_link_libraries(hypercine_decode_task_graph PUBLIC Threads::Threads)
if(HYPERCINE_BUILD_TESTS)
    enable_testing()
    add_executable(test_decode_task_graph ../test_decode_task_graph.cpp)
    target_link_libraries(test_decode_task_graph PRIVATE hypercine_decode_task_graph)
    foreach(workers 1 2 4 16)
        add_test(NAME decode_task_graph_workers_${workers} COMMAND test_decode_task_graph ${workers})
        # A test-process watchdog, NOT a promise to kill in-process C++ workers.
        set_tests_properties(decode_task_graph_workers_${workers} PROPERTIES TIMEOUT 30)
    endforeach()
endif()
