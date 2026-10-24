# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

# Fail-closed artifact contract for a benchmark executable (issues #33, #226).
#
# Three directions:
#   1. A writable nested path produces a real JSON artifact, exit 0, and the
#      tool's success banner. The parent directory is created by the tool.
#   2. An unwritable target (the artifact path itself occupied by a directory)
#      produces exit != 0 and does NOT print that banner.
#   3. A character device (POSIX /dev/null, Windows NUL) produces exit != 0 and
#      does NOT print a success, "Wrote" or "saved" banner. Those devices accept
#      every write and keep nothing, so a closed stream is not an artifact.
#
# The artifact is parsed with string(JSON) (CMake >= 3.19), not a substring
# search. A truncated or malformed record fails the test.
#
# Invoked as:
#   cmake -DBENCH=<exe> -DWORK=<dir> -DSUCCESS_BANNER=<text> -DJSON_KEY=<member>
#         [-DEXTRA_ARG=--1080p-only] -P verify_benchmark_fail_closed.cmake
# The artifact path is the final positional argument. EXTRA_ARG, when set, is
# placed before it (for example --1080p-only).

if(NOT BENCH)
    message(FATAL_ERROR "BENCH not set")
endif()
if(NOT WORK)
    message(FATAL_ERROR "WORK not set")
endif()
if(NOT SUCCESS_BANNER)
    message(FATAL_ERROR "SUCCESS_BANNER not set")
endif()
if(NOT JSON_KEY)
    message(FATAL_ERROR "JSON_KEY not set")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(cmd "${BENCH}")
if(EXTRA_ARG)
    separate_arguments(extra_list NATIVE_COMMAND "${EXTRA_ARG}")
    list(APPEND cmd ${extra_list})
endif()

# --- 1. writable path -------------------------------------------------------
set(good_json "${WORK}/nested/artifact.json")
set(good_cmd ${cmd} "${good_json}")
execute_process(
    COMMAND ${good_cmd}
    WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE good_rc
    OUTPUT_VARIABLE good_out
    ERROR_VARIABLE good_err
    TIMEOUT 300)

if(NOT good_rc EQUAL 0)
    message(FATAL_ERROR "writable-path run exited ${good_rc}, expected 0\n${good_out}\n${good_err}")
endif()
if(NOT good_out MATCHES "${SUCCESS_BANNER}")
    message(FATAL_ERROR "writable-path run did not print its success banner\n${good_out}")
endif()
if(NOT EXISTS "${good_json}")
    message(FATAL_ERROR "writable-path run exited 0 but wrote no artifact at ${good_json}")
endif()
file(SIZE "${good_json}" good_size)
if(good_size LESS 64)
    message(FATAL_ERROR "artifact is only ${good_size} bytes; not a real measurement record")
endif()
file(READ "${good_json}" good_body)
# Parse the document. string(JSON) aborts this script when the artifact is not
# valid JSON, and LENGTH of the named array is its element count.
string(JSON json_count LENGTH "${good_body}" "${JSON_KEY}")
if(json_count LESS 1)
    message(FATAL_ERROR "artifact \"${JSON_KEY}\" member is not a non-empty array")
endif()
if(NOT EXISTS "${WORK}/nested")
    message(FATAL_ERROR "the run did not create the artifact parent directory")
endif()

# --- 2. unwritable path -----------------------------------------------------
set(blocked_path "${WORK}/blocked/artifact.json")
file(MAKE_DIRECTORY "${blocked_path}")
set(bad_cmd ${cmd} "${blocked_path}")
execute_process(
    COMMAND ${bad_cmd}
    WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE bad_rc
    OUTPUT_VARIABLE bad_out
    ERROR_VARIABLE bad_err
    TIMEOUT 300)

if(bad_rc EQUAL 0)
    message(FATAL_ERROR
        "unwritable artifact path still exited 0. A run that produced no "
        "measurement must not report success.\nstdout:\n${bad_out}\nstderr:\n${bad_err}")
endif()
if(bad_out MATCHES "${SUCCESS_BANNER}" OR bad_err MATCHES "${SUCCESS_BANNER}")
    message(FATAL_ERROR
        "unwritable artifact path printed the success banner:\n${bad_out}\n${bad_err}")
endif()

# --- 3. character device: accepts writes, retains nothing -------------------
if(WIN32)
    set(device_target "NUL")
else()
    set(device_target "/dev/null")
endif()
set(device_cmd ${cmd} "${device_target}")
execute_process(
    COMMAND ${device_cmd}
    WORKING_DIRECTORY "${WORK}"
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
if(device_out MATCHES "${SUCCESS_BANNER}" OR device_err MATCHES "${SUCCESS_BANNER}")
    message(FATAL_ERROR
        "non-regular sink ${device_target} printed the success banner:\n${device_out}\n${device_err}")
endif()
if(device_out MATCHES "[Ww]rote" OR device_out MATCHES "[Ss]aved")
    message(FATAL_ERROR
        "non-regular sink ${device_target} printed a wrote/saved banner:\n${device_out}")
endif()

message(STATUS "benchmark fail-closed contract PASS: writable run wrote ${good_size} bytes, directory run exited ${bad_rc}, device run exited ${device_rc}")
