#!/usr/bin/env bash
# Issue #13 Windows Compute Sanitizer evidence runner.
set -u
CS="/c/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/bin/compute-sanitizer.bat"
T=/c/Users/User/Documents/GitHub/HyperCine-public/.worktrees/issue13-windows-asan
EV=<raw-evidence>
mkdir -p "$EV"
cd "$T" || exit 1
B=build-release-sm89/tests/Release
ROOT="$T"

run() { # run <name> <logfile> <target> <args...>
  local name="$1"; local log="$2"; shift 2
  local start end rc
  start=$(date +%s)
  "$CS" --tool "$name" --error-exitcode 99 --launch-timeout 0 --log-file "$log" "$@" >"$log.stdout" 2>&1
  rc=$?
  end=$(date +%s)
  echo "[$name] $(basename "$log" ) rc=$rc elapsed=$((end-start))s errors=$(grep -c 'ERROR SUMMARY\|RACECHECK SUMMARY' "$log")"
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
