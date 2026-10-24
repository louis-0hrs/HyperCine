# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
cmake_minimum_required(VERSION 3.25)
project(HyperCineMacPRReview NONE)
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(issue 23 44 45 46 47)
    set(HYPERCINE_REVIEW_${issue}_ROOT "" CACHE PATH "Exact reviewed source snapshot for issue ${issue}")
    if(NOT IS_DIRECTORY "${HYPERCINE_REVIEW_${issue}_ROOT}")
        message(FATAL_ERROR "Set HYPERCINE_REVIEW_${issue}_ROOT to the reviewed source snapshot")
    endif()
    add_test(NAME mac_review_issue_${issue} COMMAND "${Python3_EXECUTABLE}" -O
        "${CMAKE_CURRENT_SOURCE_DIR}/review_contracts.py" ${issue}
        "${HYPERCINE_REVIEW_${issue}_ROOT}")
    set_tests_properties(mac_review_issue_${issue} PROPERTIES TIMEOUT 180)
endforeach()
