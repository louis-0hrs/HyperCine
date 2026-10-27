#!/usr/bin/env bash
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
#
# RCA #159 follow-up sweep: pin the decoder thread cap, vary only the CPU count.
#
# The first sweep varied two quantities at once (thread cap and affinity) and
# established that divergence only appears when worker_threads > affinity_cpus.
# That left the boundary itself unmeasured: the arms stepped 1/2/4/8/16 CPUs
# against a fixed cap, so the transition could be an artefact of the coarse
# grid. This sweep fixes the cap per series and walks the CPU count one step at
# a time across ratio 1.0, so the rate can be read as a function of the ratio
# rather than as a property of a particular pool size.
#
#   series A : cap 4  -> 8 workers, CPUs 1..16   (boundary at 8)
#   series B : cap 2  -> 4 workers, CPUs 1..8    (boundary at 4)
#   series C : cap 8  -> 12 workers, CPUs 6..16  (boundary at 12)
#   series D : confirmation at higher N, concentrated on 1 < ratio < 2 and on
#              the low-ratio side that A/B/C reported as clean
#
# Each cell runs N fresh processes with ONE decode call each, so no state from a
# previous frame can be involved.
#
# Usage: HYPERCINE_THREAD_PROBE=build/hypercine_decoder_thread_probe \
#        HYPERCINE_PACKET=/path/to/packet.cfhd  ./ci/rca159_ratio_sweep.sh [all|a|b|c|d]
set -u

PROBE=${HYPERCINE_THREAD_PROBE:-}
PACKET=${HYPERCINE_PACKET:-}
OUT=${HYPERCINE_SWEEP_OUT:-rca159-ratio-sweep}
PROCS=${HYPERCINE_SWEEP_PROCS:-30}

if [ -z "$PROBE" ] || [ ! -x "$PROBE" ]; then
    echo "set HYPERCINE_THREAD_PROBE to a built hypercine_decoder_thread_probe" >&2
    exit 2
fi
if [ -z "$PACKET" ] || [ ! -f "$PACKET" ]; then
    echo "set HYPERCINE_PACKET to a CFHD packet file" >&2
    exit 2
fi

SERIES=${1:-all}
mkdir -p "$OUT"

run_cell () {
    local cm=$1 aff=$2 tag=$3
    local dir="$OUT/$tag"
    mkdir -p "$dir"
    local p
    for ((p = 0; p < PROCS; p++)); do
        taskset -c "0-$((aff - 1))" "$PROBE" 1 "$cm" 0 "$PACKET" \
            > "$dir/p$(printf %02d "$p").jsonl" 2> "$dir/p$(printf %02d "$p").err"
    done
    echo "done $tag"
}

want () {
    [ "$SERIES" = all ] || [ "$SERIES" = "$1" ]
}

# Series A: 8 workers, CPU count walks through the ratio-1.0 boundary at 8.
if want a; then
    for aff in 1 2 3 4 5 6 7 8 9 10 12 16; do
        run_cell 4 "$aff" "cap4_cpus${aff}"
    done
fi

# Series B: 4 workers, boundary at 4.
if want b; then
    for aff in 1 2 3 4 5 6 8; do
        run_cell 2 "$aff" "cap2_cpus${aff}"
    done
fi

# Series C: 12 workers, boundary at 12.
if want c; then
    for aff in 1 2 3 4 6 8 10 11 12 13 14 16; do
        run_cell 8 "$aff" "cap8_cpus${aff}"
    done
fi

# Series D: confirmation, higher N. Ratio r = workers/cpus, so the 1 < r < 2
# band is exactly where the coarse grid had no cells.
if want d; then
    PROCS=${HYPERCINE_CONFIRM_PROCS:-80}
    for aff in 5 6 7; do
        run_cell 4 "$aff" "conf_cap4_cpus${aff}"
    done
    for aff in 3 4; do
        run_cell 4 "$aff" "conf_cap4_cpus${aff}"
    done
    for aff in 5 6 7 9 10 11; do
        run_cell 8 "$aff" "conf_cap8_cpus${aff}"
    done
    run_cell 2 2 "conf_cap2_cpus2"
fi

echo "SWEEP DONE"
