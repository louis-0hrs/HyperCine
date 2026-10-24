# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

# Artifact contract for benchmark_codec (issue #33).
#
# The defect this guards: the tool printed "Benchmark Run Completed Successfully"
# and exited 0 while the JSON artifact had not been written at all. A run nobody
# can audit then looks like a measurement. Two independent properties are
# checked here, in both directions:
#
#   1. A writable target produces a real, parseable artifact and exit 0.
#   2. A non-regular sink (POSIX /dev/null, Windows NUL) produces exit != 0 and
#      NO success banner. Character devices accept writes and keep nothing, so
#      a closed stream is not evidence of a measurement record.
#   3. An unwritable target produces exit != 0 and NO success banner, so the
#      failure cannot be mistaken for a completed run.
#
# Invoked as: cmake -DBENCH=<exe> -DWORK=<dir> -P verify_benchmark_artifact_contract.cmake

if(NOT BENCH)
    message(FATAL_ERROR "BENCH not set")
endif()
if(NOT WORK)
    message(FATAL_ERROR "WORK not set")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# --- 1. writable path: artifact must exist, be non-empty and well formed ------
set(good_json "${WORK}/nested/artifact.json")
execute_process(
    COMMAND "${BENCH}" --1080p-only --json "${good_json}"
    RESULT_VARIABLE good_rc
    OUTPUT_VARIABLE good_out
    ERROR_VARIABLE good_err
    TIMEOUT 300)

if(NOT good_rc EQUAL 0)
    message(FATAL_ERROR "writable-path run exited ${good_rc}, expected 0\n${good_out}\n${good_err}")
endif()
if(NOT EXISTS "${good_json}")
    message(FATAL_ERROR "writable-path run exited 0 but wrote no artifact at ${good_json}")
endif()
file(SIZE "${good_json}" good_size)
if(good_size LESS 64)
    message(FATAL_ERROR "artifact is only ${good_size} bytes; not a real measurement record")
endif()
file(READ "${good_json}" good_body)
if(NOT good_body MATCHES "\"benchmarks\"[ \t\r\n]*:")
    message(FATAL_ERROR "artifact has no \"benchmarks\" member:\n${good_body}")
endif()
# Parse the whole document rather than trusting a substring: a truncated or
# malformed record must not pass as a measurement. string(JSON) fails the test
# with a parse error when the artifact is not valid JSON.
string(JSON bench_count LENGTH "${good_body}" benchmarks)
if(bench_count LESS 1)
    message(FATAL_ERROR "artifact \"benchmarks\" member is not a non-empty array")
endif()
# A directory component that did not exist beforehand must have been created.
if(NOT EXISTS "${WORK}/nested")
    message(FATAL_ERROR "the run did not create the artifact parent directory")
endif()

# The system block must carry a real value, not a hardcoded literal. This is a
# weaker check than the compiler metadata itself; it only refuses the two
# specific regressions this patch fixed.
if(good_body MATCHES "\"build_type\"[ \t\r\n]*:[ \t\r\n]*\"\"")
    message(FATAL_ERROR "build_type is empty")
endif()

# --- 2. non-regular sink: a device is not an artifact -------------------------
# Both canonical sinks open and accept writes while preserving nothing. Before
# this contract existed the tool reported "saved" and exit 0 for both.
if(WIN32)
    set(device_target "NUL")
else()
    set(device_target "/dev/null")
endif()

execute_process(
    COMMAND "${BENCH}" --1080p-only --json "${device_target}"
    RESULT_VARIABLE device_rc
    OUTPUT_VARIABLE device_out
    ERROR_VARIABLE device_err
    TIMEOUT 300)

if(device_rc EQUAL 0)
    message(FATAL_ERROR
        "non-regular sink ${device_target} still exited 0. A character device "
        "must not be accepted as a written measurement artifact.\n"
        "stdout:\n${device_out}\nstderr:\n${device_err}")
endif()
if(device_out MATCHES "Completed Successfully")
    message(FATAL_ERROR
        "non-regular sink ${device_target} printed the success banner:\n${device_out}")
endif()
if(device_out MATCHES "results successfully saved")
    message(FATAL_ERROR
        "non-regular sink ${device_target} claimed the results were saved:\n${device_out}")
endif()

# --- 3. unwritable path: must fail loudly, never silently ---------------------
set(bad_json "${WORK}/blocked/artifact.json")
# Occupy the parent path with a regular file so the directory cannot be created
# and the stream cannot be opened. Portable, unlike relying on permissions,
# which a privileged CI user could bypass.
file(WRITE "${WORK}/blocked" "not a directory\n")
set(blocked_path "${WORK}/blocked/artifact.json")

execute_process(
    COMMAND "${BENCH}" --1080p-only --json "${blocked_path}"
    RESULT_VARIABLE bad_rc
    OUTPUT_VARIABLE bad_out
    ERROR_VARIABLE bad_err
    TIMEOUT 300)

if(bad_rc EQUAL 0)
    message(FATAL_ERROR
        "unwritable artifact path still exited 0. A run that produced no "
        "measurement must not report success.\nstdout:\n${bad_out}\nstderr:\n${bad_err}")
endif()
if(bad_out MATCHES "Completed Successfully")
    message(FATAL_ERROR
        "unwritable artifact path printed the success banner:\n${bad_out}")
endif()
if(bad_json AND EXISTS "${bad_json}")
    message(FATAL_ERROR "unwritable path unexpectedly produced an artifact")
endif()

message(STATUS "benchmark artifact contract PASS: writable run wrote ${good_size} bytes, unwritable run exited ${bad_rc}")
