# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

# Fail-closed CLI contract for gpu_diagnostic_probe.
#
# The probe writes an evidence artifact, so its exit status and its write
# behavior are part of the contract:
#   * an existing artifact is never modified without --overwrite, including on
#     the INVALID_ARGUMENTS path;
#   * numeric tokens outside int range are rejected instead of narrowing to a
#     different workload (4294967297 used to become 1);
#   * --overwrite is the only path that may replace an existing file, and it
#     still refuses to replace a directory.

if(NOT DEFINED PROBE)
    message(FATAL_ERROR "verify_gpu_diagnostic_probe_cli: PROBE is required")
endif()
if(NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "verify_gpu_diagnostic_probe_cli: WORK_DIR is required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(SENTINEL "${WORK_DIR}/sentinel.json")
set(SENTINEL_TEXT "sentinel bytes that must survive every non-overwrite run\n")
file(WRITE "${SENTINEL}" "${SENTINEL_TEXT}")

function(expect_sentinel_intact label)
    file(READ "${SENTINEL}" actual)
    if(NOT actual STREQUAL "${SENTINEL_TEXT}")
        message(FATAL_ERROR "${label}: an existing output artifact was modified")
    endif()
endfunction()

function(run_probe label)
    execute_process(
        COMMAND "${PROBE}" ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(RC "${rc}" PARENT_SCOPE)
    set(OUT "${out}" PARENT_SCOPE)
    set(ERR "${err}" PARENT_SCOPE)
endfunction()

# 1. Invalid token plus an existing artifact: refusal, artifact untouched.
run_probe("invalid --width" "--width=abc" "--out=${SENTINEL}")
if(RC STREQUAL "0")
    message(FATAL_ERROR "invalid --width: expected a non-zero exit\n${OUT}${ERR}")
endif()
if(NOT ERR MATCHES "already exists")
    message(FATAL_ERROR "invalid --width: refusal did not name the existing artifact\n${ERR}")
endif()
expect_sentinel_intact("invalid --width")

# 2. A token that narrows to 1 must be rejected, not silently accepted.
set(NARROW "${WORK_DIR}/narrow.json")
run_probe("narrowing" "--width=4294967297" "--out=${NARROW}")
if(NOT RC STREQUAL "2")
    message(FATAL_ERROR "narrowing: expected exit 2, got ${RC}\n${OUT}${ERR}")
endif()
file(READ "${NARROW}" narrow_json)
if(NOT narrow_json MATCHES "\"status\": \"INVALID_ARGUMENTS\"")
    message(FATAL_ERROR "narrowing: token was not reported as invalid\n${narrow_json}")
endif()
if(NOT narrow_json MATCHES "\"message\": \"invalid --width\"")
    message(FATAL_ERROR "narrowing: wrong rejection message\n${narrow_json}")
endif()

# 3. Out-of-range tokens for every numeric option.
set(cases
    "width:4294967297"
    "height:4294967297"
    "frames:999999999999999999999999"
    "warmup:-1"
    "slots:5")
set(case_index 0)
foreach(case IN LISTS cases)
    string(REPLACE ":" ";" parts "${case}")
    list(GET parts 0 option)
    list(GET parts 1 value)
    math(EXPR case_index "${case_index} + 1")
    set(target "${WORK_DIR}/bounds-${case_index}.json")
    run_probe("--${option}=${value}" "--${option}=${value}" "--out=${target}")
    if(NOT RC STREQUAL "2")
        message(FATAL_ERROR "--${option}=${value}: expected exit 2, got ${RC}\n${OUT}${ERR}")
    endif()
    file(READ "${target}" bounds_json)
    if(NOT bounds_json MATCHES "\"status\": \"INVALID_ARGUMENTS\"")
        message(FATAL_ERROR "--${option}=${value}: not reported as invalid\n${bounds_json}")
    endif()
endforeach()

# 4. Valid arguments plus an existing artifact: whatever the device state, the
#    pre-existing artifact must stay byte-identical.
run_probe("valid arguments" "--width=64" "--height=64" "--frames=1" "--warmup=0"
          "--slots=1" "--out=${SENTINEL}")
if(RC STREQUAL "2")
    message(FATAL_ERROR "valid arguments were rejected\n${OUT}${ERR}")
endif()
expect_sentinel_intact("valid arguments")

# 5. A fresh path produces a complete report (exit 0 on a device, 3 when the
#    run itself is unavailable), never exit 2.
set(FRESH "${WORK_DIR}/fresh.json")
run_probe("fresh output" "--width=64" "--height=64" "--frames=1" "--warmup=0"
          "--slots=1" "--out=${FRESH}")
if(NOT RC STREQUAL "0" AND NOT RC STREQUAL "3")
    message(FATAL_ERROR "fresh output: unexpected exit ${RC}\n${OUT}${ERR}")
endif()
if(NOT EXISTS "${FRESH}")
    message(FATAL_ERROR "fresh output: report was not written")
endif()
file(READ "${FRESH}" fresh_json)
if(NOT fresh_json MATCHES "\"schema\": \"hypercine.gpu_diagnostic\"")
    message(FATAL_ERROR "fresh output: wrong schema\n${fresh_json}")
endif()
if(NOT fresh_json MATCHES "\"status\": \"(PASSED|FAILED)\"")
    message(FATAL_ERROR "fresh output: missing terminal status\n${fresh_json}")
endif()

# 6. --overwrite is the only path allowed to replace an existing artifact.
run_probe("explicit overwrite" "--width=64" "--height=64" "--frames=1" "--warmup=0"
          "--slots=1" "--overwrite" "--out=${SENTINEL}")
if(RC STREQUAL "2")
    message(FATAL_ERROR "explicit overwrite: arguments were rejected\n${OUT}${ERR}")
endif()
file(READ "${SENTINEL}" overwritten)
if(overwritten STREQUAL "${SENTINEL_TEXT}")
    message(FATAL_ERROR "explicit overwrite: sentinel was not replaced")
endif()
if(NOT overwritten MATCHES "\"schema\": \"hypercine.gpu_diagnostic\"")
    message(FATAL_ERROR "explicit overwrite: replacement is not a report\n${overwritten}")
endif()

# 7. Even --overwrite must never replace a directory.
set(DIRECTORY_TARGET "${WORK_DIR}/existing-directory")
file(MAKE_DIRECTORY "${DIRECTORY_TARGET}")
run_probe("overwrite directory" "--width=64" "--height=64" "--frames=1" "--warmup=0"
          "--slots=1" "--overwrite" "--out=${DIRECTORY_TARGET}")
if(RC STREQUAL "0")
    message(FATAL_ERROR "overwrite directory: expected a non-zero exit\n${OUT}${ERR}")
endif()
if(NOT IS_DIRECTORY "${DIRECTORY_TARGET}")
    message(FATAL_ERROR "overwrite directory: existing directory was destroyed")
endif()

# 8. A device path accepts writes but stores no artifact. Exit 0 here would be
#    a false OKAY, so it must be refused even with --overwrite.
if(WIN32)
    set(DEVICE_TARGET "NUL")
else()
    set(DEVICE_TARGET "/dev/null")
endif()
run_probe("device output" "--width=64" "--height=64" "--frames=1" "--warmup=0"
          "--slots=1" "--overwrite" "--out=${DEVICE_TARGET}")
if(RC STREQUAL "0")
    message(FATAL_ERROR "device output: exit 0 without an artifact\n${OUT}${ERR}")
endif()
if(NOT ERR MATCHES "not a regular file")
    message(FATAL_ERROR "device output: refusal did not name the non-regular path\n${ERR}")
endif()

message(STATUS "gpu_diagnostic_probe CLI contract PASS")
