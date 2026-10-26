# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# Issue #259 capability prototype.
#
# Root CMakeLists.txt is intentionally unchanged while #249 and #255 edit it.
# After those edits land, the standalone test section should add:
#   add_subdirectory(tests/encoder_capability)
# and hypercine_standalone should compile src/core/encoder_capability.cpp.
# Until then this directory is not part of the default CTest inventory.
if(NOT TARGET hypercine_standalone)
    return()
endif()

add_executable(test_encoder_capability
    "${CMAKE_CURRENT_SOURCE_DIR}/../test_encoder_capability.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/encoder_capability.cpp")
target_include_directories(test_encoder_capability PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(test_encoder_capability PRIVATE hypercine_standalone)
if(HYPERCINE_BUILD_TESTS)
    add_test(NAME encoder_capability_contract COMMAND test_encoder_capability)
    set_tests_properties(encoder_capability_contract PROPERTIES TIMEOUT 1800)
endif()
