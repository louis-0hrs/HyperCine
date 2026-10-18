#!/usr/bin/env bash
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
#
# RCA #159 interleaved control: is the divergence rate a function of the
# thread-to-CPU ratio, or of host state that drifts between sessions?
#
# The sequential sweeps disagreed with themselves on two cells at the same
# settings (e.g. 4 workers on 1 CPU: 5/30 in one series, 0/30 in another), which
# a sequential design cannot attribute. Here every round runs all four cells
# back to back, several times over, so any slow drift in host load or CPU
# frequency lands on all arms instead of on one of them.
#
#   hot   cap 4, 1..2 CPUs  - ratio >= 4, expected to diverge
#   edge  cap 4, 4 CPUs     - ratio 2.0, the contested cell
#   mid   cap 8, 4 CPUs     - ratio 3.0, second contested cell
#   cold  cap 4, 8 CPUs     - ratio 1.0, negative control
#
# Usage: HYPERCINE_THREAD_PROBE=build/hypercine_decoder_thread_probe \
#        HYPERCINE_PACKET=/path/to/packet.cfhd  ./ci/rca159_interleaved.sh
set -u

PROBE=${HYPERCINE_THREAD_PROBE:-}
PACKET=${HYPERCINE_PACKET:-}
OUT=${HYPERCINE_SWEEP_OUT:-rca159-interleaved}
ROUNDS=${HYPERCINE_ROUNDS:-4}
PROCS=${HYPERCINE_SWEEP_PROCS:-40}

if [ -z "$PROBE" ] || [ ! -x "$PROBE" ]; then
    echo "set HYPERCINE_THREAD_PROBE to a built hypercine_decoder_thread_probe" >&2
    exit 2
fi
if [ -z "$PACKET" ] || [ ! -f "$PACKET" ]; then
    echo "set HYPERCINE_PACKET to a CFHD packet file" >&2
    exit 2
fi

mkdir -p "$OUT"

run_cell () {
    local round=$1 cm=$2 aff=$3 tag=$4
    local dir="$OUT/${tag}"
    mkdir -p "$dir"
    cat /proc/loadavg >> "$dir/loadavg.log"
    local p
    for ((p = 0; p < PROCS; p++)); do
        taskset -c "0-$((aff - 1))" "$PROBE" 1 "$cm" 0 "$PACKET" \
            > "$dir/r${round}p$(printf %02d "$p").jsonl" 2> "$dir/r${round}p$(printf %02d "$p").err"
    done
    echo "round $round $tag done"
}

for ((r = 1; r <= ROUNDS; r++)); do
    run_cell "$r" 4 1 "hot_1cpu"
    run_cell "$r" 4 2 "hot_2cpu"
    run_cell "$r" 4 4 "edge_4cpu"
    run_cell "$r" 8 4 "mid_4cpu"
    run_cell "$r" 4 8 "cold_8cpu"
done

echo "INTERLEAVED DONE"
