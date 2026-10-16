#!/usr/bin/env bash
# Issue #13 Windows host-ASan evidence runner (deterministic, sequential).
set -u
export PATH="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64:$PATH"
EV=<evidence-raw>
mkdir -p "$EV"
REPEATS_B0=20
REPEATS_OTHER=5
T=/c/Users/User/Documents/GitHub/HyperCine-public/.worktrees/issue13-windows-asan
cd "$T" || exit 1
BIN=build-asan/tests/Release

run() { # run <logfile> <exe> <args...>
  local log="$1"; shift
  local start end rc
  start=$(date +%s)
  "$@" >"$log" 2>&1
  rc=$?
  end=$(date +%s)
  echo "$(basename "$log") rc=$rc elapsed=$((end-start))s asan_hits=$(grep -c 'AddressSanitizer' "$log")"
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
