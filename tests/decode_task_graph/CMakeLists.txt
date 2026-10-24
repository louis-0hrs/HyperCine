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

    # Scheduler contract derived from RCA-195 (PR #197). Every rule is REQUIRED:
    # the executable prints HOLDS / BROKEN per rule and exits non-zero on any
    # unmet rule, so there is no path that prints PASS over a known shortfall.
    # Reporting a gap as a pass is the defect RCA-195 itself is about.
    #
    # The worker count is a real parameter, not decoration: 1 worker exercises
    # the inline serial order, 16 exercises contention over the single ready
    # queue. All four counts are required -- a concurrency rule proven only at
    # one width is an untested rule at the others.
    add_executable(test_scheduler_contract_195 ../test_scheduler_contract_195.cpp)
    target_link_libraries(test_scheduler_contract_195 PRIVATE hypercine_decode_task_graph)
    foreach(workers 1 2 4 16)
        add_test(NAME scheduler_contract_195_workers_${workers}
                 COMMAND test_scheduler_contract_195 ${workers})
        set_tests_properties(scheduler_contract_195_workers_${workers} PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "SCHEDULER-CONTRACT PASS")
    endforeach()
endif()
