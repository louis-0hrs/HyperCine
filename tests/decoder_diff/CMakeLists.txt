# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
cmake_minimum_required(VERSION 3.25)
project(HyperCineDecoderDifferential NONE)
enable_testing()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
find_program(HYPERCINE_FFMPEG_EXECUTABLE ffmpeg REQUIRED)
find_program(HYPERCINE_FFPROBE_EXECUTABLE ffprobe REQUIRED)
add_test(NAME decoder_diff_metrics COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_metrics.py")
add_test(NAME decoder_diff_replay COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../compare_cfhd_decoders.py"
    --ffmpeg "${HYPERCINE_FFMPEG_EXECUTABLE}" --ffprobe "${HYPERCINE_FFPROBE_EXECUTABLE}"
    --output "${CMAKE_CURRENT_BINARY_DIR}/decoder_matrix.json")
set_tests_properties(decoder_diff_replay PROPERTIES TIMEOUT 120)
