#!/usr/bin/env bash
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
#
# Issue #13 Windows Compute Sanitizer evidence runner.
#
# Required environment:
#   HYPERCINE_WORKTREE_DIR  checkout that holds the sanitizer build
#   HYPERCINE_EVIDENCE_DIR  existing writable directory for the raw logs
# Optional environment:
#   HYPERCINE_COMPUTE_SANITIZER  compute-sanitizer executable, defaults to the
#                                binary of that name on PATH
#   HYPERCINE_SANITIZER_BIN      test binary directory, defaults to
#                                $HYPERCINE_WORKTREE_DIR/build-release-sm89/tests/Release
set -u

usage() {
  printf 'usage: HYPERCINE_WORKTREE_DIR=<checkout> HYPERCINE_EVIDENCE_DIR=<logs> %s\n' \
    "$(basename "$0")" >&2
}

fail() {
  printf '%s: %s\n' "$(basename "$0")" "$1" >&2
  exit 2
}

T=${HYPERCINE_WORKTREE_DIR:-}
EV=${HYPERCINE_EVIDENCE_DIR:-}
if [ -z "$T" ] || [ -z "$EV" ]; then
  usage
  fail "HYPERCINE_WORKTREE_DIR and HYPERCINE_EVIDENCE_DIR are required"
fi
[ -d "$T" ] || fail "HYPERCINE_WORKTREE_DIR is not a directory"
[ -d "$EV" ] || fail "HYPERCINE_EVIDENCE_DIR is not a directory"
[ -w "$EV" ] || fail "HYPERCINE_EVIDENCE_DIR is not writable"

CS=${HYPERCINE_COMPUTE_SANITIZER:-}
if [ -z "$CS" ]; then
  CS=$(command -v compute-sanitizer 2>/dev/null || true)
fi
[ -n "$CS" ] || fail "compute-sanitizer not found; set HYPERCINE_COMPUTE_SANITIZER"
# A Windows .bat launcher runs from this shell but carries no execute bit, so
# accept an existing file as well as a command name on PATH.
if [ ! -f "$CS" ] && ! command -v "$CS" >/dev/null 2>&1; then
  fail "HYPERCINE_COMPUTE_SANITIZER is neither a file nor a command on PATH; use a shell-style path"
fi

cd "$T" || fail "cannot enter HYPERCINE_WORKTREE_DIR"
T=$PWD
B=${HYPERCINE_SANITIZER_BIN:-$T/build-release-sm89/tests/Release}
[ -d "$B" ] || fail "sanitizer test binary directory not found; build it first"
for exe in sanitizer_launch_control test_decode_phase3b2a_temporal \
           test_decode_phase3b2a_reconstruction test_decode_phase3b2a_upstream_temporal; do
  [ -f "$B/$exe.exe" ] || fail "missing test binary $exe.exe; build the sanitizer configuration first"
done
ROOT="$T"

run() { # run <name> <logfile> <target> <args...>
  local name="$1"; local log="$2"; shift 2
  local start end rc
  start=$(date +%s)
  "$CS" --tool "$name" --error-exitcode 99 --launch-timeout 0 --log-file "$log" "$@" >"$log.stdout" 2>&1
  rc=$?
  end=$(date +%s)
  local errors
  errors=$(grep -c 'ERROR SUMMARY\|RACECHECK SUMMARY' "$log" 2>/dev/null || true)
  echo "[$name] $(basename "$log" ) rc=$rc elapsed=$((end-start))s errors=$errors"
}

echo "== control: memcheck, no sanitizer filter =="
"$CS" --tool memcheck --error-exitcode 99 --launch-timeout 0 --log-file "$EV/control-clean-memcheck.log" "$B/sanitizer_launch_control.exe" --clean > "$EV/control-clean-memcheck.stdout" 2>&1
echo "control clean rc=$?"
"$CS" --tool memcheck --error-exitcode 99 --launch-timeout 0 --log-file "$EV/control-violate-memcheck.log" "$B/sanitizer_launch_control.exe" --violate > "$EV/control-violate-memcheck.stdout" 2>&1
echo "control violate rc=$?"

echo "== control: kernel-name filter differential =="
"$CS" --tool memcheck --error-exitcode 99 --launch-timeout 0 --kernel-name kns=sanitizer_control_out_of_bounds \
  --log-file "$EV/control-violate-filter-oob.log" "$B/sanitizer_launch_control.exe" --violate > "$EV/control-violate-filter-oob.stdout" 2>&1
echo "control violate filtered to faulty kernel rc=$?"
"$CS" --tool memcheck --error-exitcode 99 --launch-timeout 0 --kernel-name kns=sanitizer_control_clean \
  --log-file "$EV/control-violate-filter-clean.log" "$B/sanitizer_launch_control.exe" --violate > "$EV/control-violate-filter-clean.stdout" 2>&1
echo "control violate filtered to clean kernel rc=$?"

echo "== decoder: minimal temporal target =="
for tool in memcheck initcheck racecheck synccheck; do
  run "$tool" "$EV/temporal-$tool.log" "$B/test_decode_phase3b2a_temporal.exe"
done

echo "== decoder: production reconstruction target =="
for tool in memcheck initcheck racecheck synccheck; do
  run "$tool" "$EV/reconstruction-$tool.log" "$B/test_decode_phase3b2a_reconstruction.exe" "$ROOT"
done

echo "== decoder: upstream temporal reference target =="
run memcheck "$EV/upstream-temporal-memcheck.log" "$B/test_decode_phase3b2a_upstream_temporal.exe"

echo "== done =="
