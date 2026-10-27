# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
cmake_minimum_required(VERSION 3.25)
project(HyperCineDecoderAudit NONE)
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(FFMPEG_EXECUTABLE ffmpeg REQUIRED)
find_program(FFPROBE_EXECUTABLE ffprobe REQUIRED)
add_test(NAME p7_plane_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_baseline.py")
add_test(NAME p7_json_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_json_contract.py")
add_test(NAME p7_public_sdk_fixture COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/baseline.py"
    --ffmpeg "${FFMPEG_EXECUTABLE}" --ffprobe "${FFPROBE_EXECUTABLE}"
    --verify "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/validation/macos-decode-p7/public-sdk-fixture.json")
set_tests_properties(p7_public_sdk_fixture PROPERTIES TIMEOUT 180)
