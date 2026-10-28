# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
add_library(hypercine_decode_task_graph STATIC ../../src/core/decode_task_graph.cpp)
target_compile_features(hypercine_decode_task_graph PUBLIC cxx_std_17)
set_target_properties(hypercine_decode_task_graph PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(hypercine_decode_task_graph PUBLIC ../../src/core)
target_link_libraries(hypercine_decode_task_graph PUBLIC Threads::Threads)
add_library(hypercine_decode_spatial_attachment STATIC
    ../../src/core/decode_s3_attachment.cpp
    ../../src/core/decode_s3_consumer.cpp
    ../../src/cpu/decode_spatial_scalar.cpp)
target_include_directories(hypercine_decode_spatial_attachment PRIVATE ../../src/cpu)
target_link_libraries(hypercine_decode_spatial_attachment PUBLIC hypercine_decode_task_graph)
target_compile_features(hypercine_decode_spatial_attachment PUBLIC cxx_std_17)
# Fault-injection seam used only by the T1/T2 gate below (see decode_task_graph.h).
# It is a compile-time switch: a build without this definition has no seam.
if(HYPERCINE_BUILD_TESTS)
    target_compile_definitions(hypercine_decode_task_graph PRIVATE
        HYPERCINE_DECODE_TASK_GRAPH_TEST_HOOK=1)
endif()
if(HYPERCINE_BUILD_TESTS)
    enable_testing()
    add_executable(test_decoder_attachment_292 test_decoder_attachment_292.cpp)
    target_include_directories(test_decoder_attachment_292 PRIVATE ../../src/cpu)
    target_link_libraries(test_decoder_attachment_292 PRIVATE hypercine_decode_spatial_attachment)
    foreach(workers 1 2 4 16)
        add_test(NAME decoder_attachment_292_workers_${workers}
            COMMAND test_decoder_attachment_292 ${workers})
        set_tests_properties(decoder_attachment_292_workers_${workers}
            PROPERTIES TIMEOUT 30 LABELS "scheduler;s3")
    endforeach()
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

    # T1/T2 gate for issues #144 and #195: topology rejected before the first
    # worker, versioned publication with release/acquire, bounded consumer wait
    # and a write-once error slot. A rule that does not hold exits non-zero.
    add_executable(test_scheduler_t1_t2_144 test_scheduler_t1_t2_144.cpp)
    target_link_libraries(test_scheduler_t1_t2_144 PRIVATE hypercine_decode_task_graph)
    # Same macro as the library, so the test sees set_publish_seam().
    target_compile_definitions(test_scheduler_t1_t2_144 PRIVATE
        HYPERCINE_DECODE_TASK_GRAPH_TEST_HOOK=1)
    foreach(workers 1 2 4 16)
        add_test(NAME scheduler_t1_t2_144_workers_${workers}
                 COMMAND test_scheduler_t1_t2_144 ${workers})
        set_tests_properties(scheduler_t1_t2_144_workers_${workers} PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "SCHEDULER-T1T2 PASS")
    endforeach()

    # S2 output completion: a callback return is not a result. Completion is a
    # BufferVersion of this generation retained by the graph. Same worker matrix
    # as the T1/T2 gate. Synthetic only; no decoder integration.
    add_executable(test_scheduler_s2_output_144 test_scheduler_s2_output_144.cpp)
    target_link_libraries(test_scheduler_s2_output_144 PRIVATE hypercine_decode_task_graph)
    foreach(workers 1 2 4 16)
        add_test(NAME scheduler_s2_output_144_workers_${workers}
                 COMMAND test_scheduler_s2_output_144 ${workers})
        set_tests_properties(scheduler_s2_output_144_workers_${workers} PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "SCHEDULER-S2-OUTPUT PASS")
    endforeach()

    # S3 signed-plane attachment (issues #144 / #195). Synthetic only:
    # typed extent, channel/band match, bounded wavelet5 consumer, and the
    # existing S2 publication rule. Not a GoPro/CFHD integration.
    # Removing the short-extent comparison must fail S3-extent-negative.
    add_executable(test_decode_s3_plane
        test_decode_s3_plane.cpp
        ../../src/core/decode_s3_consumer.cpp
        ../../src/cpu/decode_spatial_scalar.cpp)
    target_include_directories(test_decode_s3_plane PRIVATE ../../src/cpu)
    target_link_libraries(test_decode_s3_plane PRIVATE hypercine_decode_task_graph)
    add_test(NAME decode_s3_plane COMMAND test_decode_s3_plane)
    set_tests_properties(decode_s3_plane PROPERTIES
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "DECODE-S3 PASS")

    # T4 plane dependency (issue #144). Synthetic four-band edge plus the
    # generation budget. Not a decoder and not a fix for the #195 SDK races.
    # Guards: T4-EXTENT, T4-BUDGET, T4-KEY, T4-STALE, T4-IDENTITY, T4-GEOMETRY.
    add_executable(test_scheduler_t4_plane_144 test_scheduler_t4_plane_144.cpp)
    target_link_libraries(test_scheduler_t4_plane_144 PRIVATE hypercine_decode_task_graph)
    foreach(workers 1 2 4 8)
        add_test(NAME scheduler_t4_plane_144_workers_${workers}
                 COMMAND test_scheduler_t4_plane_144 ${workers})
        set_tests_properties(scheduler_t4_plane_144_workers_${workers} PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "SCHEDULER-T4 PASS")
    endforeach()

    # T5 publication-after-decision (issue #144, after #275). The existing gates
    # prove a published buffer is unreadable after stop/error; none measures
    # whether the producer may still install a publication after that decision.
    # Guards: T5-A cancel, T5-B recorded error, T5-C plane form, T5-D empty record.
    # Removing the stop_/error_ admission from publish() must fail T5-A..C.
    add_executable(test_scheduler_t5_publish_after_stop_144
        test_scheduler_t5_publish_after_stop_144.cpp)
    target_link_libraries(test_scheduler_t5_publish_after_stop_144
        PRIVATE hypercine_decode_task_graph)
    foreach(workers 1 2 4 16)
        add_test(NAME scheduler_t5_publish_after_stop_144_workers_${workers}
                 COMMAND test_scheduler_t5_publish_after_stop_144 ${workers})
        set_tests_properties(scheduler_t5_publish_after_stop_144_workers_${workers} PROPERTIES
            TIMEOUT 120
            PASS_REGULAR_EXPRESSION "SCHEDULER-T5 PASS")
    endforeach()
endif()
