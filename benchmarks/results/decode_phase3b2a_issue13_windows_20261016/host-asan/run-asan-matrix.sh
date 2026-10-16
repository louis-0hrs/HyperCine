#!/usr/bin/env bash
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
#
# Issue #13 Windows host-ASan evidence runner (deterministic, sequential).
#
# Required environment:
#   HYPERCINE_WORKTREE_DIR  checkout that holds the ASan build
#   HYPERCINE_EVIDENCE_DIR  existing writable directory for the raw per-run logs
# Optional environment:
#   HYPERCINE_ASAN_BIN      test binary directory, defaults to
#                           $HYPERCINE_WORKTREE_DIR/build-asan/tests/Release
#   HYPERCINE_MSVC_BIN      directory prepended to PATH when the MSVC toolset
#                           directory is not already reachable
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
if [ -n "${HYPERCINE_MSVC_BIN:-}" ]; then
  PATH="$HYPERCINE_MSVC_BIN:$PATH"
  export PATH
fi

REPEATS_B0=20
REPEATS_OTHER=5
cd "$T" || fail "cannot enter HYPERCINE_WORKTREE_DIR"
T=$PWD
BIN=${HYPERCINE_ASAN_BIN:-$T/build-asan/tests/Release}
[ -d "$BIN" ] || fail "ASan test binary directory not found; build it first"
for exe in test_decode_phase3b0 test_decode_phase3b2a_reconstruction \
           test_decode_phase3b2a_temporal test_decode_phase3b2r; do
  [ -f "$BIN/$exe.exe" ] || fail "missing test binary $exe.exe; build the ASan configuration first"
done

run() { # run <logfile> <exe> <args...>
  local log="$1"; shift
  local start end rc
  start=$(date +%s)
  "$@" >"$log" 2>&1
  rc=$?
  end=$(date +%s)
  local hits
  hits=$(grep -c 'AddressSanitizer' "$log" 2>/dev/null || true)
  echo "$(basename "$log") rc=$rc elapsed=$((end-start))s asan_hits=$hits"
}

echo "== phase3b0 repetitions =="
for i in $(seq 1 $REPEATS_B0); do
  run "$EV/phase3b0-asan-run$(printf '%02d' "$i").log" "$BIN/test_decode_phase3b0.exe" \
    "$EV/b0-results" "$EV/b0-fixtures" .
done

echo "== reconstruction (default) repetitions =="
for i in $(seq 1 $REPEATS_OTHER); do
  run "$EV/reconstruction-asan-run$(printf '%02d' "$i").log" "$BIN/test_decode_phase3b2a_reconstruction.exe" "$T"
done

echo "== reconstruction --matrix-range =="
run "$EV/reconstruction-matrix-range-asan.log" "$BIN/test_decode_phase3b2a_reconstruction.exe" \
  --matrix-range "$T" "$EV/reconstruction-matrix-range-out"
run "$EV/reconstruction-4k-smoke-asan.log" "$BIN/test_decode_phase3b2a_reconstruction.exe" \
  --4k-smoke "$T" "$EV/reconstruction-4k-smoke-out"

echo "== temporal repetitions =="
for i in $(seq 1 $REPEATS_OTHER); do
  run "$EV/temporal-asan-run$(printf '%02d' "$i").log" "$BIN/test_decode_phase3b2a_temporal.exe"
done

echo "== phase3b2r reference and math =="
run "$EV/phase3b2r-reference-asan.log" "$BIN/test_decode_phase3b2r.exe" --reference-smoke "$T"
run "$EV/phase3b2r-math-asan.log" "$BIN/test_decode_phase3b2r.exe" --math

echo "== done =="
